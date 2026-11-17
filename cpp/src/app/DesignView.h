// Sound Design (app/js/ui/design.js): build a one-shot from a swept tone (with FM), filtered noise and / or an existing
// sample, shape it (length, fades, drive, filter sweep, normalise) and run it through any number of effects, built-in or
// CLAP / VST plug-ins, in order, with a ring-out tail. It plays on every change; keep it in the library, as a new rack
// channel, or on the selected sampler.
#pragma once
#include <thread>
#include "Cards.h"
#include "Doc.h"
#include "PlugUI.h"
#include "engine/SoundDesign.h"

namespace ui {

class DesignView : public juce::Component, private juce::Timer {
public:
    explicit DesignView(Doc& d) : doc(d) {
        addAndMakeVisible(cv); D = us::designPresets()[0].second; name = us::designPresets()[0].first;
        for (juce::Component* c : std::initializer_list<juce::Component*>{ &startL, &startBox, &playB, &autoChk, &info, &saveB, &rackB, &selB }) cv.addAndMakeVisible(c);
        cv.makeBar = [this](Flow& f) { f.add(startL, startL.idealWidth()).add(startBox, 190).add(playB, playB.idealWidth()).add(autoChk, 130).right(selB, selB.idealWidth()).right(rackB, rackB.idealWidth()).right(saveB, saveB.idealWidth()).right(info, std::max(60, info.idealWidth())); };
        playB.primary = true; playB.onClick = [this] { play(); };
        autoChk.setButtonText("Play on change"); autoChk.setToggleState(true, juce::dontSendNotification); autoChk.onClick = [this] { autoPlay = autoChk.getToggleState(); };
        saveB.setTooltip("Keep this sound as a sample"); rackB.setTooltip("Save and add as a new channel"); selB.setTooltip("Save and use for the selected sampler channel");
        saveB.onClick = [this] { save(0); }; rackB.onClick = [this] { save(1); }; selB.onClick = [this] { save(2); };
        startBox.onChange = [this] { const int i = startBox.getSelectedItemIndex(); if (i > 0 && i - 1 < (int)startIds.size()) { const auto id = startIds[(size_t)i - 1]; startBox.setSelectedItemIndex(0, juce::dontSendNotification); start(id); } };
        cv.top = &wave; cv.topH = 118; cv.addAndMakeVisible(wave); wave.col = ACCENT;
        build(); render(false);
    }
    void resized() override { cv.setBounds(getLocalBounds()); }

private:
    Doc& doc; CardView cv; us::DesignP D; std::string name; std::shared_ptr<us::Sample> out; bool autoPlay = true; int gen = 0;
    Cap startL{ "Start from" }; juce::ComboBox startBox; std::vector<std::string> startIds; Btn playB{ u8("▶ Play") }, saveB{ "Save to library" }, rackB{ "+ Rack" }, selB{ u8("→ Selected") };
    juce::ToggleButton autoChk; Cap info{ "", FS * 0.88f }; Wave wave; std::unique_ptr<juce::FileChooser> chooser;
    static juce::String pct(double v) { return juce::String(juce::roundToInt(v * 100)) + "%"; }
    static juce::String secs(double v) { return v < 1 ? juce::String(juce::roundToInt(v * 1000)) + "ms" : juce::String(v, 2) + "s"; }
    static juce::String hz(double v) { return v >= 1000 ? juce::String(v / 1000, 1) + "k" : juce::String(juce::roundToInt(v)) + "Hz"; }
    void changed() { startTimer(160); }   // re-render a moment after the last change
    void timerCallback() override { stopTimer(); render(true); }
    std::function<void()> none() { return [] {}; }
    template <class F> std::function<void(double)> on(F set) { return [this, set](double v) { set(v); changed(); }; }

