// Piano roll (app/js/ui/pianoroll.js): note editor with draw / paint / select / slice / delete / mute / zoom / play / pan
// tools, snap, chord and scale stamps, key and scale with "Snap to scale", slide notes, loop mode (drag on the ruler),
// ghost notes of other channels, follow playback, a control lane (velocity, note pan, fine pitch, release, cutoff,
// resonance) and the Tools menu (legato, quantize, chop, glue, arpeggiate, strum, flam, limit, flip, reverse, randomize,
// humanize, scale levels, LFO, chord progressions...). Right-click erases; a long press opens a note menu; double-click
// a note for its velocity. Wheel scrolls, Shift+wheel sideways, Ctrl+wheel zooms, Alt+wheel row height, pinch zooms.
#pragma once
#include <set>
#include "Doc.h"
#include "Keys.h"
#include "Theme.h"
#include "engine/Analysis.h"

namespace ui {

// one dialog with a list per setting (the browser's dlg()): fields are { key, label, options, default index }
struct DlgField { std::string key; juce::String label; std::vector<std::pair<double, juce::String>> opts; int def = 0; };
inline void formDialog(const juce::String& title, std::vector<DlgField> fields, const juce::String& note, const juce::String& go, std::function<void(std::map<std::string, double>)> fn) {
    auto* w = new juce::AlertWindow(title, note, juce::MessageBoxIconType::NoIcon);
    for (auto& f : fields) { juce::StringArray items; for (auto& o : f.opts) items.add(o.second); w->addComboBox(f.key, items, f.label); if (auto* c = w->getComboBoxComponent(f.key)) c->setSelectedItemIndex(f.def, juce::dontSendNotification); }
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey)); w->addButton(go, 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([w, fields, fn](int ok) {
        if (!ok) return; std::map<std::string, double> o;
        for (auto& f : fields) if (auto* c = w->getComboBoxComponent(f.key)) { const int i = std::max(0, c->getSelectedItemIndex()); o[f.key] = f.opts[(size_t)std::min(i, (int)f.opts.size() - 1)].first; }
        fn(o); }), true);
}
inline juce::String noteName(int n) { return juce::String(us::NOTE_NAMES[((n % 12) + 12) % 12]) + juce::String(n / 12 - 1); }

class RollView : public juce::Component, private juce::ChangeListener, private juce::Timer {
public:
    explicit RollView(Doc& d) : doc(d) {
        doc.addChangeListener(this); setWantsKeyboardFocus(false);
        for (juce::Component* c : std::initializer_list<juce::Component*>{ &chanBox, &tools, &snapL, &snapBox, &stampBox, &keyL, &rootBox, &scaleBox, &slideB, &lockB, &loopB, &ghostB, &followB, &toolsB, &zoomOut, &zoomIn, &grid }) addAndMakeVisible(c);
        if (isTouch()) for (auto* b : { &allB, &copyB, &pasteB, &dupB, &delB }) { addAndMakeVisible(b); b->sm = true; }
        chanBox.setTooltip("Channel being edited"); chanBox.onChange = [this] { const int i = chanBox.getSelectedItemIndex(); if (i >= 0 && i < (int)doc.p.channels.size()) doc.selectChannel(doc.p.channels[(size_t)i].id); };
        static const char* tips[] = { "Draw (P): tap to add, drag to move, drag the right edge to resize", "Paint (B): drag along a row to paint notes", "Select (E): drag a box", "Slice (C)", "Delete (D) - or right-click",
            "Mute (T): click or drag over notes to mute / unmute them", "Zoom (Z): click to zoom in, right-click to zoom out", "Play (Y): click to play the pattern from there", "Pan the view" };
        for (int i = 0; i < 9; i++) tools.tip(i, tips[i]);
        tools.onChange = [this](int i) { V().tool = TOOLS[i]; };
        { int k = 1; for (auto& [v, l] : snaps()) snapBox.addItem(l, k++); } snapBox.onChange = [this] { const int i = snapBox.getSelectedItemIndex(); if (i >= 0) V().snap = snaps()[(size_t)i].first; grid.repaint(); };
        stampBox.setTooltip("Stamp: drawing one note adds a whole chord or scale"); stampBox.addItem("Stamp: off", 1); { int k = 2; for (auto& [n, iv] : chords()) stampBox.addItem(u8("Chord · ") + juce::String(n), k++); for (auto& [n, iv] : stampScales()) stampBox.addItem(u8("Scale · ") + juce::String(n), k++); }
        stampBox.onChange = [this] { const int i = stampBox.getSelectedItemIndex(); V().stamp = i <= 0 ? std::string() : i - 1 < (int)chords().size() ? chords()[(size_t)i - 1].first : stampScales()[(size_t)(i - 1 - (int)chords().size())].first; };
        for (int i = 0; i < 12; i++) rootBox.addItem(us::NOTE_NAMES[i], i + 1); rootBox.onChange = [this] { const int r = rootBox.getSelectedId() - 1; if (r >= 0 && r != doc.p.keyRoot) { doc.p.keyRoot = r; doc.commit(); } };
        { int k = 1; for (auto& [n, iv] : us::scales()) scaleBox.addItem(juce::String(n), k++); } scaleBox.setTooltip("Scale");
        scaleBox.onChange = [this] { const auto s = scaleBox.getText().toStdString(); if (!s.empty() && s != doc.p.keyScale) { doc.p.keyScale = s; doc.commit(); } };
        slideB.sm = true; slideB.setTooltip("Slide (S): new notes are slide notes. A slide note glides the notes already sounding to its pitch over its length, and plays as a normal note when nothing is sounding. Shift+S turns the selected notes into slide notes or back.");
        slideB.onClick = [this] { slideMode(); };
        lockB.sm = true; lockB.setTooltip("Snap new and moved notes to the key and scale chosen beside it"); lockB.onClick = [this] { doc.setp("scaleLock", !doc.pb("scaleLock", false)); sync(); };
        loopB.sm = true; loopB.setTooltip("Loop mode: play only part of the pattern, over and over. Drag on the ruler to set the range."); loopB.onClick = [this] { toggleLoop(); };
        ghostB.sm = true; ghostB.setTooltip("Show other channels faintly"); ghostB.onClick = [this] { doc.setp("ghost", !doc.pb("ghost", true)); sync(); grid.repaint(); };
        followB.sm = true; followB.setTooltip("Scroll along with the playhead while playing (off: the view stays where you are working)"); followB.onClick = [this] { doc.setp("follow", !doc.pb("follow", false)); sync(); };
        toolsB.onClick = [this] { toolsMenu(&toolsB); };
        zoomOut.sm = zoomIn.sm = true; zoomOut.setTooltip("Zoom out"); zoomIn.setTooltip("Zoom in"); zoomOut.onClick = [this] { grid.zoom(0.75); }; zoomIn.onClick = [this] { grid.zoom(1.33); };
        allB.onClick = [this] { auto* ns = notes(); if (!ns) return; bool all = !ns->empty(); for (auto& n : *ns) all &= sel.count(n.id) > 0; sel.clear(); if (!all) for (auto& n : *ns) sel.insert(n.id); grid.repaint(); };
        copyB.onClick = [this] { copy(); }; pasteB.onClick = [this] { run([this] { paste(-1); }); }; dupB.onClick = [this] { run([this] { dup(); }); }; delB.onClick = [this] { run([this] { del(); }); };
        grid.o = this; grid.getProperties().set("pinchZoom", true);
        sync(); startTimerHz(30);
    }
    ~RollView() override { doc.removeChangeListener(this); }
    void resized() override {
        auto r = getLocalBounds(); Flow f;
        f.add(chanBox, 120).add(tools, tools.idealWidth()).add(snapL, snapL.idealWidth()).add(snapBox, 76).add(stampBox, 120).add(keyL, keyL.idealWidth()).add(rootBox, 58).add(scaleBox, 112)
            .add(slideB, slideB.idealWidth()).add(lockB, lockB.idealWidth()).add(loopB, loopB.idealWidth()).add(ghostB, ghostB.idealWidth()).add(followB, followB.idealWidth()).add(toolsB, toolsB.idealWidth());
        if (isTouch()) for (auto* b : { &allB, &copyB, &pasteB, &dupB, &delB }) f.add(*b, b->idealWidth());
        f.right(zoomOut, 30).right(zoomIn, 30);
        barH = f.layout(r); r.removeFromTop(barH); grid.setBounds(r);
    }
    void paint(juce::Graphics& g) override { fillPanel(g); vbarLine(g, barH, getWidth()); }

private:
    // the view state is kept for the session (the roll is rebuilt when a deck switches views)
    struct ViewState { double sx = 0, sy = -1, zx = 0.5; float rh = 16; std::string tool = "draw"; int snap = us::STEP; double lastLen = us::STEP, lastVel = 0.8; std::string stamp, lane = "v"; bool slide = false; };
    static ViewState& V() { static ViewState v; return v; }
    static inline const char* TOOLS[] = { "draw", "paint", "select", "slice", "erase", "mute", "zoom", "play", "pan" };
    static std::vector<std::pair<int, juce::String>> snaps() { return { { 0, "Off" }, { -1, "Bar" }, { 96, "1/4" }, { 48, "1/8" }, { 32, "1/8T" }, { 24, "1/16" }, { 16, "1/16T" }, { 12, "1/32" }, { 6, "1/64" } }; }
    static const std::vector<std::pair<std::string, std::vector<int>>>& chords() { static const std::vector<std::pair<std::string, std::vector<int>>> c = {
        { "Major", { 0, 4, 7 } }, { "Minor", { 0, 3, 7 } }, { "Major 7th", { 0, 4, 7, 11 } }, { "Minor 7th", { 0, 3, 7, 10 } }, { "Dominant 7th", { 0, 4, 7, 10 } }, { "Minor 9th", { 0, 3, 7, 10, 14 } }, { "Major 9th", { 0, 4, 7, 11, 14 } },
        { "Diminished", { 0, 3, 6 } }, { "Augmented", { 0, 4, 8 } }, { "Suspended 2", { 0, 2, 7 } }, { "Suspended 4", { 0, 5, 7 } }, { "Fifth", { 0, 7 } }, { "Fifth + octave", { 0, 7, 12 } }, { "Octave", { 0, 12 } }, { "Add 9", { 0, 4, 7, 14 } }, { "Sixth", { 0, 4, 7, 9 } } }; return c; }
    static const std::vector<std::pair<std::string, std::vector<int>>>& stampScales() { static const std::vector<std::pair<std::string, std::vector<int>>> s = {
        { "Major (Ionian)", { 0, 2, 4, 5, 7, 9, 11 } }, { "Minor (Aeolian)", { 0, 2, 3, 5, 7, 8, 10 } }, { "Harmonic minor", { 0, 2, 3, 5, 7, 8, 11 } }, { "Melodic minor", { 0, 2, 3, 5, 7, 9, 11 } }, { "Dorian", { 0, 2, 3, 5, 7, 9, 10 } },
        { "Phrygian", { 0, 1, 3, 5, 7, 8, 10 } }, { "Lydian", { 0, 2, 4, 6, 7, 9, 11 } }, { "Mixolydian", { 0, 2, 4, 5, 7, 9, 10 } }, { "Locrian", { 0, 1, 3, 5, 6, 8, 10 } }, { "Major pentatonic", { 0, 2, 4, 7, 9 } }, { "Minor pentatonic", { 0, 3, 5, 7, 10 } },
        { "Blues", { 0, 3, 5, 6, 7, 10 } }, { "Whole tone", { 0, 2, 4, 6, 8, 10 } }, { "Hungarian minor", { 0, 2, 3, 6, 7, 8, 11 } }, { "Phrygian dominant", { 0, 1, 4, 5, 7, 8, 10 } }, { "Japanese (In)", { 0, 1, 5, 7, 8 } }, { "Arabic", { 0, 1, 4, 5, 7, 8, 11 } } }; return s; }
    static const std::vector<int>* stampOf(const std::string& n) { for (auto& [k, v] : chords()) if (k == n) return &v; for (auto& [k, v] : stampScales()) if (k == n) return &v; return nullptr; }
    struct LaneDef { const char* id; const char* label; double lo, hi; const char* tag; };
    static const std::vector<LaneDef>& lanes() { static const std::vector<LaneDef> l = { { "v", "Velocity", 0, 1, "VEL" }, { "pan", "Note pan", -1, 1, "PAN" }, { "fine", "Note fine pitch (cents)", -100, 100, "FINE" },
        { "rel", "Note release", -1, 1, "REL" }, { "cut", "Note filter cutoff", -1, 1, "CUT" }, { "res", "Note filter resonance", -1, 1, "RES" } }; return l; }
    static const LaneDef& laneDef() { for (auto& l : lanes()) if (V().lane == l.id) return l; return lanes()[0]; }
    static double laneGet(const us::Note& n, const std::string& k) { return k == "v" ? n.v : k == "pan" ? n.pan : k == "fine" ? n.fine : k == "rel" ? n.rel : k == "cut" ? n.cut : n.res; }
    static void laneSet(us::Note& n, const std::string& k, double v) { if (k == "v") n.v = v; else if (k == "pan") n.pan = v; else if (k == "fine") n.fine = v; else if (k == "rel") n.rel = v; else if (k == "cut") { n.cut = v; n.hasCut = v != 0; } else { n.res = v; n.hasRes = v != 0; } }
    static std::vector<us::Note>& clipboard() { static std::vector<us::Note> c; return c; }

