#include "Doc.h"

namespace ui {
static juce::String u8s(const char* s) { return juce::String::fromUTF8(s); }

const std::vector<FxSpec>& fxSpecs() {
    static const std::vector<FxSpec> v = {
        { "eq", "EQ", { { "low", "Low", -18, 18, 0, false, {} }, { "mid", "Mid", -18, 18, 0, false, {} }, { "midf", "Mid Hz", 200, 8000, 1000, true, {} }, { "high", "High", -18, 18, 0, false, {} } } },
        { "filter", "Filter", { { "type", "Type", 0, 0, 0, false, { "lowpass", "highpass", "bandpass", "notch" } }, { "cut", "Cutoff", 30, 18000, 2000, true, {} }, { "q", "Reso", 0.1, 20, 1, true, {} } } },
        { "comp", "Compressor", { { "thr", "Thresh", -60, 0, -18, false, {} }, { "ratio", "Ratio", 1, 20, 4, false, {} }, { "att", "Attack", 0.001, 0.3, 0.01, true, {} }, { "rel", "Release", 0.02, 1, 0.15, true, {} }, { "knee", "Knee", 0, 40, 12, false, {} }, { "gain", "Makeup", 0, 24, 0, false, {} } } },
        { "limiter", "Limiter", { { "in", "Input", -12, 24, 0, false, {} }, { "ceil", "Ceiling", -24, 0, -1, false, {} }, { "rel", "Release", 0.01, 0.6, 0.08, true, {} } } },
        { "reverb", "Reverb", { { "size", "Size", 0.2, 8, 2.2, true, {} }, { "damp", "Damp", 0, 1, 0.5, false, {} }, { "pre", "Pre-delay", 0, 200, 10, false, {} }, { "mix", "Mix", 0, 1, 0.3, false, {} } } },
        { "delay", "Delay", { { "time", "Time", 20, 1500, 375, true, {} }, { "fb", "Feedback", 0, 0.95, 0.4, false, {} }, { "tone", "Tone", 300, 16000, 5000, true, {} }, { "mix", "Mix", 0, 1, 0.3, false, {} } } },
        { "chorus", "Chorus", { { "rate", "Rate", 0.05, 8, 0.8, true, {} }, { "depth", "Depth", 0, 1, 0.5, false, {} }, { "mix", "Mix", 0, 1, 0.5, false, {} } } },
        { "phaser", "Phaser", { { "rate", "Rate", 0.05, 8, 0.4, true, {} }, { "depth", "Depth", 0, 1, 0.7, false, {} }, { "fb", "Feedback", 0, 0.9, 0.4, false, {} }, { "mix", "Mix", 0, 1, 0.5, false, {} } } },
        { "dist", "Distortion", { { "drive", "Drive", 0, 1, 0.4, false, {} }, { "tone", "Tone", 400, 18000, 8000, true, {} }, { "level", "Level", 0, 1.5, 0.7, false, {} }, { "mix", "Mix", 0, 1, 1, false, {} } } },
        { "crush", "Bitcrush", { { "bits", "Bits", 2, 16, 8, false, {} }, { "tone", "Tone", 400, 18000, 12000, true, {} }, { "mix", "Mix", 0, 1, 1, false, {} } } },
        { "duck", "Peak controller", { { "src", "Triggered by", 0, 0, 0, false, { "@buses" } }, { "mode", "Duck", 0, 0, 0, false, { "full", "band" } }, { "depth", "Depth", 0, 36, 12, false, {} }, { "sens", "Sensitivity", 0.5, 12, 3, false, {} },
            { "decay", "Decay", 0.02, 1, 0.15, true, {} }, { "freq", "Band Hz", 30, 12000, 120, true, {} }, { "q", "Band width", 0.3, 8, 1, true, {} } } },
        { "peq", "Parametric EQ", { { "hps", "Low cut", 0, 0, 0, false, { "off", "12 dB/oct", "24 dB/oct" } }, { "hpf", "Cut Hz", 20, 2000, 80, true, {} },
            { "lsf", "Low Hz", 30, 1000, 120, true, {} }, { "lsg", "Low", -18, 18, 0, false, {} },
            { "p1f", "Band 1 Hz", 40, 1200, 250, true, {} }, { "p1g", "Band 1", -18, 18, 0, false, {} }, { "p1q", "Width 1", 0.2, 10, 1, true, {} },
            { "p2f", "Band 2 Hz", 200, 6000, 1200, true, {} }, { "p2g", "Band 2", -18, 18, 0, false, {} }, { "p2q", "Width 2", 0.2, 10, 1, true, {} },
            { "p3f", "Band 3 Hz", 1000, 18000, 5000, true, {} }, { "p3g", "Band 3", -18, 18, 0, false, {} }, { "p3q", "Width 3", 0.2, 10, 1, true, {} },
            { "hsf", "High Hz", 1000, 18000, 8000, true, {} }, { "hsg", "High", -18, 18, 0, false, {} },
            { "lps", "High cut", 0, 0, 0, false, { "off", "12 dB/oct", "24 dB/oct" } }, { "lpf", "Cut Hz", 1000, 20000, 18000, true, {} }, { "out", "Output", -18, 18, 0, false, {} } } },
        { "deess", "De-esser", { { "mode", "Turn down", 0, 0, 0, false, { "split", "wide" } }, { "listen", "Listen", 0, 0, 0, false, { "off", "on" } },
            { "freq", "From Hz", 2000, 12000, 6000, true, {} }, { "thr", "Thresh", -60, 0, -30, false, {} }, { "range", "Range", 0, 24, 10, false, {} } } },
        { "sat", "Saturator", { { "mode", "Character", 0, 0, 0, false, { "tube", "tape", "transformer", "clip", "exciter" } }, { "drive", "Drive", 0, 30, 6, false, {} },
            { "bias", "Bias", 0, 1, 0.2, false, {} }, { "band", "Excite Hz", 1000, 12000, 3000, true, {} }, { "tone", "Tone", 1000, 20000, 14000, true, {} },
            { "mix", "Mix", 0, 1, 1, false, {} }, { "out", "Output", -18, 6, 0, false, {} } } },
        { "fdn", "Vault reverb", { { "mode", "Space", 0, 0, 0, false, { "hall", "room", "plate", "gated" } }, { "decay", "Decay", 0.2, 20, 2.5, true, {} },
            { "size", "Size", 0.3, 2, 1, false, {} }, { "damp", "Damp Hz", 1000, 20000, 6000, true, {} }, { "pre", "Pre-delay", 0, 200, 15, false, {} },
            { "mod", "Motion", 0, 1, 0.3, false, {} }, { "width", "Width", 0, 1, 1, false, {} }, { "mix", "Mix", 0, 1, 0.25, false, {} } } },
        { "gate", "Gate", { { "src", "Key from", 0, 0, 0, false, { "@buses" } }, { "thr", "Open at", -80, 0, -40, false, {} }, { "hyst", "Close below", 0, 20, 4, false, {} },
            { "att", "Attack", 0.0001, 0.05, 0.001, true, {} }, { "hold", "Hold", 0, 0.5, 0.05, false, {} }, { "rel", "Release", 0.01, 2, 0.15, true, {} },
            { "range", "Range", 0, 80, 80, false, {} }, { "hpf", "Detect from Hz", 20, 2000, 20, true, {} } } },
        { "trans", "Transients", { { "attack", "Attack", -100, 100, 0, false, {} }, { "sustain", "Sustain", -100, 100, 0, false, {} }, { "out", "Output", -18, 18, 0, false, {} }, { "mix", "Mix", 0, 1, 1, false, {} } } },
        { "imager", "Stereo width", { { "width", "Width %", 0, 200, 100, false, {} }, { "bass", "Bass", 0, 0, 0, false, { "off", "mono" } }, { "mono", "Mono below Hz", 40, 500, 120, true, {} },
            { "haas", "Widen ms", 0, 30, 0, false, {} }, { "bal", "Balance", -1, 1, 0, false, {} } } },
    };
    return v;
}
const FxSpec* fxSpec(const std::string& t) { for (auto& s : fxSpecs()) if (t == s.type) return &s; return nullptr; }

us::FxSlot newFx(const std::string& type) {
    us::FxSlot f; f.id = us::newId(); f.type = type; f.on = true;
    if (auto* s = fxSpec(type)) for (auto& q : s->params) {
        if (q.opts.empty()) f.p.set(q.k, q.def);
        else f.p.set(q.k, juce::String(q.opts[0] == "@buses" ? "" : q.opts[0]));
    }
    return f;
}

us::SynthP defSynth() {   // state.js defSynth()
    us::SynthP s;
    s.osc[0] = { true, "sawtooth", "square", 0, 0, 0, 0.8, 0, 0 };
    s.osc[1] = { false, "square", "sine", 0, 0, 7, 0.5, 0, 0 };
    s.osc[2] = { false, "sine", "triangle", -1, 0, 0, 0.5, 0, 0 };
    s.lfo[0] = { "sine", 5, 0, 0 }; s.lfo[1] = { "triangle", 0.4, 0, 0 }; s.lfo[2] = { "sh", 8, 0, 0 };
    s.mod.assign(8, us::ModP{});
    return s;
}

us::Channel newChannel(const us::Project& p, const std::string& type, const std::string& name, const std::string& sample) {
    us::Channel c; c.id = us::newId(); c.type = type; c.name = name; c.sample = sample;
    static const char* cols[] = { "#ff7a59", "#ffb84d", "#e6d15a", "#7bd88f", "#45d0c4", "#5aa9ff", "#9d8cff", "#f277c6" };
    c.color = cols[p.channels.size() % 8];
    c.bus = p.buses.size() > 1 ? p.buses[1].id : "master";
    if (type == "synth") c.synth = defSynth();
    if (sample == "inst:keys") { c.gate = true; c.rel = 0.3; }
    return c;
}

us::Project blankProject() {
    us::Project p; p.name = "Untitled";
    us::Bus m; m.id = "master"; m.name = "Master"; m.kind = "master"; m.vol = 0.9; m.fx.push_back(newFx("limiter")); p.buses.push_back(m);
    us::Pattern pt; pt.id = us::newId(); pt.name = "Pattern 1"; pt.bars = 1; p.patterns.push_back(pt);
    return p;
}

// --- document ------------------------------------------------------------------------------------------------------
void Doc::load(const juce::File& f) { us::Project np = us::loadProject(f); bank.addFromProject(np); setProject(std::move(np), f); }

void Doc::setProject(us::Project np, const juce::File& f) {
    p = std::move(np); file = f; dirty = false;
    ch = p.channels.empty() ? "" : p.channels[0].id; pat = p.patterns.empty() ? "" : p.patterns[0].id;
    hist.clear(); hi = -1; auto s = std::make_shared<us::Project>(p); push(s);
    eng.setProject(s); eng.setPattern(pat);
    sendChangeMessage();
}

void Doc::push(std::shared_ptr<us::Project> s) {
    if (hi + 1 < (int)hist.size()) hist.erase(hist.begin() + hi + 1, hist.end());
    hist.push_back(std::move(s)); if (hist.size() > 200) hist.erase(hist.begin()); hi = (int)hist.size() - 1;
}

void Doc::commit() {
    dirty = true; uncommitted = false; auto s = std::make_shared<us::Project>(p); push(s); eng.update(s);
    sendChangeMessage();
}
void Doc::live() {
    uncommitted = true; auto s = std::make_shared<us::Project>(p); recent.push_back(s); if (recent.size() > 16) recent.pop_front();
    eng.update(s);
}
void Doc::undo() {
    if (uncommitted && hi >= 0) { uncommitted = false; p = *hist[(size_t)hi]; eng.update(hist[(size_t)hi]); sendChangeMessage(); return; }   // an unfinished edit: back to the last step
    if (!canUndo()) return; hi--; p = *hist[(size_t)hi]; eng.update(hist[(size_t)hi]); dirty = true; if (!chan()) ch = p.channels.empty() ? "" : p.channels[0].id; if (!patt()) pat = p.patterns.empty() ? "" : p.patterns[0].id; sendChangeMessage(); }
void Doc::redo() { if (!canRedo()) return; hi++; p = *hist[(size_t)hi]; eng.update(hist[(size_t)hi]); dirty = true; sendChangeMessage(); }
void Doc::save(const juce::File& f) { us::saveProject(p, f); file = f; dirty = false; sendChangeMessage(); }

std::string Doc::addSampleData(const std::string& name, const juce::MemoryBlock& bytes) {
    const std::string id = "imp:" + us::newId();
    if (!bank.add(id, name, bytes)) return {};
    p.embedded->data[id] = bytes; p.embedded->names[id] = name;
    return id;
}
std::string Doc::importAudio(const juce::File& f) {
    juce::MemoryBlock mb; if (!f.loadFileAsData(mb)) return {};
    return addSampleData(f.getFileNameWithoutExtension().toStdString(), mb);
}
void Doc::addSampleChannel(const std::string& id) {
    auto s = bank.get(id); if (!s) return;
    auto c = newChannel(p, "sample", s->name.substr(0, 18), id);
    c.gate = s->id.rfind("inst:", 0) == 0 || s->seconds() > 1.5;   // long sounds play while the note is held
    p.channels.push_back(c); ch = c.id; commit();
}
void Doc::placeAudio(const std::string& id, double startBar, int track) {
    auto s = bank.get(id); if (!s) return;
    const double barSec = p.bar() * p.secondsPerTick();
    if (track < 0) { track = 0; for (int t = 0; t < p.tracks; t++) { bool used = false; for (auto& c : p.clips) used |= c.track == t; for (auto& a : p.audio) used |= a.track == t; if (!used) { track = t; break; } if (t == p.tracks - 1) { track = p.tracks; p.tracks++; } } }
    us::AudioClip a; a.id = us::newId(); a.sample = id; a.name = s->name.substr(0, 24); a.track = track; a.start = startBar >= 0 ? startBar : 0; a.len = s->seconds() / barSec; a.bpm = p.bpm; a.stretch = false;
    p.audio.push_back(a); commit();
}

std::string Doc::addBus(const std::string& name, const std::string& kind) {
    us::Bus b; b.id = us::newId(); b.name = name; b.kind = kind;
    auto at = kind == "send" ? p.buses.end() : std::find_if(p.buses.begin(), p.buses.end(), [](auto& x) { return x.kind == "send"; });
    p.buses.insert(at, b); return b.id;
}
bool Doc::reaches(const std::string& from, const std::string& to) const {
    std::set<std::string> seen; std::function<bool(const std::string&)> go = [&](const std::string& f) {
        if (f == to) return true; if (!seen.insert(f).second) return false;
        if (auto* b = p.findBus(f)) for (auto& s : b->sends) if (go(s.to)) return true; return false; };
    return go(from);
}
void Doc::deleteChannels(const std::set<std::string>& ids) {
    p.channels.erase(std::remove_if(p.channels.begin(), p.channels.end(), [&](auto& c) { return ids.count(c.id) > 0; }), p.channels.end());
    for (auto& pt : p.patterns) for (auto& id : ids) pt.notes.erase(id);
    p.autos.erase(std::remove_if(p.autos.begin(), p.autos.end(), [&](auto& l) { for (auto& id : ids) if (l.target.rfind("ch:" + id + ":", 0) == 0 || l.target.rfind("plug:" + id + ":", 0) == 0) return true; return false; }), p.autos.end());
    p.links.erase(std::remove_if(p.links.begin(), p.links.end(), [&](auto& l) { for (auto& id : ids) if (l.target.rfind("ch:" + id + ":", 0) == 0 || l.target.rfind("plug:" + id + ":", 0) == 0) return true; return false; }), p.links.end());
    chSel.clear(); if (ids.count(ch) || !chan()) ch = p.channels.empty() ? "" : p.channels[0].id;
}
std::optional<Doc::Target> Doc::target(const std::string& t) {
    juce::StringArray parts; parts.addTokens(juce::String(t), ":", ""); if (parts.size() < 2) return std::nullopt;
    const std::string kind = parts[0].toStdString(), id = parts[1].toStdString(), key = parts.size() > 2 ? parts[2].toStdString() : "";
    Target r;
    if (kind == "master") { r.label = "Master pitch"; r.min = -12; r.max = 12; r.get = [this] { return p.masterPitch; }; r.set = [this](double v) { p.masterPitch = v; }; return r; }
    if (kind == "bus") { auto* b = bus(id); if (!b) return std::nullopt; r.label = juce::String(b->name) + (key == "pan" ? " pan" : " level"); r.min = key == "pan" ? -1 : 0; r.max = key == "pan" ? 1 : 1.25;
        r.get = [this, id, key] { auto* x = bus(id); return x ? (key == "pan" ? x->pan : x->vol) : 0.0; }; r.set = [this, id, key](double v) { if (auto* x = bus(id)) (key == "pan" ? x->pan : x->vol) = v; }; return r; }
    if (kind == "ch") { us::Channel* c = nullptr; for (auto& x : p.channels) if (x.id == id) c = &x; if (!c) return std::nullopt; r.label = juce::String(c->name) + (key == "pan" ? " pan" : " level"); r.min = key == "pan" ? -1 : 0; r.max = key == "pan" ? 1 : 1.25;
        r.get = [this, id, key] { for (auto& x : p.channels) if (x.id == id) return key == "pan" ? x.pan : x.vol; return 0.0; }; r.set = [this, id, key](double v) { for (auto& x : p.channels) if (x.id == id) (key == "pan" ? x.pan : x.vol) = v; }; return r; }
    if (kind == "fx" && parts.size() > 3) {
        auto* b = bus(id); if (!b) return std::nullopt; us::FxSlot* f = nullptr; for (auto& x : b->fx) if (x.id == key) f = &x; if (!f) return std::nullopt;
        const std::string prm = parts[3].toStdString(); const FxSpec* sp = fxSpec(f->type);
        if (sp) for (auto& q : sp->params) if (prm == q.k) { r.label = juce::String(b->name) + u8s(" · ") + sp->name + " " + q.label; r.min = q.lo; r.max = q.hi; }
        if (r.label.isEmpty()) { r.label = juce::String(b->name) + " " + prm; r.min = 0; r.max = 1; }
        r.get = [this, id, key, prm] { if (auto* x = bus(id)) for (auto& s : x->fx) if (s.id == key) return s.num(prm.c_str(), 0); return 0.0; };
        r.set = [this, id, key, prm](double v) { if (auto* x = bus(id)) for (auto& s : x->fx) if (s.id == key) s.p.set(juce::Identifier(prm), v); };
        return r;
    }
    if (kind == "plug" && key.size() > 1) {
        us::Channel* c = nullptr; for (auto& x : p.channels) if (x.id == id) c = &x; if (!c) return std::nullopt;
        r.label = juce::String(c->name) + " " + juce::String(key); r.min = 0; r.max = 1;
        if (auto pi = eng.plugin("ch:" + id)) for (auto& q : pi->info().params) if ("v" + std::to_string(q.id) == key) { r.label = juce::String(c->name) + u8s(" · ") + juce::String(q.name); r.min = q.min; r.max = q.max; }
        r.get = [this, id, key] { for (auto& x : p.channels) if (x.id == id && x.plug.isObject()) return (double)x.plug.getProperty(juce::Identifier(key), 0.0); return 0.0; };
        r.set = [this, id, key](double v) { for (auto& x : p.channels) if (x.id == id) editablePlug(x).getDynamicObject()->setProperty(juce::Identifier(key), v); };
        return r;
    }
    return std::nullopt;
}
void Doc::autoWrite(const std::string& t, double v) {
    if (!recAuto || !rec || !song || !eng.isPlaying()) return;
    auto* l = lane(t); if (!l) { us::AutoLane nl; nl.id = us::newId(); nl.target = t; p.autos.push_back(nl); l = &p.autos.back(); }
    const double tick = eng.positionTicks();
    // replace points the pass has moved over since the last write (recording overwrites), then add this one
    l->pts.erase(std::remove_if(l->pts.begin(), l->pts.end(), [&](const us::AutoPt& q) { return q.t > lastAutoTick[t] && q.t <= tick; }), l->pts.end());
    l->pts.push_back({ tick, v }); std::sort(l->pts.begin(), l->pts.end(), [](auto& a, auto& b) { return a.t < b.t; });
    lastAutoTick[t] = tick; dirty = true;
}

us::Channel* Doc::chan() { for (auto& c : p.channels) if (c.id == ch) return &c; return p.channels.empty() ? nullptr : &p.channels[0]; }
us::Pattern* Doc::patt() { for (auto& x : p.patterns) if (x.id == pat) return &x; return p.patterns.empty() ? nullptr : &p.patterns[0]; }
// M = master, 1..n = inserts, A, B… = sends (state.js busLabel)
juce::String Doc::busLabel(const std::string& id) const {
    int ins = 0, snd = 0;
    for (auto& b : p.buses) {
        if (b.kind == "insert") ins++; else if (b.kind == "send") snd++;
        if (b.id == id) return b.kind == "master" ? "M" : b.kind == "send" ? juce::String::charToString((juce::juce_wchar)('A' + snd - 1)) : juce::String(ins);
    }
    return "M";
}

}   // namespace ui