    void startMenu() {
        startBox.clear(juce::dontSendNotification); startIds.clear(); startBox.addItem(u8("— choose —"), 1); int k = 2;
        for (auto& [n, p] : us::designPresets()) { startBox.addItem(u8("Preset · ") + juce::String(n), k++); startIds.push_back(std::string("p:") + n); }
        startBox.addItem(u8("Your own audio file…"), k++); startIds.push_back("file");
        for (auto& s : doc.bank.imported()) { startBox.addItem(u8("★ ") + juce::String(s->name), k++); startIds.push_back("s:" + s->id); }
        for (auto& s : doc.bank.kit()) { startBox.addItem(juce::String(s->name), k++); startIds.push_back("s:" + s->id); }
        startBox.setSelectedItemIndex(0, juce::dontSendNotification);
    }
    void start(const std::string& v) {
        if (v == "file") { chooser = std::make_unique<juce::FileChooser>("Your own audio", juce::File::getSpecialLocation(juce::File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) { if (!fc.getResult().existsAsFile()) return; auto id = doc.importAudio(fc.getResult()); if (id.empty()) toast("Could not decode that file"); else start("s:" + id); }); return; }
        if (v.rfind("s:", 0) == 0) { auto s = doc.bank.get(v.substr(2)); if (!s) return; us::DesignP d; d.len = std::min(30.0, std::max(0.05, s->seconds())); d.tone.on = false; d.smp.on = true; d.smp.id = s->id; d.smp.level = 1; d.fade = 0.005; D = d; name = s->name + " edit"; }
        else for (auto& [n, p] : us::designPresets()) if (v == std::string("p:") + n) { D = p; name = n; }
        build(); render(true);
    }
    void build() {
        startMenu(); auto& G = cv.grid; G.clear(); G.minCol = 290;
        auto& T = D.tone; auto& N = D.noise; auto& M = D.smp; auto& F = D.filt;
        if (M.id.empty()) { auto imp = doc.bank.imported(); M.id = !imp.empty() ? imp[0]->id : doc.bank.kit().empty() ? std::string() : doc.bank.kit()[0]->id; }
        G.card("Tone", T.on, [this](bool b) { D.tone.on = b; changed(); });
        G.pick("Wave", { { "sine", "Sine" }, { "triangle", "Triangle" }, { "sawtooth", "Saw" }, { "square", "Square" } }, juce::String(T.wave), [this](const juce::String& v) { D.tone.wave = v.toStdString(); changed(); }, 96);
        G.knob("Start pitch", 20, 8000, 180, T.f0, on([this](double v) { D.tone.f0 = v; }), none(), true, hz);
        G.knob("End pitch", 20, 8000, 50, T.f1, on([this](double v) { D.tone.f1 = v; }), none(), true, hz);
        G.knob("Sweep time", 0.003, 4, 0.08, T.sweep, on([this](double v) { D.tone.sweep = v; }), none(), true, secs);
        G.knob("Decay", 0.01, 6, 0.3, T.dec, on([this](double v) { D.tone.dec = v; }), none(), true, secs);
        G.knob("Level", 0, 1, 0.9, T.level, on([this](double v) { D.tone.level = v; }), none(), false, pct);
        G.knob("FM amount", 0, 1, 0, T.fm, on([this](double v) { D.tone.fm = v; }), none(), false, pct);
        G.knob("FM ratio", 0.25, 8, 2, T.ratio, on([this](double v) { D.tone.ratio = v; }), none());
        G.note("A pitch that falls fast makes kicks and toms; one that rises slowly makes risers; FM adds metallic bite.");
        G.card("Noise", N.on, [this](bool b) { D.noise.on = b; changed(); });
        G.pick("Colour", { { "highpass", "Bright (high-pass)" }, { "bandpass", "Band" }, { "lowpass", "Dark (low-pass)" } }, juce::String(N.type), [this](const juce::String& v) { D.noise.type = v.toStdString(); changed(); }, 140);
        G.knob("Frequency", 60, 16000, 4000, N.cut, on([this](double v) { D.noise.cut = v; }), none(), true, hz);
        G.knob("Focus", 0.3, 12, 0.7, N.q, on([this](double v) { D.noise.q = v; }), none(), true);
        G.knob("Decay", 0.005, 6, 0.1, N.dec, on([this](double v) { D.noise.dec = v; }), none(), true, secs);
        G.knob("Level", 0, 1, 0.5, N.level, on([this](double v) { D.noise.level = v; }), none(), false, pct);
        G.card("Layer an existing sample", M.on, [this](bool b) { D.smp.on = b; changed(); });
        std::vector<std::pair<juce::String, juce::String>> own; for (auto& s : doc.bank.imported()) own.push_back({ s->id, u8("★ ") + juce::String(s->name) }); for (auto& s : doc.bank.kit()) own.push_back({ s->id, juce::String(s->name) });
        G.pick("Sample", own, juce::String(M.id), [this](const juce::String& v) { D.smp.id = v.toStdString(); changed(); }, 150);
        G.button(u8("Upload audio…"), [this] { chooser = std::make_unique<juce::FileChooser>("Upload audio", juce::File::getSpecialLocation(juce::File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) { if (!fc.getResult().existsAsFile()) return; auto id = doc.importAudio(fc.getResult()); if (id.empty()) { toast("Could not decode that file"); return; } D.smp.id = id; D.smp.on = true; build(); changed(); }); })->setTooltip("Use one of your own audio files as the layer");
        G.knob("Start", 0, 0.95, 0, M.start, on([this](double v) { D.smp.start = v; }), none(), false, pct);
        G.knob("Pitch", -24, 24, 0, M.semi, on([this](double v) { D.smp.semi = std::round(v); }), none(), false, [](double v) { return juce::String(juce::roundToInt(v)) + "st"; }, 1);
        G.knob("Level", 0, 1.5, 0.8, M.level, on([this](double v) { D.smp.level = v; }), none(), false, pct);
        G.toggle("Reverse", M.rev, [this](bool b) { D.smp.rev = b; changed(); });
        G.note("Reshape any sound you already have: trim its start, re-pitch or reverse it, then add tone, noise, drive and filtering on top.");
        G.card("Shape");
        G.knob("Length", 0.03, 30, 0.5, D.len, on([this](double v) { D.len = v; }), none(), true, secs);
        G.knob("Fade in", 0.0005, 4, 0.001, D.att, on([this](double v) { D.att = v; }), none(), true, secs);
        G.knob("Fade out", 0.002, 2, 0.02, D.fade, on([this](double v) { D.fade = v; }), none(), true, secs);
        G.knob("Drive", 0, 1, 0, D.drive, on([this](double v) { D.drive = v; }), none(), false, pct);
        G.toggle("Normalise", D.norm, [this](bool b) { D.norm = b; changed(); }, "Bring the loudest point up to just under full scale");
        G.card("Filter sweep", F.on, [this](bool b) { D.filt.on = b; changed(); });
        G.knob("Start", 40, 16000, 8000, F.cut, on([this](double v) { D.filt.cut = v; }), none(), true, hz);
        G.knob("End", 40, 16000, 8000, F.cut2, on([this](double v) { D.filt.cut2 = v; }), none(), true, hz);
        G.knob("Resonance", 0.3, 18, 1, F.q, on([this](double v) { D.filt.q = v; }), none(), true);
        fxCard();
        cv.resized();
    }
    // effects after the sound: any number, in order, built-in or plug-in; the result becomes stereo
    void fxCard() {
        auto& G = cv.grid; G.card("Effects");
        G.button("+ Add effect", [this] { addFx(); }, true);
        G.knob("Ring-out", 0, 8, 1, D.tail, on([this](double v) { D.tail = v; }), none(), false, secs);
        for (size_t i = 0; i < D.fx.size(); i++) {
            auto& f = D.fx[i]; const FxSpec* sp = fxSpec(f.type);
            struct Title : juce::Component { Btn led, up{ u8("↑") }, dn{ u8("↓") }, rm{ u8("✕") }; Cap nm{ {}, FS }; Title() { for (auto* b : { &up, &dn, &rm }) { b->sm = true; addAndMakeVisible(b); } addAndMakeVisible(led); addAndMakeVisible(nm); led.setAlpha(0.01f); nm.setFont(fontB(FS)); nm.setColour(juce::Label::textColourId, TEXT); }
                void resized() override { auto r = getLocalBounds(); led.setBounds(r.removeFromLeft(14).withSizeKeepingCentre(14, 14)); r.removeFromLeft(6); rm.setBounds(r.removeFromRight(30)); r.removeFromRight(4); dn.setBounds(r.removeFromRight(30)); r.removeFromRight(4); up.setBounds(r.removeFromRight(30)); nm.setBounds(r); }
                void paintOverChildren(juce::Graphics& g) override { auto l = led.getBounds().toFloat(); g.setColour(led.on() ? ACCENT : BG); g.fillEllipse(l); g.setColour(led.on() ? ACCENT.darker(0.4f) : LINE); g.drawEllipse(l.reduced(1), 2); } };
            auto* t = new Title(); t->nm.setText(f.type == "plugin" ? juce::String("Plug-in") : juce::String(sp ? sp->name : f.type.c_str()), juce::dontSendNotification); t->led.setOn(f.on);
            t->led.onClick = [this, i] { if (i < D.fx.size()) { D.fx[i].on = !D.fx[i].on; build(); changed(); } };
            t->up.setTooltip("Earlier"); t->dn.setTooltip("Later"); t->rm.setTooltip("Remove");
            t->up.onClick = [this, i] { if (i > 0) { std::swap(D.fx[i], D.fx[i - 1]); build(); changed(); } };
            t->dn.onClick = [this, i] { if (i + 1 < D.fx.size()) { std::swap(D.fx[i], D.fx[i + 1]); build(); changed(); } };
            t->rm.onClick = [this, i] { if (i < D.fx.size()) { D.fx.erase(D.fx.begin() + (long)i); build(); changed(); } };
            G.full(t, CTL() - 4);
            if (f.type == "plugin") {
                G.note(f.str("name", "Plug-in"));
                if (auto pl = plugParams.find(f.id); pl != plugParams.end()) for (auto& q : pl->second) {
                    const auto key = "v" + std::to_string(q.id); const double cur = f.p.contains(juce::Identifier(key)) ? f.num(key.c_str(), q.value) : q.value;
                    G.knob(juce::String(q.name).substring(0, 12), q.min, q.max > q.min ? q.max : q.min + 1, q.def, cur, on([this, i, key](double v) { if (i < D.fx.size()) D.fx[i].p.set(juce::Identifier(key), v); }), none(), false, {}, q.stepped ? 1 : 0);
                }
            } else if (sp) for (auto& q : sp->params) {
                if (!q.opts.empty()) { if (q.opts[0] == "@buses") continue; std::vector<std::pair<juce::String, juce::String>> o; for (auto& x : q.opts) o.push_back({ x, x }); const std::string k = q.k;
                    G.pick(q.label, o, juce::String(f.str(k.c_str(), "")), [this, i, k](const juce::String& v) { if (i < D.fx.size()) { D.fx[i].p.set(juce::Identifier(k), v); changed(); } }, 110); continue; }
                const std::string k = q.k; G.knob(q.label, q.lo, q.hi, q.def, f.num(k.c_str(), q.def), on([this, i, k](double v) { if (i < D.fx.size()) D.fx[i].p.set(juce::Identifier(k), v); }), none(), q.expo);
            }
            G.br();
        }
        if (D.fx.empty()) G.note("Add EQ, compression, reverb, delay, chorus, distortion and more, or your own CLAP and VST effects. Ring-out lengthens the sound so reverb and delay tails are kept.");
    }
    std::map<std::string, std::vector<us::PluginInstance::Param>> plugParams;   // what each plug-in effect reported (its knobs)
    void addFx() {
        Menu m; m.head("Built-in");
        for (auto& s : fxSpecs()) { if (std::string(s.type) == "duck") continue; const std::string t = s.type; m.item(s.name, [this, t] { D.fx.push_back(newFx(t)); if (t == "reverb" || t == "delay") D.tail = std::max(D.tail, 2.0); build(); changed(); }); }
        m.head("Plug-ins");
        m.item(u8("Plug-in effect (CLAP / VST)…"), [this] { choosePlugin(this, false, [this](const juce::var& slot) {
            us::FxSlot f; f.id = us::newId(); f.type = "plugin"; f.on = true; if (auto* o = slot.getDynamicObject()) f.p = o->getProperties(); D.fx.push_back(f); build();
            const auto fid = f.id;   // load it once to learn its parameters
            std::thread([this, slot, fid, sp = juce::Component::SafePointer<DesignView>(this)] { auto pi = std::make_shared<us::PluginInstance>(); std::string err; const bool ok = pi->load(slot, 44100, err);
                std::vector<us::PluginInstance::Param> ps; if (ok) { for (auto& q : pi->info().params) if (!q.hidden && !q.readonly) { ps.push_back(q); if (ps.size() >= 32) break; } }
                juce::MessageManager::callAsync([sp, fid, ps, ok, err] { if (!sp) return; if (!ok) toast(err.empty() ? juce::String("The plug-in could not be loaded") : juce::String(err)); sp->plugParams[fid] = ps; sp->build(); sp->changed(); }); }).detach();
        }); });
        m.show(atMouse());
    }
    void render(bool hear) {
        const int g = ++gen; auto d = D; bool plug = false; for (auto& f : d.fx) plug |= f.on && f.type == "plugin";
        auto finish = [this, hear](std::shared_ptr<us::Sample> s) { out = s; out->id = "design:preview"; out->name = "Preview"; doc.bank.put(out); wave.data = out->ch[0]; wave.repaint();
            info.setText(juce::String(out->seconds(), 2) + " s" + u8(" · ") + (out->ch.size() > 1 ? "stereo" : "mono") + u8(" · 44.1 kHz"), juce::dontSendNotification); cv.resized(); if (hear && autoPlay) play(); };
        if (!plug) { finish(us::renderDesign(d, doc.bank)); return; }
        info.setText(u8("Rendering…"), juce::dontSendNotification);   // plug-ins load in their hosts: off the message thread
        std::thread([this, d, g, finish, sp = juce::Component::SafePointer<DesignView>(this)] { auto s = us::renderDesign(d, doc.bank); juce::MessageManager::callAsync([sp, s, g, finish] { if (sp && g == sp->gen) finish(s); }); }).detach();
    }
    void play() { if (out) doc.eng.previewSample("design:preview"); }
    void save(int where) {
        if (!out) return;
        ask("Name for this sound", juce::String(name), [this, where](juce::String v) {
            if (v.isEmpty()) return; name = v.toStdString(); const std::string id = doc.addSampleData(name, us::wavBytes(*out)); if (id.empty()) return;
            if (where == 1) { auto c = newChannel(doc.p, "sample", name.substr(0, 18), id); c.gate = false; doc.p.channels.push_back(c); doc.ch = c.id; doc.commit(); toast(u8("“") + v + u8("” added to the rack")); }
            else if (where == 2) { if (auto* c = doc.chan(); c && c->type == "sample") { c->sample = id; doc.commit(); toast(u8("“") + juce::String(c->name) + u8("” now plays ") + v); } else { doc.dirty = true; doc.sendChangeMessage(); toast("Saved to the library - select a sampler channel to use it"); } }
            else { doc.dirty = true; doc.sendChangeMessage(); toast(u8("Saved “") + v + u8("” to the library")); }
            build();
        });
    }
};

}   // namespace ui
