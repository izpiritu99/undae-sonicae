// Song arranger (app/js/ui/song.js): pattern clips and audio clips on tracks along a bar timeline, a picker panel,
// tools (draw, paint, delete, mute, slip, slice, select, zoom, play), snap in note values, arrangements, automation lanes,
// time markers, a time selection on the ruler (Insert / Delete space), clip groups and colours, and the Song menu.
#pragma once
#include "Doc.h"
#include "Keys.h"
#include "Theme.h"

namespace ui {

class SongView : public juce::Component, public juce::DragAndDropTarget, private juce::ChangeListener, private juce::Timer {
public:
    std::function<void(double)> onPlayFrom;   // (kept for the window)
    double startTick = 0;                     // the play marker in ticks (the window starts the song here)
    explicit SongView(Doc& d) : doc(d) {
        doc.addChangeListener(this);
        for (juce::Component* c : std::initializer_list<juce::Component*>{ &menuB, &tools, &snapL, &snapBox, &arrL, &arrBox, &arrMore, &autoB, &trackB, &zoomOut, &zoomIn, &pal, &tl, &hbar, &vbar }) addAndMakeVisible(c);
        if (isTouch()) for (auto* b : { &copyB, &pasteB, &dupB, &delB }) { addAndMakeVisible(b); b->sm = true; }
        menuB.sm = true; menuB.setTooltip("Song menu: view, edit, tools, snap, select, group, zoom, time markers, arrangements"); menuB.onClick = [this] { songMenu().show(at(&menuB)); };
        static const char* tips[] = { "Draw (P): click to place the picked pattern, drag to move, right edge to resize, right-click to delete", "Paint (B): drag along a track to paint repeated clips",
            "Delete (D): click or drag over clips to delete them", "Mute (T): click or drag over clips to mute / unmute them", "Slip (S): drag inside a clip to move its content, not the clip",
            "Slice (C): click a clip to cut it in two at that point", "Select (E): drag a box to select; right-click a clip for its menu", "Zoom (Z): click to zoom in, right-click to zoom out", "Play (Y): click to start playing the song from there" };
        for (int i = 0; i < 9; i++) tools.tip(i, tips[i]);
        tools.onChange = [this](int i) { tool = TOOLS[i]; };
        snapL.setTooltip("Grid that clips snap to when placed, moved or resized. Hold Alt while dragging to move freely.");
        { int k = 1; for (auto& [v, l] : snaps()) snapBox.addItem(l, k++); }
        snapBox.onChange = [this] { const int i = snapBox.getSelectedItemIndex(); if (i >= 0) { doc.setp("songSnap", snaps()[(size_t)i].first); tl.repaint(); } };
        arrL.setTooltip("Arrangements: separate versions of the song (intro-less radio edit, live set...), each with its own clips");
        arrBox.onChange = [this] { const int i = arrBox.getSelectedItemIndex(); if (i >= 0 && i < (int)doc.p.arrs.size()) switchArr(doc.p.arrs[(size_t)i].id); };
        arrMore.sm = true; arrMore.setTooltip("Add, clone, rename or delete arrangements"); arrMore.onClick = [this] { arrMenu().show(at(&arrMore)); };
        autoB.sm = true; autoB.setTooltip("Add an automation lane: draw how a level, pan or effect setting changes over the song"); autoB.onClick = [this] { autoMenu(); };
        trackB.sm = true; trackB.onClick = [this] { doc.p.tracks++; doc.commit(); };
        zoomOut.sm = zoomIn.sm = true; zoomOut.setTooltip("Zoom out (Ctrl+wheel)"); zoomIn.setTooltip("Zoom in (Ctrl+wheel)");
        zoomOut.onClick = [this] { zoom(0.75); }; zoomIn.onClick = [this] { zoom(1.33); };
        copyB.onClick = [this] { copy(); }; pasteB.onClick = [this] { paste(); }; dupB.onClick = [this] { duplicate(); }; delB.onClick = [this] { if (!picked.empty()) drop(picked); };
        pal.o = this; tl.o = this; tl.setWantsKeyboardFocus(true); tl.getProperties().set("pinchZoom", true);
        hbar.setAutoHide(false); vbar.setAutoHide(false); hbar.addListener(&tl); vbar.addListener(&tl);
        TH = (float)doc.pd("songTH", 46); startTick = doc.songStart * doc.p.bar();
        ensureArrs(); sync(); startTimerHz(30);
    }
    ~SongView() override { doc.removeChangeListener(this); }
    void resized() override {
        auto r = getLocalBounds(); Flow f;
        f.add(menuB, 34).add(tools, tools.idealWidth()).add(snapL, snapL.idealWidth()).add(snapBox, 84).add(arrL, arrL.idealWidth()).add(arrBox, 130).add(arrMore, 30);
        if (isTouch()) for (auto* b : { &copyB, &pasteB, &dupB, &delB }) f.add(*b, b->idealWidth());
        f.right(autoB, autoB.idealWidth()).right(trackB, trackB.idealWidth()).right(zoomOut, 30).right(zoomIn, 30);
        barH = f.layout(r); r.removeFromTop(barH);
        const bool showPal = doc.pb("songPicker", true); pal.setVisible(showPal);
        if (showPal) pal.setBounds(r.removeFromLeft(juce::roundToInt((isTouch() ? 104 : 132) * style().scale)));
        vbar.setBounds(r.removeFromRight(10)); hbar.setBounds(r.removeFromBottom(10)); tl.setBounds(r);
        pal.layout(); tl.updateBars();
    }
    void paint(juce::Graphics& g) override { fillPanel(g); vbarLine(g, barH, getWidth()); if (pal.isVisible()) { g.setColour(LINE); g.fillRect(pal.getRight(), pal.getY(), 1, pal.getHeight()); } }
    // drops: library sounds (audio clips) and picker chips
    bool isInterestedInDragSource(const SourceDetails& s) override { const auto d = s.description.toString(); return d.startsWith("sample:") || d.startsWith("pick:"); }
    void itemDropped(const SourceDetails& s) override {
        const auto p = tl.getLocalPoint(this, s.localPosition); if (!tl.getLocalBounds().contains(p)) return;
        const int track = juce::jlimit(0, doc.p.tracks - 1, (int)std::floor((p.y - RUL + tl.sy) / TH)); const double start = std::max(0.0, snap(tl.barAt((float)p.x), false, true));
        const auto d = s.description.toString();
        if (d.startsWith("pick:pat:")) { if (auto* pt = doc.pattern(d.fromFirstOccurrenceOf("pick:pat:", false, false).toStdString())) { us::Clip c; c.id = us::newId(); c.pat = pt->id; c.track = track; c.start = start; c.len = pt->bars; doc.p.clips.push_back(c); doc.commit(); } return; }
        if (d.startsWith("pick:audio:")) { if (auto* a = audioSource(d.fromFirstOccurrenceOf("pick:audio:", false, false).toStdString())) { us::AudioClip src = *a; audioClipAt(src, track, start); doc.commit(); } return; }
        const std::string id = d.fromFirstOccurrenceOf("sample:", false, false).toStdString();
        if (auto smp = doc.bank.get(id)) { us::AudioClip a; a.id = us::newId(); a.sample = id; a.name = smp->name.substr(0, 24); a.track = track; a.start = start; a.len = smp->seconds() / barSec(); a.bpm = doc.p.bpm; a.stretch = false; doc.p.audio.push_back(a); doc.commit(); }
    }
    static std::function<void()>& renderArmed() { static std::function<void()> f; return f; }   // the window renders armed buses (Alt+R)

private:
    static inline const char* TOOLS[] = { "draw", "paint", "erase", "mute", "slip", "slice", "select", "zoom", "play" };
    static constexpr int RUL = 22; static constexpr float AH = 46;
    std::vector<std::pair<double, juce::String>> snaps() const { const double B = doc.p.bar(); return { { 1, "Bar" }, { 0.5, "1/2 bar" }, { us::PPQ / B, "1/4" }, { us::PPQ / 2 / B, "1/8" }, { us::PPQ / 3 / B, "1/8T" }, { us::PPQ / 4 / B, "1/16" }, { us::PPQ / 6 / B, "1/16T" }, { us::PPQ / 8 / B, "1/32" }, { us::PPQ / 16 / B, "1/64" }, { 0, "Off" } }; }
    static const std::vector<std::pair<const char*, const char*>>& clipColors() { static const std::vector<std::pair<const char*, const char*>> c = { { "#ff7a59", "Coral" }, { "#ffb84d", "Amber" }, { "#e6d15a", "Yellow" }, { "#7bd88f", "Green" }, { "#45d0c4", "Teal" }, { "#5aa9ff", "Blue" }, { "#9d8cff", "Violet" }, { "#f277c6", "Pink" }, { "#e5484d", "Red" }, { "#a3a3a3", "Grey" }, { "#f0ebe2", "Paper" }, { "#6b5b4b", "Brown" } }; return c; }

    Doc& doc; std::string tool = "draw"; float bw = 56, TH = 46; int bars = 32, barH = 40;
    std::set<std::string> picked; std::string srcAudio;   // a picked audio clip source (sample id) to draw with
    struct ClipCopy { bool audio = false; us::Clip c; us::AudioClip a; };
    static std::vector<ClipCopy>& clipboard() { static std::vector<ClipCopy> v; return v; }
    std::optional<std::pair<double, double>> songSel;   // time selection (bars)
    Btn menuB{ u8("☰") }, arrMore{ u8("▾") }, autoB{ "+ Automation" }, trackB{ "+ Track" }, zoomOut{ u8("−") }, zoomIn{ "+" }, copyB{ "Copy" }, pasteB{ "Paste" }, dupB{ "Dup" }, delB{ "Del" };
    Seg tools{ { "", "", "", "", "", "", "", "", "" }, { "draw", "paint", "erase", "mute", "slip", "slice", "select", "zoom", "playfrom" } };
    Cap snapL{ "Snap" }, arrL{ "Arrangement" }; juce::ComboBox snapBox, arrBox; juce::ScrollBar hbar{ false }, vbar{ true };

