// Channel rack (app/js/ui/rack.js): a step sequencer with Notes and Wave row views. Each row: mute LED, solo, mixer bus
// box (click to choose, scroll to step), pan and level knobs, the channel name (click: select, Ctrl+click: select several,
// double-click: instrument, right-click: menu), Piano Roll and options buttons, then the steps (or the row view). Drag
// across steps to paint, right-drag removes. Rack hotkeys (Ctrl+L, Alt+C, Alt+G, Alt+Del) act on the selected channels.
#pragma once
#include "Doc.h"
#include "Keys.h"
#include "PlugUI.h"
#include "Theme.h"

namespace ui {

// the shape of a synth's first oscillator, for the Wave view (synth.js waveShape)
inline float waveAt(const std::string& w, double ph) {
    ph -= std::floor(ph);
    if (w == "sine") return (float)std::sin(ph * 2 * juce::MathConstants<double>::pi);
    if (w == "square" || w == "pulse") return ph < 0.5 ? 0.8f : -0.8f;
    if (w == "triangle") return (float)(ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph);
    if (w == "sawtooth" || w == "saw") return (float)(2 * ph - 1);
    if (w == "noise" || w == "sh") return (float)(std::sin(ph * 91.7) * 0.9);
    return (float)(0.6 * std::sin(ph * 2 * juce::MathConstants<double>::pi) + 0.3 * std::sin(ph * 6 * juce::MathConstants<double>::pi));
}

class RackView : public juce::Component, public juce::DragAndDropTarget, private juce::ChangeListener, private juce::Timer {
public:
    explicit RackView(Doc& d) : doc(d) {
        doc.addChangeListener(this); setWantsKeyboardFocus(true);
        for (juce::Component* c : std::initializer_list<juce::Component*>{ &beatsL, &beats, &viewSeg, &swing, &gridL, &grid, &groupL, &groupBox, &addB }) addAndMakeVisible(c);
        beats.setInputRestrictions(3, "0123456789"); beats.setTooltip("Pattern length in beats - type a number or scroll");
        beats.onDone = [this](const juce::String& t) { setBeats(t.getIntValue()); };
        viewSeg.tip(0, "What each channel row shows"); viewSeg.onChange = [this](int i) { doc.setp("rackView", juce::String(i == 0 ? "steps" : i == 1 ? "roll" : "wave")); rebuild(); };
        swing.sized(Knob::Sm); swing.fmt = [](double v) { return juce::String(juce::roundToInt(v * 100)) + "%"; };
        swing.onChange = [this](double v) { doc.p.swing = v; doc.live(); }; swing.onDone = [this] { doc.commit(); };
        gridL.setTooltip("Which notes are pushed late: every second 16th, or every second 8th");
        grid.addItem("1/16", 16); grid.addItem("1/8", 8); grid.onChange = [this] { if (doc.p.swingDiv != grid.getSelectedId()) { doc.p.swingDiv = grid.getSelectedId(); doc.commit(); } };
        groupL.setTooltip("Show one group of channels (Alt+G groups the selected channels)");
        groupBox.onChange = [this] { const int i = groupBox.getSelectedItemIndex(); std::string v = i <= 0 ? "" : i == 1 ? "-" : groupBox.getText().toStdString(); if (v != doc.p.rackGroup) { doc.p.rackGroup = v; doc.commit(); } };
        addB.primary = true; addB.onClick = [this] { addMenu(); };
        rows.o = this; view.setViewedComponent(&rows, false); view.setScrollBarsShown(true, true); view.o = this; addAndMakeVisible(view);
        refresh(true); startTimerHz(30);
    }
    ~RackView() override { doc.removeChangeListener(this); }
    void resized() override {
        auto r = getLocalBounds(); Flow f;
        f.add(beatsL, beatsL.idealWidth()).add(beats, 54).add(viewSeg, viewSeg.idealWidth()).add(swing, swing.idealWidth(), swing.idealHeight()).add(gridL, gridL.idealWidth()).add(grid, 70);
        const bool hasGroups = !groups().empty(); groupL.setVisible(hasGroups); groupBox.setVisible(hasGroups);
        if (hasGroups) f.add(groupL, groupL.idealWidth()).add(groupBox, 110);
        f.right(addB, addB.idealWidth());
        barH = f.layout(r); r.removeFromTop(barH); view.setBounds(r); rows.layout();
    }
    void paint(juce::Graphics& g) override { fillPanel(g); vbarLine(g, barH, getWidth()); }
    // drop library sounds: onto a sampler's name to replace its sound, anywhere else for a new channel
    bool isInterestedInDragSource(const SourceDetails& s) override { return s.description.toString().startsWith("sample:"); }
    void itemDropped(const SourceDetails& s) override {
        const std::string id = s.description.toString().fromFirstOccurrenceOf("sample:", false, false).toStdString(); auto smp = doc.bank.get(id); if (!smp) return;
        const auto pt = rows.getLocalPoint(this, s.localPosition); const int row = rows.rowAt(pt.y);
        if (row >= 0 && pt.x >= rows.nameX() && pt.x < rows.nameX() + rows.nameW()) if (auto* c = rows.find(rows.shown[(size_t)row])) if (c->type == "sample") { c->sample = id; doc.commit(); toast(u8("“") + juce::String(c->name) + u8("” now plays ") + juce::String(smp->name)); return; }
        auto c = newChannel(doc.p, "sample", smp->name.substr(0, 18), id); c.gate = smp->seconds() > 1.2; c.rel = 0.25; doc.p.channels.push_back(c); doc.ch = c.id; doc.commit();
    }
    bool keyPressed(const juce::KeyPress& k) override {
        const auto id = actionFor(k, "rack"); if (id.isEmpty()) return false;
        auto ids = doc.selectedChannels(); std::vector<us::Channel*> chs; for (auto& c : doc.p.channels) if (ids.count(c.id)) chs.push_back(&c); if (chs.empty()) return false;
        if (id == "rack.route") { for (auto* c : chs) { const auto name = c->name.substr(0, 16); const auto cid = c->id; const auto b = doc.addBus(name); for (auto& x : doc.p.channels) if (x.id == cid) x.bus = b; }
            doc.commit(); toast(juce::String((int)chs.size()) + " channel" + (chs.size() == 1 ? "" : "s") + " now each have their own mixer bus"); }
        else if (id == "rack.clone") { std::vector<std::string> src; for (auto* c : chs) src.push_back(c->id); for (auto& s : src) cloneChannel(s); doc.commit(); toast("Cloned " + juce::String((int)src.size()) + " channel" + (src.size() == 1 ? "" : "s")); }
        else if (id == "rack.group") groupDialog(ids);
        else if (id == "rack.del") { if (chs.size() >= doc.p.channels.size()) { toast("Keep at least one channel"); return true; } doc.deleteChannels(ids); doc.commit(); toast("Channels deleted"); }
        return true;
    }

private:
    struct View : juce::Viewport { RackView* o = nullptr; void visibleAreaChanged(const juce::Rectangle<int>& r) override { if (o) o->rows.stick(r.getX()); } };
    // the rows: heads are child components kept at the left edge while scrolling sideways (CSS position: sticky)
    struct Rows : public juce::Component, public juce::SettableTooltipClient {
        RackView* o = nullptr; std::vector<std::string> shown; int paintVal = -1, paintRow = -1; bool changed = false; double playTick = -1;
        struct Head : juce::Component {
            Rows* rows = nullptr; std::string id; juce::Colour col; bool sel = false, multi = false;
            Btn led{ {}, Btn::Text }, solo{ "S", Btn::Mini }, busB{ "M", Btn::BusNo }, name, rollB{ {}, Btn::Mini, "roll" }, moreB{ u8("⋯"), Btn::Mini };
            Knob pan{ "Pan", -1, 1, 0 }, vol{ "Vol", 0, 1.25, 0.8 };
            Head() { for (juce::Component* c : std::initializer_list<juce::Component*>{ &led, &solo, &busB, &pan, &vol, &name, &rollB, &moreB }) addAndMakeVisible(c);
                pan.sized(Knob::Xs); vol.sized(Knob::Xs); pan.showCaption = vol.showCaption = false; led.setTooltip("Mute"); solo.setTooltip("Solo"); rollB.setTooltip("Open in piano roll"); moreB.setTooltip("Channel options");
                busB.setTooltip("Mixer bus this channel plays through - click to choose, scroll to step");
                name.setTooltip("Click: select - Ctrl+click: select several - double-click: edit instrument - right-click: more - drop a sound here to replace it");
                setInterceptsMouseClicks(false, true); }   // the gaps between the controls belong to the rows (the column's resize grip)
            // The column can be dragged narrower: the controls give way one by one (piano roll, bus, pan, solo, volume, options) and
            // then the name shrinks; the mute LED and the name always stay (a long press or right-click on the name has the rest).
            static int S(int w) { return juce::roundToInt(w * style().scale); }
            static int nameFull() { return S(isTouch() ? 84 : 104); }
            static int nameMin() { return S(isTouch() ? 64 : 56); }
            static int baseW(int dropped) { static const int ew[] = { 22, 24, 30, 22, 30, 22 }; int w = 2 + 14 + 6 + 6 + 2; for (int i = dropped; i < 6; i++) w += S(ew[i]) + 6; return w; }   // all but the name
            static int fullW() { return baseW(0) + nameFull(); }
            static int minW() { return baseW(6) + nameMin(); }
            static int dropsFor(int w) { int d = 0; while (d < 6 && w - baseW(d) < nameFull()) d++; return d; }
            void resized() override {
                const int c = CTL(), d = dropsFor(getWidth()), nameW = std::max(nameMin(), getWidth() - baseW(d));
                juce::Component* drop[] = { &rollB, &busB, &pan, &solo, &vol, &moreB }; for (int i = 0; i < 6; i++) drop[i]->setVisible(i >= d);
                int x = 2; auto put = [&](juce::Component& k, int w, int h) { if (!k.isVisible()) return; k.setBounds(x, (getHeight() - h) / 2, w, h); x += w + 6; };
                put(led, 14, 14); put(solo, S(22), c); put(busB, S(24), c); put(pan, S(30), c); put(vol, S(30), c); put(name, nameW, c); put(rollB, S(22), c); put(moreB, S(22), c); }
            void paint(juce::Graphics& g) override {
                auto r = getLocalBounds().toFloat().withTrimmedBottom(0); g.setColour(sel ? mix(col, BG2, 0.14f) : BG2); g.fillRoundedRectangle(r, RAD() * 0.7f);
            }
            void paintOverChildren(juce::Graphics& g) override {
                // the LED and the name button's colour strip are drawn here (they are plain buttons underneath)
                auto l = led.getBounds().toFloat(); g.setColour(led.on() ? BG : ACCENT); g.fillEllipse(l); g.setColour(led.on() ? LINE : ACCENT.darker(0.4f)); g.drawEllipse(l.reduced(1), 2);
                if (!led.on() && style().glow) { g.setColour(ACCENT.withAlpha(0.25f)); g.drawEllipse(l.expanded(2), 2); }
                auto n = name.getBounds().toFloat(); g.saveState(); juce::Path clip; clip.addRoundedRectangle(n, RAD() * 0.6f); g.reduceClipRegion(clip);
                g.setColour(BG3.brighter(name.isMouseOver() ? 0.08f : 0)); g.fillRect(n); g.setColour(col); g.fillRect(n.withWidth(5));
                g.restoreState(); if (multi) { g.setColour(ACCENT2); g.drawRoundedRectangle(n.reduced(1), RAD() * 0.6f, 2); }
                g.setColour(TEXT); g.setFont(font(FS)); g.drawFittedText(name.getButtonText(), n.toNearestInt().withTrimmedLeft(12).withTrimmedRight(6), juce::Justification::centredLeft, 1, 0.9f);
            }
        };
        std::vector<std::unique_ptr<Head>> heads;
        int RH() const { return CTL() + 2 * PAD(); }
        // the channel column: its width (dragged by the grip at its right edge, or tapped there to switch compact / full) is kept
        // per orientation; until then it is full when that leaves room for half a bar or so of steps, else compact (a phone upright)
        int hw = 0; bool gripping = false, gripMoved = false; int gripFrom = 0, gripX = 0;
        int headW() const { return hw > 0 ? hw : Head::fullW(); }
        int gap() const { return isTouch() ? 26 : 16; }   // between the column and the steps; the grip sits in it
        const char* headKey() const { auto* t = o->getTopLevelComponent(); return t && t->getHeight() > t->getWidth() ? "rackHeadTall" : "rackHeadWide"; }
        int wantHeadW() const {
            const double v = o->doc.pd(headKey(), 0); if (v > 0) return juce::jlimit(Head::minW(), Head::fullW(), (int)v);
            return o->view.getMaximumVisibleWidth() - Head::fullW() - gap() >= 8 * (stepW() + 4) + 40 ? Head::fullW() : Head::minW();
        }
        bool onGrip(int x) const { const int e = (int)viewX() + headW(); return !heads.empty() && x >= e - 4 && x < e + gap(); }
        void setHeadW(int w) { w = juce::jlimit(Head::minW(), Head::fullW(), w); if (w == hw) return; hw = w; for (auto& h : heads) h->setSize(w, h->getHeight()); fit(); repaint(); }
        void fit() {   // the rows' size: the column, then the steps (or the row views)
            const int n = steps(); const int w = o->doc.ps("rackView", "steps") == "steps" ? (int)stepX(n - 1) + (int)stepW() + 16 : headW() + gap() + std::min(4000, n * 22) + 16;
            setSize(std::max(w, o->view.getMaximumVisibleWidth()), std::max((int)shown.size() * RH() + 12, o->view.getMaximumVisibleHeight()));
            stick(o->view.getViewPositionX());
        }
        int nameX() const { return heads.empty() ? 0 : heads[0]->name.getX() + heads[0]->getX(); }
        int nameW() const { return heads.empty() ? 0 : heads[0]->name.getWidth(); }
        int steps() const { auto* pt = o->doc.patt(); return pt ? std::max(1, (int)std::round(pt->bars * o->doc.p.bar() / us::STEP)) : 16; }
        // step geometry: .st width ctl*.82, gap 4, an extra 5 px before each beat and 14 px before each bar
        float stepW() const { return CTL() * (style().stepShape == "dot" ? 1.f : 0.82f); }
        float stepX(int i) const {
            const int bs = std::max(1, o->doc.p.beat() / us::STEP), spb = std::max(1, o->doc.p.bar() / us::STEP);
            const int bars = i / spb, beats = i / bs + 1; const float x0 = (float)(headW() + gap());   // margins of every step up to i
            return x0 + i * (stepW() + 4) + (beats - bars) * 5.f + bars * 14.f;
        }
        int stepAt(int x) const { const int n = steps(); for (int i = 0; i < n; i++) { const float sx = stepX(i); if (x >= sx - 2 && x < sx + stepW() + 2) return i; } return -1; }
        int rowAt(int y) const { const int r = (y - 6) / RH(); return r >= 0 && r < (int)shown.size() ? r : -1; }
        us::Channel* find(const std::string& id) { for (auto& c : o->doc.p.channels) if (c.id == id) return &c; return nullptr; }
        void stick(int x) { for (auto& h : heads) h->setTopLeftPosition(x, h->getY()); repaint(); }
        float viewX() const { return heads.empty() ? 0.f : (float)heads[0]->getX(); }
        void layout() {
            auto& D = o->doc; heads.clear(); removeAllChildren(); shown.clear(); if (!gripping) hw = wantHeadW();
            for (auto& c : D.p.channels) { const auto& k = D.p.rackGroup; if (k.empty() || (k == "-" ? c.group.empty() : c.group == k)) shown.push_back(c.id); }
            const int rh = RH();
            for (size_t i = 0; i < shown.size(); i++) {
                auto* c = find(shown[i]); auto h = std::make_unique<Head>(); h->rows = this; h->id = c->id; h->col = hex(c->color); h->sel = c->id == D.ch; h->multi = D.chSel.count(c->id) > 0;
                const std::string id = c->id;
                h->led.setOn(c->mute); h->led.setAlpha(0.01f); h->led.onClick = [this, id] { if (auto* ch = find(id)) { ch->mute = !ch->mute; o->doc.commit(); } };
                h->solo.setOn(c->solo); h->solo.onClick = [this, id] { if (auto* ch = find(id)) { ch->solo = !ch->solo; o->doc.commit(); } };
                h->busB.setButtonText(D.busLabel(c->bus)); h->busB.onClick = [this, id, b = &h->busB] { busMenu(id, b); };
                h->busB.addMouseListener(this, false); h->busB.getProperties().set("chan", juce::String(id));
                h->pan.set(c->pan); h->pan.setLink("ch:" + id + ":pan"); h->pan.onChange = [this, id](double v) { if (auto* ch = find(id)) { ch->pan = v; o->doc.live(); o->doc.autoWrite("ch:" + id + ":pan", v); } }; h->pan.onDone = [this] { o->doc.commit(); };
                h->vol.set(c->vol); h->vol.setLink("ch:" + id + ":vol"); h->vol.onChange = [this, id](double v) { if (auto* ch = find(id)) { ch->vol = v; o->doc.live(); o->doc.autoWrite("ch:" + id + ":vol", v); } }; h->vol.onDone = [this] { o->doc.commit(); };
                h->name.setButtonText(juce::String(c->name)); h->name.setAlpha(0.01f);   // painted by the head
                h->name.onClick = [this, id, b = &h->name] { if (wantsMenu()) { rowMenu(id, b); return; } pick(id, juce::ModifierKeys::currentModifiers.isCtrlDown() || juce::ModifierKeys::currentModifiers.isCommandDown()); };
                h->name.onRight = [this, id, b = &h->name] { rowMenu(id, b); };
                h->name.addMouseListener(this, false); h->name.getProperties().set("chan", juce::String(id));
                h->rollB.onClick = [this, id] { pick(id, false); o->doc.go("roll", "rack"); };
                h->moreB.onClick = [this, id, b = &h->moreB] { rowMenu(id, b); };
                h->setBounds(0, 6 + (int)i * rh + PAD() - 3, headW(), rh - 2 * PAD() + 6);
                addAndMakeVisible(*h); heads.push_back(std::move(h));
            }
            fit();
        }
        juce::Rectangle<float> rviewArea(int row) const { const int rh = RH(); return { (float)(headW() + gap()), 6.f + row * rh + (rh - 36) / 2.f, (float)std::min(4000, steps() * 22), 36.f }; }
        void paint(juce::Graphics& g) override {
            auto& D = o->doc; auto* pt = D.patt(); const int ns = steps(); const auto view = D.ps("rackView", "steps"); const int rh = RH();
            if (D.p.channels.empty()) { g.setColour(DIM); g.setFont(font(FS * 0.88f)); auto tr = getLocalBounds().reduced(18); if (auto* vp = findParentComponentOfClass<juce::Viewport>()) tr = tr.withX(vp->getViewPositionX() + 18).withWidth(std::max(120, vp->getViewWidth() - 36));   // wrap within what is on screen (a phone)
                g.drawFittedText(u8("The rack is empty. Use “+ Channel” to add a synth or sampler, or drop audio files here."), tr.removeFromTop(64), juce::Justification::topLeft, 3, 1.f); return; }
            if (shown.empty()) { g.setColour(DIM); g.setFont(font(FS * 0.88f)); g.drawText("No channels in this group.", getLocalBounds().reduced(18).removeFromTop(20), juce::Justification::centredLeft); return; }
            const int playing = D.eng.isPlaying() && !D.song ? (int)(D.eng.positionTicks() / us::STEP) : -1;
            const int bs = std::max(1, D.p.beat() / us::STEP); const float sw = stepW(), shH = (float)CTL();
            for (size_t r = 0; r < shown.size(); r++) {
                auto* c = find(shown[r]); if (!c) continue; const juce::Colour col = hex(c->color); const float y = 6.f + r * rh + (rh - shH) / 2;
                const std::vector<us::Note>* list = nullptr; if (pt) if (auto it = pt->notes.find(c->id); it != pt->notes.end()) list = &it->second;
                if (view == "steps") {
                    std::vector<double> on((size_t)ns, 0.0); if (list) for (auto& n : *list) { const int k = (int)(n.t / us::STEP); if (k >= 0 && k < ns) on[(size_t)k] = std::max(on[(size_t)k], n.v); }
                    for (int k = 0; k < ns; k++) {
                        juce::Rectangle<float> st(stepX(k), y, sw, shH); const bool now = k == (playing >= 0 ? playing % ns : -1);
                        if (now) st = st.withSizeKeepingCentre(sw * 1.08f, shH * 1.08f);
                        juce::Path p; if (style().stepShape == "square") p.addRoundedRectangle(st, 4); else if (style().stepShape == "dot") p.addEllipse(st.withSizeKeepingCentre(std::min(st.getWidth(), st.getHeight()), std::min(st.getWidth(), st.getHeight())));
                        else p.addRoundedRectangle(st.getX(), st.getY(), st.getWidth(), st.getHeight(), st.getWidth() * 0.45f, st.getHeight() * 0.3f, true, true, true, true);   // 45% / 30%
                        const double v = on[(size_t)k];
                        if (v > 0) { if (style().glow) { g.setColour(col.withAlpha(0.28f)); g.strokePath(p, juce::PathStrokeType(5)); } g.setColour(col.withMultipliedAlpha((float)(0.3 + 0.7 * v))); }
                        else g.setColour((k / bs) & 1 ? mix(BG3, col, 0.86f) : BG3);
                        g.fillPath(p);
                        if (now) { g.setColour(TEXT); g.strokePath(p, juce::PathStrokeType(1)); }
                    }
                } else {
                    auto a = rviewArea((int)r); drawRowView(g, a, *c, list);
                }
            }
            if (view != "steps" && playTick >= 0) { auto a = rviewArea(0); const float L = (float)(pt ? pt->bars * D.p.bar() : D.p.bar()), x = a.getX() + (float)std::fmod(playTick, L) / L * a.getWidth();
                g.setColour(ACCENT.withAlpha(0.5f)); g.fillRect(x - 2, 6.f, 5.f, (float)shown.size() * rh); g.setColour(TEXT); g.fillRect(x - 1, 6.f, 2.f, (float)shown.size() * rh); }
            paintGrip(g);
        }
        void paintGrip(juce::Graphics& g) {   // the column's right edge: a line and a handle with arrows, in the middle of what is on screen
            const float x = viewX() + headW(), w = (float)gap() - 4, bottom = 6.f + shown.size() * RH();
            g.setColour(BG2); g.fillRect(x, 0.f, w, (float)getHeight());
            const float cx = x + w / 2, vy = (float)o->view.getViewPositionY(), vh = (float)o->view.getViewHeight();
            const float cy = juce::jlimit(6.f + 24, std::max(30.f, bottom - 24), vy + std::min(vh, bottom - vy) / 2);
            const bool hot = gripping || overGrip;
            g.setColour(hot ? ACCENT : LINE); g.fillRect(cx - 0.5f, 6.f, 1.f, bottom - 6);
            auto pill = juce::Rectangle<float>(cx - w / 2 + 1, cy - 22, w - 2, 44); g.setColour(hot ? ACCENT : BG3); g.fillRoundedRectangle(pill, pill.getWidth() / 2);
            g.setColour(hot ? ONACC : DIM); juce::Path a; const float s = std::min(4.f, w / 4);
            a.addTriangle(cx - s, cy - 9, cx + s, cy - 9 - s, cx + s, cy - 9 + s); a.addTriangle(cx + s, cy + 9, cx - s, cy + 9 - s, cx - s, cy + 9 + s); g.fillPath(a);
        }
        // the Notes view (a miniature piano roll) or the Wave view (the channel's sound at every hit)
        void drawRowView(juce::Graphics& g, juce::Rectangle<float> a, const us::Channel& c, const std::vector<us::Note>* list) {
            auto& D = o->doc; auto* pt = D.patt(); const double L = pt ? pt->bars * D.p.bar() : D.p.bar(); const float W = a.getWidth(), H = a.getHeight();
            g.setColour(BG3); g.fillRoundedRectangle(a, RAD() * 0.5f);
            for (double t = 0; t < L; t += D.p.beat()) { g.setColour(juce::Colour(0xff808080).withAlpha(std::fmod(t, D.p.bar()) == 0 ? 0.5f : 0.2f)); g.fillRect(a.getX() + std::round((float)(t / L) * W), a.getY(), 1.f, H); }
            const juce::Colour col = hex(c.color); std::vector<us::Note> ns; if (list) for (auto& n : *list) if (n.t < L) ns.push_back(n);
            if (D.ps("rackView", "steps") == "roll") {
                if (ns.empty()) return; int lo = 127, hi = 0; for (auto& n : ns) { lo = std::min(lo, n.n); hi = std::max(hi, n.n); } const float span = (float)std::max(1, hi - lo);
                for (auto& n : ns) {
                    const float x = a.getX() + (float)(n.t / L) * W, y = a.getY() + 3 + (1 - (n.n - lo) / span) * (H - 10), w = std::max(2.f, (float)(n.d / L) * W - 1); const bool on = playTick >= 0 && std::fmod(playTick, L) >= n.t && std::fmod(playTick, L) < n.t + n.d;
                    if (on) { g.setColour(juce::Colours::white.withAlpha(0.85f)); g.fillRect(x, y - 1, w, 6.f); g.setColour(col); g.fillRect(x + 1, y, std::max(1.f, w - 2), 4.f); }
                    else { g.setColour(col.withAlpha((float)(0.4 + 0.6 * n.v))); g.fillRect(x, y, w, 4.f); }
                }
                return;
            }
            auto smp = c.type == "sample" ? D.bank.get(c.sample) : nullptr; const double spt = D.p.secondsPerTick();
            const float full = smp ? (float)(smp->seconds() / spt / L) * W : 0;
            auto one = [&](float x0, float wpx, float alpha) {
                g.setColour(col.withAlpha(alpha));
                if (smp && !smp->ch.empty()) { const auto& d = smp->ch[0]; const double per = d.size() / std::max(1.f, full); const size_t stride = std::max<size_t>(1, (size_t)(per / 24));
                    for (int x = 0; x < (int)wpx && x * per < d.size(); x++) { float m = 0; for (size_t i = (size_t)(x * per), e = std::min(d.size(), (size_t)(x * per + per)); i < e; i += stride) m = std::max(m, std::abs(d[i])); const float yy = std::max(1.f, m * H); g.fillRect(a.getX() + x0 + x, a.getY() + (H - yy) / 2, 1.f, yy); } }
                else if (c.synth) { const auto& w = c.synth->osc[0].wave; for (int x = 0; x < (int)wpx; x++) { const float v = waveAt(w, x * 1.5 / 24.0) * (H / 2 - 3); g.fillRect(a.getX() + x0 + x, a.getY() + H / 2 - std::max(0.f, v), 1.f, std::max(1.f, std::abs(v))); } }
            };
            if (ns.empty()) { one(0, smp ? std::min(W, full) : 48, 0.3f); return; }
            std::sort(ns.begin(), ns.end(), [](auto& x, auto& y) { return x.t < y.t; });
            for (size_t i = 0; i < ns.size(); i++) { const float x0 = (float)(ns[i].t / L) * W, next = i + 1 < ns.size() ? (float)(ns[i + 1].t / L) * W : W;
                one(x0, std::max(2.f, std::min(next - x0 - 1, smp && !c.gate ? W : (float)(ns[i].d / L) * W)), (float)(0.4 + 0.6 * ns[i].v)); }
        }
        void setStep(int row, int k, bool on) {
            auto& P = o->doc.p; auto* pt = o->doc.patt(); if (!pt || row < 0 || row >= (int)shown.size() || k < 0 || k >= steps()) return;
            auto* c = find(shown[(size_t)row]); if (!c) return; auto& list = pt->notes[c->id];
            const bool has = std::any_of(list.begin(), list.end(), [&](const us::Note& n) { return (int)(n.t / us::STEP) == k; });
            if (on == has) return;
            if (on) { us::Note n; n.id = us::newId(); n.t = k * us::STEP; n.d = us::STEP; n.n = c->type == "synth" ? 60 : c->root; n.v = 0.8; list.push_back(n); if (!o->doc.eng.isPlaying()) { o->doc.eng.noteOn(c->id, n.n, 0.8); o->doc.eng.noteOff(c->id, n.n); } }
            else list.erase(std::remove_if(list.begin(), list.end(), [&](const us::Note& n) { return (int)(n.t / us::STEP) == k; }), list.end());
            (void)P; o->doc.live(); changed = true; repaint();
        }
        void mouseDown(const juce::MouseEvent& e) override {
            if (e.eventComponent != this) return;
            o->grabKeyboardFocus();
            if (gripping) return;   // a long press on the grip (a right-click made by the touch helper) while the finger is still down
            if (onGrip(e.x)) { gripping = true; gripMoved = false; gripFrom = headW(); gripX = e.getScreenX(); setViewportIgnoreDragFlag(true); repaint(); return; }
            const int row = rowAt(e.y); if (row < 0) return;
            if (o->doc.ps("rackView", "steps") != "steps") {   // a row view: notes -> Piano Roll, wave -> hear it
                if (!rviewArea(row).contains(e.position)) return; pick(shown[(size_t)row], false);
                if (o->doc.ps("rackView", "steps") == "roll") o->doc.go("roll", "rack"); else if (auto* c = find(shown[(size_t)row])) { o->doc.eng.noteOn(c->id, c->type == "synth" ? 60 : c->root, 0.85); juce::Timer::callAfterDelay(500, [this, id = c->id, n = c->type == "synth" ? 60 : c->root] { o->doc.eng.noteOff(id, n); }); }
                return;
            }
            const int k = stepAt(e.x); if (k < 0) return;
            auto* pt = o->doc.patt(); auto* c = find(shown[(size_t)row]); if (!pt || !c) return;
            const auto& list = pt->notes[c->id]; const bool has = std::any_of(list.begin(), list.end(), [&](const us::Note& n) { return (int)(n.t / us::STEP) == k; });
            paintVal = e.mods.isPopupMenu() ? 0 : has ? 0 : 1; paintRow = row; setStep(row, k, paintVal == 1);
        }
        void mouseDrag(const juce::MouseEvent& e) override {
            if (e.eventComponent == this && gripping) { const int dx = e.getScreenX() - gripX; if (std::abs(dx) > 4) gripMoved = true; if (gripMoved) setHeadW(gripFrom + dx); return; }
            if (e.eventComponent != this || paintVal < 0) return; const int k = stepAt(e.x); if (k >= 0) setStep(paintRow, k, paintVal == 1); }
        void mouseUp(const juce::MouseEvent& e) override {
            if (e.eventComponent == this && gripping) {   // a drag keeps its width; a tap switches between compact and full
                if (e.mods.isPopupMenu()) return;
                gripping = false; setViewportIgnoreDragFlag(false);
                if (!gripMoved) setHeadW(headW() * 2 > Head::minW() + Head::fullW() ? Head::minW() : Head::fullW());
                o->doc.setp(headKey(), headW()); repaint(); return; }
            if (e.eventComponent != this) return; paintVal = -1; if (changed) { changed = false; o->doc.commit(); } }
        void mouseMove(const juce::MouseEvent& e) override {
            if (e.eventComponent != this) return; const bool g = onGrip(e.x);
            setMouseCursor(g ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
            setTooltip(g ? "Drag to resize the channel column - click to switch between compact and full" : ""); if (g != overGrip) { overGrip = g; repaint(); } }
        void mouseExit(const juce::MouseEvent& e) override { if (e.eventComponent == this && overGrip) { overGrip = false; repaint(); } }
        bool overGrip = false;
        void mouseDoubleClick(const juce::MouseEvent& e) override { if (e.eventComponent != this && e.eventComponent->getProperties().contains("chan") && dynamic_cast<Btn*>(e.eventComponent) && static_cast<Btn*>(e.eventComponent)->kind == Btn::Text) { pick(e.eventComponent->getProperties()["chan"].toString().toStdString(), false); o->doc.go("inst", "rack"); } }
        void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override {
            if (e.eventComponent != this && e.eventComponent->getProperties().contains("chan") && dynamic_cast<Btn*>(e.eventComponent) && static_cast<Btn*>(e.eventComponent)->kind == Btn::BusNo) {   // scroll on the bus box: step through the buses
                auto* c = find(e.eventComponent->getProperties()["chan"].toString().toStdString()); if (!c) return; auto& bs = o->doc.p.buses; int i = 0; for (size_t k = 0; k < bs.size(); k++) if (bs[k].id == c->bus) i = (int)k;
                const int j = juce::jlimit(0, (int)bs.size() - 1, i + (w.deltaY < 0 ? 1 : -1)); if (j != i) { c->bus = bs[(size_t)j].id; o->doc.commit(); } return;
            }
            if (e.eventComponent == this || e.eventComponent->getParentComponent() == this) Component::mouseWheelMove(e, w);
        }
        void pick(const std::string& id, bool ctrl) {
            auto& D = o->doc;
            if (ctrl) { if (D.chSel.empty() && !D.ch.empty()) D.chSel.insert(D.ch); if (D.chSel.count(id)) D.chSel.erase(id); else D.chSel.insert(id); for (auto& h : heads) { h->multi = D.chSel.count(h->id) > 0; h->repaint(); } return; }
            if (!D.chSel.empty()) { D.chSel.clear(); for (auto& h : heads) { h->multi = false; h->repaint(); } }
            if (D.ch != id) D.selectChannel(id);
        }
        void busMenu(const std::string& id, juce::Component* at) {
            auto* c = find(id); if (!c) return; Menu m; m.head("Send " + juce::String(c->name) + " to");
            for (auto& b : o->doc.p.buses) { const auto bid = b.id; m.item(o->doc.busLabel(b.id) + u8(" · ") + juce::String(b.name), [this, id, bid] { if (auto* ch = find(id)) { ch->bus = bid; o->doc.commit(); } }, true, c->bus == b.id); }
            m.show(ui::at(at));
        }
        void rowMenu(const std::string& id, juce::Component* at) { o->rowMenu(id, at); }
    };

