// Audio Lab (app/js/ui/lab.js): 1 · audio in (record the microphone, load a file, any sound or take), a live tuner that
// can play the selected channel from your voice, 2 · key and tempo (with the pitch-class chart), 3 · audio to MIDI (melody
// or chords, sensitivity, shortest note, quantize, start at bar 1, replace, snap to the key) with a preview of the notes.
#pragma once
#include <thread>
#include "Cards.h"
#include "Doc.h"
#include "engine/Analysis.h"

namespace ui {

class LabView : public juce::Component, private juce::ChangeListener, private juce::Timer {
public:
    explicit LabView(Doc& d) : doc(d) { doc.addChangeListener(this); addAndMakeVisible(cv); build(); startTimerHz(30); }
    ~LabView() override { doc.removeChangeListener(this); alive->store(false); stopMic(); }
    void resized() override { cv.setBounds(getLocalBounds()); }

private:
    Doc& doc; CardView cv; std::vector<float> mono; double sr = 44100; std::string srcId, srcName; juce::String infoText = "Record or load some audio.";
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true); std::atomic<double> prog{ -1 }; bool busy = false;
    us::KeyResult key; double bpm = 0; bool haveKey = false; std::vector<us::FoundNote> found; double foundDur = 0;
    std::string mode = "mono"; int quant = us::STEP; double gate = 0.08, minDur = 0.07; bool align = true, toKey = false, replace = false;
    bool recording = false, live = false; std::vector<float> rec, tap; double tuneMidi = -1; int liveCur = -1, liveCand = -1, liveN = 0;
    std::unique_ptr<juce::FileChooser> chooser; Btn* recB = nullptr; Btn* liveB = nullptr; Wave* wave = nullptr; Cap* info = nullptr; juce::Component* tuner = nullptr; juce::ProgressBar* bar = nullptr; double barV = 0;
    static juce::String noteName(int n) { return juce::MidiMessage::getMidiNoteName(n, true, true, 4); }