    double songSnap() const { return doc.pd("songSnap", 1); }
    double barSec() const { return doc.p.bar() * doc.p.secondsPerTick(); }
    double snap(double v, bool free, bool floorIt = false) const { const double s = free ? 0 : songSnap(); return s > 0 ? (floorIt ? std::floor(v / s + 1e-6) : std::round(v / s)) * s : std::round(v * doc.p.bar()) / doc.p.bar(); }
    double grid1() const { const double s = songSnap(); return s > 0 ? s : 1.0 / 16; }
    juce::Colour patColour(const std::string& id) const { for (size_t i = 0; i < doc.p.patterns.size(); i++) if (doc.p.patterns[i].id == id) return doc.p.patterns[i].color.empty() ? PALETTE[i % 8] : hex(doc.p.patterns[i].color); return PALETTE[0]; }
    juce::Colour colourOf(const us::Clip& c) const { return c.color.empty() ? patColour(c.pat) : hex(c.color); }
    juce::Colour colourOf(const us::AudioClip& a) const { return a.color.empty() ? ACCENT2 : hex(a.color); }
    double ratio(const us::AudioClip& a) const { return a.stretch && a.bpm > 0 ? doc.p.bpm / a.bpm : 1.0; }
    const us::AudioClip* audioSource(const std::string& sample) const { for (auto& a : doc.p.audio) if (a.sample == sample) return &a; return nullptr; }
    std::vector<const us::AudioClip*> audioSources() const { std::vector<const us::AudioClip*> r; std::set<std::string> seen; for (auto& a : doc.p.audio) if (seen.insert(a.sample).second) r.push_back(&a); return r; }
    us::AudioClip& audioClipAt(const us::AudioClip& src, int track, double start) { us::AudioClip z = src; z.id = us::newId(); z.track = track; z.start = start; z.off = 0; z.group.clear(); z.mute = false; doc.p.audio.push_back(z); return doc.p.audio.back(); }
    us::Clip* clipById(const std::string& id) { for (auto& c : doc.p.clips) if (c.id == id) return &c; return nullptr; }
    us::AudioClip* audioById(const std::string& id) { for (auto& a : doc.p.audio) if (a.id == id) return &a; return nullptr; }
    // a pattern clip or an audio clip
    struct Ref {
        us::Clip* c = nullptr; us::AudioClip* a = nullptr;
        double& start() { return c ? c->start : a->start; } double& len() { return c ? c->len : a->len; } int& track() { return c ? c->track : a->track; } bool& mute() { return c ? c->mute : a->mute; }
        std::string& group() { return c ? c->group : a->group; } std::string& color() { return c ? c->color : a->color; } const std::string& id() const { return c ? c->id : a->id; } double& off() { return c ? c->off : a->off; }
        explicit operator bool() const { return c || a; }
    };
    Ref ref(const std::string& id) { Ref r; r.c = clipById(id); if (!r.c) r.a = audioById(id); return r; }
    std::vector<Ref> all() { std::vector<Ref> v; for (auto& c : doc.p.clips) { Ref r; r.c = &c; v.push_back(r); } for (auto& a : doc.p.audio) { Ref r; r.a = &a; v.push_back(r); } return v; }
    std::vector<Ref> chosen() { std::vector<Ref> v; for (auto& r : all()) if (picked.count(r.id())) v.push_back(r); return v; }
    std::set<std::string> withGroups(std::set<std::string> ids) { std::set<std::string> gs; for (auto& r : all()) if (ids.count(r.id()) && !r.group().empty()) gs.insert(r.group()); if (!gs.empty()) for (auto& r : all()) if (gs.count(r.group())) ids.insert(r.id()); return ids; }
    double patBars(const us::Clip& c) const { for (auto& p : doc.p.patterns) if (p.id == c.pat) return p.bars; return 1; }
    static juce::String fmtBars(double d) { return juce::String(d, 3).trimCharactersAtEnd("0").trimCharactersAtEnd("."); }

    // --- arrangements: the current one lives in the project's song, the others keep theirs as JSON ----------------------
    void ensureArrs() {
        if (doc.p.arrs.empty()) { doc.p.arrs.push_back({ us::newId(), "Arrangement", {} }); doc.p.arrCur = doc.p.arrs[0].id; }
        bool ok = false; for (auto& a : doc.p.arrs) ok |= a.id == doc.p.arrCur; if (!ok) doc.p.arrCur = doc.p.arrs[0].id;
    }
    void switchArr(const std::string& id) {
        ensureArrs(); if (id == doc.p.arrCur) return; us::Project::Arr* cur = nullptr; us::Project::Arr* next = nullptr;
        for (auto& a : doc.p.arrs) { if (a.id == doc.p.arrCur) cur = &a; if (a.id == id) next = &a; } if (!next) return;
        if (doc.eng.isPlaying()) doc.eng.stop();
        if (cur) cur->song = us::songToVar(doc.p);
        const int tracks = doc.p.tracks; juce::var nv = next->song; next->song = juce::var(); doc.p.arrCur = id;
        if (nv.isObject()) us::songFromVar(doc.p, nv); else { doc.p.clips.clear(); doc.p.audio.clear(); doc.p.autos.clear(); doc.p.markers.clear(); doc.p.tracks = tracks; }
        picked.clear(); doc.commit();
    }
    Menu arrMenu() {
        ensureArrs(); std::string curName; for (auto& a : doc.p.arrs) if (a.id == doc.p.arrCur) curName = a.name;
        Menu m; m.head(u8("Arrangement · ") + juce::String(curName));
        m.item(u8("Add one…"), [this] { ask("Arrangement name", "Arrangement " + juce::String((int)doc.p.arrs.size() + 1), [this](juce::String v) { if (v.isEmpty()) return; us::Project::Arr a{ us::newId(), v.toStdString(), {} }; doc.p.arrs.push_back(a); switchArr(a.id); }); });
        m.item(u8("Clone…"), [this, curName] { ask("Name of the copy", juce::String(curName) + " copy", [this](juce::String v) { if (v.isEmpty()) return;
            us::Project copy = doc.p; for (auto& c : copy.clips) c.id = us::newId(); for (auto& a : copy.audio) a.id = us::newId();
            us::Project::Arr a{ us::newId(), v.toStdString(), us::songToVar(copy) }; doc.p.arrs.push_back(a); switchArr(a.id); }); });
        m.item(u8("Rename…"), [this, curName] { ask("Arrangement name", juce::String(curName), [this](juce::String v) { if (v.isEmpty()) return; for (auto& a : doc.p.arrs) if (a.id == doc.p.arrCur) a.name = v.toStdString(); doc.commit(); }); });
        m.item("Delete", [this] { const auto gone = doc.p.arrCur; std::string other; for (auto& a : doc.p.arrs) if (a.id != gone) { other = a.id; break; } switchArr(other);
            doc.p.arrs.erase(std::remove_if(doc.p.arrs.begin(), doc.p.arrs.end(), [&](auto& a) { return a.id == gone; }), doc.p.arrs.end()); doc.commit(); }, doc.p.arrs.size() > 1);
        return m;
    }

