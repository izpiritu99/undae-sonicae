// Instrument editor for the selected channel (app/js/ui/inst.js): a header (colour, name, type, mixer bus, level, pan,
// Test) over cards: the whole synth (preset, macros, three oscillators with their wave shapes, blend and unison, sub and
// noise, two filters with their response curves, shaper, four envelopes, three LFOs, the modulation matrix, voice), the
// sampler (sound source with its waveform, upload, pitch detection; pitch and shape), or a plug-in instrument's panel.
#pragma once
#include <complex>
#include "Cards.h"
#include "Doc.h"
#include "PlugUI.h"
#include "Presets.h"
#include "Theme.h"
#include "engine/Analysis.h"

namespace ui {

// a synth wave's shape, n points -1..1 (synth.js waveShape)
inline std::vector<float> waveShape(const std::string& w, int n = 64) {
    std::vector<float> v((size_t)n); const double tau = juce::MathConstants<double>::twoPi;
    for (int i = 0; i < n; i++) { const double p = (double)i / n; double y;
        if (w == "sine") y = std::sin(tau * p); else if (w == "triangle") y = 1 - 4 * std::abs(std::fmod(p + 0.25, 1.0) - 0.5); else if (w == "sawtooth") y = 2 * p - 1; else if (w == "square") y = p < 0.5 ? 1 : -1;
        else if (w == "pulse25") y = p < 0.25 ? 1 : -1; else if (w == "pulse10") y = p < 0.1 ? 1 : -1; else if (w == "softsaw") y = std::tanh(3 * (2 * p - 1)) / std::tanh(3.0);
        else if (w == "organ") y = (std::sin(tau * p) + 0.5 * std::sin(2 * tau * p) + 0.3 * std::sin(3 * tau * p)) / 1.5; else if (w == "bell") y = (std::sin(tau * p) + 0.6 * std::sin(2.76 * tau * p) + 0.3 * std::sin(5.4 * tau * p)) / 1.7;
        else if (w == "vocal") y = (std::sin(tau * p) + 0.7 * std::sin(3 * tau * p) * std::exp(-2 * p) + 0.4 * std::sin(5 * tau * p)) / 1.8; else if (w == "metal") y = std::sin(tau * p + 2.5 * std::sin(3.5 * tau * p)); else if (w == "stair") y = std::floor(p * 8) / 4 - 1;
        else y = std::sin(tau * p);
        v[(size_t)i] = (float)juce::jlimit(-1.0, 1.0, y); }
    return v;
}
// a biquad's response in dB over 80 log-spaced frequencies (the filter cards' curve), RBJ formulas at 48 kHz
inline std::vector<float> filterCurve(std::string type, double f0, double q) {
    const bool steep = type.size() > 2 && type.substr(type.size() - 2) == "24"; if (steep) { type = type.substr(0, type.size() - 2); q *= 0.55; }
    const double sr = 48000, w0 = juce::MathConstants<double>::twoPi * juce::jlimit(10.0, 23000.0, f0) / sr, cw = std::cos(w0), sw = std::sin(w0), al = sw / (2 * std::max(0.05, q));
    double b0, b1, b2, a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al;
    if (type == "highpass") { b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; } else if (type == "bandpass") { b0 = al; b1 = 0; b2 = -al; } else if (type == "notch") { b0 = 1; b1 = -2 * cw; b2 = 1; }
    else if (type == "peaking") { const double A = std::pow(10, 6 / 40.0); b0 = 1 + al * A; b1 = -2 * cw; b2 = 1 - al * A; a0 = 1 + al / A; a2 = 1 - al / A; } else if (type == "allpass") { b0 = 1 - al; b1 = -2 * cw; b2 = 1 + al; }
    else { b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0; }
    std::vector<float> out(80);
    for (int i = 0; i < 80; i++) { const double f = 20 * std::pow(1000.0, i / 79.0), w = juce::MathConstants<double>::twoPi * f / sr; const std::complex<double> z = std::polar(1.0, -w), z2 = z * z;
        double m = std::abs((b0 + b1 * z + b2 * z2) / (a0 + a1 * z + a2 * z2)); if (steep) m *= m; out[(size_t)i] = (float)juce::jlimit(-1.0, 1.0, 20 * std::log10(std::max(1e-4, m)) / 30); }
    return out;
}

class InstView : public juce::Component, private juce::ChangeListener {
public:
    explicit InstView(Doc& d) : doc(d) {
        doc.addChangeListener(this); addAndMakeVisible(cv);
        cv.makeBar = [this](Flow& f) { if (!hasHead) return; f.add(dot, 12, 12).add(nameB, std::min(180, nameB.idealWidth())).add(tag, textW(font(FS * 0.8f), tag.getText()) + 18, 20).add(busL, busL.idealWidth()).add(busBox, 150).add(vol, vol.idealWidth(), vol.idealHeight()).add(pan, pan.idealWidth(), pan.idealHeight()).right(testB, testB.idealWidth()); };
        for (juce::Component* c : std::initializer_list<juce::Component*>{ &dot, &nameB, &tag, &busL, &busBox, &vol, &pan, &testB }) cv.addAndMakeVisible(c);
        nameB.setTooltip("Rename"); nameB.onClick = [this] { if (auto* c = doc.chan()) ask("Channel name", juce::String(c->name), [this](juce::String v) { if (auto* x = doc.chan(); x && v.isNotEmpty()) { x->name = v.toStdString(); doc.commit(); } }); };
        tag.setJustificationType(juce::Justification::centred); tag.setFont(font(FS * 0.8f));
        busBox.onChange = [this] { const int i = busBox.getSelectedItemIndex(); if (auto* c = doc.chan(); c && i >= 0 && i < (int)doc.p.buses.size()) { c->bus = doc.p.buses[(size_t)i].id; doc.commit(); } };
        vol.sized(Knob::Xs); pan.sized(Knob::Xs);
        vol.onChange = [this](double v) { if (auto* c = doc.chan()) { c->vol = v; doc.live(); doc.autoWrite("ch:" + c->id + ":vol", v); } }; vol.onDone = [this] { doc.commit(); };
        pan.onChange = [this](double v) { if (auto* c = doc.chan()) { c->pan = v; doc.live(); doc.autoWrite("ch:" + c->id + ":pan", v); } }; pan.onDone = [this] { doc.commit(); };
        testB.sm = true; testB.onClick = [this] { if (auto* c = doc.chan()) { const int n = c->type == "synth" ? 60 : c->root; doc.eng.noteOn(c->id, n, 0.85); juce::Timer::callAfterDelay(600, [this, id = c->id, n] { doc.eng.noteOff(id, n); }); } };
        rebuild();
    }
    ~InstView() override { doc.removeChangeListener(this); }
    void resized() override { cv.setBounds(getLocalBounds()); }

private:
    Doc& doc; CardView cv; bool hasHead = false; std::string shown;
    struct Dot : juce::Component { juce::Colour c; void paint(juce::Graphics& g) override { g.setColour(c); g.fillEllipse(getLocalBounds().toFloat()); } } dot;
    struct Tag : Cap { void paint(juce::Graphics& g) override { g.setColour(BG3); g.fillRoundedRectangle(getLocalBounds().toFloat(), getHeight() / 2.f); Cap::paint(g); } } tag;
    Btn nameB{ "" }, testB{ u8("▶ Test") }; Cap busL{ "Bus" }; juce::ComboBox busBox; Knob vol{ "Vol", 0, 1.25, 0.8 }, pan{ "Pan", -1, 1, 0 };

