// undae-pluginhost: the plug-in host built on JUCE. The macOS edition's host (Audio Units, VST3 and VST2); it builds on
// Windows and Linux too, where the app uses it only when UNDAE_PLUGIN_HOST=juce (for testing). It speaks the protocol in
// native/PROTOCOL.md, so the app talks to it exactly as to the other hosts:
//   - prints {"port":N,"token":"..."} on start, then takes one WebSocket connection per plug-in instance;
//   - binary messages are audio blocks, answered in order; text messages are JSON requests and events;
//   - with --child it exits when its stdin closes (the app holds it open).
// VST3 and Audio Units go through JUCE. VST2 uses the interface written out in Vst2.h (JUCE's own VST2 hosting needs
// Steinberg's VST2 SDK, which is not distributed any more).
//
// Threads: each connection has its own thread, which reads the socket and processes audio (so it is that instance's
// audio thread). Requests that load, scan, show windows or touch state run on the message thread, in order. A global
// reader-writer lock keeps plug-in loading and unloading (exclusive) away from processing (shared), as in vsthost.c:
// plug-ins often share tables between their instances.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <thread>
#include <map>
#include <mutex>
#include <shared_mutex>
#include "Vst2.h"
#if JUCE_MAC
 #include <sys/sysctl.h>
#endif

namespace {

constexpr int kMaxBlock = 1024;    // largest piece handed to a plug-in in one call
constexpr int kMaxFrames = 8192;   // largest audio message
constexpr uint64_t kMaxMsg = 64u << 20;
std::shared_mutex loadLock;
juce::String token;

juce::AudioPluginFormatManager& formats() { static auto* fm = [] { auto* m = new juce::AudioPluginFormatManager(); juce::addDefaultFormatsToManager(*m); return m; }(); return *fm; }

// ------------------------------------------------------------------------------------------------ small helpers
void sha1(const uint8_t* msg, size_t len, uint8_t out[20]) {
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    std::vector<uint8_t> m(msg, msg + len); m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    const uint64_t bits = (uint64_t)len * 8; for (int i = 7; i >= 0; i--) m.push_back((uint8_t)(bits >> (i * 8)));
    auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    for (size_t o = 0; o < m.size(); o += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)m[o + i * 4] << 24 | (uint32_t)m[o + i * 4 + 1] << 16 | (uint32_t)m[o + i * 4 + 2] << 8 | m[o + i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; } else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; } else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i]; e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++) { out[i * 4] = (uint8_t)(h[i] >> 24); out[i * 4 + 1] = (uint8_t)(h[i] >> 16); out[i * 4 + 2] = (uint8_t)(h[i] >> 8); out[i * 4 + 3] = (uint8_t)h[i]; }
}

juce::var obj(std::initializer_list<std::pair<const char*, juce::var>> kv) { auto* o = new juce::DynamicObject(); for (auto& [k, v] : kv) o->setProperty(k, v); return juce::var(o); }
void set(juce::var& o, const char* k, const juce::var& v) { if (auto* d = o.getDynamicObject()) d->setProperty(k, v); }
juce::var reply(const juce::var& req, bool ok) {
    juce::var r = obj({ { "re", req.getProperty("op", "") } }); if (req.hasProperty("seq")) set(r, "seq", req.getProperty("seq", 0)); set(r, "ok", ok); return r;
}
juce::var errorReply(const juce::var& req, const juce::String& msg) { auto r = reply(req, false); set(r, "error", msg); return r; }

// A window for a plug-in's editor. Closing it only asks the owner, which closes it later (not inside its own callback).
struct PluginWindow : juce::DocumentWindow {
    std::function<void()> onClose;
    explicit PluginWindow(const juce::String& title) : DocumentWindow(title, juce::Colours::black, DocumentWindow::closeButton | DocumentWindow::minimiseButton) { setUsingNativeTitleBar(true); }
    void closeButtonPressed() override { setVisible(false); if (onClose) onClose(); }
};

// ------------------------------------------------------------------------------------------------ plug-ins
struct MidiEv { int offset; uint8_t d[3]; };

struct Plug {
    virtual ~Plug() = default;
    juce::String name, vendor, id; int latency = 0, inputs = 2, outputs = 2; bool notes = false, hasGui = false;
    virtual int numParams() const = 0;
    virtual juce::var param(int i, bool full) const = 0;
    virtual juce::String text(int i, float v) const = 0;
    virtual void setParam(int i, float v) = 0;                 // audio thread, at the start of a piece
    virtual void process(const float* inL, const float* inR, float* outL, float* outR, int n, const MidiEv* ev, int nev) = 0;   // n <= kMaxBlock
    virtual void reset() = 0;
    virtual juce::MemoryBlock getState() = 0;
    virtual bool setState(const juce::MemoryBlock&) = 0;
    virtual juce::String showGui() = 0;                        // "" on success, else why not
    virtual void closeGui() = 0;
    virtual bool guiOpen() const = 0;
    // from the plug-in (any thread)
    std::function<void(int, float)> onParam; std::function<void()> onDirty, onRescan, onGuiClosed;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    void askClose() {   // the window's close button: close later, on the message thread, if this plug-in still exists
        std::weak_ptr<bool> w = alive;
        juce::MessageManager::callAsync([this, w] { if (w.expired()) return; closeGui(); if (onGuiClosed) onGuiClosed(); if (onDirty) onDirty(); });
    }
};

// ---- VST3 and Audio Units, through JUCE
struct PlayHead : juce::AudioPlayHead {   // a steady 120 BPM, 4/4, playing (as the other hosts report)
    std::atomic<int64_t> pos{ 0 }; double sr = 48000;
    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo p; const int64_t s = pos.load(); p.setBpm(120.0); p.setTimeSignature(TimeSignature{ 4, 4 }); p.setIsPlaying(true);
        p.setTimeInSamples(s); p.setTimeInSeconds(s / sr); p.setPpqPosition(s / sr * 2.0); return p;
    }
};

struct JucePlug : Plug, juce::AudioProcessorListener {
    std::unique_ptr<juce::AudioPluginInstance> inst; juce::AudioBuffer<float> buf; juce::MidiBuffer mb; PlayHead ph;
    std::unique_ptr<juce::AudioProcessorEditor> ed; std::unique_ptr<PluginWindow> win;
    mutable std::mutex hm; std::map<int, float> fromApp;   // values the app set, so their echoes are not sent back to it