    void build() {
        auto& G = cv.grid; G.clear(); G.minCol = 330;
        G.card(u8("1 · Audio in"));
        recB = G.button(recording ? u8("■ Stop recording") : u8("● Record mic"), [this] { toggleRec(); }); recB->rec = !recording; recB->setOn(recording);
        G.button(u8("Load audio file…"), [this] { loadDialog(); });
        G.button(u8("▶ / ■"), [this] { if (!srcId.empty()) doc.eng.previewSample(srcId); })->setTooltip("Preview");
        G.button("Add as instrument", [this] { asSample(); })->setTooltip("Use this audio as a playable sampler channel");
        G.button(u8("✕ Clear"), [this] { mono.clear(); srcId.clear(); haveKey = false; found.clear(); infoText = "Record or load some audio."; build(); })->setTooltip("Remove this audio and its results so you can record or load something new");
        std::vector<std::pair<juce::String, juce::String>> opts = { { "", u8("Or use a sound / take…") } }; for (auto& s : doc.bank.imported()) opts.push_back({ s->id, u8("★ ") + juce::String(s->name) }); for (auto& s : doc.bank.kit()) opts.push_back({ s->id, juce::String(s->name) });
        auto* p = new Pick({}, opts, juce::String(srcId)); p->onPick = [this](const juce::String& v) { if (v.isNotEmpty()) use(v.toStdString()); }; G.add(p, 200, Pick::height());
        wave = G.full(new Wave(), 64); wave->data = mono;
        info = G.note(infoText);
        bar = G.full(new juce::ProgressBar(barV), 8); bar->setVisible(busy);
        G.card("Live pitch");
        struct Tuner : juce::Component { LabView* o = nullptr; void paint(juce::Graphics& g) override {
            const double m = o->tuneMidi; const int n = m < 0 ? -1 : (int)std::round(m); g.setColour(ACCENT); g.setFont(fontB(FS * 1.9f)); g.drawText(n < 0 ? u8("–") : noteName(n), 0, 0, 80, getHeight(), juce::Justification::centredLeft);
            auto r = juce::Rectangle<float>(92, getHeight() / 2.f - 4, std::min(220.f, getWidth() - 100.f), 8); g.setColour(BG); g.fillRoundedRectangle(r, 4); g.setColour(DIM); g.fillRect(r.getCentreX(), r.getY() - 3, 1.f, r.getHeight() + 6);
            const float off = n < 0 ? 0.f : (float)(m - n); g.setColour(ACCENT2); g.fillRoundedRectangle(r.getCentreX() - 5 + off * r.getWidth(), r.getY() - 2, 10, 12, 3); } };
        auto* t = new Tuner(); t->o = this; tuner = G.full(t, 40);
        liveB = G.button(u8("Live audio → MIDI"), [this] { toggleLive(); }); liveB->setOn(live); liveB->setTooltip("Play the selected channel with your voice or instrument; records like a MIDI keyboard");
        G.note("Tracks one note at a time from the microphone (input 1).");
        G.card(u8("2 · Key & tempo"));
        G.button(u8("Detect key & tempo"), [this] { detect(); }, true); G.note("Works on full songs, loops or a hummed tune.");
        if (haveKey) {
            struct KeyBig : juce::Component { LabView* o = nullptr; void paint(juce::Graphics& g) override { auto& k = o->key; g.setColour(ACCENT); g.setFont(fontB(FS * 1.5f)); const int w = textW(fontB(FS * 1.5f), juce::String(k.name)); g.drawText(juce::String(k.name), 0, 0, w + 4, getHeight(), juce::Justification::centredLeft);
                int x = w + 12; for (auto& t : { "Camelot " + juce::String(k.camelot), juce::String(juce::roundToInt(k.conf * 100)) + "% confidence", o->bpm > 0 ? u8("≈ ") + juce::String(juce::roundToInt(o->bpm)) + " BPM" : juce::String() }) { if (t.isEmpty()) continue; const int tw = textW(font(FS * 0.8f), t) + 16; g.setColour(BG3); g.fillRoundedRectangle((float)x, getHeight() / 2.f - 10, (float)tw, 20, 10); g.setColour(DIM); g.setFont(font(FS * 0.8f)); g.drawText(t, x, 0, tw, getHeight(), juce::Justification::centred); x += tw + 6; } } };
            auto* kb = new KeyBig(); kb->o = this; G.full(kb, 32);
            struct Chroma : juce::Component { LabView* o = nullptr; void paint(juce::Graphics& g) override { const float w = getWidth() / 12.f; for (int i = 0; i < 12; i++) { const float v = (float)o->key.chroma[(size_t)i], h = std::max(2.f, v * (getHeight() - 16)); g.setColour(ACCENT.withAlpha(0.85f)); g.fillRoundedRectangle(i * w + 1.5f, getHeight() - 14 - h, w - 3, h, 3); g.setColour(DIM); g.setFont(font(FS * 0.7f)); g.drawText(us::NOTE_NAMES[i], (int)(i * w), getHeight() - 14, (int)w, 14, juce::Justification::centred); } } };
            auto* ch = new Chroma(); ch->o = this; G.full(ch, 74);
            juce::String alts; for (size_t i = 0; i < key.alts.size(); i++) alts << juce::String(key.alts[i].name) << (i + 1 < key.alts.size() ? ", " : ""); G.note("Also possible: " + alts);
            G.button("Use as project key", [this] { doc.p.keyRoot = key.root; doc.p.keyScale = key.minor ? "minor" : "major"; doc.commit(); toast("Piano roll now highlights " + juce::String(key.name)); }, true);
            if (bpm > 0) G.button("Set tempo to " + juce::String(juce::roundToInt(bpm)), [this] { doc.p.bpm = std::round(bpm); doc.commit(); });
        }
        G.card(u8("3 · Audio → MIDI"));
        G.pick("Source type", { { "mono", "Melody (one note at a time)" }, { "poly", "Chords (experimental)" } }, juce::String(mode), [this](const juce::String& v) { mode = v.toStdString(); }, 200);
        G.knob("Sensitivity", 0.01, 0.4, 0.08, gate, [this](double v) { gate = v; }, [] {}, true, [](double v) { return juce::String(juce::roundToInt((1 - std::log(v / 0.01) / std::log(40.0)) * 100)) + "%"; });
        G.knob("Min note", 0.03, 0.4, 0.07, minDur, [this](double v) { minDur = v; }, [] {}, true, [](double v) { return juce::String(juce::roundToInt(v * 1000)) + "ms"; });
        G.pick("Quantize", { { "0", "Off" }, { "48", "1/8" }, { "24", "1/16" }, { "12", "1/32" } }, juce::String(quant), [this](const juce::String& v) { quant = v.getIntValue(); }, 90);
        G.br(); G.check("Start at bar 1", align, [this](bool b) { align = b; }); G.check("Replace existing notes", replace, [this](bool b) { replace = b; }); G.check("Snap to project key", toKey, [this](bool b) { toKey = b; });
        G.br(); G.button("Convert to MIDI", [this] { convert(); }, true);
        if (!found.empty() || foundDur > 0) {
            G.note(found.empty() ? juce::String("No notes found - try lowering Sensitivity") : juce::String((int)found.size()) + " notes found", FS);
            if (!found.empty()) {
                struct Roll : juce::Component { LabView* o = nullptr; void paint(juce::Graphics& g) override { g.setColour(BG); g.fillRoundedRectangle(getLocalBounds().toFloat(), RAD() * 0.6f); int lo = 127, hi = 0; for (auto& n : o->found) { lo = std::min(lo, n.n); hi = std::max(hi, n.n); } lo--; hi++;
                    const float W = (float)getWidth(), H = (float)getHeight(), rh = std::min(8.f, H / (hi - lo + 1)); const double dur = std::max(0.1, o->foundDur);
                    for (auto& n : o->found) { g.setColour(ACCENT.withAlpha((float)(0.4 + 0.6 * n.v))); g.fillRect((float)(n.s / dur) * W, (float)(hi - n.n) / (hi - lo + 1) * H, std::max(2.f, (float)((n.e - n.s) / dur) * W - 1), std::max(2.f, rh - 1)); } } };
                auto* r = new Roll(); r->o = this; G.full(r, 64);
                G.button("Insert into piano roll", [this] { insert(); }, true);
                G.note(u8("Goes to “") + juce::String(doc.chan() ? doc.chan()->name : std::string("a channel you add first")) + u8("” in “") + juce::String(doc.patt() ? doc.patt()->name : std::string()) + u8("” at ") + juce::String(doc.p.bpm) + " BPM");
            }
        }
        cv.resized();
    }
    void use(const std::string& id) {
        auto s = doc.bank.get(id); if (!s) return;
        srcId = id; srcName = s->name; mono = us::toMono(*s); sr = s->sr; haveKey = false; bpm = 0; found.clear(); foundDur = 0;
        infoText = juce::String(s->name) + u8(" · ") + juce::String(s->seconds(), 1) + " s" + u8(" · ") + juce::String(s->sr, 0) + " Hz"; build();
    }
    void loadDialog() {
        chooser = std::make_unique<juce::FileChooser>("Load audio", juce::File::getSpecialLocation(juce::File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
            auto f = fc.getResult(); if (!f.existsAsFile()) return; juce::MemoryBlock mb; f.loadFileAsData(mb); const std::string id = "lab:" + us::newId();
            if (doc.bank.add(id, f.getFileNameWithoutExtension().toStdString(), mb)) use(id); else toast("That file could not be read as audio.");
        });
    }
    // the microphone (the audio device's input 1, or 1 + 2), through the engine's input tap
    void startMic() { doc.eng.tapOn = true; tap.clear(); }
    void stopMic() { if (!recording && !live) doc.eng.tapOn = false; }
    void toggleRec() {
        if (recording) { recording = false; stopMic(); if (rec.size() > (size_t)(devRate() * 0.2)) { auto s = std::make_shared<us::Sample>(); s->sr = devRate(); s->ch = { rec }; s->name = "Recording"; s->id = "lab:" + us::newId();
                const std::string id = doc.addSampleData("Recording", us::wavBytes(*s)); if (!id.empty()) use(id); } rec.clear(); build(); return; }
        rec.clear(); recording = true; startMic(); build();
    }
    void toggleLive() {
        if (live) { live = false; if (liveCur >= 0) doc.audition(liveCur, false); liveCur = -1; stopMic(); build(); return; }
        live = true; startMic(); build(); toast(u8("Live audio → MIDI is on. Use headphones to avoid feedback; arm record and press play to capture it."));
    }
    static double devRate() { return 48000; }
    void timerCallback() override {
        if (doc.eng.tapOn) {   // read what the microphone sent since the last tick
            float buf[16384]; int n; while ((n = doc.eng.readTap(buf, 16384)) > 0) { if (recording) rec.insert(rec.end(), buf, buf + n); tap.insert(tap.end(), buf, buf + n); }
            if (tap.size() > 4096) tap.erase(tap.begin(), tap.end() - 4096);
            if (tap.size() >= 2048) { tuneMidi = us::pitchOf(std::vector<float>(tap.end() - 2048, tap.end()), devRate()); if (tuner) tuner->repaint();
                if (live) { const int nn = tuneMidi < 0 ? -1 : (int)std::round(tuneMidi); if (nn == liveCand) liveN++; else { liveCand = nn; liveN = 1; }
                    if (liveN >= 2 && nn != liveCur) { if (liveCur >= 0) doc.audition(liveCur, false); if (nn >= 0) doc.audition(nn, true, 0.8); liveCur = nn; } } }
            if (recording && wave) { wave->data = rec; wave->repaint(); }
        }
        if (busy && prog >= 0) { barV = prog; if (bar) bar->repaint(); }
    }
    template <class Work, class Done> void run(Work work, Done done) {
        if (busy) return; if (mono.empty()) { toast("Record or load some audio first"); return; }
        busy = true; prog = 0; barV = 0; if (bar) bar->setVisible(true);
        auto flag = alive; auto x = mono; const double rate = sr;
        std::thread([this, flag, x, rate, work, done] {
            auto r = work(x, rate, [this, flag](double p) { if (*flag) prog = p; });
            juce::MessageManager::callAsync([this, flag, r, done] { if (!*flag) return; busy = false; prog = -1; done(r); });
        }).detach();
    }
    void detect() { run([](const std::vector<float>& x, double rate, us::Progress p) { auto k = us::detectKey(x, rate, p); return std::make_pair(k, us::detectBpm(x, rate)); },
        [this](const std::pair<us::KeyResult, double>& r) { key = r.first; bpm = r.second; haveKey = true; build(); }); }
    void convert() {
        const bool poly = mode == "poly"; const double g = gate, md = minDur; foundDur = mono.size() / sr;
        run([poly, g, md](const std::vector<float>& x, double rate, us::Progress p) { if (poly) { us::ChordOpts o; o.gate = g; return us::chordNotes(x, rate, o, p); } us::MelodyOpts o; o.gate = g; o.minDur = md; return us::melodyNotes(x, rate, o, p); },
            [this](const std::vector<us::FoundNote>& f) { found = f; if (found.empty()) foundDur = 0.0001; build(); });
    }
    void insert() {
        auto* c = doc.chan(); auto* pt = doc.patt(); if (!c || !pt) { toast("Add a channel in the Rack first"); return; }
        auto notes = us::toTicks(found, doc.p.bpm, quant, align); if (toKey) for (auto& n : notes) n.n = us::snapToScale(n.n, doc.p.keyRoot, doc.p.keyScale);
        const size_t before = notes.size(); notes.erase(std::remove_if(notes.begin(), notes.end(), [&](auto& n) { return n.t >= 64.0 * doc.p.bar(); }), notes.end());
        auto& list = pt->notes[c->id]; if (replace) list.clear(); for (auto& n : notes) { n.id = us::newId(); list.push_back(n); }
        double end = 0; for (auto& n : notes) end = std::max(end, n.t + n.d); double b = 1; while (b * doc.p.bar() < end && b < 64) b *= 2; pt->bars = std::max(pt->bars, b);
        doc.commit(); doc.go("roll", "lab");
        toast(before > notes.size() ? "Inserted " + juce::String((int)notes.size()) + " notes (audio longer than 64 bars was trimmed)" : "Inserted " + juce::String((int)notes.size()) + " notes");
    }
    void asSample() {
        if (srcId.empty()) { toast("Record or load some audio first"); return; } auto s = doc.bank.get(srcId); if (!s) return;
        std::string id = srcId; if (id.rfind("lab:", 0) == 0) id = doc.addSampleData(srcName, us::wavBytes(*s));
        auto c = newChannel(doc.p, "sample", srcName.substr(0, 18), id); c.gate = true; c.rel = 0.2; doc.p.channels.push_back(c); doc.ch = c.id; doc.commit(); doc.go("inst", "lab");
    }
    void changeListenerCallback(juce::ChangeBroadcaster*) override {}
};

}   // namespace ui