    us::Channel* ch() { return doc.chan(); }
    us::SynthP* sp() { auto* c = ch(); return c && c->synth ? &*c->synth : nullptr; }
    static juce::String pct(double v) { return juce::String(juce::roundToInt(v * 100)) + "%"; }
    static juce::String secs(double v) { return v < 1 ? juce::String(juce::roundToInt(v * 1000)) + "ms" : juce::String(v, 2) + "s"; }
    static juce::String hz(double v) { return v >= 1000 ? juce::String(v / 1000, 1) + "k" : juce::String(juce::roundToInt(v)) + "Hz"; }
    std::function<void()> done() { return [this] { doc.commit(); }; }
    template <class F> std::function<void(double)> live(F set) { return [this, set](double v) { set(v); doc.live(); }; }
    using Opts = std::vector<std::pair<juce::String, juce::String>>;
    static Opts waves() { Opts o = { { "sine", "Sine" }, { "triangle", "Triangle" }, { "sawtooth", "Saw" }, { "square", "Square" }, { "pulse25", "Pulse 25%" }, { "pulse10", "Pulse 10%" }, { "softsaw", "Soft saw" }, { "organ", "Organ" }, { "bell", "Bell" }, { "vocal", "Vocal" }, { "metal", "Metal" }, { "stair", "Stair" } };
        for (auto& w : us::waveSets()) o.push_back({ w.id, juce::String("Table: ") + w.label }); return o; }   // a table: Morph scans its frames