    JucePlug(std::unique_ptr<juce::AudioPluginInstance> p, double sr) : inst(std::move(p)) {
        auto l = inst->getBusesLayout();   // stereo main buses where the plug-in allows it
        if (!l.inputBuses.isEmpty()) l.inputBuses.getReference(0) = juce::AudioChannelSet::stereo();
        if (!l.outputBuses.isEmpty()) l.outputBuses.getReference(0) = juce::AudioChannelSet::stereo();
        if (inst->checkBusesLayoutSupported(l)) inst->setBusesLayout(l);
        ph.sr = sr; inst->setPlayHead(&ph); inst->setRateAndBufferSizeDetails(sr, kMaxBlock); inst->prepareToPlay(sr, kMaxBlock);
        buf.setSize(std::max(2, std::max(inst->getTotalNumInputChannels(), inst->getTotalNumOutputChannels())), kMaxBlock); mb.ensureSize(8192);
        const auto d = inst->getPluginDescription(); name = d.name; vendor = d.manufacturerName; id = d.createIdentifierString();
        latency = inst->getLatencySamples(); inputs = inst->getMainBusNumInputChannels(); outputs = inst->getMainBusNumOutputChannels();
        notes = inst->acceptsMidi(); hasGui = inst->hasEditor();
        inst->addListener(this);
    }
    ~JucePlug() override { closeGui(); inst->removeListener(this); inst->releaseResources(); }

    static juce::String textOf(const juce::AudioProcessorParameter* p, float v) {
        auto t = p->getText(v, 64).trim(); const auto l = p->getLabel().trim(); return l.isNotEmpty() && !t.endsWith(l) ? t + " " + l : t;
    }
    int numParams() const override { return inst->getParameters().size(); }
    juce::var param(int i, bool full) const override {
        const auto* p = inst->getParameters()[i]; const float v = p->getValue();
        auto o = obj({ { "id", i }, { "value", v }, { "text", textOf(p, v) } });
        if (full) { set(o, "name", p->getName(128)); set(o, "module", ""); set(o, "min", 0); set(o, "max", 1); set(o, "def", p->getDefaultValue());
                    set(o, "stepped", p->isDiscrete()); set(o, "hidden", false); set(o, "readonly", false); }
        return o;
    }
    juce::String text(int i, float v) const override { auto& ps = inst->getParameters(); return juce::isPositiveAndBelow(i, ps.size()) ? textOf(ps[i], v) : juce::String(); }
    void setParam(int i, float v) override {
        auto& ps = inst->getParameters(); if (!juce::isPositiveAndBelow(i, ps.size())) return;
        { std::lock_guard<std::mutex> l(hm); fromApp[i] = v; } ps[i]->setValue(v);
    }
    void process(const float* inL, const float* inR, float* outL, float* outR, int n, const MidiEv* ev, int nev) override {
        buf.setSize(buf.getNumChannels(), n, false, false, true); buf.clear();
        if (inputs >= 2) { buf.copyFrom(0, 0, inL, n); buf.copyFrom(1, 0, inR, n); }
        else if (inputs == 1) { buf.copyFrom(0, 0, inL, n, 0.5f); buf.addFrom(0, 0, inR, n, 0.5f); }
        mb.clear(); for (int i = 0; i < nev; i++) mb.addEvent(juce::MidiMessage(ev[i].d[0], ev[i].d[1], ev[i].d[2]), ev[i].offset);
        inst->processBlock(buf, mb); ph.pos += n;
        if (outputs <= 0) { std::fill_n(outL, n, 0.f); std::fill_n(outR, n, 0.f); return; }
        const float* L = buf.getReadPointer(0); const float* R = outputs > 1 ? buf.getReadPointer(1) : L;
        std::copy_n(L, n, outL); std::copy_n(R, n, outR);
    }
    void reset() override { inst->reset(); }
    juce::MemoryBlock getState() override { juce::MemoryBlock m; inst->getStateInformation(m); juce::MemoryBlock r("J", 1); r.append(m.getData(), m.getSize()); return r; }
    bool setState(const juce::MemoryBlock& m) override {
        if (m.getSize() < 1 || static_cast<const char*>(m.getData())[0] != 'J') return false;
        inst->setStateInformation(static_cast<const char*>(m.getData()) + 1, (int)m.getSize() - 1); return true;
    }
    juce::String showGui() override {
        if (win) { win->setVisible(true); win->toFront(true); return {}; }
        if (!inst->hasEditor()) return "This plug-in has no window of its own";
        ed.reset(inst->createEditorAndMakeActive()); if (!ed) return "The plug-in could not open its window";
        win = std::make_unique<PluginWindow>(name); win->onClose = [this] { askClose(); };
        win->setContentNonOwned(ed.get(), true); win->setResizable(ed->isResizable(), false);
        win->centreWithSize(win->getWidth(), win->getHeight()); win->setVisible(true); win->toFront(true);
        return {};
    }
    void closeGui() override { if (win) { win->clearContentComponent(); win.reset(); } ed.reset(); }
    bool guiOpen() const override { return win != nullptr; }

    void audioProcessorParameterChanged(juce::AudioProcessor*, int i, float v) override {
        { std::lock_guard<std::mutex> l(hm); auto it = fromApp.find(i); if (it != fromApp.end() && std::abs(it->second - v) < 1e-6f) return; }
        if (onParam) onParam(i, v);
    }
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& d) override { if (d.parameterInfoChanged) { if (onRescan) onRescan(); } else if (onDirty) onDirty(); }
    void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int) override { if (onDirty) onDirty(); }
};

// ---- VST2, through the interface in Vst2.h
// Libraries stay loaded until the host exits: unloading a plug-in whose threads were still winding down wedged the next
// load on Windows (see HANDOVER.md), and macOS bundles are no safer to unload.
std::vector<juce::DynamicLibrary*>& keptLibs() { static auto* v = new std::vector<juce::DynamicLibrary*>(); return *v; }

juce::File vst2Binary(const juce::File& f) {   // a .vst bundle's executable (macOS), or the file itself (a Windows .dll)
    if (!f.isDirectory()) return f;
    const auto dir = f.getChildFile("Contents").getChildFile("MacOS");
    if (dir.getChildFile(f.getFileNameWithoutExtension()).existsAsFile()) return dir.getChildFile(f.getFileNameWithoutExtension());
    for (auto& c : dir.findChildFiles(juce::File::findFiles, false)) return c;
    return {};
}