    Doc& doc; std::set<std::string> sel; std::set<int> keysDown; int barH = 40; std::string lastCh;
    juce::ComboBox chanBox, snapBox, stampBox, rootBox, scaleBox; Cap snapL{ "Snap" }, keyL{ "Key" };
    Seg tools{ { "", "", "", "", "", "", "", "", "" }, { "draw", "paint", "select", "slice", "erase", "mute", "zoom", "playfrom", "pan" } };
    Btn slideB{ "Slide" }, lockB{ "Snap to scale" }, loopB{ "Loop" }, ghostB{ "Ghost" }, followB{ "Follow" }, toolsB{ u8("Tools ▾") }, zoomOut{ u8("−") }, zoomIn{ "+" };
    Btn allB{ "All" }, copyB{ "Copy" }, pasteB{ "Paste" }, dupB{ "Dup" }, delB{ "Del" };

    std::vector<us::Note>* notes(bool create = false) {
        auto* c = doc.chan(); auto* p = doc.patt(); if (!c || !p) return nullptr;
        auto it = p->notes.find(c->id); if (it != p->notes.end()) return &it->second; if (!create) return nullptr; return &p->notes[c->id];
    }
    std::vector<us::Note*> targets() { std::vector<us::Note*> r; if (auto* ns = notes()) { for (auto& n : *ns) if (sel.empty() || sel.count(n.id)) r.push_back(&n); } return r; }
    double snapT(double t, bool floorIt = false) const { const double s = V().snap < 0 ? doc.p.bar() : V().snap; return std::max(0.0, s > 0 ? (floorIt ? std::floor(t / s) : std::round(t / s)) * s : std::round(t)); }
    double snapD(double d) const { const double s = V().snap < 0 ? doc.p.bar() : V().snap; return s > 0 ? std::round(d / s) * s : std::round(d); }
    double snapLen() const { return V().snap < 0 ? doc.p.bar() : V().snap > 0 ? V().snap : us::STEP; }
    std::set<int> scaleSet() const { std::set<int> s; if (doc.p.keyScale == "chromatic") return s; for (auto& [n, iv] : us::scales()) if (n == doc.p.keyScale) for (int i : iv) s.insert((i + doc.p.keyRoot) % 12); return s; }
    int lockN(int n) const { if (!doc.pb("scaleLock", false)) return n; auto sc = scaleSet(); if (sc.empty()) return n; for (int d : { 0, 1, -1, 2, -2 }) if (sc.count((((n + d) % 12) + 12) % 12)) return juce::jlimit(0, 127, n + d); return n; }
    void audition(int n) { if (auto* c = doc.chan()) { doc.eng.noteOn(c->id, n, V().lastVel); juce::Timer::callAfterDelay(220, [this, id = c->id, n] { doc.eng.noteOff(id, n); }); } }
    // after an edit: grow the pattern to fit, commit
    void done() {
        auto* p = doc.patt(); auto* ns = notes(); double end = 0; if (ns) for (auto& n : *ns) end = std::max(end, n.t + n.d);
        if (ns) for (auto it = sel.begin(); it != sel.end();) { bool has = false; for (auto& n : *ns) has |= n.id == *it; it = has ? std::next(it) : sel.erase(it); }
        if (p && end > p->bars * doc.p.bar() && p->bars < 64) { double b = 1; while (b * doc.p.bar() < end && b < 64) b *= 2; p->bars = b; }
        doc.commit(); grid.repaint();
    }
    void run(std::function<void()> fn) { fn(); done(); }
    void removeIf(std::function<bool(const us::Note&)> pred) { if (auto* ns = notes()) ns->erase(std::remove_if(ns->begin(), ns->end(), pred), ns->end()); }