    // --- editing ----------------------------------------------------------------------------------------------------------
    void drop(std::set<std::string> ids) {
        doc.p.clips.erase(std::remove_if(doc.p.clips.begin(), doc.p.clips.end(), [&](auto& c) { return ids.count(c.id) > 0; }), doc.p.clips.end());
        doc.p.audio.erase(std::remove_if(doc.p.audio.begin(), doc.p.audio.end(), [&](auto& c) { return ids.count(c.id) > 0; }), doc.p.audio.end());
        for (auto& i : ids) picked.erase(i); doc.commit();
    }
    void duplicate() {
        auto g = chosen(); if (g.empty()) return; double a = 1e18, b = 0; for (auto& r : g) { a = std::min(a, r.start()); b = std::max(b, r.start() + r.len()); }
        std::vector<us::Clip> nc; std::vector<us::AudioClip> na;
        for (auto& r : g) { if (r.c) { us::Clip z = *r.c; z.id = us::newId(); z.start += b - a; nc.push_back(z); } else { us::AudioClip z = *r.a; z.id = us::newId(); z.start += b - a; na.push_back(z); } }
        picked.clear(); for (auto& z : nc) { doc.p.clips.push_back(z); picked.insert(z.id); } for (auto& z : na) { doc.p.audio.push_back(z); picked.insert(z.id); } doc.commit();
    }
    void shift(double d) { auto g = chosen(); if (g.empty()) return; double m = 1e18; for (auto& r : g) m = std::min(m, r.start()); d = std::max(-m, d); for (auto& r : g) r.start() += d; doc.commit(); }
    void setMute(bool v) { for (auto& r : chosen()) r.mute() = v; doc.commit(); }
    void colour(std::set<std::string> ids, const std::string& col) { for (auto& r : all()) if (ids.count(r.id())) r.color() = col; doc.commit(); }
    // cut a clip in two at a bar position (no commit); returns the new right half's id
    std::string split(const std::string& id, double at) {
        Ref r = ref(id); if (!r || at <= r.start() + 1e-6 || at >= r.start() + r.len() - 1e-6) return {};
        const double left = at - r.start();
        if (r.c) { us::Clip z = *r.c; z.id = us::newId(); z.start = at; z.len = r.c->len - left; const double pl = patBars(*r.c); z.off = std::fmod(std::fmod(r.c->off + left, pl) + pl, pl); r.c->len = left; doc.p.clips.push_back(z); return z.id; }
        us::AudioClip z = *r.a; z.id = us::newId(); z.start = at; z.len = r.a->len - left; z.off = r.a->off + left * barSec() / ratio(*r.a); r.a->len = left; doc.p.audio.push_back(z); return z.id;
    }
    std::pair<double, double> spaceRange() const { if (songSel && songSel->second > songSel->first) return *songSel; const double s = songSnap() > 0 ? songSnap() : 1; return { doc.songStart, doc.songStart + s }; }
    void insertSpace() {
        auto [a, b] = spaceRange(); const double d = b - a; std::vector<std::string> ids; for (auto& r : all()) ids.push_back(r.id()); for (auto& id : ids) split(id, a);
        for (auto& r : all()) if (r.start() >= a - 1e-6) r.start() += d;
        for (auto& l : doc.p.autos) for (auto& q : l.pts) if (q.t >= a * doc.p.bar()) q.t += d * doc.p.bar();
        for (auto& m : doc.p.markers) if (m.t >= a) m.t += d;
        doc.commit(); toast("Inserted " + fmtBars(d) + " bar" + (d == 1 ? "" : "s") + " of space");
    }
    void deleteSpace() {
        auto [a, b] = spaceRange(); const double d = b - a;
        auto keep = [&](Ref r) {
            const double e = r.start() + r.len();
            if (r.start() >= a && e <= b) return false;
            if (r.start() < a && e > a) { r.len() = (a - r.start()) + std::max(0.0, e - b); return true; }
            if (r.start() >= a && r.start() < b) { const double cut = b - r.start(); r.len() = e - b; r.start() = a; if (r.a) r.a->off += cut * barSec() / ratio(*r.a); else { const double pl = patBars(*r.c); r.c->off = std::fmod(std::fmod(r.c->off + cut, pl) + pl, pl); } return true; }
            if (r.start() >= b) r.start() -= d; return true;
        };
        std::set<std::string> gone; for (auto& r : all()) if (!keep(r)) gone.insert(r.id());
        doc.p.clips.erase(std::remove_if(doc.p.clips.begin(), doc.p.clips.end(), [&](auto& c) { return gone.count(c.id) > 0; }), doc.p.clips.end());
        doc.p.audio.erase(std::remove_if(doc.p.audio.begin(), doc.p.audio.end(), [&](auto& c) { return gone.count(c.id) > 0; }), doc.p.audio.end());
        const double B = doc.p.bar();
        for (auto& l : doc.p.autos) { l.pts.erase(std::remove_if(l.pts.begin(), l.pts.end(), [&](auto& q) { return q.t >= a * B && q.t < b * B; }), l.pts.end()); for (auto& q : l.pts) if (q.t >= b * B) q.t -= d * B; if (l.pts.empty()) l.pts.push_back({ 0, 0 }); }
        doc.p.markers.erase(std::remove_if(doc.p.markers.begin(), doc.p.markers.end(), [&](auto& m) { return m.t >= a && m.t < b; }), doc.p.markers.end()); for (auto& m : doc.p.markers) if (m.t >= b) m.t -= d;
        picked.clear(); doc.commit(); toast("Deleted " + fmtBars(d) + " bar" + (d == 1 ? "" : "s") + " of space");
    }
    void quantStarts() { auto g = chosen(); if (g.empty()) g = all(); const double q = songSnap() > 0 ? songSnap() : 1; for (auto& r : g) r.start() = std::max(0.0, std::round(r.start() / q) * q); doc.commit(); toast("Clip starts quantized"); }
    void makeUnique(std::set<std::string> ids) {
        std::vector<us::Pattern> added;
        for (auto& c : doc.p.clips) if (ids.count(c.id)) { auto* p = doc.pattern(c.pat); if (!p) continue; us::Pattern n = *p; n.id = us::newId(); n.name += " (unique)"; for (auto& [k, ns] : n.notes) for (auto& z : ns) z.id = us::newId(); c.pat = n.id; added.push_back(n); }
        for (auto& n : added) doc.p.patterns.push_back(n);
        doc.commit();
    }
    void copy() {
        auto g = chosen(); if (g.empty()) return; double a = 1e18; for (auto& r : g) a = std::min(a, r.start()); clipboard().clear();
        for (auto& r : g) { ClipCopy cc; cc.audio = r.a != nullptr; if (r.c) { cc.c = *r.c; cc.c.start -= a; } else { cc.a = *r.a; cc.a.start -= a; } clipboard().push_back(cc); }
        toast("Copied " + juce::String((int)g.size()) + " clip" + (g.size() == 1 ? "" : "s"));
    }
    void paste() {
        if (clipboard().empty()) return; picked.clear();
        for (auto& cc : clipboard()) { if (cc.audio) { us::AudioClip z = cc.a; z.id = us::newId(); z.start += doc.songStart; doc.p.audio.push_back(z); picked.insert(z.id); } else { us::Clip z = cc.c; z.id = us::newId(); z.start += doc.songStart; doc.p.clips.push_back(z); picked.insert(z.id); } }
        doc.commit();
    }
    void group() { auto g = chosen(); if (g.size() < 2) { toast("Select two or more clips to group"); return; } const auto id = us::newId(); for (auto& r : g) r.group() = id; doc.commit(); toast("Grouped: selecting one selects all of them"); }
    void ungroup() { for (auto& r : chosen()) r.group().clear(); doc.commit(); }
    void selectWhere(std::function<bool(Ref&)> f) { picked.clear(); for (auto& r : all()) if (f(r)) picked.insert(r.id()); tl.repaint(); }
    void bySource() {
        auto g = chosen(); if (g.empty()) return; std::set<std::string> pats, smps; for (auto& r : g) { if (r.c) pats.insert(r.c->pat); else smps.insert(r.a->sample); }
        selectWhere([&](Ref& r) { return r.c ? pats.count(r.c->pat) > 0 : smps.count(r.a->sample) > 0; });
    }
    void addMarker() { us::Marker m; m.id = us::newId(); m.t = doc.songStart; m.name = "Marker " + std::to_string(doc.p.markers.size() + 1); doc.p.markers.push_back(m); doc.commit(); }
    void playFrom(double barPos) {
        if (doc.eng.isPlaying()) doc.eng.stop(); if (!doc.song) { doc.song = true; doc.sendChangeMessage(); }
        doc.songStart = barPos; startTick = barPos * doc.p.bar(); doc.eng.play(us::Mode::Song, doc.pat, startTick);
    }
    void zoom(double k, float vx = -1) { if (vx < 0) vx = tl.getWidth() / 2.f; const double atb = (tl.sx + vx) / bw; bw = (float)juce::jlimit(10.0, 480.0, bw * k); tl.sx = std::max(0.0, atb * bw - vx); tl.updateBars(); tl.repaint(); }
    void zoomFit(double a, double b) { bw = (float)juce::jlimit(10.0, 480.0, (tl.getWidth() - 20) / std::max(1.0, b - a)); tl.sx = a * bw; tl.updateBars(); tl.repaint(); }
    void setTH(double v) { TH = (float)juce::jlimit(22.0, 140.0, std::round(v)); doc.setp("songTH", TH); tl.updateBars(); tl.repaint(); }
    void flip(const char* k) { doc.setp(k, !doc.pb(k, true)); resized(); tl.updateBars(); repaint(); tl.repaint(); }
    Menu colourItems(std::set<std::string> ids, bool patternDefault = true) {
        Menu m; for (auto& [c, n] : clipColors()) { const std::string col = c; m.item(u8("● ") + n, [this, ids, col] { colour(ids, col); }); }
        m.item(patternDefault ? "Back to the pattern colour" : "Default colour", [this, ids] { colour(ids, ""); });
        m.item(u8("Pick any colour…"), [this, ids] { pickColour([this, ids](juce::Colour c) { colour(ids, toHex(c)); }); });
        return m;
    }
    // a colour picker in a call-out; the colour applies when it closes
    struct ColourPick : juce::ColourSelector { std::function<void(juce::Colour)> fn; ColourPick() : juce::ColourSelector(showColourspace | showSliders) { setSize(300, 280); } ~ColourPick() override { if (fn) fn(getCurrentColour()); } };
    void pickColour(std::function<void(juce::Colour)> fn) {
        auto cs = std::make_unique<ColourPick>(); cs->setCurrentColour(ACCENT); cs->fn = [fn](juce::Colour c) { juce::MessageManager::callAsync([fn, c] { fn(c); }); };
        juce::CallOutBox::launchAsynchronously(std::move(cs), menuB.getScreenBounds(), nullptr);
    }
    Menu viewMenu() {
        Menu m; m.head("Track height"); const std::pair<int, const char*> hs[] = { { 26, "Compact" }, { 34, "Small" }, { 46, "Normal" }, { 64, "Large" }, { 90, "Extra large" } };
        for (auto& [v, l] : hs) { const int vv = v; m.item(l, [this, vv] { setTH(vv); }, true, (int)TH == v, juce::String(v) + " px"); }
        m.item("Taller", [this] { setTH(TH * 1.2); }, true, false, "Alt+wheel"); m.item("Shorter", [this] { setTH(TH / 1.2); }, true, false, "Alt+wheel"); m.sep();
        m.item("Picker panel", [this] { flip("songPicker"); }, true, doc.pb("songPicker", true));
        m.item("Clip contents (notes and waveforms)", [this] { flip("songPreview"); }, true, doc.pb("songPreview", true));
        m.item("Automation lanes", [this] { flip("songAutos"); }, true, doc.pb("songAutos", true));
        m.item("Time markers", [this] { flip("songMarkers"); }, true, doc.pb("songMarkers", true));
        return m;
    }
    Menu songMenu() {
        const int n = (int)picked.size(); auto g = chosen(); bool anyPat = false; for (auto& r : g) anyPat |= r.c != nullptr;
        Menu m; auto v = viewMenu(); m.sub("View", v);
        Menu e;
        e.item("Cut", [this] { copy(); drop(picked); }, n > 0, false, "Ctrl+X"); e.item("Copy", [this] { copy(); }, n > 0, false, "Ctrl+C"); e.item("Paste at the play marker", [this] { paste(); }, !clipboard().empty(), false, "Ctrl+V");
        e.item("Duplicate", [this] { duplicate(); }, n > 0, false, "Ctrl+B"); e.item("Delete", [this] { drop(picked); }, n > 0, false, "Del"); e.sep();
        e.item("Shift left", [this] { shift(-grid1()); }, n > 0, false, "one snap step"); e.item("Shift right", [this] { shift(grid1()); }, n > 0); e.sep();
        e.item("Mute", [this] { setMute(true); }, n > 0); e.item("Unmute", [this] { setMute(false); }, n > 0);
        auto ci = colourItems(picked); e.sub("Colour", ci, n > 0); e.item("Make unique (own copy of the pattern)", [this] { makeUnique(picked); }, anyPat); e.sep();
        e.item("Insert space", [this] { insertSpace(); }, true, false, "Ctrl+Ins"); e.item("Delete space", [this] { deleteSpace(); }, true, false, "Ctrl+Del");
        m.sub("Edit", e);
        Menu t; t.item("Quick quantize clip starts", [this] { quantStarts(); }, true, false, "Shift+Q"); t.item("Render armed mixer buses to WAV", [] { if (renderArmed()) renderArmed()(); }, true, false, "Alt+R in the Mixer"); m.sub("Tools", t);
        Menu s; for (auto& [val, l] : snaps()) { const double vv = val; s.item(l, [this, vv] { doc.setp("songSnap", vv); sync(); }, true, std::abs(songSnap() - val) < 1e-9); } m.sub("Snap", s);
        Menu se;
        se.item("Deselect", [this] { selectWhere([](Ref&) { return false; }); }); se.item("Select all", [this] { selectWhere([](Ref&) { return true; }); }, true, false, "Ctrl+A");
        se.item("Select by source", [this] { bySource(); }, n > 0, false, "Shift+C"); se.item("Select muted", [this] { selectWhere([](Ref& r) { return r.mute(); }); });
        se.item("Invert selection", [this] { auto was = picked; selectWhere([was](Ref& r) { return !was.count(r.id()); }); });
        se.item("Select time around selection", [this] { auto gg = chosen(); double a = 1e18, b = 0; for (auto& r : gg) { a = std::min(a, r.start()); b = std::max(b, r.start() + r.len()); } songSel = std::make_pair(a, b); tl.repaint(); }, n > 0);
        m.sub("Select", se);
        Menu gr; gr.item("Group", [this] { group(); }, true, false, "Shift+G"); gr.item("Ungroup", [this] { ungroup(); }, true, false, "Alt+G"); m.sub("Group", gr);
        Menu z; z.item("Zoom in", [this] { zoom(1.33); }); z.item("Zoom out", [this] { zoom(0.75); }); z.item("Zoom out far (whole song)", [this] { zoomFit(0, std::max(4, doc.p.songBars())); });
        z.item("Zoom on selection", [this] { auto gg = chosen(); double a = 1e18, b = 0; for (auto& r : gg) { a = std::min(a, r.start()); b = std::max(b, r.start() + r.len()); } zoomFit(a, b); }, n > 0);
        m.sub("Zoom", z);
        Menu mk; mk.item("Add one at the play marker", [this] { addMarker(); }, true, false, "Alt+T");
        mk.item("Loop the time selection", [this] { doc.songStart = songSel->first; startTick = doc.songStart * doc.p.bar(); toast("Playback starts at the selection; the song loops at its end"); tl.repaint(); }, songSel.has_value());
        mk.item("Clear the time selection", [this] { songSel.reset(); tl.repaint(); }, songSel.has_value());
        for (auto& mm : doc.p.markers) { const auto id = mm.id; mk.item(u8("Delete “") + juce::String(mm.name) + u8("”"), [this, id] { doc.p.markers.erase(std::remove_if(doc.p.markers.begin(), doc.p.markers.end(), [&](auto& x) { return x.id == id; }), doc.p.markers.end()); doc.commit(); }); }
        m.sub("Time markers", mk);
        auto am = arrMenu(); m.sub("Arrangements", am);
        return m;
    }
    void clipMenu(const std::string& id) {
        Ref c = ref(id); if (!c) return;
        auto ids = withGroups(picked.count(id) ? picked : std::set<std::string>{ id }); us::Pattern* p = c.c ? doc.pattern(c.c->pat) : nullptr; const bool m0 = c.mute();
        bool grouped = false; for (auto& r : all()) if (ids.count(r.id()) && !r.group().empty()) grouped = true;
        Menu m; m.head(c.a ? juce::String(c.a->name.empty() ? "Audio clip" : c.a->name) : juce::String(p ? p->name : "Clip"));
        m.item(m0 ? "Unmute" : "Mute", [this, ids, m0] { for (auto& r : all()) if (ids.count(r.id())) r.mute() = !m0; doc.commit(); });
        auto ci = colourItems(ids); m.sub("Colour", ci);
        if (p) { const auto pid = p->id;
            m.item(u8("Rename pattern…"), [this, pid] { if (auto* pp = doc.pattern(pid)) ask("Pattern name", juce::String(pp->name), [this, pid](juce::String v) { if (auto* q = doc.pattern(pid); q && v.isNotEmpty()) { q->name = v.toStdString(); doc.commit(); } }); });
            m.item("Make unique", [this, ids] { makeUnique(ids); }); }
        m.item("Select all from this source", [this, id] { picked = { id }; bySource(); });
        m.item(grouped ? "Ungroup" : "Group with the selection", [this, ids, grouped] { for (auto& i : ids) picked.insert(i); if (grouped) ungroup(); else group(); });
        m.item("Duplicate", [this, ids] { picked = ids; duplicate(); });
        if (c.a) { m.sep(); const auto aid = id;
            m.item(u8("Rename…"), [this, aid] { if (auto* a = audioById(aid)) ask("Clip name", juce::String(a->name.empty() ? "Audio" : a->name), [this, aid](juce::String v) { if (auto* x = audioById(aid); x && v.isNotEmpty()) { x->name = v.toStdString(); doc.commit(); } }); });
            for (auto& b : doc.p.buses) { const auto bid = b.id; m.item("Play through " + doc.busLabel(b.id) + u8(" · ") + juce::String(b.name), [this, aid, bid] { if (auto* a = audioById(aid)) { a->bus = bid; doc.commit(); } }, true, c.a->bus == b.id); }
            m.item("Stretch to project tempo (keeps pitch)", [this, aid] { if (auto* a = audioById(aid)) { const double r0 = ratio(*a); a->stretch = !a->stretch; if (a->bpm <= 0) a->bpm = doc.p.bpm; a->len *= ratio(*a) / r0; doc.commit(); } }, true, c.a->stretch);
        }
        m.sep(); m.item("Delete", [this, ids] { drop(ids); });
        m.show(atMouse());
    }
    void autoMenu() {
        auto& P = doc.p; auto add = [this](const std::string& target, double v) { if (!doc.lane(target)) { us::AutoLane l; l.id = us::newId(); l.target = target; l.pts.push_back({ 0, v }); doc.p.autos.push_back(l); } doc.commit(); };
        Menu m; m.head("Mixer buses");
        for (auto& b : P.buses) { const auto id = b.id; const double vol = b.vol, pan = b.pan;
            m.item(doc.busLabel(b.id) + u8(" · ") + juce::String(b.name) + " level", [add, id, vol] { add("bus:" + id + ":vol", vol); });
            m.item(doc.busLabel(b.id) + u8(" · ") + juce::String(b.name) + " pan", [add, id, pan] { add("bus:" + id + ":pan", pan); }); }
        m.head("Effects");
        for (auto& b : P.buses) for (auto& f : b.fx) if (auto* sp = fxSpec(f.type)) for (auto& q : sp->params) if (q.opts.empty()) { const auto t = "fx:" + b.id + ":" + f.id + ":" + q.k; const double v = f.num(q.k, q.def); m.item(doc.busLabel(b.id) + u8(" · ") + sp->name + " " + q.label, [add, t, v] { add(t, v); }); }
        m.head("Channels");
        for (auto& c : P.channels) { const auto id = c.id; const double vol = c.vol, pan = c.pan;
            m.item(juce::String(c.name) + " level", [add, id, vol] { add("ch:" + id + ":vol", vol); }); m.item(juce::String(c.name) + " pan", [add, id, pan] { add("ch:" + id + ":pan", pan); }); }
        // plug-ins can have hundreds of parameters: each gets its own sub-menu, from what the loaded plug-in reports
        bool head = false;
        auto plugSub = [&](const juce::String& label, const std::string& key, const std::string& prefix, const juce::var& slot) {
            auto pi = doc.eng.plugin(key); Menu s; int k = 0;
            if (pi && pi->ready()) for (auto& q : pi->info().params) { if (q.hidden || q.readonly) continue; const auto t = prefix + "v" + std::to_string(q.id); const double v = slot.getProperty(juce::Identifier("v" + std::to_string(q.id)), q.value);
                s.item(juce::String(q.name.empty() ? "Parameter " + std::to_string(q.id) : q.name), [add, t, v] { add(t, v); }); if (++k > 400) break; }
            if (!head) { m.head("Plug-in parameters"); head = true; }
            if (k) m.sub(label, s); else m.item(label + u8(" (not loaded yet)"), [] {}, false);
        };
        for (auto& b : P.buses) for (auto& f : b.fx) if (f.type == "plugin") { juce::var slot(new juce::DynamicObject()); for (auto& kv : f.p) slot.getDynamicObject()->setProperty(kv.name, kv.value); plugSub(doc.busLabel(b.id) + u8(" · ") + slot.getProperty("name", "Plug-in").toString(), "fx:" + b.id + ":" + f.id, "fx:" + b.id + ":" + f.id + ":", slot); }
        for (auto& c : P.channels) if (c.type == "plugin" && c.plug.isObject()) plugSub(juce::String(c.name), "ch:" + c.id, "plug:" + c.id + ":", c.plug);
        m.show(at(&autoB));
    }