struct Vst2Plug;
thread_local Vst2Plug* vst2Loading = nullptr;

struct Vst2Plug : Plug, private juce::Timer {
    vst2::AEffect* e = nullptr; vst2::TimeInfo time{}; double sr = 48000; bool started = false;
    std::vector<std::vector<float>> inB, outB; std::vector<float*> ip, op; vst2::MidiEvent evs[vst2::kMaxEvents]{}; vst2::Events events{};
    std::unique_ptr<juce::Component> view; std::unique_ptr<PluginWindow> win;

    static intptr_t master(vst2::AEffect* fx, int32_t op, int32_t index, intptr_t value, void* ptr, float opt) {   // the plug-in calling its host
        auto* p = fx && fx->user ? static_cast<Vst2Plug*>(fx->user) : vst2Loading;
        auto copy = [](void* dst, const char* s) { if (dst) std::memcpy(dst, s, std::strlen(s) + 1); };
        switch (op) {
            case vst2::amAutomate: if (p && p->onParam) p->onParam(index, opt); return 0;
            case vst2::amVersion: return 2400;
            case vst2::amSizeWindow:
                if (p && p->view) { const int w = index, h = (int)value; auto* v = p->view.get(); auto resize = [v, w, h] { v->setSize(w, h); };
                    if (juce::MessageManager::existsAndIsCurrentThread()) resize(); else juce::MessageManager::callAsync(resize); return 1; }
                return 0;
            case vst2::amGetTime: return p ? (intptr_t)&p->time : 0;
            case vst2::amGetSampleRate: return p ? (intptr_t)p->sr : 48000;
            case vst2::amGetBlockSize: return kMaxBlock;
            case vst2::amGetProcessLevel: return juce::MessageManager::existsAndIsCurrentThread() ? 1 : 2;
            case vst2::amGetVendorString: case vst2::amGetProductString: copy(ptr, "Undae Sonicae"); return 1;
            case vst2::amGetVendorVersion: return 100;
            case vst2::amCanDo: { const char* s = static_cast<const char*>(ptr); return s && (!std::strcmp(s, "sendVstEvents") || !std::strcmp(s, "sendVstMidiEvent") || !std::strcmp(s, "sendVstTimeInfo") || !std::strcmp(s, "supplyIdle")) ? 1 : 0; }
            case vst2::amGetLanguage: return 1;
            case vst2::amBeginEdit: return 1;
            case vst2::amEndEdit: if (p && p->onDirty) p->onDirty(); return 1;
            default: return 0;
        }
    }
    juce::String str(int32_t op, int32_t index = 0) const { char b[512] = {}; e->dispatcher(e, op, index, 0, b, 0); b[255] = 0; return juce::String::fromUTF8(b).trimEnd(); }

    // Opens the plug-in (no processing set up yet). Call with loadLock held exclusively.
    static std::unique_ptr<Vst2Plug> open(const juce::File& file, juce::String& err) {
        const auto bin = vst2Binary(file); auto* lib = new juce::DynamicLibrary();
        if (bin == juce::File() || !lib->open(bin.getFullPathName())) { delete lib; err = "This file could not be loaded as a VST2 plug-in for this computer"; return nullptr; }
        using Entry = vst2::AEffect* (*)(vst2::MasterFn);
        auto entry = (Entry)lib->getFunction("VSTPluginMain"); if (!entry) entry = (Entry)lib->getFunction("main_macho"); if (!entry) entry = (Entry)lib->getFunction("main");
        if (!entry) { delete lib; err = "This is not a VST2 plug-in"; return nullptr; }
        keptLibs().push_back(lib);
        auto p = std::make_unique<Vst2Plug>(); vst2Loading = p.get(); vst2::AEffect* fx = entry(&master); vst2Loading = nullptr;
        if (!fx || fx->magic != vst2::kMagic) { err = "The plug-in did not start"; return nullptr; }
        p->e = fx; fx->user = p.get(); fx->dispatcher(fx, vst2::effOpen, 0, 0, nullptr, 0);
        p->name = p->str(vst2::effGetEffectName); if (p->name.isEmpty()) p->name = p->str(vst2::effGetProductString); if (p->name.isEmpty()) p->name = file.getFileNameWithoutExtension();
        p->vendor = p->str(vst2::effGetVendorString); p->id = juce::String::toHexString((uint32_t)fx->uniqueID).paddedLeft('0', 8);
        const bool synth = (fx->flags & vst2::flagIsSynth) != 0;
        p->inputs = synth ? 0 : fx->numInputs; p->outputs = fx->numOutputs; p->latency = fx->initialDelay; p->hasGui = (fx->flags & vst2::flagHasEditor) != 0;
        p->notes = synth || fx->dispatcher(fx, vst2::effCanDo, 0, 0, (void*)"receiveVstMidiEvent", 0) > 0;
        return p;
    }
    void start(double rate) {   // with loadLock held exclusively
        sr = rate; time = {}; time.sampleRate = sr; time.tempo = 120; time.sigNum = 4; time.sigDen = 4; time.flags = 1 << 1 | 1 << 10 | 1 << 13;   // playing; tempo and signature valid
        e->dispatcher(e, vst2::effSetSampleRate, 0, 0, nullptr, (float)sr); e->dispatcher(e, vst2::effSetBlockSize, 0, kMaxBlock, nullptr, 0);
        e->dispatcher(e, vst2::effMainsChanged, 0, 1, nullptr, 0); e->dispatcher(e, vst2::effStartProcess, 0, 0, nullptr, 0); started = true;
        inB.assign((size_t)std::max(0, e->numInputs), std::vector<float>(kMaxBlock)); outB.assign((size_t)std::max(0, e->numOutputs), std::vector<float>(kMaxBlock));
        ip.resize(inB.size()); op.resize(outB.size());
    }
    ~Vst2Plug() override {   // with loadLock held exclusively
        closeGui(); if (!e) return;
        if (started) { e->dispatcher(e, vst2::effStopProcess, 0, 0, nullptr, 0); e->dispatcher(e, vst2::effMainsChanged, 0, 0, nullptr, 0); }
        e->dispatcher(e, vst2::effClose, 0, 0, nullptr, 0);
    }