    // --- note tools -----------------------------------------------------------------------------------------------
    struct Chord { double t, end; std::vector<us::Note*> notes; };
    static std::vector<Chord> chordsOf(std::vector<us::Note*> ns) {
        std::sort(ns.begin(), ns.end(), [](auto* a, auto* b) { return a->t < b->t || (a->t == b->t && a->n < b->n); }); std::vector<Chord> out;
        for (auto* n : ns) { if (!out.empty() && n->t < out.back().end) { out.back().notes.push_back(n); out.back().end = std::max(out.back().end, n->t + n->d); } else out.push_back({ n->t, n->t + n->d, { n } }); }
        return out;
    }
    void quantize(double strength = 1, bool ends = false) { const double s = snapLen(); for (auto* n : targets()) { const double q = std::round(n->t / s) * s; n->t = std::round(n->t + (q - n->t) * strength); if (ends) n->d = std::max(s, std::round(n->d / s) * s); } }
    void legato() { auto ts = targets(); std::set<double> starts; for (auto* n : ts) starts.insert(n->t); for (auto* n : ts) { auto it = starts.upper_bound(n->t); if (it != starts.end()) n->d = *it - n->t; } }
    void chop() { const double s = snapLen(); auto ts = targets(); std::vector<us::Note> made; for (auto* n : ts) { if (n->d <= s) continue; const double end = n->t + n->d; double t = (std::floor(n->t / s) + 1) * s; n->d = t - n->t; for (; t < end; t += s) { us::Note z = *n; z.id = us::newId(); z.t = t; z.d = std::min(s, end - t); made.push_back(z); } }
        auto* ns = notes(true); for (auto& z : made) { ns->push_back(z); if (!sel.empty()) sel.insert(z.id); } }
    void transpose(int d) { for (auto* n : targets()) n->n = juce::jlimit(0, 127, n->n + d); }
    void scaleLen(double k) { for (auto* n : targets()) n->d = std::max(3.0, std::round(n->d * k)); }
    void humanize() { auto& r = juce::Random::getSystemRandom(); for (auto* n : targets()) { n->t = std::max(0.0, n->t + std::round((r.nextDouble() - 0.5) * 6)); n->v = juce::jlimit(0.05, 1.0, n->v * (0.88 + r.nextDouble() * 0.24)); } }
    void strum(double step) { for (auto& c : chordsOf(targets())) { std::sort(c.notes.begin(), c.notes.end(), [](auto* a, auto* b) { return a->n < b->n; }); for (size_t i = 0; i < c.notes.size(); i++) { auto* n = c.notes[i]; const double sh = std::min(i * step, n->d - 3); n->t += sh; n->d -= sh; } } }
    void reverse() { auto ts = targets(); if (ts.empty()) return; double a = 1e18, b = 0; for (auto* n : ts) { a = std::min(a, n->t); b = std::max(b, n->t + n->d); } for (auto* n : ts) n->t = a + b - (n->t + n->d); }
    void arp(const std::string& mode, double rate, int octs, double gate) {
        auto ts = targets(); std::set<std::string> ids; for (auto* n : ts) ids.insert(n->id); std::vector<us::Note> made; auto& rnd = juce::Random::getSystemRandom();
        for (auto& c : chordsOf(ts)) {
            std::set<int> s; double v = 0; for (auto* n : c.notes) { s.insert(n->n); v += n->v; } v /= c.notes.size();
            std::vector<int> pcs; for (int o = 0; o < octs; o++) for (int n : s) if (n + 12 * o <= 127) pcs.push_back(n + 12 * o);
            std::vector<int> seq = pcs; if (mode == "down") std::reverse(seq.begin(), seq.end()); else if (mode == "updown" && pcs.size() > 2) for (size_t i = pcs.size() - 2; i >= 1; i--) seq.push_back(pcs[i]);
            int i = 0; for (double t = c.t; t < c.end; t += rate, i++) { us::Note z; z.id = us::newId(); z.t = t; z.d = std::max(3.0, std::round(std::min(rate, c.end - t) * gate)); z.n = mode == "random" ? pcs[(size_t)rnd.nextInt((int)pcs.size())] : seq[(size_t)i % seq.size()]; z.v = v; made.push_back(z); }
        }
        removeIf([&](const us::Note& n) { return ids.count(n.id) > 0; }); auto* ns = notes(true); sel.clear(); for (auto& z : made) { ns->push_back(z); sel.insert(z.id); }
    }
    void glue() {
        auto ts = targets(); std::sort(ts.begin(), ts.end(), [](auto* a, auto* b) { return a->n < b->n || (a->n == b->n && a->t < b->t); }); std::set<std::string> gone;
        for (size_t i = 0; i < ts.size(); i++) { auto* a = ts[i]; if (gone.count(a->id)) continue; for (size_t j = i + 1; j < ts.size() && ts[j]->n == a->n; j++) { auto* b = ts[j]; if (b->t <= a->t + a->d + 1) { a->d = std::max(a->d, b->t + b->d - a->t); gone.insert(b->id); } } }
        removeIf([&](const us::Note& n) { return gone.count(n.id) > 0; }); for (auto& id : gone) sel.erase(id);
    }
    void flam(double gap = 6) { std::vector<us::Note> made; for (auto* n : targets()) if (n->t >= gap) { us::Note z = *n; z.id = us::newId(); z.t = n->t - gap; z.d = std::min(gap, n->d); z.v = n->v * 0.6; made.push_back(z); } auto* ns = notes(true); for (auto& z : made) ns->push_back(z); }
    void limit(int lo, int hi) { for (auto* n : targets()) { while (n->n < lo) n->n += 12; while (n->n > hi) n->n -= 12; n->n = juce::jlimit(0, 127, n->n); } }
    void flip() { auto ts = targets(); if (ts.empty()) return; int a = 127, b = 0; for (auto* n : ts) { a = std::min(a, n->n); b = std::max(b, n->n); } for (auto* n : ts) n->n = a + b - n->n; }
    void randomize(double vel, double time, double pan) { auto& r = juce::Random::getSystemRandom(); for (auto* n : targets()) { if (vel > 0) n->v = juce::jlimit(0.05, 1.0, n->v + (r.nextDouble() - 0.5) * vel); if (time > 0) n->t = std::max(0.0, n->t + std::round((r.nextDouble() - 0.5) * time)); if (pan > 0) n->pan = juce::jlimit(-1.0, 1.0, (r.nextDouble() - 0.5) * 2 * pan); } }
    void levels(double lo, double hi) { auto ts = targets(); std::sort(ts.begin(), ts.end(), [](auto* a, auto* b) { return a->t < b->t; }); if (ts.empty()) return; const double a = ts[0]->t, b = ts.back()->t; for (auto* n : ts) n->v = juce::jlimit(0.02, 1.0, lo + (hi - lo) * (b > a ? (n->t - a) / (b - a) : 1)); }
    void lfo(const std::string& lane, double period, double depth, double center) { for (auto* n : targets()) { const double s = std::sin(n->t / (period * doc.p.bar()) * juce::MathConstants<double>::twoPi); if (lane == "v") n->v = juce::jlimit(0.02, 1.0, center + s * depth); else if (lane == "fine") n->fine = std::round(s * depth * 100); else laneSet(*n, lane, juce::jlimit(-1.0, 1.0, s * depth)); } }
    void progression(const std::vector<int>& degrees, double len, int oct) {
        std::vector<int> sc = { 0, 2, 4, 5, 7, 9, 11 }; if (doc.p.keyScale != "chromatic") for (auto& [n, iv] : us::scales()) if (n == doc.p.keyScale) sc = iv;
        auto* ns = notes(true); const int base = (oct + 1) * 12 + doc.p.keyRoot; sel.clear();
        for (size_t i = 0; i < degrees.size(); i++) for (int step : { 0, 2, 4 }) { const int idx = degrees[i] + step; us::Note z; z.id = us::newId(); z.t = i * len; z.d = len; z.n = juce::jlimit(0, 127, base + sc[(size_t)idx % sc.size()] + 12 * (idx / (int)sc.size())); z.v = 0.75; ns->push_back(z); sel.insert(z.id); }
    }
    void del() { if (sel.empty()) return; auto ids = sel; removeIf([&](const us::Note& n) { return ids.count(n.id) > 0; }); sel.clear(); }
    void copy() { auto ts = targets(); if (ts.empty()) return; double t0 = 1e18; for (auto* n : ts) t0 = std::min(t0, n->t); clipboard().clear(); for (auto* n : ts) { us::Note z = *n; z.t -= t0; clipboard().push_back(z); } toast("Copied " + juce::String((int)ts.size()) + " note" + (ts.size() > 1 ? "s" : "")); }
    void paste(double at) {
        if (clipboard().empty()) return; double t0 = at;
        if (t0 < 0) { double e = -1; if (auto* ns = notes()) for (auto& n : *ns) if (sel.count(n.id)) e = std::max(e, n.t + n.d); t0 = e >= 0 ? e : snapT(grid.tOf((float)grid.KW()), true); }
        auto* ns = notes(true); sel.clear(); for (auto& n : clipboard()) { us::Note z = n; z.id = us::newId(); z.t = t0 + n.t; ns->push_back(z); sel.insert(z.id); }
    }
    void dup() { auto* ns = notes(); if (!ns) return; std::vector<us::Note*> ts; for (auto& n : *ns) if (sel.count(n.id)) ts.push_back(&n); if (ts.empty()) return; double a = 1e18, b = 0; for (auto* n : ts) { a = std::min(a, n->t); b = std::max(b, n->t + n->d); }
        auto save = clipboard(); clipboard().clear(); for (auto* n : ts) { us::Note z = *n; z.t -= a; clipboard().push_back(z); } const double d = snapD(b - a); paste(a + (d > 0 ? d : b - a)); clipboard() = save; }
    void slideMode() { V().slide = !V().slide; sync(); toast(V().slide ? "New notes are slide notes" : "New notes are normal notes"); }
    void slideSel() { auto ts = targets(); if (ts.empty()) return; bool all = true; for (auto* n : ts) all &= n->slide; for (auto* n : ts) n->slide = !all; done(); }
    void toggleLoop() {
        auto* p = doc.patt(); if (!p) return;
        if (p->loop.on) p->loop.on = false;
        else { double a = 1e18, b = -1; if (auto* ns = notes()) for (auto& n : *ns) if (sel.count(n.id)) { a = std::min(a, n.t); b = std::max(b, n.t + n.d); }
            const double bt = doc.p.beat(); if (b >= 0) p->loop = { true, std::floor(a / bt) * bt, std::ceil(b / bt) * bt }; else if (p->loop.b > p->loop.a) p->loop.on = true; else p->loop = { true, 0, std::min((double)doc.p.bar(), p->bars * doc.p.bar()) }; }
        doc.commit(); sync(); grid.repaint();
    }
    static std::vector<std::pair<double, juce::String>> pct(std::vector<double> a) { std::vector<std::pair<double, juce::String>> r; for (double v : a) r.push_back({ v, juce::String(juce::roundToInt(v * 100)) + "%" }); return r; }
    std::vector<std::pair<double, juce::String>> snapOpts(bool withBar = true) { std::vector<std::pair<double, juce::String>> r; for (auto& [v, l] : snaps()) if (v != 0 && (withBar || v > 0)) r.push_back({ v < 0 ? (double)doc.p.bar() : (double)v, l }); return r; }
    int idxOf(const std::vector<std::pair<double, juce::String>>& o, double v) { for (size_t i = 0; i < o.size(); i++) if (std::abs(o[i].first - v) < 1e-9) return (int)i; return 0; }
    void quantDialog() { auto g = snapOpts(); formDialog("Quantize", { { "grid", "Grid", g, idxOf(g, snapLen()) }, { "strength", "Strength", pct({ 1, 0.75, 0.5, 0.25 }), 0 }, { "ends", "Lengths too", { { 0, "No" }, { 1, "Yes" } }, 0 } }, {}, "Apply",
        [this](auto o) { const int s = V().snap; V().snap = (int)o["grid"]; run([&] { quantize(o["strength"], o["ends"] > 0); }); V().snap = s; }); }
    void strumDialog() { formDialog("Strum", { { "step", "Time between notes", { { 2, "Very tight" }, { 4, "Tight" }, { 8, "Medium" }, { 16, "Loose" }, { 24, "Very loose" } }, 1 } }, "Chords roll in from the lowest note up, like a guitar.", "Apply", [this](auto o) { run([&] { strum(o["step"]); }); }); }
    void limitDialog() { std::vector<std::pair<double, juce::String>> ns; for (int i = 12; i <= 108; i++) ns.push_back({ (double)i, noteName(i) });
        formDialog("Limit range", { { "lo", "Lowest note", ns, 36 }, { "hi", "Highest note", ns, 60 } }, "Notes outside are moved by octaves until they fit.", "Apply", [this](auto o) { run([&] { limit((int)std::min(o["lo"], o["hi"]), (int)std::max(o["lo"], o["hi"])); }); }); }
    void randDialog() { formDialog("Randomize", { { "vel", "Velocity", { { 0, "Leave" }, { 0.2, "A little" }, { 0.4, "Some" }, { 0.8, "A lot" } }, 1 }, { "time", "Timing", { { 0, "Leave" }, { 4, "A little" }, { 8, "Some" }, { 16, "A lot" } }, 1 }, { "pan", "Pan", { { 0, "Leave" }, { 0.3, "A little" }, { 0.7, "A lot" } }, 0 } }, {}, "Apply",
        [this](auto o) { run([&] { randomize(o["vel"], o["time"], o["pan"]); }); }); }
    void levelsDialog() { formDialog("Scale levels", { { "lo", "Velocity at the start", pct({ 0.1, 0.25, 0.5, 0.75, 1 }), 1 }, { "hi", "Velocity at the end", pct({ 0.1, 0.25, 0.5, 0.75, 1 }), 4 } }, "A ramp of velocities across the notes, for fades and builds.", "Apply", [this](auto o) { run([&] { levels(o["lo"], o["hi"]); }); }); }
    void lfoDialog() {
        static const char* ln[] = { "v", "pan", "fine", "rel", "cut", "res" };
        formDialog("LFO", { { "lane", "Changes", { { 0, "Velocity" }, { 1, "Note pan" }, { 2, "Note fine pitch" }, { 3, "Note release" }, { 4, "Note filter cutoff" }, { 5, "Note filter resonance" } }, 0 },
            { "period", "One cycle every", { { 0.25, "1 beat" }, { 0.5, "2 beats" }, { 1, "1 bar" }, { 2, "2 bars" }, { 4, "4 bars" } }, 2 }, { "depth", "Depth", pct({ 0.1, 0.25, 0.5, 0.75, 1 }), 2 }, { "center", "Centre (velocity)", pct({ 0.25, 0.5, 0.75 }), 1 } }, {}, "Apply",
            [this](auto o) { run([&] { lfo(ln[(int)o["lane"]], o["period"], o["depth"], o["center"]); }); });
    }
    void progDialog() {
        struct Prog { const char* first; std::vector<int> second; };
        static const std::vector<Prog> progs = { { "I – V – vi – IV (pop)", { 0, 4, 5, 3 } }, { "vi – IV – I – V", { 5, 3, 0, 4 } }, { "I – vi – IV – V (50s)", { 0, 5, 3, 4 } }, { "ii – V – I – I (jazz)", { 1, 4, 0, 0 } },
            { "i – VI – III – VII (minor pop)", { 0, 5, 2, 6 } }, { "i – iv – v – i", { 0, 3, 4, 0 } }, { "I – IV – V – IV", { 0, 3, 4, 3 } } };
        std::vector<std::pair<double, juce::String>> po; for (size_t i = 0; i < progs.size(); i++) po.push_back({ (double)i, u8(progs[i].first) });
        const double bt = doc.p.beat(), br = doc.p.bar();
        formDialog("Chord progression", { { "prog", "Progression", po, 0 }, { "len", "Each chord lasts", { { bt * 2, "2 beats" }, { br, "1 bar" }, { br * 2, "2 bars" } }, 1 }, { "oct", "Octave", { { 2, "2" }, { 3, "3" }, { 4, "4" }, { 5, "5" } }, 2 } },
            "Built from the key chosen above (major if none). The new chords are selected, ready to move or arpeggiate.", "Apply", [this](auto o) { run([&] { progression(progs[(size_t)o["prog"]].second, o["len"], (int)o["oct"]); }); });
    }
    void arpDialog() { auto r = snapOpts(false);
        formDialog("Arpeggiate", { { "mode", "Direction", { { 0, "Up" }, { 1, "Down" }, { 2, "Up & down" }, { 3, "Random" } }, 0 }, { "rate", "Rate", r, idxOf(r, us::STEP) }, { "octs", "Octaves", { { 1, "1" }, { 2, "2" }, { 3, "3" }, { 4, "4" } }, 0 }, { "gate", "Gate", { { 0.4, "40%" }, { 0.6, "60%" }, { 0.8, "80%" }, { 1, "100%" } }, 2 } },
            "Applies to the selection (or every note). Overlapping notes are treated as one chord.", "Arpeggiate", [this](auto o) { static const char* m[] = { "up", "down", "updown", "random" }; run([&] { arp(m[(int)o["mode"]], o["rate"], (int)o["octs"], o["gate"]); }); }); }
    void toolsMenu(juce::Component* atC, bool mouse = false) {
        Menu m; m.head(sel.empty() ? juce::String("All notes") : juce::String((int)sel.size()) + " selected");
        m.item(u8("Generate chord progression…"), [this] { progDialog(); }).sep();
        m.item("Quick legato", [this] { run([this] { legato(); }); }, true, false, "Ctrl+L").item("Quick quantize", [this] { run([this] { quantize(); }); }, true, false, "Ctrl+Q")
         .item("Quick quantize starts + lengths", [this] { run([this] { quantize(1, true); }); }).item(u8("Quantize…"), [this] { quantDialog(); }, true, false, "Alt+Q").sep();
        m.item("Quick chop", [this] { run([this] { chop(); }); }, true, false, "Ctrl+U").item("Glue", [this] { run([this] { glue(); }); }, true, false, "Ctrl+G").sep();
        m.item(u8("Arpeggiate…"), [this] { arpDialog(); }).item(u8("Strum…"), [this] { strumDialog(); }, true, false, "Alt+S").item("Flam", [this] { run([this] { flam(); }); }).item(u8("Limit…"), [this] { limitDialog(); });
        m.item("Flip (mirror pitches)", [this] { run([this] { flip(); }); }).item("Reverse in time", [this] { run([this] { reverse(); }); }).item(u8("Randomize…"), [this] { randDialog(); }).item("Humanize", [this] { run([this] { humanize(); }); }).sep();
        m.item(u8("Scale levels…"), [this] { levelsDialog(); }).item(u8("LFO…"), [this] { lfoDialog(); });
        m.item("Make slide notes / back to normal", [this] { slideSel(); }, true, false, "Shift+S").item("Draw slide notes", [this] { slideMode(); }, true, V().slide, "S").sep();
        m.item(u8("Length × 2"), [this] { run([this] { scaleLen(2); }); }).item(u8("Length ÷ 2"), [this] { run([this] { scaleLen(0.5); }); });
        m.item("Transpose +1 octave", [this] { run([this] { transpose(12); }); }).item(u8("Transpose −1 octave"), [this] { run([this] { transpose(-12); }); }).sep();
        m.item("Detect key from notes", [this] { auto* ns = notes(); if (!ns || ns->size() < 3) { toast("Add a few notes first"); return; } auto k = us::keyFromNotes(*ns); doc.p.keyRoot = k.root; doc.p.keyScale = k.minor ? "minor" : "major"; doc.commit(); toast("Looks like " + juce::String(k.name)); });
        m.item("Select all", [this] { if (auto* ns = notes()) for (auto& n : *ns) sel.insert(n.id); grid.repaint(); }, true, false, "Ctrl+A");
        m.item("Duplicate", [this] { run([this] { dup(); }); }, true, false, "Ctrl+B").item("Delete", [this] { run([this] { del(); }); }, true, false, "Del");
        m.show(mouse ? atMouse() : at(atC));
    }