    void header() {
        auto* c = ch(); hasHead = c != nullptr; for (juce::Component* x : std::initializer_list<juce::Component*>{ &dot, &nameB, &tag, &busL, &busBox, &vol, &pan, &testB }) x->setVisible(hasHead); if (!c) return;
        dot.c = hex(c->color); nameB.setButtonText(juce::String(c->name)); tag.setText(c->type == "synth" ? "Synth" : c->type == "plugin" ? "Plug-in" : "Sampler", juce::dontSendNotification);
        busBox.clear(juce::dontSendNotification); int i = 1; for (auto& b : doc.p.buses) busBox.addItem(doc.busLabel(b.id) + u8(" · ") + juce::String(b.name), i++);
        for (size_t k = 0; k < doc.p.buses.size(); k++) if (doc.p.buses[k].id == c->bus) busBox.setSelectedItemIndex((int)k, juce::dontSendNotification);
        vol.set(c->vol); pan.set(c->pan); vol.setLink("ch:" + c->id + ":vol"); pan.setLink("ch:" + c->id + ":pan");
    }
    void rebuild() {
        auto& G = cv.grid; G.clear(); auto* c = ch(); shown = c ? c->id + c->type : ""; header();
        if (!c) { G.card("Instrument"); G.note(u8("No channel selected. Add one in the Rack with “+ Channel”.")); }
        else if (c->type == "plugin") plugUI();
        else if (c->type == "sample") samplerUI();
        else if (sp()) synthUI();
        if (c) arpUI();   // every instrument type
        cv.resized();
    }
    void plugUI() {
        auto& G = cv.grid; auto* c = ch(); const std::string id = c->id; G.minCol = 600;
        G.card("Plug-in instrument");
        auto* pp = new PlugPanel(doc, "ch:" + id, "plug:" + id + ":");
        pp->slot = [this, id] { for (auto& x : doc.p.channels) if (x.id == id) return x.plug; return juce::var(); };
        pp->put = [this, id](const juce::Identifier& k, const juce::var& v) { for (auto& x : doc.p.channels) if (x.id == id) Doc::editablePlug(x).getDynamicObject()->setProperty(k, v); };
        pp->live = [this] { doc.live(); }; pp->commit = [this] { doc.commit(); };
        pp->change = [this, id, pp] { choosePlugin(pp, true, [this, id](const juce::var& s) { for (auto& x : doc.p.channels) if (x.id == id) { x.plug = s; x.name = s.getProperty("name", "Plug-in").toString().substring(0, 18).toStdString(); } doc.commit(); }); };
        G.full(pp, 400);
        cv.grid.onLayout = [this, pp] { const int h = pp->idealHeight(); if (std::abs(h - pp->getHeight()) > 2) { for (auto& cd : cv.grid.cards) for (auto& e : cd.e) if (e.c == pp) e.h = h; juce::MessageManager::callAsync([sp = juce::Component::SafePointer<InstView>(this)] { if (sp) sp->cv.relayout(); }); } };
    }
    void samplerUI() {
        auto& G = cv.grid; G.minCol = 290; auto* c = ch(); const std::string id = c->id;
        G.card("Sound source");
        auto* w = new Wave(); if (auto s = doc.bank.get(c->sample)) if (!s->ch.empty()) w->data = s->ch[0]; w->col = hex(c->color); w->marker = (float)c->start; G.full(w, 64);
        Opts opts; bool found = false; for (auto& s : doc.bank.kit()) { opts.push_back({ s->id, juce::String(s->name) }); found |= s->id == c->sample; } for (auto& s : doc.bank.imported()) { opts.push_back({ s->id, u8("★ ") + juce::String(s->name) }); found |= s->id == c->sample; }
        if (!found) opts.insert(opts.begin(), { juce::String(c->sample), "(missing sample)" });
        auto* p = new Pick({}, opts, juce::String(c->sample)); p->onPick = [this](const juce::String& v) { if (auto* x = ch()) { x->sample = v.toStdString(); doc.commit(); } }; G.add(p, 170, Pick::height());
        G.button(u8("Upload audio…"), [this] { upload(); }, true);
        G.button(u8("Detect pitch → root"), [this] { detect(); })->setTooltip("Analyse the sample so it plays in tune across the keyboard");
        G.note(u8("Any audio you upload becomes this channel’s instrument: it is re-pitched across the piano roll from its root note."));
        G.card("Pitch & shape");
        auto C = [this]() -> us::Channel& { return *ch(); };
        G.knob("Root", 0, 127, 60, c->root, live([C](double v) { C().root = (int)std::round(v); }), done(), false, [](double v) { return juce::MidiMessage::getMidiNoteName((int)std::round(v), true, true, 4); }, 1);
        G.knob("Tune", -24, 24, 0, c->tune, live([C](double v) { C().tune = std::round(v); }), done(), false, [](double v) { return juce::String(juce::roundToInt(v)) + "st"; }, 1);
        G.knob("Fine", -100, 100, 0, c->fine, live([C](double v) { C().fine = std::round(v); }), done(), false, [](double v) { return juce::String(juce::roundToInt(v)) + "c"; }, 1);
        G.knob("Start", 0, 0.95, 0, c->start, live([C](double v) { C().start = v; }), done(), false, pct);
        G.knob("Attack", 0.001, 2, 0.001, std::max(0.001, c->att), live([C](double v) { C().att = v; }), done(), true, secs);
        G.knob("Release", 0.005, 4, 0.08, c->rel, live([C](double v) { C().rel = v; }), done(), true, secs);
        G.br();
        G.toggle("Hold", c->gate, [this](bool b) { if (auto* x = ch()) { x->gate = b; doc.commit(); } }, "Stop the sample when the note ends (for instruments). Off = one-shot (for drums)");
        G.toggle("Reverse", c->rev, [this](bool b) { if (auto* x = ch()) { x->rev = b; doc.commit(); } }, "Play backwards");
        G.toggle("Cut self", c->cut, [this](bool b) { if (auto* x = ch()) { x->cut = b; doc.commit(); } }, "Each new note cuts the previous one");
        G.pick("Choke group", { { "0", "None" }, { "1", "1" }, { "2", "2" }, { "3", "3" }, { "4", "4" } }, juce::String(c->choke), [this](const juce::String& v) { if (auto* x = ch()) { x->choke = v.getIntValue(); doc.commit(); } }, 90);
        G.card("Granular", c->gran.on, [this](bool b) { if (auto* x = ch()) { x->gran.on = b; doc.commit(); } });
        auto Gr = [this]() -> us::GranP& { return ch()->gran; };
        G.knob("Position", 0, 1, 0.3, c->gran.pos, live([Gr](double v) { Gr().pos = v; }), done(), false, pct);
        G.knob("Scan", -1, 1, 0, c->gran.scan, live([Gr](double v) { Gr().scan = v; }), done(), false, [](double v) { return juce::String(v, 2) + "/s"; });
        G.knob("Grain size", 0.01, 0.5, 0.08, c->gran.size, live([Gr](double v) { Gr().size = v; }), done(), true, secs);
        G.knob("Density", 1, 120, 24, c->gran.density, live([Gr](double v) { Gr().density = v; }), done(), true, [](double v) { return juce::String(juce::roundToInt(v)) + "/s"; });
        G.knob("Jitter", 0, 0.5, 0.03, c->gran.jitter, live([Gr](double v) { Gr().jitter = v; }), done(), false, pct);
        G.knob("Pitch spray", 0, 12, 0, c->gran.spray, live([Gr](double v) { Gr().spray = v; }), done(), false, [](double v) { return juce::String(v, 1) + "st"; });
        G.knob("Spread", 0, 1, 0.6, c->gran.spread, live([Gr](double v) { Gr().spread = v; }), done(), false, pct);
        G.knob("Reverse", 0, 1, 0, c->gran.rev, live([Gr](double v) { Gr().rev = v; }), done(), false, pct);
        G.note("Plays the sample as a cloud of short grains: hold a note for a texture. Scan moves through the sample by itself; Jitter and Pitch spray scatter the grains.");
        G.card("Slicer", c->slice, [this](bool b) { if (auto* x = ch()) { x->slice = b; if (b && x->slices.empty()) sliceHits(); doc.commit(); } });
        G.knob("Sensitivity", 0, 1, 0.5, sliceSens, [this](double v) { sliceSens = v; }, [this] {}, false, pct);
        G.button("Find hits", [this] { sliceHits(); if (auto* x = ch()) { x->slice = true; doc.commit(); } })->setTooltip("Cut the sound where its hits start");
        G.button("Cut in 8", [this] { sliceEven(8); }); G.button("Cut in 16", [this] { sliceEven(16); });
        G.button("Write pattern", [this] { slicePattern(); })->setTooltip("Fill the current pattern with the slices in their original places, so the loop plays back and can be rearranged");
        G.note(c->slices.empty() ? juce::String("Cuts a loop into slices, one per key from the root note up: rearrange the notes to remix the loop. Tune changes the speed of every slice.")
                                 : juce::String((int)c->slices.size()) + " slices on keys " + juce::MidiMessage::getMidiNoteName(c->root, true, true, 4) + " to " + juce::MidiMessage::getMidiNoteName(c->root + (int)c->slices.size() - 1, true, true, 4) + ".");
        G.card("Zones (multi-sample)");
        G.button("+ Zone", [this] { if (auto* x = ch()) { us::Zone z; z.sample = x->sample; z.root = x->root; x->zones.push_back(z); doc.commit(); } });
        G.button(u8("Auto-map imported…"), [this] { autoMap(); })->setTooltip("Spread every imported sample across the keyboard by its pitch (from its name, such as \"Piano C4\", or by listening)");
        if (c->zones.empty()) G.note("No zones: the channel plays the sound source above. Add zones to give different notes or velocities their own samples; zones that overlap take turns (round robin).");
        auto nn = [](double v) { return juce::MidiMessage::getMidiNoteName((int)std::round(v), true, true, 4); };
        for (size_t zi = 0; zi < c->zones.size(); zi++) {
            auto Z = [this, zi]() -> us::Zone& { return ch()->zones[zi]; };
            G.br();
            auto* zp = new Pick({}, opts, juce::String(c->zones[zi].sample)); zp->onPick = [this, zi](const juce::String& v) { if (auto* x = ch()) if (zi < x->zones.size()) { x->zones[zi].sample = v.toStdString(); doc.commit(); } }; G.add(zp, 150, Pick::height());
            G.knob("Low", 0, 127, 0, c->zones[zi].lo, live([Z](double v) { Z().lo = (int)std::round(v); }), done(), false, nn, 1);
            G.knob("High", 0, 127, 127, c->zones[zi].hi, live([Z](double v) { Z().hi = (int)std::round(v); }), done(), false, nn, 1);
            G.knob("Root", 0, 127, 60, c->zones[zi].root, live([Z](double v) { Z().root = (int)std::round(v); }), done(), false, nn, 1);
            G.knob("Vel low", 0, 127, 0, c->zones[zi].vlo, live([Z](double v) { Z().vlo = (int)std::round(v); }), done(), false, {}, 1);
            G.knob("Vel high", 0, 127, 127, c->zones[zi].vhi, live([Z](double v) { Z().vhi = (int)std::round(v); }), done(), false, {}, 1);
            G.knob("Gain", -24, 12, 0, c->zones[zi].gain, live([Z](double v) { Z().gain = v; }), done(), false, [](double v) { return juce::String(v, 1) + " dB"; });
            G.button(u8("✕"), [this, zi] { if (auto* x = ch()) if (zi < x->zones.size()) { x->zones.erase(x->zones.begin() + (long)zi); doc.commit(); } })->setTooltip("Remove this zone");
        }
    }
    // a note name inside a sample's name ("Piano C#4", "bass_A1"), or -1
    static int noteInName(const std::string& s) {
        static const char* L = "CDEFGAB"; static const int pc[] = { 0, 2, 4, 5, 7, 9, 11 };
        for (size_t i = 0; i < s.size(); i++) {
            const char* p = std::strchr(L, std::toupper((unsigned char)s[i])); if (!p || !*p) continue;
            if (i > 0 && std::isalpha((unsigned char)s[i - 1])) continue;
            size_t j = i + 1; int n = pc[p - L]; if (j < s.size() && (s[j] == '#' || s[j] == 's')) { n++; j++; } else if (j < s.size() && s[j] == 'b') { n--; j++; }
            bool neg = false; if (j < s.size() && s[j] == '-') { neg = true; j++; }
            if (j < s.size() && std::isdigit((unsigned char)s[j])) { const int oct = (s[j] - '0') * (neg ? -1 : 1); if (j + 1 < s.size() && std::isalnum((unsigned char)s[j + 1])) continue; return juce::jlimit(0, 127, (oct + 1) * 12 + n); }
        }
        return -1;
    }
    double sliceSens = 0.5;
    void sliceHits() { auto* c = ch(); if (!c) return; auto smp = doc.bank.get(c->sample); if (!smp || smp->ch.empty()) return; c->slices = us::detectOnsets(us::toMono(*smp), smp->sr, sliceSens); toast(juce::String((int)c->slices.size()) + " slices"); }
    void sliceEven(int n) { auto* c = ch(); if (!c) return; auto smp = doc.bank.get(c->sample); if (!smp) return; c->slices.clear(); for (int k = 0; k < n; k++) c->slices.push_back(smp->seconds() * k / n); c->slice = true; doc.commit(); }
    void slicePattern() {
        auto* c = ch(); if (!c || c->slices.empty()) { toast("Find the hits first"); return; } auto smp = doc.bank.get(c->sample); if (!smp) return;
        us::Pattern* pt = nullptr; for (auto& x : doc.p.patterns) if (x.id == doc.pat) pt = &x; if (!pt) return;
        const double spt = doc.p.secondsPerTick() * std::pow(2.0, c->tune / 12.0); auto& ns = pt->notes[c->id]; ns.clear();
        for (size_t k = 0; k < c->slices.size(); k++) { const double a0 = c->slices[k], a1 = k + 1 < c->slices.size() ? c->slices[k + 1] : smp->seconds();
            us::Note n; n.id = us::newId(); n.t = std::round(a0 / spt); n.d = std::max(1.0, std::round((a1 - a0) / spt)); n.n = c->root + (int)k; n.v = 0.8; ns.push_back(n); }
        const double endT = smp->seconds() / spt; pt->bars = std::max(pt->bars, std::ceil(endT / doc.p.bar())); c->slice = true; doc.commit();
        toast("Wrote " + juce::String((int)c->slices.size()) + " slices into " + juce::String(pt->name));
    }
    void autoMap() {
        auto* c = ch(); if (!c) return; std::vector<std::pair<int, std::string>> roots;
        for (auto& s : doc.bank.imported()) {
            int r = noteInName(s->name);
            if (r < 0 && !s->ch.empty()) { auto mono = us::toMono(*s); std::vector<double> hits; const size_t W = (size_t)(s->sr * 0.06);
                for (size_t off = 0; off + W < mono.size() && hits.size() < 30; off += W / 2) { std::vector<float> seg(mono.begin() + (long)off, mono.begin() + (long)(off + W)); const double m = us::pitchOf(seg, s->sr); if (m > 0) hits.push_back(m); }
                if (hits.size() >= 2) { std::sort(hits.begin(), hits.end()); r = (int)std::round(hits[hits.size() / 2]); } }
            if (r >= 0) roots.push_back({ r, s->id });
        }
        if (roots.empty()) { toast("No imported samples with a pitch to map (upload some, or name them like \"Piano C4\")"); return; }
        std::sort(roots.begin(), roots.end()); c->zones.clear();
        for (size_t i = 0; i < roots.size(); i++) { us::Zone z; z.sample = roots[i].second; z.root = roots[i].first;
            z.lo = i == 0 ? 0 : (roots[i - 1].first + roots[i].first) / 2 + 1; z.hi = i + 1 == roots.size() ? 127 : (roots[i].first + roots[i + 1].first) / 2; c->zones.push_back(z); }
        doc.commit(); toast("Mapped " + juce::String((int)roots.size()) + " samples across the keyboard");
    }
    void arpUI() {
        auto& G = cv.grid; auto* c = ch(); if (!c) return;
        G.card("Arpeggiator", c->arp.on, [this](bool b) { if (auto* x = ch()) { x->arp.on = b; doc.commit(); } });
        auto A = [this]() -> us::ArpP& { return ch()->arp; };
        G.pick("Order", { { "up", "Up" }, { "down", "Down" }, { "updown", "Up and down" }, { "played", "As played" }, { "random", "Random" } }, juce::String(c->arp.mode), [this, A](const juce::String& v) { A().mode = v.toStdString(); doc.commit(); }, 110);
        G.pick("Rate", { { "1/32", "1/32" }, { "1/16", "1/16" }, { "1/16 triplet", "1/16 triplet" }, { "1/8", "1/8" }, { "1/8 triplet", "1/8 triplet" }, { "1/8 dotted", "1/8 dotted" }, { "1/4", "1/4" } }, juce::String(c->arp.rate), [this, A](const juce::String& v) { A().rate = v.toStdString(); doc.commit(); }, 100);
        G.knob("Octaves", 1, 4, 1, c->arp.oct, live([A](double v) { A().oct = (int)std::round(v); }), done(), false, {}, 1);
        G.knob("Gate", 0.05, 1, 0.6, c->arp.gate, live([A](double v) { A().gate = v; }), done(), false, pct);
        G.knob("Steps", 1, 16, 16, c->arp.len, live([A](double v) { A().len = (int)std::round(v); }), done(), false, {}, 1);
        G.br();
        for (int k = 0; k < 16; k++) G.toggle(juce::String(k + 1), (c->arp.steps >> k) & 1, [this, k](bool b) { if (auto* x = ch()) { x->arp.steps = b ? (x->arp.steps | (1 << k)) : (x->arp.steps & ~(1 << k)); doc.commit(); } }, "Step " + juce::String(k + 1) + ": on plays, off rests");
        G.note("Hold a chord (or draw one in the Piano Roll) and the arpeggiator plays its notes one at a time, locked to the song. The numbered steps make a rhythm: switched-off steps rest.");
    }
    std::unique_ptr<juce::FileChooser> chooser;
    void upload() {
        chooser = std::make_unique<juce::FileChooser>("Upload audio", juce::File::getSpecialLocation(juce::File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) { if (!fc.getResult().existsAsFile()) return; auto id = doc.importAudio(fc.getResult()); if (id.empty()) { toast("Could not decode that file"); return; } if (auto* x = ch()) { x->sample = id; doc.commit(); } });
    }
    void detect() {   // the median pitch of the sample's voiced frames -> root note and fine tune
        auto* c = ch(); if (!c) return; auto s = doc.bank.get(c->sample); if (!s || s->ch.empty()) return;
        auto mono = us::toMono(*s); std::vector<double> hits; const size_t W = (size_t)(s->sr * 0.06);
        for (size_t off = 0; off + W < mono.size() && hits.size() < 60; off += W / 2) { std::vector<float> seg(mono.begin() + (long)off, mono.begin() + (long)(off + W)); const double m = us::pitchOf(seg, s->sr); if (m > 0) hits.push_back(m); }
        if (hits.size() < 2) { toast("No clear pitch found in this sample"); return; }
        std::sort(hits.begin(), hits.end()); const double m = hits[hits.size() / 2]; c->root = (int)std::round(m); c->fine = std::round((c->root - m) * 100); doc.commit();
        toast("Root set to " + juce::MidiMessage::getMidiNoteName(c->root, true, true, 4) + " (" + (c->fine >= 0 ? "+" : "") + juce::String((int)c->fine) + " cents)");
    }
    void synthUI() {
        auto& G = cv.grid; G.minCol = 290; auto S = [this]() -> us::SynthP& { return *sp(); }; auto& s = S();
        G.card("Preset");
        Opts po = { { "", u8("— choose —") } }; for (auto& [n, f] : synthPresets()) po.push_back({ n, n });
        auto* p = new Pick({}, po, ""); p->onPick = [this](const juce::String& name) { for (auto& [n, f] : synthPresets()) if (name == n) { us::SynthP x = defSynth(); f(x); *sp() = x; doc.commit(); toast("Loaded " + name); } }; G.add(p, 170, Pick::height());
        G.note("Changes apply to the next note; filter cutoff, resonance and macros respond while a note is held.");
        G.card("Macros");
        for (int i = 0; i < 4; i++) G.knob("Macro " + juce::String(i + 1), 0, 1, 0, s.macro[i], live([S, i](double v) { S().macro[i] = v; }), done(), false, pct);
        G.note(u8("Four performance knobs. Pick “Macro 1-4” as a source in the matrix and one knob can sweep several things at once."));
        for (int i = 0; i < 3; i++) {
            G.card(juce::String("Oscillator ") + "ABC"[i], s.osc[i].on, [this, S, i](bool b) { S().osc[i].on = b; doc.commit(); });
            auto* g = G.gfx([S, i](juce::Graphics& gr, juce::Rectangle<float> r) {
                const auto& O = S().osc[i]; const float m = (float)O.morph;
                if (us::isWaveSet(O.wave)) {   // the frame at the current position, from the engine's own tables
                    const double x = m * (us::WT_FRAMES - 1); const int k0 = std::min(us::WT_FRAMES - 2, (int)x); const double fx = x - k0;
                    const auto& A = us::WaveTable::get(us::frameName(O.wave, k0)); const auto& B = us::WaveTable::get(us::frameName(O.wave, k0 + 1));
                    std::vector<float> a(96); for (size_t k = 0; k < a.size(); k++) { const double p = (double)k / a.size(); a[k] = (float)(A.read(p, 20, 48000) * (1 - fx) + B.read(p, 20, 48000) * fx); }
                    Gfx::plot(gr, r, a); return;
                }
                auto a = waveShape(O.wave), b = waveShape(O.wave2); for (size_t k = 0; k < a.size(); k++) a[k] = a[k] * (1 - m) + b[k] * m; Gfx::plot(gr, r, a); });
            G.pick("Wave", waves(), juce::String(s.osc[i].wave), [this, S, i, g](const juce::String& v) { S().osc[i].wave = v.toStdString(); g->repaint(); doc.commit(); }, 96);
            G.pick("Morph to", waves(), juce::String(s.osc[i].wave2), [this, S, i, g](const juce::String& v) { S().osc[i].wave2 = v.toStdString(); g->repaint(); doc.commit(); }, 96);
            G.knob(us::isWaveSet(s.osc[i].wave) ? "Position" : "Morph", 0, 1, 0, s.osc[i].morph, [this, S, i, g](double v) { S().osc[i].morph = v; g->repaint(); doc.live(); }, done(), false, pct);
            G.knob("Octave", -3, 3, 0, s.osc[i].oct, live([S, i](double v) { S().osc[i].oct = std::round(v); }), done(), false, {}, 1);
            G.knob("Semi", -12, 12, 0, s.osc[i].semi, live([S, i](double v) { S().osc[i].semi = std::round(v); }), done(), false, {}, 1);
            G.knob("Fine", -100, 100, 0, s.osc[i].fine, live([S, i](double v) { S().osc[i].fine = std::round(v); }), done(), false, [](double v) { return juce::String(juce::roundToInt(v)) + "c"; }, 1);
            G.knob("Level", 0, 1, 0.8, s.osc[i].level, live([S, i](double v) { S().osc[i].level = v; }), done(), false, pct);
            G.knob("Pan", -1, 1, 0, s.osc[i].pan, live([S, i](double v) { S().osc[i].pan = v; }), done());
        }
        G.card("Blend & unison");
        G.knob("Voices", 1, 9, 1, s.uni, live([S](double v) { S().uni = (int)std::round(v); }), done(), false, {}, 1);
        G.knob("Detune", 0, 60, 14, s.detune, live([S](double v) { S().detune = v; }), done(), false, [](double v) { return juce::String(juce::roundToInt(v)) + "c"; });
        G.knob("Stereo", 0, 1, 0.5, s.spread, live([S](double v) { S().spread = v; }), done(), false, pct);
        G.knob("Drift", 0, 30, 0, s.drift, live([S](double v) { S().drift = v; }), done(), false, [](double v) { return juce::String(juce::roundToInt(v)) + "c"; });
        G.knob(u8("FM B→A"), 0, 1, 0, s.fm, live([S](double v) { S().fm = v; }), done(), false, pct);
        G.knob(u8("Ring A×B"), 0, 1, 0, s.ring, live([S](double v) { S().ring = v; }), done(), false, pct);
        G.card("Sub & noise");
        G.knob("Sub", 0, 1, 0, s.sub, live([S](double v) { S().sub = v; }), done(), false, pct);
        G.pick("Sub wave", { { "sine", "Sine" }, { "triangle", "Triangle" }, { "sawtooth", "Saw" }, { "square", "Square" } }, juce::String(s.subWave), [this, S](const juce::String& v) { S().subWave = v.toStdString(); doc.commit(); }, 90);
        G.pick("Sub octave", { { "-1", u8("−1") }, { "-2", u8("−2") } }, juce::String((int)s.subOct), [this, S](const juce::String& v) { S().subOct = v.getIntValue(); doc.commit(); }, 70);
        G.knob("Noise", 0, 1, 0, s.noise, live([S](double v) { S().noise = v; }), done(), false, pct);
        G.knob("Noise tone", 0, 1, 1, s.noiseTone, live([S](double v) { S().noiseTone = v; }), done(), false, [](double v) { return juce::String(v > 0.97 ? "white" : v > 0.6 ? "pink-ish" : "dark"); });
        G.card("Physical model");
        G.pick("Model", { { "off", "Off" }, { "pluck", "Plucked string" }, { "mallet", "Struck (mallet)" } }, juce::String(s.pm.type), [this, S](const juce::String& v) { S().pm.type = v.toStdString(); doc.commit(); }, 130);
        G.pick("Material", { { "bar", "Metal bar" }, { "glass", "Glass" }, { "wood", "Wood" }, { "bell", "Bell" } }, juce::String(s.pm.mat), [this, S](const juce::String& v) { S().pm.mat = v.toStdString(); doc.commit(); }, 110);
        G.knob("Decay", 0.05, 12, 2, s.pm.decay, live([S](double v) { S().pm.decay = v; }), done(), true, secs);
        G.knob("Bright", 0, 1, 0.6, s.pm.bright, live([S](double v) { S().pm.bright = v; }), done(), false, pct);
        G.knob("Pluck pos", 0.02, 0.5, 0.2, s.pm.pos, live([S](double v) { S().pm.pos = v; }), done(), false, pct);
        G.knob("Hardness", 0, 1, 0.5, s.pm.hard, live([S](double v) { S().pm.hard = v; }), done(), false, pct);
        G.knob("Level", 0, 1, 0.8, s.pm.level, live([S](double v) { S().pm.level = v; }), done(), false, pct);
        G.note("A plucked string (Material is for the mallet) or a struck bar, glass, wood block or bell, played beside the oscillators and through the filters. Turn the oscillators off to hear it alone.");
        const Opts ftypes = { { "lowpass", "Low-pass 12" }, { "lowpass24", "Low-pass 24" }, { "highpass", "High-pass 12" }, { "highpass24", "High-pass 24" }, { "bandpass", "Band-pass" }, { "notch", "Notch" }, { "peaking", "Peak" }, { "allpass", "Phaser (all-pass)" } };
        for (int k = 0; k < 2; k++) {
            auto F = [S, k]() -> us::FilterP& { return k ? S().f2 : S().f1; };
            G.card(k ? "Filter 2" : "Filter 1", F().on, [this, F](bool b) { F().on = b; doc.commit(); });
            auto* g = G.gfx([F](juce::Graphics& gr, juce::Rectangle<float> r) { Gfx::plot(gr, r, filterCurve(F().type, F().cutoff, F().q)); });
            G.pick("Type", ftypes, juce::String(F().type), [this, F, g](const juce::String& v) { F().type = v.toStdString(); g->repaint(); doc.commit(); }, 120);
            G.knob("Cutoff", 30, 18000, 6000, F().cutoff, [this, F, g](double v) { F().cutoff = v; g->repaint(); doc.live(); }, done(), true, hz);
            G.knob("Reso", 0.1, 24, 1, F().q, [this, F, g](double v) { F().q = v; g->repaint(); doc.live(); }, done(), true);
            G.knob("Env amt", -72, 72, 0, F().env, live([F](double v) { F().env = v; }), done(), false, [](double v) { return juce::String(juce::roundToInt(v)) + "st"; });
            G.knob("Key trk", 0, 1, 0, F().key, live([F](double v) { F().key = v; }), done(), false, pct);
            if (!k) G.knob("Drive", 0, 1, 0, F().drive, live([F](double v) { F().drive = v; }), done(), false, pct);
            else G.pick("Routing", { { "serial", "After filter 1" }, { "parallel", "Beside filter 1" } }, juce::String(s.froute), [this, S](const juce::String& v) { S().froute = v.toStdString(); doc.commit(); }, 120);
        }
        G.card("Shaper (after filters)");
        G.pick("Type", { { "off", "Off" }, { "soft", "Soft clip" }, { "hard", "Hard clip" }, { "fold", "Wavefolder" }, { "asym", "Asymmetric" }, { "crush", "Bit crush" } }, juce::String(s.shaperType), [this, S](const juce::String& v) { S().shaperType = v.toStdString(); doc.commit(); }, 120);
        G.knob("Amount", 0, 1, 0.3, s.shaperAmt, live([S](double v) { S().shaperAmt = v; }), done(), false, pct);
        const char* envNames[] = { "Amp envelope", "Filter envelope", "Mod envelope 1", "Mod envelope 2" };
        for (int k = 0; k < 4; k++) {
            auto E = [S, k]() -> us::EnvP& { auto& x = S(); return k == 0 ? x.aenv : k == 1 ? x.fenv : k == 2 ? x.menv : x.menv2; };
            G.card(envNames[k]);
            auto* g = G.gfx([E](juce::Graphics& gr, juce::Rectangle<float> r) { const auto& e = E(); const float sx = r.getWidth() / 130.f, sy = r.getHeight() / 42.f;
                const float a = (float)std::min(1.0, e.a / 2) * 30 + 2, d = (float)std::min(1.0, e.d / 2) * 30 + 2, rl = (float)std::min(1.0, e.r / 3) * 30 + 2, y = 38 - (float)e.s * 34;
                juce::Path p; p.startNewSubPath(r.getX(), r.getY() + 40 * sy); p.lineTo(r.getX() + a * sx, r.getY() + 4 * sy); p.lineTo(r.getX() + (a + d) * sx, r.getY() + y * sy); p.lineTo(r.getX() + (a + d + 26) * sx, r.getY() + y * sy); p.lineTo(r.getX() + (a + d + 26 + rl) * sx, r.getY() + 40 * sy);
                gr.setColour(ACCENT); gr.strokePath(p, juce::PathStrokeType(2, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)); });
            auto lv = [this, g](std::function<void(double)> f) { return [this, g, f](double v) { f(v); g->repaint(); doc.live(); }; };
            G.knob("Attack", 0.001, 8, 0.005, E().a, lv([E](double v) { E().a = v; }), done(), true, secs);
            G.knob("Decay", 0.001, 8, 0.25, E().d, lv([E](double v) { E().d = v; }), done(), true, secs);
            G.knob("Sustain", 0, 1, 0.7, E().s, lv([E](double v) { E().s = v; }), done(), false, pct);
            G.knob("Release", 0.001, 8, 0.2, E().r, lv([E](double v) { E().r = v; }), done(), true, secs);
        }
        const Opts lfoW = { { "sine", "Sine" }, { "triangle", "Triangle" }, { "sawtooth", "Ramp up" }, { "sawdown", "Ramp down" }, { "square", "Square" }, { "sh", "Sample & hold" }, { "rand", "Smooth random" } };
        const Opts syncs = { { "0", "Free (Hz)" }, { "16", "4 bars" }, { "8", "2 bars" }, { "4", "1 bar" }, { "2", "1/2" }, { "1", "1/4" }, { "0.5", "1/8" }, { "0.333333", "1/8 triplet" }, { "0.25", "1/16" } };
        for (int k = 0; k < 3; k++) {
            G.card("LFO " + juce::String(k + 1));
            auto* g = G.gfx([S, k](juce::Graphics& gr, juce::Rectangle<float> r) { const auto& w = S().lfo[k].wave; std::vector<float> pts(96); float rr[10]; for (int j = 0; j < 10; j++) rr[j] = (float)std::sin(j * 12.9898 + k) * 0.9f;
                for (int j = 0; j < 96; j++) { const double p = std::fmod(j / 96.0 * 2, 1.0), q = j / 96.0 * 8; const int qi = (int)q;
                    pts[(size_t)j] = (float)(w == "sine" ? std::sin(juce::MathConstants<double>::twoPi * p) : w == "triangle" ? 1 - 4 * std::abs(std::fmod(p + 0.25, 1.0) - 0.5) : w == "sawtooth" ? p * 2 - 1 : w == "sawdown" ? 1 - p * 2 : w == "square" ? (p < 0.5 ? 1 : -1) : w == "sh" ? rr[qi] : rr[qi] + (rr[qi + 1] - rr[qi]) * (q - qi)); }
                Gfx::plot(gr, r, pts); });
            G.pick("Shape", lfoW, juce::String(s.lfo[k].wave), [this, S, k, g](const juce::String& v) { S().lfo[k].wave = v.toStdString(); g->repaint(); doc.commit(); }, 120);
            juce::String cur = "0"; for (auto& [v, l] : syncs) if (std::abs(v.getDoubleValue() - s.lfo[k].sync) < 1e-3) cur = v;
            G.pick("Tempo sync", syncs, cur, [this, S, k](const juce::String& v) { S().lfo[k].sync = v.getDoubleValue(); doc.commit(); }, 110);
            G.knob("Rate", 0.02, 40, 5, s.lfo[k].rate, live([S, k](double v) { S().lfo[k].rate = v; }), done(), true, [](double v) { return juce::String(v, 2) + "Hz"; });
            G.knob("Fade in", 0, 4, 0, s.lfo[k].fade, live([S, k](double v) { S().lfo[k].fade = v; }), done(), false, secs);
        }
        G.card("Modulation matrix");
        const Opts srcs = { { "none", u8("—") }, { "lfo1", "LFO 1" }, { "lfo2", "LFO 2" }, { "lfo3", "LFO 3" }, { "menv", "Mod env 1" }, { "menv2", "Mod env 2" }, { "fenv", "Filter env" }, { "vel", "Velocity" }, { "key", "Key track" }, { "wheel", "Mod wheel" }, { "rand", "Random per note" }, { "macro1", "Macro 1" }, { "macro2", "Macro 2" }, { "macro3", "Macro 3" }, { "macro4", "Macro 4" } };
        const Opts dsts = { { "none", u8("—") }, { "pitch", "Pitch" }, { "cutoff", "Filter 1 cutoff" }, { "res", "Filter 1 reso" }, { "cutoff2", "Filter 2 cutoff" }, { "amp", "Amplitude" }, { "pan", "Pan" }, { "fm", "FM amount" }, { "ring", "Ring mod" }, { "morph", "Wave morph" }, { "oscA", "Osc A level" }, { "oscB", "Osc B level" }, { "oscC", "Osc C level" }, { "sub", "Sub level" }, { "noise", "Noise level" }, { "lfo1rate", "LFO 1 rate" }, { "lfo2rate", "LFO 2 rate" }, { "lfo3rate", "LFO 3 rate" } };
        if (s.mod.size() < 8) s.mod.resize(8);
        for (size_t k = 0; k < 8; k++) {
            struct MRow : juce::Component { juce::ComboBox a, b; Cap arrow{ u8("→"), FS }; std::unique_ptr<Knob> amt; void resized() override { auto r = getLocalBounds(); const int kw = amt->idealWidth(); amt->setBounds(r.removeFromRight(kw)); r.removeFromRight(6); const int w = (r.getWidth() - 20) / 2; a.setBounds(r.removeFromLeft(w).withSizeKeepingCentre(w, CTL())); arrow.setBounds(r.removeFromLeft(20)); arrow.setJustificationType(juce::Justification::centred); b.setBounds(r.withSizeKeepingCentre(r.getWidth(), CTL())); } };
            auto* row = new MRow(); row->addAndMakeVisible(row->a); row->addAndMakeVisible(row->b); row->addAndMakeVisible(row->arrow);
            for (size_t i = 0; i < srcs.size(); i++) row->a.addItem(srcs[i].second, (int)i + 1); for (size_t i = 0; i < dsts.size(); i++) row->b.addItem(dsts[i].second, (int)i + 1);
            for (size_t i = 0; i < srcs.size(); i++) if (srcs[i].first == juce::String(s.mod[k].src)) row->a.setSelectedItemIndex((int)i, juce::dontSendNotification);
            for (size_t i = 0; i < dsts.size(); i++) if (dsts[i].first == juce::String(s.mod[k].dst)) row->b.setSelectedItemIndex((int)i, juce::dontSendNotification);
            row->a.onChange = [this, S, k, row, srcs] { const int i = row->a.getSelectedItemIndex(); if (i >= 0) { S().mod[k].src = srcs[(size_t)i].first.toStdString(); doc.commit(); } };
            row->b.onChange = [this, S, k, row, dsts] { const int i = row->b.getSelectedItemIndex(); if (i >= 0) { S().mod[k].dst = dsts[(size_t)i].first.toStdString(); doc.commit(); } };
            row->amt = std::make_unique<Knob>("Amount", -1, 1, 0); row->amt->sized(Knob::Xs); row->amt->showCaption = false; row->amt->fmt = pct; row->amt->set(s.mod[k].amt);
            row->amt->onChange = live([S, k](double v) { S().mod[k].amt = v; }); row->amt->onDone = done(); row->addAndMakeVisible(*row->amt);
            G.full(row, CTL());
        }
        G.note("Route any source to any destination. Stack several rows on one destination; amounts can be negative.");
        G.card("Voice");
        G.pick("Mode", { { "poly", "Poly" }, { "mono", "Mono" } }, juce::String(s.mode), [this, S](const juce::String& v) { S().mode = v.toStdString(); doc.commit(); }, 80);
        G.knob("Max voices", 1, 32, 8, s.voices, live([S](double v) { S().voices = (int)std::round(v); }), done(), false, {}, 1);
        G.knob("Glide", 0.001, 2, 0.001, std::max(0.001, s.glide), live([S](double v) { S().glide = v; }), done(), true, secs);
        G.knob("Vel sens", 0, 1, 0.6, s.vel, live([S](double v) { S().vel = v; }), done(), false, pct);
        G.knob("Gain", 0, 1.5, 0.7, s.gain, live([S](double v) { S().gain = v; }), done(), false, pct);
        G.knob("Bend", 1, 24, 2, s.bendRange, live([S](double v) { S().bendRange = std::round(v); }), done(), false, [](double v) { return u8("±") + juce::String(juce::roundToInt(v)); }, 1);
    }
    // any committed change: rebuild after the current event (never inside a control's own callback), keeping the scroll
    bool pending = false;
    void changeListenerCallback(juce::ChangeBroadcaster*) override {
        if (pending) return; pending = true;
        juce::MessageManager::callAsync([sp = juce::Component::SafePointer<InstView>(this)] { if (!sp) return; sp->pending = false; const auto pos = sp->cv.view.getViewPosition(); sp->rebuild(); sp->cv.view.setViewPosition(pos); });
    }
};

}   // namespace ui