    int numParams() const override { return e->numParams; }
    juce::var param(int i, bool full) const override {
        const float v = e->getParameter(e, i); auto o = obj({ { "id", i }, { "value", v }, { "text", text(i, v) } });
        if (full) { set(o, "name", str(vst2::effGetParamName, i)); set(o, "module", ""); set(o, "min", 0); set(o, "max", 1); set(o, "def", v);
                    set(o, "stepped", false); set(o, "hidden", false); set(o, "readonly", false); }
        return o;
    }
    juce::String text(int i, float) const override { const auto d = str(vst2::effGetParamDisplay, i), l = str(vst2::effGetParamLabel, i); return l.isNotEmpty() ? d + " " + l : d; }
    void setParam(int i, float v) override { if (juce::isPositiveAndBelow(i, e->numParams)) e->setParameter(e, i, v); }
    void process(const float* inL, const float* inR, float* outL, float* outR, int n, const MidiEv* ev, int nev) override {
        if (inB.size() >= 2) { std::copy_n(inL, n, inB[0].data()); std::copy_n(inR, n, inB[1].data()); for (size_t c = 2; c < inB.size(); c++) std::fill_n(inB[c].data(), n, 0.f); }
        else if (inB.size() == 1) for (int i = 0; i < n; i++) inB[0][(size_t)i] = (inL[i] + inR[i]) * 0.5f;
        if (nev > 0) {
            const int k = std::min(nev, vst2::kMaxEvents); events.numEvents = k; events.reserved = 0;
            for (int i = 0; i < k; i++) { auto& m = evs[i]; m = {}; m.type = 1; m.byteSize = (int32_t)sizeof m; m.deltaFrames = ev[i].offset; std::memcpy(m.midi, ev[i].d, 3); events.events[i] = &m; }
            e->dispatcher(e, vst2::effProcessEvents, 0, 0, &events, 0);
        }
        for (size_t c = 0; c < inB.size(); c++) ip[c] = inB[c].data();
        for (size_t c = 0; c < outB.size(); c++) op[c] = outB[c].data();
        if ((e->flags & vst2::flagCanReplacing) && e->processReplacing) e->processReplacing(e, ip.data(), op.data(), n);
        else { for (auto& b : outB) std::fill_n(b.data(), n, 0.f); if (e->process) e->process(e, ip.data(), op.data(), n); }
        time.samplePos += n;
        if (outB.empty()) { std::fill_n(outL, n, 0.f); std::fill_n(outR, n, 0.f); return; }
        const float* L = outB[0].data(); const float* R = outB.size() > 1 ? outB[1].data() : L;
        std::copy_n(L, n, outL); std::copy_n(R, n, outR);
    }
    void reset() override { e->dispatcher(e, vst2::effMainsChanged, 0, 0, nullptr, 0); e->dispatcher(e, vst2::effMainsChanged, 0, 1, nullptr, 0); }
    juce::MemoryBlock getState() override {   // "C" + the plug-in's chunk when it has one, else "P" + its parameter values (as vsthost.c)
        juce::MemoryBlock r;
        if (e->flags & vst2::flagProgramChunks) { void* chunk = nullptr; const intptr_t len = e->dispatcher(e, vst2::effGetChunk, 0, 0, &chunk, 0); r.append("C", 1); if (chunk && len > 0) r.append(chunk, (size_t)len); }
        else { r.append("P", 1); for (int i = 0; i < e->numParams; i++) { const float v = e->getParameter(e, i); r.append(&v, 4); } }
        return r;
    }
    bool setState(const juce::MemoryBlock& m) override {
        const auto* d = static_cast<const uint8_t*>(m.getData()); const size_t n = m.getSize(); if (!n) return false;
        if (d[0] == 'C' && (e->flags & vst2::flagProgramChunks)) { e->dispatcher(e, vst2::effSetChunk, 0, (intptr_t)(n - 1), (void*)(d + 1), 0); return true; }
        if (d[0] == 'P') { for (int i = 0; i < e->numParams && 1 + (size_t)(i + 1) * 4 <= n; i++) { float v; std::memcpy(&v, d + 1 + i * 4, 4); e->setParameter(e, i, v); } return true; }
        return false;
    }
    juce::String showGui() override {
        if (!hasGui) return "This plug-in has no window of its own";
        if (win) { win->setVisible(true); win->toFront(true); return {}; }
        view = std::make_unique<juce::Component>(); view->setSize(640, 480);
        win = std::make_unique<PluginWindow>(name); win->onClose = [this] { askClose(); };
        win->setContentNonOwned(view.get(), true); win->setResizable(false, false); win->centreWithSize(win->getWidth(), win->getHeight()); win->setVisible(true);
        e->dispatcher(e, vst2::effEditOpen, 0, 0, view->getWindowHandle(), 0);   // an NSView on macOS, an HWND on Windows
        vst2::ERect* r = nullptr; e->dispatcher(e, vst2::effEditGetRect, 0, 0, &r, 0);
        if (r && r->right > r->left && r->bottom > r->top) view->setSize(r->right - r->left, r->bottom - r->top);
        win->toFront(true); startTimer(30);
        return {};
    }
    void closeGui() override {
        if (!win) return;
        stopTimer(); e->dispatcher(e, vst2::effEditClose, 0, 0, nullptr, 0);
        win->clearContentComponent(); win.reset(); view.reset();
    }
    bool guiOpen() const override { return win != nullptr; }
    void timerCallback() override { if (e && win) e->dispatcher(e, vst2::effEditIdle, 0, 0, nullptr, 0); }
};