    // --- the editing surface (canvas) -----------------------------------------------------------------------------
    struct Grid : public juce::Component {
        RollView* o = nullptr; static constexpr int RUL = 20, VEL = 64;
        struct Drag { std::string type; float x = 0, y = 0; std::string ref; std::map<std::string, std::tuple<double, double, int>> orig; int lastN = -1; bool created = false, moved = false;
            float x0 = 0, y0 = 0, x1 = 0, y1 = 0; std::set<std::string> base; double sx = 0, sy = 0, t0 = 0; int n = -1; bool muteTo = false; std::set<std::string> touched; std::set<double> made; int n0 = 0; double len = 0; };
        std::optional<Drag> drag;
        int KW() const { return getWidth() < 560 ? 40 : 52; }
        float W() const { return (float)getWidth(); } float H() const { return (float)getHeight(); }
        float gridH() const { return H() - VEL - RUL; }
        float xOf(double t) const { return (float)(KW() + t * V().zx - V().sx); } double tOf(float x) const { return (x - KW() + V().sx) / V().zx; }
        float yOf(int n) const { return (float)(RUL + (127 - n) * V().rh - V().sy); } int nOf(float y) const { return 127 - (int)std::floor((y - RUL + V().sy) / V().rh); }
        void clampScroll() { V().sx = std::max(0.0, V().sx); V().sy = juce::jlimit(0.0, std::max(0.0, 128.0 * V().rh - gridH()), V().sy); }
        void focusNotes() { auto* ns = o->notes(); auto* c = o->doc.chan(); double mid = c && c->type == "sample" ? c->root : 64; if (ns && !ns->empty()) { mid = 0; for (auto& n : *ns) mid += n.n; mid /= ns->size(); } V().sy = (127 - mid) * V().rh - gridH() / 2; clampScroll(); repaint(); }
        void resized() override { if (V().sy < 0 && getHeight() > 100) focusNotes(); clampScroll(); }
        void zoom(double k, float x = -1) { if (x < 0) x = KW() + (W() - KW()) / 2; const double t = tOf(x); V().zx = juce::jlimit(0.04, 6.0, V().zx * k); V().sx = t * V().zx - (x - KW()); clampScroll(); repaint(); }
        juce::Colour noteCol(const us::Note& n, juce::Colour c) const {
            static const float hue[] = { 0, 30, 55, 90, 130, 165, 190, 215, 245, 275, 305, 335 }; const auto& nc = style().noteColor;
            if (nc == "accent") return ACCENT; if (nc == "velocity") return juce::Colour::fromHSL((float)((220 - n.v * 220) / 360), 0.75f, 0.58f, 1.f); if (nc == "pitch") return juce::Colour::fromHSL(hue[n.n % 12] / 360.f, 0.65f, 0.6f, 1.f); return c;
        }
        us::Note* noteAt(float x, float y) { if (x < KW() || y < RUL || y > H() - VEL) return nullptr; auto* ns = o->notes(); if (!ns) return nullptr; const double t = tOf(x); const int n = nOf(y);
            for (int i = (int)ns->size() - 1; i >= 0; i--) { auto& q = (*ns)[(size_t)i]; if (q.n == n && t >= q.t && t < q.t + std::max(q.d, 3 / V().zx)) return &q; } return nullptr; }
        void paint(juce::Graphics& g) override {
            auto& D = o->doc; auto* c = D.chan(); auto* p = D.patt(); const double L = p ? p->bars * D.p.bar() : D.p.bar(); const float gb = H() - VEL; const auto sc = o->scaleSet(); const int root = D.p.keyRoot;
            const juce::Colour ccol = c ? hex(c->color) : juce::Colour(0xff888888); const bool light = style().light; const float k = style().gridK; const juce::Colour ink = light ? juce::Colours::black : juce::Colours::white;
            auto a = [k](float x) { return std::min(1.f, x * k); };
            fillPanel(g);
            g.saveState(); g.reduceClipRegion(juce::Rectangle<int>(KW(), RUL, getWidth() - KW(), (int)gb - RUL));
            const int n0 = juce::jlimit(0, 127, nOf(gb)), n1 = juce::jlimit(0, 127, nOf((float)RUL));
            for (int n = n0; n <= n1; n++) {
                const float y = yOf(n); const bool off = sc.empty() ? juce::MidiMessage::isMidiNoteBlack(n) : !sc.count(n % 12);
                if (off) { g.setColour(juce::Colours::black.withAlpha(light ? 0.07f : 0.2f)); g.fillRect((float)KW(), y, W() - KW(), V().rh); }
                else if (!sc.empty() && n % 12 == root) { g.setColour(ACCENT.withAlpha(0.1f)); g.fillRect((float)KW(), y, W() - KW(), V().rh); }
                if (n % 12 == 0) { g.setColour(ink.withAlpha(a(0.13f))); g.fillRect((float)KW(), y + V().rh - 1, W() - KW(), 1.f); }
            }
            double gs = V().snap < 0 ? D.p.bar() : V().snap > 0 ? V().snap : us::STEP; while (gs * V().zx < 7) gs *= 2;
            for (double t = std::max(0.0, std::floor(tOf((float)KW()) / gs) * gs), t1 = tOf(W()); t <= t1; t += gs) {
                g.setColour(ink.withAlpha(std::fmod(t, D.p.bar()) == 0 ? a(0.26f) : std::fmod(t, D.p.beat()) == 0 ? a(0.13f) : a(0.055f))); g.fillRect(std::round(xOf(t)), (float)RUL, 1.f, gb - RUL);
            }
            const float xe = xOf(L); if (xe < W()) { g.setColour(juce::Colours::black.withAlpha(light ? 0.12f : 0.32f)); g.fillRect(std::max((float)KW(), xe), (float)RUL, W(), gb); }
            if (D.pb("ghost", true) && p && c) for (auto& oc : D.p.channels) if (oc.id != c->id) if (auto it = p->notes.find(oc.id); it != p->notes.end()) {
                g.setColour(hex(oc.color).withAlpha(0.22f)); for (auto& n : it->second) g.fillRect(xOf(n.t), yOf(n.n) + 3, std::max(2.f, (float)(n.d * V().zx) - 1), V().rh - 6);
            }
            if (auto* ns = o->notes()) for (auto& n : *ns) {
                const float x = xOf(n.t), w = std::max(3.f, (float)(n.d * V().zx) - 1), y = yOf(n.n);
                if (x > W() || x + w < KW() || y > gb || y + V().rh < RUL) continue;
                const bool on = o->sel.count(n.id) > 0; juce::Rectangle<float> r(x, y + 1, w, V().rh - 2); const float rad = std::min(4.f, RAD() / 3);
                g.setColour((n.mute ? DIM : noteCol(n, ccol)).withAlpha(n.mute ? 0.18f : (float)(0.45 + 0.55 * n.v))); g.fillRoundedRectangle(r, rad);
                if (on) { g.setColour(TEXT); g.drawRoundedRectangle(r, rad, 1.5f); }
                else if (n.slide) { juce::Path pth; pth.addRoundedRectangle(r, rad); juce::Path ds; const float dl[] = { 3, 2 }; juce::PathStrokeType(1).createDashedStroke(ds, pth, dl, 2); g.setColour(TEXT); g.fillPath(ds); }
                g.setColour(juce::Colours::black.withAlpha(0.35f)); g.fillRect(x + w - 3, y + 3, 1.5f, V().rh - 6);
                if (n.slide) { const float m = y + V().rh / 2, kk = std::min(5.f, V().rh / 2 - 2); juce::Path tri; tri.addTriangle(x + 2, m - kk, x + 2 + kk * 1.4f, m, x + 2, m + kk); g.setColour(juce::Colours::black.withAlpha(0.75f)); g.fillPath(tri); }
                if (w > 30 && V().rh >= 13) { g.setColour(juce::Colours::black.withAlpha(0.7f)); g.setFont(font(10)); g.drawText(noteName(n.n), (int)(x + (n.slide ? 12 : 4)), (int)y, (int)w, (int)V().rh, juce::Justification::centredLeft); }
            }
            if (drag && drag->type == "box") { const float bx = std::min(drag->x0, drag->x1), by = std::min(drag->y0, drag->y1), bw = std::abs(drag->x1 - drag->x0), bh = std::abs(drag->y1 - drag->y0);
                g.setColour(ACCENT2.withAlpha(0.15f)); g.fillRect(bx, by, bw, bh); g.setColour(ACCENT2); g.drawRect(bx + 0.5f, by + 0.5f, bw, bh); }
            g.restoreState();
            // control lane
            const auto& ld = laneDef(); const bool bi = ld.lo < 0; auto laneY = [&](double v) { return (float)(H() - 4 - (v - ld.lo) / (ld.hi - ld.lo) * (VEL - 10)); };
            g.setColour(BG); g.fillRect((float)KW(), gb, W() - KW(), (float)VEL); g.setColour(LINE); g.fillRect(0.f, gb, W(), 1.f);
            if (bi) { g.setColour(ink.withAlpha(a(0.055f))); g.fillRect((float)KW(), laneY(0), W() - KW(), 1.f); }
            if (auto* ns = o->notes()) for (auto& n : *ns) { const float x = xOf(n.t); if (x < KW() - 2 || x > W()) continue; const float y = laneY(laneGet(n, V().lane)), y0 = bi ? laneY(0) : H() - 4;
                g.setColour(o->sel.count(n.id) ? ACCENT2 : noteCol(n, ccol)); g.fillRect(x, std::min(y, y0), 3.f, std::max(1.f, std::abs(y0 - y))); g.fillEllipse(x + 1.5f - 3, y - 3, 6, 6); }
            g.setColour(BG2); g.fillRect(0.f, gb + 1, (float)KW(), (float)VEL); g.setColour(DIM); g.setFont(font(10)); g.drawText(juce::String(ld.tag) + u8(" ▾"), 6, (int)gb, KW() - 6, VEL, juce::Justification::centredLeft);
            // ruler
            g.setColour(BG); g.fillRect(0.f, 0.f, W(), (float)RUL);
            if (p && p->loop.b > p->loop.a) { const float lx = std::max((float)KW(), xOf(p->loop.a)); g.setColour(ACCENT2.withAlpha(p->loop.on ? 0.45f : 0.15f)); g.fillRect(lx, 0.f, xOf(p->loop.b) - lx, (float)RUL); }
            g.setColour(DIM); g.setFont(font(10));
            for (int b = std::max(0, (int)std::floor(tOf((float)KW()) / D.p.bar())); xOf(b * D.p.bar()) < W(); b++) { const float x = xOf(b * D.p.bar()); if (x >= KW() - 1) { g.fillRect(x, (float)RUL - 6, 1.f, 6.f); g.drawText(juce::String(b + 1), (int)x + 4, 0, 40, RUL, juce::Justification::centredLeft); } }
            g.setColour(BG); g.fillRect(0, 0, KW(), RUL);
            // keys
            g.saveState(); g.reduceClipRegion(0, RUL, KW(), (int)gb - RUL);
            for (int n = n0; n <= n1; n++) {
                const float y = yOf(n); const bool bk = juce::MidiMessage::isMidiNoteBlack(n);
                g.setColour(o->keysDown.count(n) ? ACCENT : bk ? juce::Colour(0xff23212a) : juce::Colour(0xffebe7e1)); g.fillRect(0.f, y, bk ? KW() * 0.64f : KW() - 1.f, V().rh - (bk ? 0 : 1));
                if (bk) { g.setColour(juce::Colour(0xffebe7e1)); g.fillRect(KW() * 0.64f, y, KW() * 0.36f - 1, V().rh); g.setColour(juce::Colours::black.withAlpha(0.25f)); g.fillRect(KW() * 0.64f, y + V().rh / 2, KW() * 0.36f - 1, 1.f); }
                if (n % 12 == 0 || (c && c->type == "sample" && n == c->root)) { g.setColour(c && n == c->root && c->type == "sample" ? juce::Colour(0xffc2410c) : juce::Colour(0xff55504a)); g.setFont(font(10)); g.drawText(noteName(n), 0, (int)y, KW() - 5, (int)V().rh, juce::Justification::centredRight); }
            }
            g.restoreState();
            g.setColour(LINE); g.fillRect((float)KW() - 1, 0.f, 1.f, H());
            // playhead
            if (D.eng.isPlaying() && !D.song) { const float x = xOf(D.eng.positionTicks()); if (x >= KW() && x <= W()) { g.setColour(ACCENT2); g.fillRect(x, 0.f, 2.f, H()); } }
        }
        void eraseAt(float x, float y) { if (auto* hit = noteAt(x, y)) { const auto id = hit->id; o->removeIf([&](const us::Note& n) { return n.id == id; }); o->sel.erase(id); repaint(); } }
        void sliceAt(float x, float y) { auto* hit = noteAt(x, y); if (!hit) return; const double t = o->snapT(tOf(x)); if (t <= hit->t || t >= hit->t + hit->d) return;
            us::Note z = *hit; z.id = us::newId(); z.t = t; z.d = hit->t + hit->d - t; hit->d = t - hit->t; o->notes(true)->push_back(z); repaint(); }
        void setVel(float x, float y) {
            const auto& ld = laneDef(); double v = juce::jlimit(V().lane == "v" ? 0.02 : ld.lo, ld.hi, ld.lo + (H() - 4 - y) / (VEL - 10) * (ld.hi - ld.lo));
            if (V().lane == "v") V().lastVel = v; const double val = V().lane == "fine" ? std::round(v) : V().lane != "v" && std::abs(v) < 0.04 ? 0 : v;
            for (auto* n : o->targets()) if (std::abs(xOf(n->t) + 1.5f - x) < 7) laneSet(*n, V().lane, val);
            repaint();
        }
        void beginMove(const std::string& type, float x, float y, us::Note& ref) {
            Drag d; d.type = type; d.x = x; d.y = y; d.ref = ref.id; d.lastN = ref.n; if (auto* ns = o->notes()) for (auto& n : *ns) if (o->sel.count(n.id)) d.orig[n.id] = { n.t, n.d, n.n }; drag = d;
        }
        void mouseDown(const juce::MouseEvent& e) override {
            grabKeyboardFocus(); auto& D = o->doc; if (!D.chan()) { toast("Add a channel in the Rack first"); return; }
            const float x = e.position.x, y = e.position.y; const bool right = e.mods.isPopupMenu();
            if (touchMenu()) { touchContext(x, y); return; }   // long press (touch)
            if (e.mods.isMiddleButtonDown() || V().tool == "pan") { Drag d; d.type = "pan"; d.x = x; d.y = y; d.sx = V().sx; d.sy = V().sy; drag = d; return; }
            if (y < RUL) { if (x >= KW() && !right) { Drag d; d.type = "loop"; d.t0 = o->snapT(tOf(x)); drag = d; } return; }
            if (y > H() - VEL) {
                if (x >= KW()) { Drag d; d.type = "vel"; drag = d; setVel(x, y); }
                else { Menu m; m.head("Control lane"); for (auto& l : lanes()) { const std::string id = l.id; m.item(l.label, [this, id] { V().lane = id; repaint(); }, true, V().lane == id); } m.show(atMouse()); }
                return;
            }
            if (x < KW()) { const int n = juce::jlimit(0, 127, nOf(y)); D.audition(n, true); o->keysDown.insert(n); Drag d; d.type = "key"; d.n = n; drag = d; repaint(); return; }
            us::Note* hit = noteAt(x, y); const auto& tool = V().tool;
            if (tool == "zoom") { zoom(right ? 1 / 1.5 : 1.5, x); return; }
            if (tool == "play" && !right) { if (D.eng.isPlaying()) D.eng.stop(); if (D.song) { D.song = false; D.sendChangeMessage(); } D.eng.play(us::Mode::Pattern, D.pat, std::max(0.0, o->snapT(tOf(x), true))); return; }
            if (tool == "mute" && !right) { Drag d; d.type = "mute"; d.muteTo = hit ? !hit->mute : true; drag = d; muteAt(x, y); repaint(); return; }
            if (tool == "paint" && !right) { Drag d; d.type = "paint"; d.n0 = o->lockN(juce::jlimit(0, 127, nOf(y))); d.t0 = o->snapT(tOf(x), true); d.len = V().lastLen; drag = d; paintTo(x); o->audition(d.n0); repaint(); return; }
            if (right || tool == "erase") { Drag d; d.type = "erase"; drag = d; eraseAt(x, y); return; }
            if (tool == "slice") { Drag d; d.type = "slice"; drag = d; sliceAt(x, y); return; }
            if (e.mods.isCtrlDown() || e.mods.isCommandDown()) { if (!e.mods.isShiftDown()) o->sel.clear(); Drag d; d.type = "box"; d.x0 = d.x1 = x; d.y0 = d.y1 = y; d.base = o->sel; drag = d; repaint(); return; }
            if (hit) {
                if (e.mods.isShiftDown()) { if (o->sel.count(hit->id)) o->sel.erase(hit->id); else o->sel.insert(hit->id); repaint(); return; }
                if (!o->sel.count(hit->id)) { o->sel.clear(); o->sel.insert(hit->id); }
                const bool edge = x > xOf(hit->t + hit->d) - std::min(9.f, (float)(hit->d * V().zx * 0.35));
                V().lastLen = hit->d; V().lastVel = hit->v; const int hn = hit->n; beginMove(edge ? "size" : "move", x, y, *hit); if (!edge) o->audition(hn);
            } else if (tool == "draw") {
                us::Note note; note.id = us::newId(); note.t = o->snapT(tOf(x), true); note.d = V().lastLen; note.n = o->lockN(juce::jlimit(0, 127, nOf(y))); note.v = V().lastVel; note.slide = V().slide;
                auto* ns = o->notes(true); ns->push_back(note); o->sel.clear(); o->sel.insert(note.id);
                const auto* st = stampOf(V().stamp);
                if (st) for (size_t i = 1; i < st->size(); i++) { const int n = note.n + (*st)[i]; if (n > 127) break; us::Note z = note; z.id = us::newId(); z.n = n; ns->push_back(z); o->sel.insert(z.id); }
                beginMove("move", x, y, ns->back()); drag->ref = note.id; if (auto* nn = o->notes()) for (auto& q : *nn) if (o->sel.count(q.id)) drag->orig[q.id] = { q.t, q.d, q.n };
                drag->created = !st; o->audition(note.n);
            } else { if (!e.mods.isShiftDown()) o->sel.clear(); Drag d; d.type = "box"; d.x0 = d.x1 = x; d.y0 = d.y1 = y; d.base = o->sel; drag = d; }
            repaint();
        }
        void muteAt(float x, float y) { if (auto* n = noteAt(x, y)) if (drag->touched.insert(n->id).second) n->mute = drag->muteTo; }
        void paintTo(float x) { const double t = o->snapT(tOf(x), true); for (double s = drag->t0; s <= t; s += drag->len) if (drag->made.insert(s).second) { us::Note z; z.id = us::newId(); z.t = s; z.d = drag->len; z.n = drag->n0; z.v = V().lastVel; z.slide = V().slide; o->notes(true)->push_back(z); } }
        void mouseMove(const juce::MouseEvent& e) override {
            auto* hit = noteAt(e.position.x, e.position.y);
            setMouseCursor(V().tool == "pan" ? juce::MouseCursor::DraggingHandCursor : hit && e.position.x > xOf(hit->t + hit->d) - 9 ? juce::MouseCursor::LeftRightResizeCursor : hit ? juce::MouseCursor::DraggingHandCursor : e.position.x < KW() ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::CrosshairCursor);
        }
        void mouseDrag(const juce::MouseEvent& e) override {
            if (!drag) return; auto& d = *drag; const float x = e.position.x, y = e.position.y;
            if (d.type == "pan") { V().sx = d.sx - (x - d.x); V().sy = d.sy - (y - d.y); clampScroll(); }
            else if (d.type == "move") {
                double dt = o->snapD((x - d.x) / V().zx); const int dn = -(int)std::round((y - d.y) / V().rh);
                for (auto& [id, t] : d.orig) dt = std::max(dt, -std::get<0>(t));
                int refN = -1; if (auto* ns = o->notes()) for (auto& n : *ns) { auto it = d.orig.find(n.id); if (it != d.orig.end()) { n.t = std::get<0>(it->second) + dt; n.n = o->lockN(juce::jlimit(0, 127, std::get<2>(it->second) + dn)); if (n.id == d.ref) refN = n.n; } }
                if (refN >= 0 && refN != d.lastN) { d.lastN = refN; o->audition(refN); }
                d.moved = true; o->doc.live();
            } else if (d.type == "size") {
                const double dd = o->snapD((x - d.x) / V().zx), mn = V().snap > 0 ? V().snap : V().snap < 0 ? o->doc.p.bar() : 6;
                if (auto* ns = o->notes()) for (auto& n : *ns) { auto it = d.orig.find(n.id); if (it != d.orig.end()) { const double od = std::get<1>(it->second); n.d = std::max(std::min(mn, od), od + dd); } }
                o->doc.live();
            } else if (d.type == "box") {
                d.x1 = x; d.y1 = y; o->sel = d.base; const double ta = tOf(std::min(d.x0, x)), tb = tOf(std::max(d.x0, x)); const int na = nOf(std::max(d.y0, y)), nb = nOf(std::min(d.y0, y));
                if (auto* ns = o->notes()) for (auto& n : *ns) if (n.t < tb && n.t + n.d > ta && n.n >= na && n.n <= nb) o->sel.insert(n.id);
            } else if (d.type == "erase") eraseAt(x, y);
            else if (d.type == "slice") sliceAt(x, y);
            else if (d.type == "vel") setVel(x, y);
            else if (d.type == "mute") muteAt(x, y);
            else if (d.type == "paint") paintTo(x);
            else if (d.type == "loop") { const double t1 = o->snapT(tOf(x)); if (auto* p = o->doc.patt()) p->loop = { true, std::min(d.t0, t1), std::max(d.t0, t1) }; d.moved = true; }
            else if (d.type == "key") { const int n = juce::jlimit(0, 127, nOf(y)); if (n != d.n) { o->doc.audition(d.n, false); o->keysDown.erase(d.n); o->doc.audition(n, true); o->keysDown.insert(n); d.n = n; } }
            repaint();
        }
        void mouseUp(const juce::MouseEvent&) override {
            if (!drag) return; auto d = *drag; drag.reset();
            if (d.type == "key") { o->doc.audition(d.n, false); o->keysDown.erase(d.n); }
            else if (d.type == "size") { if (auto* ns = o->notes()) for (auto& n : *ns) if (n.id == d.ref) V().lastLen = n.d; o->done(); }
            else if (d.type == "move" || d.type == "erase" || d.type == "slice" || d.type == "vel" || d.type == "mute" || d.type == "paint") o->done();
            else if (d.type == "loop") { if (auto* p = o->doc.patt()) if (!d.moved || p->loop.b - p->loop.a < 3) p->loop.on = false; o->doc.commit(); o->sync(); }
            repaint();
        }
        // double-click a note: its velocity (and the selected notes' when it is one of them)
        void mouseDoubleClick(const juce::MouseEvent& e) override {
            auto* hit = noteAt(e.position.x, e.position.y); if (!hit) return; drag.reset();
            std::vector<std::string> group; if (o->sel.count(hit->id)) group.assign(o->sel.begin(), o->sel.end()); else group = { hit->id };
            const double hv = hit->v; const int hn = hit->n; Menu m; m.head(u8("Velocity · ") + juce::String(juce::roundToInt(hv * 100)) + "%" + (group.size() > 1 ? u8(" · ") + juce::String((int)group.size()) + " notes" : juce::String()));
            for (double v : { 1.0, 0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1 }) m.item(juce::String(juce::roundToInt(v * 100)) + "%", [this, group, v, hn] { if (auto* ns = o->notes()) for (auto& n : *ns) if (std::find(group.begin(), group.end(), n.id) != group.end()) n.v = v; o->doc.commit(); repaint(); if (!o->doc.eng.isPlaying()) { V().lastVel = v; o->audition(hn); } }, true, std::abs(hv - v) < 0.05);
            m.show(atMouse());
        }
        // a long press: the note's actions, or paste / select / tools on empty grid
        void touchContext(float x, float y) {
            if (y < RUL || y > H() - VEL || x < KW()) return; drag.reset();
            auto* hit = noteAt(x, y);
            if (hit) {
                if (!o->sel.count(hit->id)) { o->sel.clear(); o->sel.insert(hit->id); } repaint(); const bool mute = hit->mute, slide = hit->slide; const int n = (int)o->sel.size();
                Menu m; m.head(n > 1 ? juce::String(n) + " notes" : noteName(hit->n) + u8(" · ") + juce::String(juce::roundToInt(hit->v * 100)) + "%");
                m.item("Delete", [this] { o->run([this] { o->del(); }); }).item("Duplicate", [this] { o->run([this] { o->dup(); }); }).item("Copy", [this] { o->copy(); });
                Menu vel; for (double v : { 1.0, 0.85, 0.7, 0.55, 0.4, 0.25 }) vel.item(juce::String(juce::roundToInt(v * 100)) + "%", [this, v] { for (auto* q : o->targets()) q->v = v; o->done(); }); m.sub("Velocity", vel);
                m.item(mute ? "Unmute" : "Mute", [this, mute] { for (auto* q : o->targets()) q->mute = !mute; o->done(); });
                m.item(slide ? "Make normal note" : "Make slide note", [this] { o->slideSel(); });
                m.item("Add to selection by dragging", [this] { V().tool = "select"; o->sync(); toast("Select tool: drag a box around notes"); }).sep();
                m.item("Deselect", [this] { o->sel.clear(); repaint(); });
                m.show(atMouse());
            } else {
                Menu m; m.head("Piano Roll"); m.item("Paste", [this] { o->run([this] { o->paste(-1); }); }).item("Select all", [this] { if (auto* ns = o->notes()) for (auto& n : *ns) o->sel.insert(n.id); repaint(); });
                m.item("Deselect", [this] { o->sel.clear(); repaint(); }, !o->sel.empty()); m.item(u8("Tools…"), [this] { o->toolsMenu(this, true); }); m.show(atMouse());
            }
        }
        void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override {
            const float dy = -w.deltaY * 120, dx = -w.deltaX * 120;
            if (e.mods.isCtrlDown() || e.mods.isCommandDown()) zoom(w.deltaY > 0 ? 1.18 : 0.85, e.position.x);
            else if (e.mods.isAltDown()) { const int mid = nOf(H() / 2); V().rh = juce::jlimit(8.f, 32.f, V().rh + (w.deltaY > 0 ? 2 : -2)); V().sy = (127 - mid) * V().rh - gridH() / 2; }
            else if (e.mods.isShiftDown()) V().sx += dy != 0 ? dy : dx;
            else { V().sy += dy; V().sx += dx; }
            clampScroll(); repaint();
        }
        void mouseMagnify(const juce::MouseEvent& e, float k) override { zoom(k, e.position.x); }
        bool keyPressed(const juce::KeyPress& k) override { return o->rollKey(k); }
    };
    Grid grid;
    friend struct Grid;