    // --- picker panel -----------------------------------------------------------------------------------------------------
    struct Picker : public juce::Component {
        SongView* o = nullptr; juce::Viewport view; juce::Component inner; Btn more{ u8("▾") }, addPat{ "+ Pattern" };
        struct Chip : juce::Component, juce::SettableTooltipClient {
            Picker* pk = nullptr; std::string kind, id; juce::String name, sub; juce::Colour col; bool on = false;
            void paint(juce::Graphics& g) override {
                auto r = getLocalBounds().toFloat(); juce::Path p; p.addRoundedRectangle(r, RAD() * 0.6f); g.saveState(); g.reduceClipRegion(p);
                g.setColour(BG3.brighter(isMouseOver() ? 0.06f : 0)); g.fillRect(r); g.setColour(col); g.fillRect(r.withWidth(5)); g.restoreState();
                if (on) { g.setColour(col); g.drawRoundedRectangle(r.reduced(1), RAD() * 0.6f, 2); }
                auto tr = getLocalBounds().reduced(11, 3); g.setColour(TEXT); g.setFont(font(kind == "audio" ? FS * 0.92f : FS)); g.drawFittedText((kind == "audio" ? u8("〜 ") : juce::String()) + name, tr.removeFromTop(tr.getHeight() * 11 / 20), juce::Justification::centredLeft, 1, 0.8f);
                g.setColour(DIM); g.setFont(font(FS * 0.78f)); g.drawText(sub, tr, juce::Justification::centredLeft);
            }
            void mouseEnter(const juce::MouseEvent&) override { repaint(); } void mouseExit(const juce::MouseEvent&) override { repaint(); }
            void mouseDown(const juce::MouseEvent& e) override { if (e.mods.isPopupMenu() || touchMenu()) pk->chipMenu(*this); }
            void mouseDrag(const juce::MouseEvent& e) override { if (e.getDistanceFromDragStart() > 6 && !e.mods.isPopupMenu()) if (auto* dc = juce::DragAndDropContainer::findParentDragContainerFor(this)) if (!dc->isDragAndDropActive()) dc->startDragging("pick:" + juce::String(kind) + ":" + juce::String(id), this); }
            void mouseUp(const juce::MouseEvent& e) override { if (!e.mods.isPopupMenu() && !e.mouseWasDraggedSinceMouseDown() && !touchMenu()) pk->chipClick(*this); }
        };
        juce::OwnedArray<Chip> chips;
        Picker() {
            addAndMakeVisible(view); view.setViewedComponent(&inner, false); view.setScrollBarsShown(true, false); inner.addAndMakeVisible(more); inner.addAndMakeVisible(addPat);
            more.sm = true; more.setTooltip("What the picker shows and how it is sorted"); more.onClick = [this] { pickerMenu(); }; addPat.sm = true;
            addPat.onClick = [this] { ask("Pattern name", "Pattern " + juce::String((int)o->doc.p.patterns.size() + 1), [this](juce::String v) { if (v.isEmpty()) return; us::Pattern p; p.id = us::newId(); p.name = v.toStdString(); p.bars = 1; o->doc.p.patterns.push_back(p); o->doc.pat = p.id; o->doc.eng.setPattern(p.id); o->doc.commit(); }); };
        }
        void resized() override { view.setBounds(getLocalBounds()); layout(); }
        void paintOverChildren(juce::Graphics&) override {}
        juce::var opt(const char* k, const juce::var& def) const { auto v = juce::JSON::parse(o->doc.ps("picker", "{}")); return v.isObject() && v.hasProperty(k) ? v.getProperty(k, def) : def; }
        void setOpt(const char* k, const juce::var& v) { auto p = juce::JSON::parse(o->doc.ps("picker", "{}")); if (!p.isObject()) p = juce::var(new juce::DynamicObject()); p.getDynamicObject()->setProperty(k, v); o->doc.setp("picker", juce::JSON::toString(p, true)); build(); }
        struct Head : juce::Component { void paint(juce::Graphics& g) override { g.setColour(DIM); auto f = fontB(FS * 0.8f); f.setExtraKerningFactor(0.06f); g.setFont(f); g.drawText("PICKER", getLocalBounds().withTrimmedLeft(2), juce::Justification::centredLeft); } } head;
        void build() {
            chips.clear(); inner.addAndMakeVisible(head); auto& P = o->doc.p; const auto sort = opt("sort", "order").toString(); const bool used = opt("used", false);
            struct E { std::string kind, id; juce::String name, sub; juce::Colour col; double len; bool on; }; std::vector<E> pats, auds;
            if ((bool)opt("pats", true)) for (auto& p : P.patterns) { bool u = false; for (auto& c : P.clips) u |= c.pat == p.id; if (used && !u) continue;
                pats.push_back({ "pat", p.id, juce::String(p.name), juce::String(juce::roundToInt(p.bars * P.bar() / P.beat())) + " beats", o->patColour(p.id), p.bars, p.id == o->doc.pat && o->srcAudio.empty() }); }
            if ((bool)opt("audio", true)) for (auto* a : o->audioSources()) auds.push_back({ "audio", a->sample, juce::String(a->name.empty() ? "Audio" : a->name), juce::String(a->len * o->barSec(), 1) + " s", o->colourOf(*a), a->len, o->srcAudio == a->sample });
            auto order = [&](std::vector<E>& v) {
                if (sort == "name") std::stable_sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.name < b.name; });
                else if (sort == "colour") std::stable_sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.col.getARGB() < b.col.getARGB(); });
                else if (sort == "len") std::stable_sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.len > b.len; }); };
            order(pats); order(auds);
            for (auto* list : { &pats, &auds }) for (auto& e : *list) { auto* c = chips.add(new Chip()); c->pk = this; c->kind = e.kind; c->id = e.id; c->name = e.name; c->sub = e.sub; c->col = e.col; c->on = e.on;
                c->setTooltip(e.kind == "pat" ? "Click to draw with this pattern, or drag it onto a track" : "Click to draw with this audio clip, or drag it onto a track"); inner.addAndMakeVisible(c); }
            layout();
        }
        void layout() {
            const int w = std::max(40, getWidth() - (view.isVerticalScrollBarShown() ? 10 : 0)); int y = 30; head.setBounds(8, 4, w - 40, 24); more.setBounds(w - 32, 4, 26, 24);
            const int ch = std::max(CTL() + 8, juce::roundToInt((isTouch() ? 46 : 40) * style().scale)); bool audioHead = false;
            for (auto* c : chips) { if (c->kind == "audio" && !audioHead) { audioHead = true; addPat.setBounds(6, y, w - 12, CTL() - 4); y += CTL() + 18; } c->setBounds(6, y, w - 12, ch); y += ch + 5; }
            if (!audioHead) { addPat.setBounds(6, y, w - 12, CTL() - 4); y += CTL(); }
            inner.setSize(w, y + 8);
        }
        void chipClick(Chip& c) { if (c.kind == "pat") { o->srcAudio.clear(); o->doc.selectPattern(c.id); } else o->srcAudio = o->srcAudio == c.id ? std::string() : c.id; build(); }
        void chipMenu(Chip& c) {
            const auto id = c.id; Menu m; m.head(c.name);
            if (c.kind == "pat") {
                m.item(u8("Rename…"), [this, id] { if (auto* p = o->doc.pattern(id)) ask("Pattern name", juce::String(p->name), [this, id](juce::String v) { if (auto* q = o->doc.pattern(id); q && v.isNotEmpty()) { q->name = v.toStdString(); o->doc.commit(); } }); });
                Menu col; for (auto& [cc, n] : clipColors()) { const std::string cs = cc; col.item(u8("● ") + n, [this, id, cs] { if (auto* p = o->doc.pattern(id)) { p->color = cs; o->doc.commit(); } }); }
                col.item("Default colour", [this, id] { if (auto* p = o->doc.pattern(id)) { p->color.clear(); o->doc.commit(); } }); m.sub("Colour", col);
                m.item("Select its clips", [this, id] { o->selectWhere([id](Ref& r) { return r.c && r.c->pat == id; }); });
            } else {
                auto mine = [id](Ref& r) { return r.a && r.a->sample == id; };
                m.item(u8("Rename…"), [this, id, nm = c.name] { ask("Clip name", nm, [this, id](juce::String v) { if (v.isEmpty()) return; for (auto& a : o->doc.p.audio) if (a.sample == id) a.name = v.toStdString(); o->doc.commit(); }); });
                std::set<std::string> ids; for (auto& a : o->doc.p.audio) if (a.sample == id) ids.insert(a.id); auto ci = o->colourItems(ids, false); m.sub("Colour", ci);
                m.item("Select its clips", [this, mine] { o->selectWhere(mine); }); m.item("Delete all its clips", [this, ids] { o->drop(ids); });
            }
            m.show(atMouse());
        }
        void pickerMenu() {
            Menu m; m.head("Picker");
            m.item("Show patterns", [this] { setOpt("pats", !(bool)opt("pats", true)); }, true, opt("pats", true)); m.item("Show audio clips", [this] { setOpt("audio", !(bool)opt("audio", true)); }, true, opt("audio", true));
            m.item("Hide what the song does not use", [this] { setOpt("used", !(bool)opt("used", false)); }, true, opt("used", false)); m.sep(); m.head("Sort");
            const std::pair<const char*, const char*> so[] = { { "order", "In order" }, { "name", "By name" }, { "colour", "By colour" }, { "len", "By length" } };
            for (auto& [k, l] : so) { const juce::String kk = k; m.item(l, [this, kk] { setOpt("sort", kk); }, true, opt("sort", "order").toString() == kk); }
            m.sep(); m.item("Hide the picker panel", [this] { o->flip("songPicker"); }); m.show(at(&more));
        }
    };
    Picker pal;

    // --- the timeline: ruler, tracks with clips, automation lanes ---------------------------------------------------------
    struct Timeline : public juce::Component, public juce::ScrollBar::Listener {
        SongView* o = nullptr; double sx = 0, sy = 0;
        struct Drag {
            std::string type; float x0 = 0, y0 = 0; std::set<std::string> base, done; std::map<std::string, std::tuple<double, int, double>> orig; std::string id; bool resize = false, moved = false, muteTo = false;
            double minS = 0; int minT = 0, maxT = 0; double off0 = 0, a0 = 0; int track = 0; double x0b = 0, len = 0; std::vector<std::string> made; std::string pat, src, col; size_t lane = 0; int pt = -1;
        };
        std::optional<Drag> drag; juce::Rectangle<float> boxR;
        float contentH() const { return RUL + o->doc.p.tracks * o->TH + (o->doc.pb("songAutos", true) ? (float)o->doc.p.autos.size() * AH : 0) + 20; }
        float contentW() const { return o->bars * o->bw; }
        void updateBars() {
            o->bars = std::max(o->bars, std::max(32, o->doc.p.songBars() + 8));
            sx = juce::jlimit(0.0, std::max(0.0, (double)contentW() - getWidth()), sx); sy = juce::jlimit(0.0, std::max(0.0, (double)contentH() - getHeight()), sy);
            o->hbar.setRangeLimits(0, std::max((double)getWidth(), (double)contentW())); o->hbar.setCurrentRange(sx, getWidth(), juce::dontSendNotification);
            o->vbar.setRangeLimits(0, std::max((double)getHeight(), (double)contentH())); o->vbar.setCurrentRange(sy, getHeight(), juce::dontSendNotification);
        }
        void scrollBarMoved(juce::ScrollBar* b, double v) override { if (b == &o->hbar) sx = v; else sy = v; repaint(); }
        void resized() override { updateBars(); }
        double barAt(float x) const { return (x + sx) / o->bw; }
        int trackAt(float y) const { return (int)std::floor((y - RUL + sy) / o->TH); }
        float xOf(double b) const { return (float)(b * o->bw - sx); }
        float yOfTrack(int t) const { return (float)(RUL + t * o->TH - sy); }
        float lanesTop() const { return (float)(RUL + o->doc.p.tracks * o->TH - sy); }
        juce::Rectangle<float> clipRect(Ref& r) const { return { xOf(r.start()), yOfTrack(r.track()) + 2, std::max(4.f, (float)(r.len() * o->bw) - 2), o->TH - 4 }; }
        Ref clipAt(float x, float y) {
            if (y < RUL || y >= lanesTop()) return {};
            auto list = o->all(); for (auto it = list.rbegin(); it != list.rend(); ++it) { if (it->c && !o->doc.pattern(it->c->pat)) continue; if (clipRect(*it).contains(x, y)) return *it; }
            return {};
        }
        void paint(juce::Graphics& g) override {
            auto& D = o->doc; auto& P = D.p; const float bw = o->bw, TH = o->TH, W = (float)getWidth(); const juce::Colour ink = style().light ? juce::Colours::black : juce::Colours::white; const float k = style().gridK;
            fillPanel(g);
            const float top = (float)RUL, lt = lanesTop();
            g.saveState(); g.reduceClipRegion(0, RUL, getWidth(), getHeight() - RUL);
            const double ss = o->songSnap(); const double subB = ss > 0 && ss < 1 && bw * ss >= 7 ? ss : 1.0;
            for (double b = std::floor(sx / bw / subB) * subB; xOf(b) < W; b += subB) { const float x = std::round(xOf(b)); const bool isBar = std::abs(b - std::round(b)) < 1e-6; g.setColour(ink.withAlpha(std::min(1.f, (isBar ? 0.16f : 0.05f) * k))); g.fillRect(x, top, 1.f, lt - top); }
            for (int t = 1; t <= P.tracks; t++) { g.setColour(LINE); g.fillRect(0.f, yOfTrack(t) - 1, W, 1.f); }
            if (o->songSel) { g.setColour(ACCENT.withAlpha(0.12f)); g.fillRect(xOf(o->songSel->first), top, (float)((o->songSel->second - o->songSel->first) * bw), lt - top); }
            for (auto& r : o->all()) {
                auto rc = clipRect(r); if (rc.getRight() < 0 || rc.getX() > W || rc.getBottom() < top || rc.getY() > lt) continue;
                us::Pattern* p = r.c ? D.pattern(r.c->pat) : nullptr; if (r.c && !p) continue;
                juce::Colour col = r.c ? o->colourOf(*r.c) : mix(o->colourOf(*r.a), BG, 0.78f); const bool pk = o->picked.count(r.id()) > 0;
                if (pk) col = col.brighter(0.12f);
                if (r.mute()) col = col.withSaturation(col.getSaturation() * 0.3f).withMultipliedAlpha(0.38f);
                juce::Path cp; cp.addRoundedRectangle(rc, RAD() * 0.5f); g.saveState(); g.reduceClipRegion(cp);
                g.setColour(col); g.fillRect(rc); g.setColour(juce::Colours::black.withAlpha(0.18f * (r.mute() ? 0.38f : 1))); g.fillRect(rc.withLeft(rc.getRight() - 6));
                const juce::Colour ink2 = juce::Colour(0xff16120f).withAlpha(r.mute() ? 0.4f : 1.f);
                juce::String label = r.c ? juce::String(p->name) + (r.c->off != 0 ? u8(" ↔") : "") : juce::String(r.a->name.empty() ? "Audio" : r.a->name) + u8(" → ") + D.busLabel(r.a->bus) + (std::abs(o->ratio(*r.a) - 1) > 0.002 ? u8(" · ") + juce::String(juce::roundToInt(100 / o->ratio(*r.a))) + "% speed" : juce::String());
                if (!r.group().empty()) label = u8("⛓ ") + label;
                g.setColour(ink2); g.setFont(fontB(FS * 0.76f)); g.drawText(label, rc.toNearestInt().reduced(6, 0).removeFromTop(17), juce::Justification::centredLeft, false);
                if (D.pb("songPreview", true) && TH >= 30) { auto pr = rc.withTrimmedTop(18).withTrimmedBottom(2); if (r.c) drawPreview(g, *p, pr, r.c->off); else drawWave(g, *r.a, pr); }
                g.restoreState();
                if (pk) { g.setColour(TEXT); g.drawRoundedRectangle(rc.reduced(1), RAD() * 0.5f, 2); }
            }
            if (drag && drag->type == "box") { g.setColour(ACCENT2.withAlpha(0.16f)); g.fillRect(boxR); g.setColour(ACCENT2); g.drawRect(boxR, 1); }
            if (D.pb("songAutos", true)) for (size_t i = 0; i < P.autos.size(); i++) {
                auto& l = P.autos[i]; auto tg = D.target(l.target); const float y0 = lt + i * AH; if (y0 > getHeight() || y0 + AH < top) continue;
                g.setColour(ACCENT2.withAlpha(0.04f)); g.fillRect(0.f, y0, W, AH); g.setColour(LINE); g.fillRect(0.f, y0, W, 1.f);
                if (!tg) continue;
                auto yv = [&](double v) { return y0 + 4 + (float)(1 - (v - tg->min) / (tg->max - tg->min)) * (AH - 8); };
                if (!l.pts.empty()) { juce::Path pth; pth.startNewSubPath(0, yv(l.pts[0].v)); for (auto& q : l.pts) pth.lineTo(xOf(q.t / P.bar()), yv(q.v)); pth.lineTo(W, yv(l.pts.back().v)); g.setColour(ACCENT2); g.strokePath(pth, juce::PathStrokeType(2)); }
                for (auto& q : l.pts) { const float x = xOf(q.t / P.bar()), y = yv(q.v); g.setColour(BG2); g.fillEllipse(x - 4.5f, y - 4.5f, 9, 9); g.setColour(ACCENT2); g.drawEllipse(x - 4.5f, y - 4.5f, 9, 9, 2); }
                auto lab = laneLabel(i); g.setColour(BG2); g.fillRoundedRectangle(lab, 9); g.setColour(LINE); g.drawRoundedRectangle(lab, 9, 1); g.setColour(TEXT); g.setFont(fontB(FS * 0.72f)); g.drawText(tg->label, lab.reduced(7, 0), juce::Justification::centredLeft, true);
            }
            if (D.eng.isPlaying() && D.song) { const float x = xOf(D.eng.positionTicks() / P.bar()); g.setColour(ACCENT2); g.fillRect(x, top, 2.f, (float)getHeight()); }
            g.restoreState();
            // ruler
            g.setColour(BG); g.fillRect(0.f, 0.f, W, (float)RUL); g.setColour(LINE); g.fillRect(0.f, (float)RUL - 1, W, 1.f);
            if (o->songSel) { g.setColour(ACCENT.withAlpha(0.45f)); g.fillRect(xOf(o->songSel->first), 0.f, (float)((o->songSel->second - o->songSel->first) * bw), (float)RUL); }
            g.setFont(font(FS * 0.78f));
            for (int b = std::max(0, (int)(sx / bw)); xOf(b) < W; b++) { const float x = xOf(b); g.setColour(LINE); g.fillRect(x, 4.f, 1.f, 18.f); if (bw >= 30 || b % 4 == 0) { g.setColour(DIM); g.drawText(juce::String(b + 1), (int)x + 4, 4, 40, 16, juce::Justification::centredLeft); } }
            if (D.pb("songMarkers", true)) for (auto& m : P.markers) { auto mr = markerRect(m); g.setColour(ACCENT2); g.fillRoundedRectangle(mr, 6); g.fillRect(mr.withWidth(6)); g.setColour(juce::Colour(0xff16120f)); g.setFont(fontB(FS * 0.72f)); g.drawText(juce::String(m.name), mr.reduced(6, 0), juce::Justification::centredLeft, false); }
            { const float x = xOf(D.songStart); juce::Path t; t.addTriangle(x - 6, 0, x + 6, 0, x, 9); g.setColour(ACCENT); g.fillPath(t); }
        }
        juce::Rectangle<float> markerRect(const us::Marker& m) const { return { xOf(m.t), 0.f, (float)textW(fontB(FS * 0.72f), juce::String(m.name)) + 12, (float)RUL }; }
        juce::Rectangle<float> laneLabel(size_t i) const { auto tg = o->doc.target(o->doc.p.autos[i].target); const float w = (float)textW(fontB(FS * 0.72f), tg ? tg->label : juce::String()) + 16; return { 4.f, lanesTop() + i * AH + 3, w, 18.f }; }
        void drawPreview(juce::Graphics& g, const us::Pattern& p, juce::Rectangle<float> r, double off) {
            std::vector<const us::Note*> ns; int lo = 127, hi = 0; for (auto& [k, l] : p.notes) for (auto& n : l) { ns.push_back(&n); lo = std::min(lo, n.n); hi = std::max(hi, n.n); } if (ns.empty()) return;
            const float span = (float)std::max(1, hi - lo); const double pl = p.bars * o->doc.p.bar(); const float kx = o->bw / o->doc.p.bar(); g.setColour(juce::Colours::black.withAlpha(0.55f));
            const double offT = off * o->doc.p.bar();
            for (float x0 = (float)(-offT * kx); x0 < r.getWidth(); x0 += (float)(pl * kx)) for (auto* n : ns) if (n->t < pl) {
                const float x = r.getX() + x0 + (float)(n->t * kx); if (x + n->d * kx < r.getX() || x > r.getRight()) continue;
                g.fillRect(x, r.getY() + 2 + (1 - (n->n - lo) / span) * (r.getHeight() - 6), std::max(1.5f, (float)(n->d * kx) - 0.5f), 2.f); }
        }
        void drawWave(juce::Graphics& g, const us::AudioClip& a, juce::Rectangle<float> r) {
            auto s = o->doc.bank.get(a.sample); if (!s || s->ch.empty()) return; const auto& d = s->ch[0]; const double sr = s->sr; const size_t st = (size_t)std::max(0.0, a.off * sr); if (st >= d.size()) return;
            const double n = std::min((double)d.size() - st, a.len * o->barSec() / o->ratio(a) * sr); const int W = (int)std::min(4000.f, r.getWidth()); if (W <= 0 || n <= 0) return;
            const double per = n / W; const size_t stride = std::max<size_t>(1, (size_t)(per / 40)); g.setColour(juce::Colours::black.withAlpha(0.6f));
            for (int x = 0; x < W; x++) { float m = 0; for (size_t i = st + (size_t)(x * per), e = std::min(d.size(), st + (size_t)(x * per + per)); i < e; i += stride) m = std::max(m, std::abs(d[i])); const float y = std::max(1.f, m * r.getHeight()); g.fillRect(r.getX() + x, r.getCentreY() - y / 2, 1.f, y); }
        }
        // --- mouse ---
        void mouseDown(const juce::MouseEvent& e) override {
            grabKeyboardFocus(); auto& D = o->doc; const float x = e.position.x, y = e.position.y; const bool right = e.mods.isPopupMenu(), touch = touchMenu(); drag.reset();
            if (y < RUL) {   // ruler: markers, the time selection, the play marker
                if (D.pb("songMarkers", true)) for (auto& m : D.p.markers) if (markerRect(m).contains(x, y)) { const auto id = m.id; if (right || touch) markerMenu(id); else { D.songStart = m.t; o->startTick = m.t * D.p.bar(); repaint(); } return; }
                if (right) return; Drag d; d.type = "ruler"; d.x0 = x; d.a0 = std::max(0.0, o->snap(barAt(x), e.mods.isAltDown())); drag = d; return;
            }
            if (y >= lanesTop() && D.pb("songAutos", true)) { autoDown(e); return; }
            Ref c = clipAt(x, y); const auto& tool = o->tool;
            if (right || touch) {
                if (tool == "zoom") { o->zoom(1 / 1.5, x); return; }
                if (!c) return;
                if (tool == "select" || touch || c.a) { o->clipMenu(c.id()); return; }   // a long press never deletes
                o->drop(o->withGroups(o->picked.count(c.id()) ? o->picked : std::set<std::string>{ c.id() })); return;
            }
            if (tool == "zoom") { o->zoom(1.5, x); return; }
            if (tool == "play") { o->playFrom(std::max(0.0, o->snap(barAt(x), e.mods.isAltDown(), true))); return; }
            if (tool == "erase") { Drag d; d.type = "erase"; drag = d; eraseAt(x, y); return; }
            if (tool == "mute") { if (!c) return; Drag d; d.type = "mute"; d.muteTo = !c.mute(); drag = d; muteAt(x, y); return; }
            if (tool == "slice") { if (c && !o->split(c.id(), o->snap(barAt(x), e.mods.isAltDown())).empty()) D.commit(); return; }
            if (tool == "slip") { if (!c) return; Drag d; d.type = "slip"; d.id = c.id(); d.x0 = x; d.off0 = c.off(); drag = d; return; }
            if (tool == "paint") { paintDown(e, c); return; }
            const bool mod = e.mods.isCtrlDown() || e.mods.isCommandDown() || (tool == "select" && !c);
            if (!c && !mod) { Drag d; d.type = "empty"; d.x0 = x; d.y0 = y; drag = d; return; }
            if (!c) { Drag d; d.type = "box"; d.x0 = x; d.y0 = y; if (e.mods.isShiftDown()) d.base = o->picked; drag = d; return; }
            if (mod) { if (o->picked.count(c.id())) o->picked.erase(c.id()); else o->picked.insert(c.id()); repaint(); return; }
            if (!o->picked.count(c.id())) o->picked = o->withGroups({ c.id() });
            Drag d; d.type = "move"; d.id = c.id(); d.x0 = x; d.y0 = y; d.resize = x > clipRect(c).getRight() - 10; d.minS = 1e18; d.minT = 1 << 20; d.maxT = 0;
            for (auto& r : o->chosen()) { d.orig[r.id()] = { r.start(), r.track(), r.len() }; d.minS = std::min(d.minS, r.start()); d.minT = std::min(d.minT, r.track()); d.maxT = std::max(d.maxT, r.track()); }
            drag = d; repaint();
        }
        void eraseAt(float x, float y) {
            Ref c = clipAt(x, y); if (!c || drag->done.count(c.id())) return; for (auto& i : o->withGroups({ c.id() })) drag->done.insert(i);
            auto& P = o->doc.p; auto& dn = drag->done;
            P.clips.erase(std::remove_if(P.clips.begin(), P.clips.end(), [&](auto& q) { return dn.count(q.id) > 0; }), P.clips.end());
            P.audio.erase(std::remove_if(P.audio.begin(), P.audio.end(), [&](auto& q) { return dn.count(q.id) > 0; }), P.audio.end()); repaint();
        }
        void muteAt(float x, float y) { Ref c = clipAt(x, y); if (c && drag->done.insert(c.id()).second) { c.mute() = drag->muteTo; repaint(); } }
        // paint: drag right along a track, copies of the clip under the pointer (or the picked pattern / audio clip) one after another
        void paintDown(const juce::MouseEvent& e, Ref c0) {
            auto& D = o->doc; Drag d; d.type = "paint"; const float x = e.position.x, y = e.position.y;
            if (!c0 && !o->srcAudio.empty()) if (auto* a = o->audioSource(o->srcAudio)) { d.src = o->srcAudio; d.track = juce::jlimit(0, D.p.tracks - 1, trackAt(y)); d.x0b = std::max(0.0, o->snap(barAt(x), e.mods.isAltDown(), true)); d.len = a->len; drag = d; placePaint(0); return; }
            us::Pattern* p = c0 && c0.c ? D.pattern(c0.c->pat) : D.patt(); if (!p) return;
            d.pat = p->id; d.track = c0 ? c0.track() : juce::jlimit(0, D.p.tracks - 1, trackAt(y)); d.len = c0 && c0.c ? c0.c->len : p->bars;
            d.x0b = c0 ? c0.start() + c0.len() : std::max(0.0, o->snap(barAt(x), e.mods.isAltDown(), true)); if (c0 && c0.c) d.col = c0.c->color;
            drag = d; if (!c0) placePaint(0); else paintTo(x);
        }
        void placePaint(size_t k) {
            auto& d = *drag; auto& D = o->doc;
            if (!d.src.empty()) { if (auto* a = o->audioSource(d.src)) { us::AudioClip src = *a; auto& z = o->audioClipAt(src, d.track, d.x0b + k * d.len); d.made.push_back(z.id); } }
            else { us::Clip z; z.id = us::newId(); z.pat = d.pat; z.track = d.track; z.start = d.x0b + k * d.len; z.len = d.len; z.color = d.col; D.p.clips.push_back(z); d.made.push_back(z.id); }
            repaint();
        }
        void paintTo(float x) { auto& d = *drag; const double end = barAt(x); while (d.len > 0 && d.x0b + d.made.size() * d.len < end) placePaint(d.made.size()); }
        void autoDown(const juce::MouseEvent& e) {
            auto& P = o->doc.p; const float y = e.position.y, x = e.position.x; const size_t i = (size_t)((y - lanesTop()) / AH); if (i >= P.autos.size()) return;
            auto& l = P.autos[i]; auto tg = o->doc.target(l.target); if (!tg) return;
            if (laneLabel(i).contains(x, y)) { const auto lid = l.id; Menu m; m.head(tg->label);
                m.item("Flatten to the current value", [this, lid] { for (auto& a : o->doc.p.autos) if (a.id == lid) if (auto t = o->doc.target(a.target)) a.pts = { { 0, t->get() } }; o->doc.commit(); });
                m.item("Delete lane", [this, lid] { auto& A = o->doc.p.autos; A.erase(std::remove_if(A.begin(), A.end(), [&](auto& a) { return a.id == lid; }), A.end()); o->doc.commit(); });
                m.show(atMouse()); return; }
            const float y0 = lanesTop() + i * AH; int hit = -1;
            for (size_t k = 0; k < l.pts.size(); k++) { const float px = xOf(l.pts[k].t / P.bar()), py = y0 + 4 + (float)(1 - (l.pts[k].v - tg->min) / (tg->max - tg->min)) * (AH - 8); if (std::hypot(px - x, py - y) < 7) hit = (int)k; }
            if (e.mods.isPopupMenu() || touchMenu()) { if (hit >= 0 && l.pts.size() > 1) { l.pts.erase(l.pts.begin() + hit); o->doc.commit(); } return; }
            Drag d; d.type = "auto"; d.lane = i; d.pt = hit;
            if (hit < 0) { const auto np = autoAt(e, i, *tg); l.pts.push_back(np); std::sort(l.pts.begin(), l.pts.end(), [](auto& a, auto& b) { return a.t < b.t; }); for (size_t k = 0; k < l.pts.size(); k++) if (l.pts[k].t == np.t && l.pts[k].v == np.v) d.pt = (int)k; }
            drag = d; repaint();
        }
        us::AutoPt autoAt(const juce::MouseEvent& e, size_t i, const Doc::Target& tg) {
            const float y0 = lanesTop() + i * AH;
            return { std::max(0.0, std::round(o->snap(barAt(e.position.x), e.mods.isAltDown()) * o->doc.p.bar())), juce::jlimit(tg.min, tg.max, tg.min + (1 - (e.position.y - y0 - 4) / (AH - 8)) * (tg.max - tg.min)) };
        }
        void mouseDrag(const juce::MouseEvent& e) override {
            if (!drag) return; auto& d = *drag; auto& D = o->doc; const float x = e.position.x, y = e.position.y;
            if (d.type == "ruler") { if (std::abs(x - d.x0) < 4 && !d.moved) return; d.moved = true; const double b = std::max(0.0, o->snap(barAt(x), e.mods.isAltDown())); o->songSel = std::make_pair(std::min(d.a0, b), std::max(d.a0, b)); }
            else if (d.type == "erase") eraseAt(x, y);
            else if (d.type == "mute") muteAt(x, y);
            else if (d.type == "paint") paintTo(x);
            else if (d.type == "slip") {
                Ref c = o->ref(d.id); if (!c) return; const double dd = (x - d.x0) / o->bw;
                if (c.a) c.a->off = std::max(0.0, d.off0 - dd * o->barSec() / o->ratio(*c.a)); else { const double pl = o->patBars(*c.c); c.c->off = std::fmod(std::fmod(d.off0 - o->snap(dd, e.mods.isAltDown()), pl) + pl, pl); }
                D.live();
            }
            else if (d.type == "box" || d.type == "empty") {
                if (d.type == "empty") { if (e.getDistanceFromDragStart() < 5) return; d.type = "box"; }
                boxR = juce::Rectangle<float>(juce::Point<float>(d.x0, d.y0), e.position); o->picked = d.base;
                for (auto& r : o->all()) if (clipRect(r).intersects(boxR)) o->picked.insert(r.id());
            }
            else if (d.type == "move") {
                Ref c = o->ref(d.id); if (!c) return; const double dx = (x - d.x0) / o->bw; const int dt = juce::jlimit(-d.minT, D.p.tracks - 1 - d.maxT, (int)std::round((y - d.y0) / o->TH));
                auto [s0, t0, l0] = d.orig[d.id]; juce::ignoreUnused(t0);
                if (d.resize) {
                    const double s = o->songSnap() > 0 && !e.mods.isAltDown() ? o->songSnap() : 1.0 / 16; double mx = 1e18;
                    if (c.a) if (auto smp = D.bank.get(c.a->sample)) mx = (smp->seconds() - c.a->off) * o->ratio(*c.a) / o->barSec();
                    c.len() = juce::jlimit(std::min(s, mx), mx, o->snap(s0 + l0 + dx, e.mods.isAltDown()) - s0); d.moved |= c.len() != l0;
                } else {
                    const double dd = std::max(-d.minS, o->snap(s0 + dx, e.mods.isAltDown()) - s0);
                    for (auto& [id, q] : d.orig) { Ref r = o->ref(id); if (r) { r.start() = std::get<0>(q) + dd; r.track() = std::get<1>(q) + dt; } }
                    d.moved |= dd != 0 || dt != 0;
                }
                D.live();
            }
            else if (d.type == "auto") {
                auto& l = D.p.autos[d.lane]; auto tg = D.target(l.target); if (!tg || d.pt < 0 || d.pt >= (int)l.pts.size()) return;
                l.pts[(size_t)d.pt] = autoAt(e, d.lane, *tg); const auto keep = l.pts[(size_t)d.pt];
                std::sort(l.pts.begin(), l.pts.end(), [](auto& a, auto& b) { return a.t < b.t; }); for (size_t k = 0; k < l.pts.size(); k++) if (l.pts[k].t == keep.t && l.pts[k].v == keep.v) d.pt = (int)k;
                D.live();
            }
            repaint();
        }
        void mouseUp(const juce::MouseEvent& e) override {
            if (!drag) return; auto d = *drag; drag.reset(); auto& D = o->doc;
            if (d.type == "ruler") { if (!d.moved) { o->songSel.reset(); D.songStart = std::max(0.0, std::floor(barAt(e.position.x))); } else D.songStart = o->songSel->first; o->startTick = D.songStart * D.p.bar(); }
            else if (d.type == "empty") {   // a click on an empty spot: deselect, or place a clip with the draw tool
                if (o->tool != "draw") { repaint(); return; }
                if (!o->picked.empty()) { o->picked.clear(); repaint(); return; }
                const int track = trackAt(e.position.y); if (track < 0 || track >= D.p.tracks) return; const double start = std::max(0.0, o->snap(barAt(e.position.x), e.mods.isAltDown(), true));
                if (auto* a = o->srcAudio.empty() ? nullptr : o->audioSource(o->srcAudio)) { us::AudioClip src = *a; o->audioClipAt(src, track, start); }
                else if (auto* p = D.patt()) { us::Clip c; c.id = us::newId(); c.pat = p->id; c.track = track; c.start = start; c.len = p->bars; D.p.clips.push_back(c); }
                D.commit();
            }
            else if (d.type == "box") boxR = {};
            else if (d.type == "erase") { if (!d.done.empty()) { for (auto& i : d.done) o->picked.erase(i); D.commit(); } }
            else if (d.type == "mute" || d.type == "slip" || d.type == "auto") D.commit();
            else if (d.type == "paint") { if (!d.made.empty()) D.commit(); }
            else if (d.type == "move") { if (d.moved) D.commit(); else { Ref c = o->ref(d.id); if (c && c.c && D.pat != c.c->pat) D.selectPattern(c.c->pat); } }
            repaint();
        }
        void markerMenu(const std::string& id) {
            us::Marker* m = nullptr; for (auto& x : o->doc.p.markers) if (x.id == id) m = &x; if (!m) return;
            Menu mm; mm.head(juce::String(m->name));
            mm.item(u8("Rename…"), [this, id, n = m->name] { ask("Marker name", juce::String(n), [this, id](juce::String v) { for (auto& x : o->doc.p.markers) if (x.id == id && v.isNotEmpty()) x.name = v.toStdString(); o->doc.commit(); }); });
            mm.item("Delete", [this, id] { auto& M = o->doc.p.markers; M.erase(std::remove_if(M.begin(), M.end(), [&](auto& x) { return x.id == id; }), M.end()); o->doc.commit(); });
            mm.show(atMouse());
        }
        void mouseMove(const juce::MouseEvent& e) override {
            Ref c = clipAt(e.position.x, e.position.y); const auto& t = o->tool;
            setMouseCursor(t == "erase" || t == "slice" ? juce::MouseCursor::CrosshairCursor : t == "slip" && c ? juce::MouseCursor::LeftRightResizeCursor : t == "mute" && c ? juce::MouseCursor::PointingHandCursor
                : c && e.position.x > clipRect(c).getRight() - 10 ? juce::MouseCursor::LeftRightResizeCursor : c ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        }
        void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override {
            if (e.mods.isAltDown() && !e.mods.isCtrlDown()) { o->setTH(o->TH * (w.deltaY > 0 ? 1.1 : 1 / 1.1)); return; }
            if (e.mods.isCtrlDown() || e.mods.isCommandDown()) { o->zoom(w.deltaY > 0 ? 1.2 : 1 / 1.2, e.position.x); return; }
            if (e.mods.isShiftDown()) sx -= (w.deltaY != 0 ? w.deltaY : w.deltaX) * 240; else { sy -= w.deltaY * 240; sx -= w.deltaX * 240; }
            updateBars(); repaint();
        }
        void mouseMagnify(const juce::MouseEvent& e, float k) override { o->zoom(k, e.position.x); }
        bool keyPressed(const juce::KeyPress& k) override {
            const juce::String id = k.getKeyCode() == juce::KeyPress::backspaceKey ? juce::String("song.del") : actionFor(k, "song"); if (id.isEmpty()) return false;
            if (id.startsWith("song.t.")) { const auto t = id.substring(7).toStdString(); for (int i = 0; i < 9; i++) if (t == TOOLS[i]) { o->tools.set(i); o->tool = t; } return true; }
            if (id == "song.del") { if (!o->picked.empty()) o->drop(o->picked); }
            else if (id == "song.all") o->selectWhere([](Ref&) { return true; });
            else if (id == "song.none") { o->picked.clear(); o->songSel.reset(); repaint(); }
            else if (id == "song.dup") o->duplicate(); else if (id == "song.source") o->bySource(); else if (id == "song.group") o->group(); else if (id == "song.ungroup") o->ungroup();
            else if (id == "song.insert") o->insertSpace(); else if (id == "song.space") o->deleteSpace(); else if (id == "song.quant") o->quantStarts(); else if (id == "song.marker") o->addMarker();
            else if (id == "song.copy") o->copy(); else if (id == "song.cut") { o->copy(); if (!o->picked.empty()) o->drop(o->picked); } else if (id == "song.paste") o->paste();
            else return false;
            return true;
        }
    };
    Timeline tl;

    void sync() {
        ensureArrs();
        for (auto it = picked.begin(); it != picked.end();) it = ref(*it) ? std::next(it) : picked.erase(it);
        if (!srcAudio.empty() && !audioSource(srcAudio)) srcAudio.clear();
        { auto s = snaps(); for (size_t k = 0; k < s.size(); k++) if (std::abs(s[k].first - songSnap()) < 1e-9) snapBox.setSelectedItemIndex((int)k, juce::dontSendNotification); }
        arrBox.clear(juce::dontSendNotification); int i = 1; for (auto& a : doc.p.arrs) arrBox.addItem(juce::String(a.name), i++);
        for (size_t k = 0; k < doc.p.arrs.size(); k++) if (doc.p.arrs[k].id == doc.p.arrCur) arrBox.setSelectedItemIndex((int)k, juce::dontSendNotification);
        for (int t = 0; t < 9; t++) if (tool == TOOLS[t]) tools.set(t);
        startTick = doc.songStart * doc.p.bar();
        pal.build(); resized(); tl.updateBars(); tl.repaint();
    }
    void changeListenerCallback(juce::ChangeBroadcaster*) override { sync(); }
    bool wasPlaying = false;
    void timerCallback() override {
        if (!isShowing()) return; const bool pl = doc.eng.isPlaying() && doc.song;
        if (pl) { const float x = (float)(doc.eng.positionTicks() / doc.p.bar() * bw); if (x > (bars - 4) * bw) { bars += 16; tl.updateBars(); }
            if (x > tl.sx + tl.getWidth() - 30 || x < tl.sx) { tl.sx = std::max(0.0, (double)x - 40); tl.updateBars(); } }
        if (pl || wasPlaying) tl.repaint(); wasPlaying = pl;
    }
};

}   // namespace ui