// ------------------------------------------------------------------------------------------------ which plug-ins this copy serves
// On an Apple Silicon Mac the app runs this host twice: natively, and as Intel code under Rosetta for Intel-only
// plug-ins (they cannot load into an arm64 process). The native copy serves bundles with an arm64 slice (and every Audio
// Unit: macOS bridges those itself); the Rosetta copy serves only bundles without one. On an Intel Mac and elsewhere,
// one copy serves everything.
#if JUCE_MAC
int machArchs(const juce::File& bin) {   // bit 1: x86_64, bit 2: arm64, from the Mach-O header (fat or thin)
    juce::FileInputStream in(bin); if (!in.openedOk()) return 0;
    const uint32_t magic = (uint32_t)in.readIntBigEndian(); int a = 0;
    auto cpu = [&a](uint32_t c) { if (c == 0x01000007u) a |= 1; else if (c == 0x0100000Cu) a |= 2; };
    if (magic == 0xCAFEBABEu || magic == 0xCAFEBABFu) {   // fat: big-endian table of (cputype, subtype, offset, size, align[, reserved])
        const uint32_t n = (uint32_t)in.readIntBigEndian(); const bool f64 = magic == 0xCAFEBABFu;
        for (uint32_t i = 0; i < n && i < 16; i++) { cpu((uint32_t)in.readIntBigEndian()); in.skipNextBytes(f64 ? 28 : 16); }
    } else if (magic == 0xCFFAEDFEu) cpu((uint32_t)in.readInt());   // thin 64-bit, little-endian (MH_MAGIC_64 read big-endian)
    return a;
}
bool sysctlOn(const char* name) { int v = 0; size_t n = sizeof v; return sysctlbyname(name, &v, &n, nullptr, 0) == 0 && v == 1; }
bool underRosetta() { static const bool r = sysctlOn("sysctl.proc_translated"); return r; }
bool appleSilicon() { static const bool a = sysctlOn("hw.optional.arm64"); return a; }
bool servesHere(const juce::File& bundle) {
    if (!appleSilicon()) return true;
    const int a = machArchs(vst2Binary(bundle));
    if (a == 0) return !underRosetta();   // unreadable: let the native copy try
    return underRosetta() ? ((a & 1) && !(a & 2)) : (a & 2) != 0;
}
#else
bool underRosetta() { return false; }
bool servesHere(const juce::File&) { return true; }
#endif

bool isVst2Name(const juce::File& f) {
#if JUCE_MAC
    return f.hasFileExtension("vst");
#else
    return f.hasFileExtension("dll") || f.hasFileExtension("so");
#endif
}

// ------------------------------------------------------------------------------------------------ scanning
juce::String formatId(const juce::String& juceName) { return juceName == "VST3" ? "vst3" : juceName == "AudioUnit" ? "au" : juceName.toLowerCase(); }

juce::Array<juce::var> scan(const juce::StringArray& extra) {   // message thread
    juce::Array<juce::var> out;
    for (auto* f : formats().getFormats()) {
        const bool au = f->getName() == "AudioUnit"; if (au && underRosetta()) continue;   // the native copy lists every Audio Unit
        juce::FileSearchPath paths = f->getDefaultLocationsToSearch(); for (auto& x : extra) if (juce::File(x).isDirectory()) paths.addIfNotAlreadyThere(juce::File(x));
        for (auto& ident : f->searchPathsForPlugins(paths, true, false)) {
            if (!au && !servesHere(juce::File(ident))) continue;   // the other copy (Apple Silicon / Rosetta) has this one
            juce::OwnedArray<juce::PluginDescription> ds;
            { std::unique_lock<std::shared_mutex> lk(loadLock); f->findAllTypesForFile(ds, ident); }
            for (auto* d : ds) out.add(obj({ { "id", d->createIdentifierString() }, { "name", d->name }, { "vendor", d->manufacturerName }, { "version", d->version },
                                             { "path", d->fileOrIdentifier }, { "format", formatId(d->pluginFormatName) }, { "instrument", d->isInstrument } }));
        }
    }
    juce::Array<juce::File> dirs;   // VST2
#if JUCE_MAC
    dirs.add(juce::File("/Library/Audio/Plug-Ins/VST")); dirs.add(juce::File("~/Library/Audio/Plug-Ins/VST"));
#endif
    for (auto& x : extra) dirs.addIfNotAlreadyThere(juce::File(x));
    std::function<void(const juce::File&, int)> walk = [&](const juce::File& dir, int depth) {
        for (auto& f : dir.findChildFiles(juce::File::findFilesAndDirectories, false)) {
            if (isVst2Name(f)) {
                if (!servesHere(f)) continue;
                juce::String err; std::unique_lock<std::shared_mutex> lk(loadLock); auto p = Vst2Plug::open(f, err); if (!p) continue;
                out.add(obj({ { "id", p->id }, { "name", p->name }, { "vendor", p->vendor }, { "version", "" }, { "path", f.getFullPathName() }, { "format", "vst2" },
                              { "instrument", (p->e->flags & vst2::flagIsSynth) != 0 } }));
            } else if (f.isDirectory() && depth < 4 && !f.hasFileExtension("vst3;component")) walk(f, depth + 1);
        }
    };
    for (auto& d : dirs) if (d.isDirectory()) walk(d, 0);
    return out;
}

// ------------------------------------------------------------------------------------------------ connections
struct Conn : juce::Thread, std::enable_shared_from_this<Conn> {
    std::unique_ptr<juce::StreamingSocket> sock; std::mutex wm;
    std::mutex procM; std::unique_ptr<Plug> plug;   // swapped on the message thread under procM; used by this thread under it
    juce::var loadReq; std::atomic<bool> dead{ false }; bool serial = false; static inline std::mutex serialM;
    struct QMidi { uint32_t at; bool timed; uint8_t d[3]; }; struct QParam { uint32_t at; bool timed; int id; float v; };
    std::mutex qm; std::vector<QMidi> midi; std::vector<QParam> pq;
    std::mutex om; std::vector<std::pair<int, float>> outEv; std::atomic<bool> wantDirty{ false }, wantRescan{ false }, wantClosed{ false };
    std::vector<float> inL, inR, outL, outR, replyBuf; std::vector<MidiEv> cur, piece; std::vector<QParam> pcur;   // the audio thread's, allocated once

    explicit Conn(juce::StreamingSocket* s) : juce::Thread("undae plug-in connection"), sock(s) {
        inL.resize(kMaxFrames); inR.resize(kMaxFrames); outL.resize(kMaxFrames); outR.resize(kMaxFrames); replyBuf.resize(3 + 2 * kMaxFrames);
        cur.reserve(1024); piece.reserve(1024); pcur.reserve(4096);
        const auto se = juce::SystemStats::getEnvironmentVariable("UNDAE_SERIAL", ""); serial = se == "1";
    }
    ~Conn() override { stopThread(2000); }