    bool rollKey(const juce::KeyPress& k) {
        juce::String id = k.getKeyCode() == juce::KeyPress::backspaceKey ? juce::String("roll.del") : actionFor(k, "roll"); if (id.isEmpty()) return false;
        const double s = snapLen();
        auto nudge = [this](double d) { if (sel.empty()) return; for (auto* n : targets()) n->t = std::max(0.0, n->t + d); done(); };
        auto tr = [this](int d) { if (!sel.empty()) run([this, d] { transpose(d); }); };
        if (id.startsWith("roll.t.")) { V().tool = id.substring(7).toStdString(); sync(); return true; }
        if (id == "roll.del") run([this] { del(); }); else if (id == "roll.all") { if (auto* ns = notes()) for (auto& n : *ns) sel.insert(n.id); grid.repaint(); } else if (id == "roll.none") { sel.clear(); grid.repaint(); }
        else if (id == "roll.copy") copy(); else if (id == "roll.cut") { copy(); run([this] { del(); }); } else if (id == "roll.paste") run([this] { paste(-1); }); else if (id == "roll.dup") run([this] { dup(); });
        else if (id == "roll.quant" || id == "roll.quant2") run([this] { quantize(); }); else if (id == "roll.quantDlg") quantDialog();
        else if (id == "roll.legato") run([this] { legato(); }); else if (id == "roll.chop") run([this] { chop(); }); else if (id == "roll.glue") run([this] { glue(); }); else if (id == "roll.strum") strumDialog();
        else if (id == "roll.slide") slideMode(); else if (id == "roll.slideSel") slideSel();
        else if (id == "roll.up" || id == "roll.sUp") tr(1); else if (id == "roll.down" || id == "roll.sDown") tr(-1); else if (id == "roll.octUp") tr(12); else if (id == "roll.octDown") tr(-12);
        else if (id == "roll.left" || id == "roll.left2") nudge(-s); else if (id == "roll.right" || id == "roll.right2") nudge(s);
        else return false;
        return true;
    }
    void sync() {
        chanBox.clear(juce::dontSendNotification); int i = 1; for (auto& c : doc.p.channels) chanBox.addItem(juce::String(c.name), i++);
        for (size_t k = 0; k < doc.p.channels.size(); k++) if (doc.chan() && doc.p.channels[k].id == doc.chan()->id) chanBox.setSelectedItemIndex((int)k, juce::dontSendNotification);
        for (int t = 0; t < 9; t++) if (V().tool == TOOLS[t]) tools.set(t);
        { auto s = snaps(); for (size_t k = 0; k < s.size(); k++) if (s[k].first == V().snap) snapBox.setSelectedItemIndex((int)k, juce::dontSendNotification); }
        int si = 0; { int k = 1; for (auto& [n, iv] : chords()) { if (n == V().stamp) si = k; k++; } for (auto& [n, iv] : stampScales()) { if (n == V().stamp) si = k; k++; } } stampBox.setSelectedItemIndex(si, juce::dontSendNotification);
        rootBox.setSelectedId(doc.p.keyRoot + 1, juce::dontSendNotification);
        { int k = 1; for (auto& [n, iv] : us::scales()) { if (n == doc.p.keyScale) scaleBox.setSelectedId(k, juce::dontSendNotification); k++; } }
        slideB.setOn(V().slide); lockB.setOn(doc.pb("scaleLock", false)); loopB.setOn(doc.patt() && doc.patt()->loop.on); ghostB.setOn(doc.pb("ghost", true)); followB.setOn(doc.pb("follow", false));
        resized(); grid.repaint();
    }
    void changeListenerCallback(juce::ChangeBroadcaster*) override {
        const std::string c = doc.chan() ? doc.chan()->id : ""; if (c != lastCh) { lastCh = c; sel.clear(); grid.focusNotes(); }
        if (auto* ns = notes()) for (auto it = sel.begin(); it != sel.end();) { bool has = false; for (auto& n : *ns) has |= n.id == *it; it = has ? std::next(it) : sel.erase(it); }
        sync();
    }
    bool wasPlaying = false;
    void timerCallback() override {
        if (!isShowing()) return; const bool pl = doc.eng.isPlaying() && !doc.song;
        if (pl && doc.pb("follow", false) && !grid.drag) { const double t = doc.eng.positionTicks(); const float x = grid.xOf(t); if (x > grid.W()) V().sx += (grid.W() - grid.KW()) * 0.9; else if (x < grid.KW()) V().sx = std::max(0.0, t * V().zx); }
        if (pl || wasPlaying) grid.repaint(); wasPlaying = pl;
    }
public:
    void keyDown(int n, bool on) { if (on) keysDown.insert(n); else keysDown.erase(n); grid.repaint(); }
};

}   // namespace ui