#include "Presets.h"
namespace ui {
// The starter kit and the demo song (state.js kitProject / demoProject): drums, keys, bass and lead on their own buses.
us::Project demoProject() {
    us::Project P; P.name = "First Light"; P.bpm = 92; P.swing = 0.18; P.keyRoot = 9; P.keyScale = "minor";
    auto bus = [&](const std::string& name, const std::string& kind = "insert") { us::Bus b; b.id = kind == "master" ? "master" : us::newId(); b.name = name; b.kind = kind; P.buses.push_back(b); return b.id; };
    bus("Master", "master"); P.buses[0].vol = 0.9; P.buses[0].fx.push_back(newFx("limiter"));
    const auto d = bus("Drums"), k = bus("Keys"), b = bus("Bass"), l = bus("Lead"), sa = bus("Send A", "send"), sb = bus("Send B", "send");
    for (auto& x : P.buses) { if (x.id == sa) { x.fx.push_back(newFx("reverb")); x.fx.back().p.set("mix", 1.0); } if (x.id == sb) { x.fx.push_back(newFx("delay")); x.fx.back().p.set("mix", 1.0); }
        if (x.id == k) x.sends.push_back({ sa, 0.25 }); if (x.id == l) { x.sends.push_back({ sa, 0.2 }); x.sends.push_back({ sb, 0.25 }); } }
    static const char* cols[] = { "#ff7a59", "#ffb84d", "#e6d15a", "#7bd88f", "#45d0c4", "#5aa9ff", "#9d8cff", "#f277c6" };
    auto add = [&](const std::string& type, const std::string& name, const std::string& sample, const std::string& busId) { auto c = newChannel(P, type, name, sample); c.color = cols[P.channels.size() % 8]; c.bus = busId; P.channels.push_back(c); return &P.channels.back(); };
    add("sample", "Kick", "808:kick", d); add("sample", "Snare", "808:snare", d); add("sample", "Clap", "808:clap", d);
    { auto* c = add("sample", "Hat", "808:hat", d); c->choke = 1; c->vol = 0.6; } { auto* c = add("sample", "Open Hat", "808:ohat", d); c->choke = 1; c->vol = 0.55; }
    { auto* c = add("sample", "Keys", "inst:keys", k); c->gate = true; c->rel = 0.3; }
    for (auto& [n, fn] : synthPresets()) { if (std::string(n) == "Sub Bass") { auto* c = add("synth", "Bass", "inst:keys", b); fn(*c->synth); } }
    for (auto& [n, fn] : synthPresets()) { if (std::string(n) == "Soft Lead") { auto* c = add("synth", "Lead", "inst:keys", l); fn(*c->synth); c->vol = 0.6; } }
    std::vector<std::string> id; for (auto& c : P.channels) id.push_back(c.id);
    auto pat = [&](const std::string& name, double bars) { us::Pattern p; p.id = us::newId(); p.name = name; p.bars = bars; P.patterns.push_back(p); return P.patterns.size() - 1; };
    const size_t beat = pat("Beat", 1), chords = pat("Chords", 2), bassP = pat("Bass", 2), leadP = pat("Lead", 2);
    auto st = [&](size_t p, size_t ch, std::vector<int> steps, int n = 60, double v = 0.8, double dur = 1) { for (int s : steps) { us::Note x; x.id = us::newId(); x.t = s * us::STEP; x.d = dur * us::STEP; x.n = n; x.v = v; P.patterns[p].notes[id[ch]].push_back(x); } };
    st(beat, 0, { 0, 7, 10 }); st(beat, 1, { 4, 12 }); st(beat, 2, { 12 }, 60, 0.5); st(beat, 3, { 0, 4, 8, 12 }, 60, 0.8); st(beat, 3, { 2, 6, 10 }, 60, 0.5); st(beat, 4, { 14 }, 60, 0.7);
    for (int n : { 57, 60, 64, 67 }) st(chords, 5, { 0 }, n, 0.7, 15); for (int n : { 53, 57, 60, 64 }) st(chords, 5, { 16 }, n, 0.7, 15);
    st(bassP, 6, { 0 }, 45, 0.9, 3); st(bassP, 6, { 3, 6 }, 45, 0.7, 2); st(bassP, 6, { 10 }, 45, 0.8, 4); st(bassP, 6, { 16 }, 41, 0.9, 3); st(bassP, 6, { 19, 22 }, 41, 0.7, 2); st(bassP, 6, { 26 }, 41, 0.8, 3); st(bassP, 6, { 30 }, 43, 0.8, 2);
    st(leadP, 7, { 0 }, 76, 0.8, 5); st(leadP, 7, { 6 }, 74, 0.7, 2); st(leadP, 7, { 8 }, 72, 0.8, 6); st(leadP, 7, { 18 }, 69, 0.7, 4); st(leadP, 7, { 24 }, 72, 0.8, 4); st(leadP, 7, { 28 }, 76, 0.8, 4);
    auto clip = [&](size_t p, int track, double start, double len) { us::Clip c; c.id = us::newId(); c.pat = P.patterns[p].id; c.track = track; c.start = start; c.len = len; P.clips.push_back(c); };
    clip(beat, 0, 0, 8); clip(chords, 1, 0, 8); clip(bassP, 2, 2, 6); clip(leadP, 3, 4, 4);
    return P;
}
}   // namespace ui