    Doc& doc; View view; Rows rows; int barH = 40; juce::String sig;
    Cap beatsL{ "Beats" }, gridL{ "Swing grid" }, groupL{ "Group" }; Field beats{ true, true }; Seg viewSeg{ { "Steps", "Notes", "Wave" } };
    Knob swing{ "Swing", 0, 1, 0 }; juce::ComboBox grid, groupBox; Btn addB{ "+ Channel" };

    std::vector<std::string> groups() const { std::set<std::string> s; for (auto& c : doc.p.channels) if (!c.group.empty()) s.insert(c.group); return { s.begin(), s.end() }; }
    void setBeats(int v) { if (auto* pt = doc.patt()) { const int mx = 64 * doc.p.bar() / doc.p.beat(); pt->bars = juce::jlimit(1, mx, v > 0 ? v : 1) * (double)doc.p.beat() / doc.p.bar(); doc.commit(); } }
    void cloneChannel(const std::string& id) {
        auto& P = doc.p; auto it = std::find_if(P.channels.begin(), P.channels.end(), [&](auto& x) { return x.id == id; }); if (it == P.channels.end()) return;
        us::Channel n = *it; n.id = us::newId(); n.name += " 2"; if (n.plug.getDynamicObject()) n.plug = juce::var(new juce::DynamicObject(*n.plug.getDynamicObject()));
        for (auto& pt : P.patterns) if (pt.notes.count(id)) { auto ns = pt.notes[id]; for (auto& x : ns) x.id = us::newId(); pt.notes[n.id] = ns; }
        P.channels.insert(it + 1, n); doc.ch = n.id;
    }
    void groupDialog(std::set<std::string> ids) {
        juce::String cur; for (auto& c : doc.p.channels) if (ids.count(c.id) && !c.group.empty()) cur = c.group;
        ask("Group name for the selected channels (empty to ungroup)", cur.isEmpty() ? "Drums" : cur, [this, ids](juce::String v) {
            for (auto& c : doc.p.channels) if (ids.count(c.id)) c.group = v.trim().toStdString(); doc.commit();
            toast(v.trim().isEmpty() ? juce::String("Ungrouped") : u8("Grouped as “") + v.trim() + u8("” - choose it under Group to show only these")); });
    }
    void addMenu() {
        Menu m;
        m.item("Synth channel", [this] { add(newChannel(doc.p, "synth", "Synth")); });
        m.item("Keys sampler", [this] { add(newChannel(doc.p, "sample", "Keys", "inst:keys")); });
        m.item(u8("Plug-in instrument (CLAP / VST)…"), [this] { choosePlugin(&addB, true, [this](const juce::var& slot) { auto c = newChannel(doc.p, "plugin", slot.getProperty("name", "Plug-in").toString().substring(0, 18).toStdString()); c.plug = slot; add(c); }); });
        m.item(u8("Upload audio samples…"), [this] { upload(); });
        Menu kit; for (auto& s : doc.bank.kit()) { const auto sid = s->id; const auto nm = s->name; kit.item(juce::String(nm), [this, sid, nm] { auto c = newChannel(doc.p, "sample", nm, sid); c.gate = sid.rfind("inst:", 0) == 0; add(c); }); }
        m.sub("Kit and instrument sounds", kit);
        m.item(u8("Browse 808 kit & library"), [this] { doc.go("lib", "rack"); });
        m.item("Design a new sound", [this] { doc.go("design", "rack"); });
        m.show(at(&addB));
    }
    void add(us::Channel c) { doc.p.channels.push_back(c); doc.ch = c.id; doc.commit(); }
    std::unique_ptr<juce::FileChooser> chooser;
    void upload() {
        chooser = std::make_unique<juce::FileChooser>("Upload audio samples", juce::File::getSpecialLocation(juce::File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems, [this](const juce::FileChooser& fc) {
            for (auto& f : fc.getResults()) { auto id = doc.importAudio(f); if (id.empty()) { toast("Could not decode " + f.getFileName()); continue; } auto c = newChannel(doc.p, "sample", f.getFileNameWithoutExtension().substring(0, 18).toStdString(), id); doc.p.channels.push_back(c); doc.ch = c.id; }
            doc.commit(); });
    }
    void rowMenu(const std::string& id, juce::Component* atC) {
        us::Channel* c = rows.find(id); if (!c) return; auto* pt = doc.patt(); if (!pt) return;
        const int steps = rows.steps(); const double L = pt->bars * doc.p.bar();
        auto put = [this, id, steps](std::function<double(int)> fn) { auto* p = doc.patt(); auto* ch = rows.find(id); if (!p || !ch) return; auto& l = p->notes[id]; l.clear();
            for (int i = 0; i < steps; i++) { const double v = fn(i); if (v > 0) { us::Note n; n.id = us::newId(); n.t = i * us::STEP; n.d = us::STEP; n.n = ch->type == "synth" ? 60 : ch->root; n.v = v; l.push_back(n); } } doc.commit(); };
        auto shift = [this, id, L](double d) { if (auto* p = doc.patt()) for (auto& n : p->notes[id]) n.t = std::fmod(n.t + d + L, L); doc.commit(); };
        Menu m; m.head(juce::String(c->name));
        m.item(u8("Rename…"), [this, id] { if (auto* ch = rows.find(id)) ask("Channel name", juce::String(ch->name), [this, id](juce::String v) { if (auto* x = rows.find(id); x && v.isNotEmpty()) { x->name = v.toStdString(); doc.commit(); } }); });
        m.item("Next colour", [this, id] { static const char* cols[] = { "#ff7a59", "#ffb84d", "#e6d15a", "#7bd88f", "#45d0c4", "#5aa9ff", "#9d8cff", "#f277c6" }; if (auto* x = rows.find(id)) { int k = 0; for (int i = 0; i < 8; i++) if (x->color == cols[i]) k = i + 1; x->color = cols[k % 8]; doc.commit(); } });
        m.item(u8("Group") + (c->group.empty() ? juce::String() : " (" + juce::String(c->group) + ")") + u8("…"), [this, id] { auto ids = doc.selectedChannels(); if (!ids.count(id)) ids = { id }; groupDialog(ids); }, true, false, "Alt+G");
        m.item("Edit instrument", [this, id] { rows.pick(id, false); doc.go("inst", "rack"); });
        if (c->type == "sample") { Menu s; for (auto& smp : doc.bank.kit()) { const auto sid = smp->id; s.item(juce::String(smp->name), [this, id, sid] { if (auto* x = rows.find(id)) { x->sample = sid; doc.commit(); } }, true, smp->id == c->sample); }
            for (auto& smp : doc.bank.imported()) { const auto sid = smp->id; s.item(juce::String(smp->name), [this, id, sid] { if (auto* x = rows.find(id)) { x->sample = sid; doc.commit(); } }, true, smp->id == c->sample); }
            m.sub("Sound", s); }
        m.sep();
        m.item("Fill every 2 steps", [put] { put([](int i) { return i % 2 == 0 ? 0.8 : 0.0; }); });
        m.item("Fill every 4 steps", [put] { put([](int i) { return i % 4 == 0 ? 0.8 : 0.0; }); });
        m.item(u8("Euclidean fill…"), [put, steps] { ask("Hits to spread over " + juce::String(steps) + " steps", "5", [put, steps](juce::String v) { const int k = v.getIntValue(); if (k > 0) put([k, steps](int i) { return (i * k) % steps < k ? 0.8 : 0.0; }); }); });
        m.item("Randomise", [put] { auto& rnd = juce::Random::getSystemRandom(); put([&rnd](int i) { return rnd.nextDouble() < (i % 4 ? 0.22 : 0.5) ? 0.5 + rnd.nextDouble() * 0.5 : 0.0; }); });
        m.item("Shift left", [shift] { shift(-us::STEP); }); m.item("Shift right", [shift] { shift(us::STEP); });
        m.item("Clear steps", [put] { put([](int) { return 0.0; }); });
        m.sep();
        const bool hasNotes = pt->notes.count(id) && !pt->notes[id].empty();
        m.item(u8("Move this channel’s notes to a new pattern"), [this, id] { auto* p = doc.patt(); auto* ch = rows.find(id); if (!p || !ch) return; us::Pattern n; n.id = us::newId(); n.name = p->name + u8s(" · ") + ch->name; n.bars = p->bars; n.notes[id] = p->notes[id]; p->notes.erase(id);
            doc.p.patterns.push_back(n); doc.commit(); toast(u8("“") + juce::String(n.name) + u8("” created")); }, hasNotes);
        m.item("Duplicate channel", [this, id] { cloneChannel(id); doc.commit(); });
        m.item("Move up", [this, id] { auto& a = doc.p.channels; for (size_t i = 1; i < a.size(); i++) if (a[i].id == id) { std::swap(a[i], a[i - 1]); doc.commit(); return; } });
        m.item("Move down", [this, id] { auto& a = doc.p.channels; for (size_t i = 0; i + 1 < a.size(); i++) if (a[i].id == id) { std::swap(a[i], a[i + 1]); doc.commit(); return; } });
        m.item("Delete channel", [this, id] { doc.deleteChannels({ id }); doc.commit(); }, doc.p.channels.size() > 1);
        m.show(at(atC));
    }
    static std::string u8s(const char* s) { return s; }
    // rebuild the rows only when the channel list changes; otherwise just repaint
    void refresh(bool force = false) {
        juce::String s; for (auto& c : doc.p.channels) s << c.id << c.name << c.color << c.bus << (int)c.mute << (int)c.solo << c.group << (c.id == doc.ch ? "*" : "") << ";";
        s << doc.pat << (doc.patt() ? doc.patt()->bars : 0) << doc.p.buses.size() << doc.p.rackGroup << doc.ps("rackView", "steps") << doc.p.sigN << doc.p.sigD << CTL();
        for (auto& id : doc.chSel) s << id;
        if (s != sig || force) { sig = s; rows.layout(); resized(); }
        for (auto& h : rows.heads) if (auto* c = rows.find(h->id)) { h->vol.set(c->vol); h->pan.set(c->pan); }
        if (auto* pt = doc.patt()) beats.setText(juce::String(juce::roundToInt(pt->bars * doc.p.bar() / doc.p.beat())), false);
        const auto v = doc.ps("rackView", "steps"); viewSeg.set(v == "roll" ? 1 : v == "wave" ? 2 : 0);
        swing.set(doc.p.swing); grid.setSelectedId(doc.p.swingDiv == 8 ? 8 : 16, juce::dontSendNotification);
        groupBox.clear(juce::dontSendNotification); groupBox.addItem("All", 1); groupBox.addItem("Unsorted", 2); int k = 3; for (auto& gname : groups()) groupBox.addItem(juce::String(gname), k++);
        groupBox.setSelectedItemIndex(doc.p.rackGroup.empty() ? 0 : doc.p.rackGroup == "-" ? 1 : [&] { int i = 2; for (auto& gname : groups()) { if (gname == doc.p.rackGroup) return i; i++; } return 0; }(), juce::dontSendNotification);
        rows.repaint();
    }
    void rebuild() { refresh(true); }
    void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(); }
    int lastStep = -2;
    void timerCallback() override {
        if (!isShowing()) return;
        const bool pl = doc.eng.isPlaying() && !doc.song; const double t = pl ? doc.eng.positionTicks() : -1; const int st = pl ? (int)(t / us::STEP) : -1;
        if (doc.ps("rackView", "steps") != "steps") { if (pl || rows.playTick >= 0) { rows.playTick = t; rows.repaint(); } }
        else if (st != lastStep) { lastStep = st; rows.repaint(); }
    }
};

}   // namespace ui