    bool readN(void* d, int n) { auto* p = static_cast<char*>(d); while (n > 0) { const int k = sock->read(p, n, true); if (k <= 0) return false; p += k; n -= k; } return true; }
    void send(int op, const void* d, size_t n) {
        uint8_t h[10]; size_t hn = 2; h[0] = (uint8_t)(0x80 | op);
        if (n < 126) h[1] = (uint8_t)n; else if (n < 65536) { h[1] = 126; h[2] = (uint8_t)(n >> 8); h[3] = (uint8_t)n; hn = 4; }
        else { h[1] = 127; for (int i = 0; i < 8; i++) h[2 + i] = (uint8_t)((uint64_t)n >> (8 * (7 - i))); hn = 10; }
        std::vector<uint8_t> f(hn + n); std::memcpy(f.data(), h, hn); if (n) std::memcpy(f.data() + hn, d, n);
        std::lock_guard<std::mutex> l(wm); sock->write(f.data(), (int)f.size());
    }
    void sendJson(const juce::var& v) { const auto s = juce::JSON::toString(v, true).toStdString(); send(1, s.data(), s.size()); }

    bool handshake() {
        std::string req; char c;
        while (req.size() < 8192 && req.find("\r\n\r\n") == std::string::npos) { if (sock->read(&c, 1, true) != 1) return false; req += c; }
        const juce::String r(req); const auto first = r.upToFirstOccurrenceOf("\r\n", false, false);
        bool ok = first.startsWith("GET ") && (first.contains("?t=" + token + " ") || first.contains("?t=" + token + "&") || first.contains("&t=" + token + " ") || first.contains("&t=" + token + "&"));
        juce::String key;
        for (auto& line : juce::StringArray::fromLines(r)) {
            const auto name = line.upToFirstOccurrenceOf(":", false, false).trim().toLowerCase(), val = line.fromFirstOccurrenceOf(":", false, false).trim();
            if (name == "origin" && !val.startsWith("http://127.0.0.1") && !val.startsWith("http://localhost")) ok = false;
            if (name == "sec-websocket-key") key = val;
        }
        if (!ok || key.isEmpty() || key.length() > 100) { const char* no = "HTTP/1.1 403 Forbidden\r\n\r\n"; sock->write(no, (int)std::strlen(no)); return false; }
        const auto cat = (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").toStdString(); uint8_t dig[20]; sha1((const uint8_t*)cat.data(), cat.size(), dig);
        const auto resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + juce::Base64::toBase64(dig, 20) + "\r\n\r\n";
        return sock->write(resp.toRawUTF8(), (int)resp.getNumBytesAsUTF8()) > 0;
    }

    void run() override {
        if (!handshake()) { dead = true; return; }
        std::vector<uint8_t> msg; int msgOp = 0;
        while (!threadShouldExit()) {
            uint8_t h[2], ext[8], mask[4]; if (!readN(h, 2)) break;
            const bool fin = h[0] & 0x80; const int op = h[0] & 15; uint64_t n = h[1] & 127;
            if (n == 126) { if (!readN(ext, 2)) break; n = (uint64_t)ext[0] << 8 | ext[1]; }
            else if (n == 127) { if (!readN(ext, 8)) break; n = 0; for (int i = 0; i < 8; i++) n = n << 8 | ext[i]; }
            if (!(h[1] & 0x80) || n > kMaxMsg || msg.size() + n > kMaxMsg) break;   // client frames are masked
            if (!readN(mask, 4)) break;
            const size_t at = msg.size(); msg.resize(at + n); if (n && !readN(msg.data() + at, (int)n)) break;
            for (uint64_t i = 0; i < n; i++) msg[at + i] ^= mask[i & 3];
            if (op == 8) break;
            if (op == 9) { send(10, msg.data() + at, n); msg.resize(at); continue; }
            if (op == 10) { msg.resize(at); continue; }
            if (op) msgOp = op;
            if (!fin) continue;
            if (msgOp == 2) { if (serial) { std::lock_guard<std::mutex> l(serialM); processBlock(msg.data(), msg.size()); } else processBlock(msg.data(), msg.size()); }
            else if (msgOp == 1) control(juce::String::fromUTF8((const char*)msg.data(), (int)msg.size()));
            msg.clear();
        }
        dead = true;
    }

    // --- requests that need no reply are queued for the audio; the rest go to the message thread, in order
    static uint32_t frameOf(const juce::var& v) { return (uint32_t)(juce::int64)v; }
    void control(const juce::String& text) {
        const juce::var req = juce::JSON::parse(text); const auto op = req.getProperty("op", "").toString();
        if (op == "note" || op == "midi") {
            uint8_t d[3] = { 0, 0, 0 };
            if (op == "note") { const bool on = (bool)req.getProperty("on", false); const int v = juce::jlimit(1, 127, (int)((double)req.getProperty("vel", 0.8) * 127));
                d[0] = (uint8_t)(on ? 0x90 : 0x80); d[1] = (uint8_t)((int)req.getProperty("key", 60) & 127); d[2] = (uint8_t)(on ? v : 0); }
            else if (auto* a = req.getProperty("data", juce::var()).getArray()) for (int i = 0; i < std::min(3, a->size()); i++) d[i] = (uint8_t)(int)(*a)[i];
            const bool timed = req.hasProperty("at"); std::lock_guard<std::mutex> l(qm);
            if (midi.size() < 1024) midi.push_back({ timed ? frameOf(req.getProperty("at", 0)) : 0u, timed, { d[0], d[1], d[2] } });
            return;
        }
        if (op == "param") {
            const bool timed = req.hasProperty("at"); std::lock_guard<std::mutex> l(qm);
            if (pq.size() < 4096) pq.push_back({ timed ? frameOf(req.getProperty("at", 0)) : 0u, timed, (int)req.getProperty("id", -1), (float)(double)req.getProperty("value", 0.0) });
            return;
        }
        auto self = shared_from_this();
        juce::MessageManager::callAsync([self, req] { if (!self->dead) self->job(req); });
    }

    // --- audio
    void processBlock(const uint8_t* m, size_t len) {
        if (len < 12) return;
        uint32_t head[3]; std::memcpy(head, m, 12); const uint32_t n = head[2], pos = head[1];
        if (head[0] != 1 || !n || n > (uint32_t)kMaxFrames || len < 12 + (size_t)n * 8) return;
        const float* in = reinterpret_cast<const float*>(m + 12); std::memcpy(replyBuf.data(), head, 12); float* o = replyBuf.data() + 3;
        std::unique_lock<std::mutex> pl(procM, std::try_to_lock);
        if (!pl.owns_lock() || !plug) { std::memcpy(o, in, (size_t)n * 8); send(2, replyBuf.data(), 12 + (size_t)n * 8); return; }   // loading: pass through
        cur.clear(); pcur.clear();
        {   // notes and parameter changes that fall inside this block, in time order
            std::lock_guard<std::mutex> l(qm); size_t keep = 0;
            for (auto& q : midi) { const int32_t off = q.timed ? (int32_t)(q.at - pos) : 0; if (off >= (int32_t)n) { midi[keep++] = q; continue; } cur.push_back({ std::max(0, off), { q.d[0], q.d[1], q.d[2] } }); }
            midi.resize(keep); keep = 0;
            for (auto& q : pq) { const int32_t off = q.timed ? (int32_t)(q.at - pos) : 0; if (off >= (int32_t)n) { pq[keep++] = q; continue; } auto c = q; c.at = (uint32_t)std::max(0, off); pcur.push_back(c); }
            pq.resize(keep);
        }
        std::stable_sort(cur.begin(), cur.end(), [](auto& a, auto& b) { return a.offset < b.offset; });
        std::stable_sort(pcur.begin(), pcur.end(), [](auto& a, auto& b) { return a.at < b.at; });
        for (uint32_t i = 0; i < n; i++) { inL[i] = in[i * 2]; inR[i] = in[i * 2 + 1]; }
        std::shared_lock<std::shared_mutex> ll(loadLock);
        size_t mi = 0, pi = 0;
        for (uint32_t off = 0; off < n;) {   // in pieces that end where a parameter changes
            while (pi < pcur.size() && pcur[pi].at <= off) { plug->setParam(pcur[pi].id, pcur[pi].v); pi++; }
            uint32_t end = std::min(n, off + (uint32_t)kMaxBlock); if (pi < pcur.size() && pcur[pi].at < end) end = pcur[pi].at;
            piece.clear(); while (mi < cur.size() && (uint32_t)cur[mi].offset < end) { auto e = cur[mi++]; e.offset -= (int)off; piece.push_back(e); }
            plug->process(inL.data() + off, inR.data() + off, outL.data() + off, outR.data() + off, (int)(end - off), piece.data(), (int)piece.size());
            off = end;
        }
        for (uint32_t i = 0; i < n; i++) { const float l = outL[i], r = outR[i]; o[i * 2] = l > -64 && l < 64 ? l : 0; o[i * 2 + 1] = r > -64 && r < 64 ? r : 0; }   // drops NaN and runaway values
        send(2, replyBuf.data(), 12 + (size_t)n * 8);
    }

    // --- message thread
    juce::var paramsOf(bool full) const { juce::Array<juce::var> a; if (plug) for (int i = 0; i < plug->numParams(); i++) a.add(plug->param(i, full)); return a; }
    void unload() {
        std::unique_ptr<Plug> old; { std::lock_guard<std::mutex> l(procM); old = std::move(plug); }
        if (old) { std::unique_lock<std::shared_mutex> lk(loadLock); old.reset(); }
    }
    juce::String load(const juce::var& req, juce::var& r) {
        const juce::String path = req.getProperty("path", "").toString(), want = req.getProperty("id", "").toString(); const double sr = (double)req.getProperty("sampleRate", 48000.0);
        if (path.isEmpty()) return "No plug-in file given";
        unload();
        std::unique_ptr<Plug> p;
        if (isVst2Name(juce::File(path)) && !path.startsWith("AudioUnit:")) {
            juce::String err; std::unique_lock<std::shared_mutex> lk(loadLock); auto v = Vst2Plug::open(juce::File(path), err); if (!v) return err;
            v->start(sr); p = std::move(v);
        } else {
            juce::AudioPluginFormat* fmt = nullptr;
            for (auto* f : formats().getFormats()) if ((path.startsWith("AudioUnit:") && f->getName() == "AudioUnit") || (path.endsWithIgnoreCase(".vst3") && f->getName() == "VST3")) fmt = f;
            if (!fmt) return "This kind of plug-in cannot be loaded here";
            juce::String err; std::unique_ptr<juce::AudioPluginInstance> inst;
            {
                std::unique_lock<std::shared_mutex> lk(loadLock);
                juce::OwnedArray<juce::PluginDescription> ds; fmt->findAllTypesForFile(ds, path);
                const juce::PluginDescription* d = nullptr; for (auto* x : ds) if (want.isEmpty() || x->createIdentifierString() == want) { d = x; break; }
                if (!d && !ds.isEmpty()) d = ds[0];
                if (!d) return "The plug-in was not found (moved or uninstalled?)";
                inst = formats().createPluginInstance(*d, sr, kMaxBlock, err);
                if (!inst) return err.isNotEmpty() ? err : juce::String("The plug-in could not be loaded");
                p = std::make_unique<JucePlug>(std::move(inst), sr);
            }
        }
        auto self = weak_from_this();
        p->onParam = [self](int i, float v) { if (auto s = self.lock()) { std::lock_guard<std::mutex> l(s->om); if (s->outEv.size() < 256) s->outEv.push_back({ i, v }); s->wantDirty = true; } };
        p->onDirty = [self] { if (auto s = self.lock()) s->wantDirty = true; };
        p->onRescan = [self] { if (auto s = self.lock()) s->wantRescan = true; };
        p->onGuiClosed = [self] { if (auto s = self.lock()) s->wantClosed = true; };
        r = reply(req, true);
        set(r, "id", p->id); set(r, "name", p->name); set(r, "vendor", p->vendor); set(r, "latency", p->latency); set(r, "inputs", p->inputs); set(r, "outputs", p->outputs);
        set(r, "notes", p->notes); set(r, "state", true); set(r, "gui", p->hasGui);
        { std::lock_guard<std::mutex> l(qm); midi.clear(); pq.clear(); }
        { std::lock_guard<std::mutex> l(procM); plug = std::move(p); }
        set(r, "params", paramsOf(true)); loadReq = req;
        return {};
    }
    void job(const juce::var& req) {
        const auto op = req.getProperty("op", "").toString();
        if (op == "hello") {
            auto r = reply(req, true); set(r, "host", "undae-pluginhost"); set(r, "version", 1); set(r, "gui", true);
            juce::Array<juce::var> f; f.add("vst3");
#if JUCE_MAC
            f.add("au");
#endif
            f.add("vst2"); set(r, "formats", f); sendJson(r);
        } else if (op == "scan") {
            juce::StringArray extra; if (auto* a = req.getProperty("paths", juce::var()).getArray()) for (auto& x : *a) extra.add(x.toString());
            auto r = reply(req, true); set(r, "plugins", scan(extra)); sendJson(r);
        } else if (op == "load") { juce::var r; const auto err = load(req, r); sendJson(err.isEmpty() ? r : errorReply(req, err)); }
        else if (op == "unload") { unload(); sendJson(reply(req, true)); }
        else if (op == "params") { if (!plug) { sendJson(errorReply(req, "No plug-in loaded")); return; } auto r = reply(req, true); set(r, "params", paramsOf(false)); sendJson(r); }
        else if (op == "state") {
            if (!plug) { sendJson(errorReply(req, "No plug-in loaded")); return; }
            if (req.hasProperty("data")) {
                juce::MemoryOutputStream raw; juce::Base64::convertFromBase64(raw, req.getProperty("data", "").toString());
                bool ok; { std::lock_guard<std::mutex> l(procM); ok = plug->setState(raw.getMemoryBlock()); }
                if (!ok) { sendJson(errorReply(req, "The saved state does not fit this plug-in")); return; }
                auto r = reply(req, true); set(r, "params", paramsOf(false)); sendJson(r);
            } else { const auto m = plug->getState(); auto r = reply(req, true); set(r, "data", juce::Base64::toBase64(m.getData(), m.getSize())); sendJson(r); }
        } else if (op == "gui") {
            if (!plug) { sendJson(errorReply(req, "No plug-in loaded")); return; }
            juce::String err; if ((bool)req.getProperty("show", false)) err = plug->showGui(); else { plug->closeGui(); wantDirty = true; }
            if (err.isNotEmpty()) { sendJson(errorReply(req, err)); return; }
            auto r = reply(req, true); set(r, "open", plug->guiOpen()); sendJson(r);
        } else if (op == "panic") {   // transport stopped: drop what is queued, reset the plug-in, and send all-notes-off on every channel
            { std::lock_guard<std::mutex> l(qm); midi.clear(); pq.clear();
              for (uint8_t ch = 0; ch < 16; ch++) { midi.push_back({ 0, false, { (uint8_t)(0xB0 | ch), 123, 0 } }); midi.push_back({ 0, false, { (uint8_t)(0xB0 | ch), 120, 0 } }); } }
            std::lock_guard<std::mutex> l(procM); if (plug) plug->reset();
        } else sendJson(errorReply(req, "Unknown request"));
    }
    void flushEvents() {   // message thread, every 40 ms
        if (!plug) return;
        if (wantRescan.exchange(false)) { auto e = obj({ { "ev", "params" } }); set(e, "params", paramsOf(true)); sendJson(e); }
        std::vector<std::pair<int, float>> ev; { std::lock_guard<std::mutex> l(om); ev.swap(outEv); }
        if (!ev.empty()) { juce::Array<juce::var> ch; for (auto& [i, v] : ev) ch.add(obj({ { "id", i }, { "value", v }, { "text", plug->text(i, v) } })); auto e = obj({ { "ev", "param" } }); set(e, "changes", ch); sendJson(e); }
        if (wantClosed.exchange(false)) sendJson(obj({ { "ev", "gui" }, { "open", false } }));
        if (wantDirty.exchange(false)) sendJson(obj({ { "ev", "dirty" } }));
    }
};

// ------------------------------------------------------------------------------------------------ server
struct Server : juce::Thread, juce::Timer {
    juce::StreamingSocket listener; std::mutex nm; std::vector<std::shared_ptr<Conn>> fresh, conns;
    Server() : juce::Thread("undae plug-in listener") {}
    ~Server() override { listener.close(); stopThread(2000); stopTimer(); for (auto& c : conns) { c->sock->close(); c->stopThread(2000); c->unload(); } }
    bool start() { if (!listener.createListener(0, "127.0.0.1")) return false; startThread(); startTimer(40); return true; }
    void run() override {
        while (!threadShouldExit()) {
            std::unique_ptr<juce::StreamingSocket> s(listener.waitForNextConnection()); if (!s) { if (!listener.isConnected()) break; continue; }
            auto c = std::make_shared<Conn>(s.release()); c->startThread(juce::Thread::Priority::highest);
            std::lock_guard<std::mutex> l(nm); fresh.push_back(c);
        }
    }
    void timerCallback() override {
        { std::lock_guard<std::mutex> l(nm); for (auto& c : fresh) conns.push_back(c); fresh.clear(); }
        for (auto it = conns.begin(); it != conns.end();) {
            auto& c = *it;
            if (c->dead) { c->sock->close(); c->stopThread(2000); c->unload(); it = conns.erase(it); continue; }
            c->flushEvents(); ++it;
        }
    }
};

}   // namespace

// ------------------------------------------------------------------------------------------------ the app
class HostApp : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "undae-pluginhost"; }
    const juce::String getApplicationVersion() override { return "1"; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void initialise(const juce::String& cmd) override {
        token = juce::SystemStats::getEnvironmentVariable("UNDAE_HOST_TOKEN", "");
        if (token.isEmpty()) { juce::Random r; for (int i = 0; i < 4; i++) token << juce::String::toHexString(r.nextInt()).paddedLeft('0', 8); }
        server = std::make_unique<Server>();
        if (!server->start()) { std::printf("{\"error\":\"listen failed\"}\n"); std::fflush(stdout); setApplicationReturnValue(1); quit(); return; }
        std::printf("{\"port\":%d,\"token\":\"%s\"}\n", server->listener.getBoundPort(), token.toRawUTF8()); std::fflush(stdout);
        if (cmd.contains("--child")) watcher = std::thread([] {   // the app holds our stdin open; when it goes, so do we
            char b[64]; while (std::fread(b, 1, sizeof b, stdin) > 0) {}
            juce::MessageManager::callAsync([] { if (auto* a = juce::JUCEApplicationBase::getInstance()) a->systemRequestedQuit(); });
        });
    }
    void shutdown() override { server.reset(); if (watcher.joinable()) watcher.detach(); }
    void systemRequestedQuit() override { quit(); }
private:
    std::unique_ptr<Server> server; std::thread watcher;
};

START_JUCE_APPLICATION(HostApp)
