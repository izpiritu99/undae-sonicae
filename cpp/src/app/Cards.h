// The app's card grid (.cards / .card / .krow / .hrow / .pick / .envgfx in app.css): equal columns at least 290 px wide,
// each card a titled box (optionally with an on / off LED) whose controls flow in rows aligned at the bottom, with full-width
// pieces (graphs, waveforms, notes) between them. The Instrument, Sound Design, Audio Lab and Library views build on it.
#pragma once
#include "Theme.h"

namespace ui {

// a label over a list (.pick)
struct Pick : juce::Component {
    Cap l; juce::ComboBox box; std::vector<juce::String> values; std::function<void(const juce::String&)> onPick;
    Pick(const juce::String& label, std::vector<std::pair<juce::String, juce::String>> opts, const juce::String& cur) : l(label, FS * 0.78f) {
        addAndMakeVisible(l); addAndMakeVisible(box);
        for (size_t i = 0; i < opts.size(); i++) { box.addItem(opts[i].second, (int)i + 1); values.push_back(opts[i].first); }
        set(cur); box.onChange = [this] { const int i = box.getSelectedItemIndex(); if (i >= 0 && onPick) onPick(values[(size_t)i]); };
    }
    void set(const juce::String& v) { for (size_t i = 0; i < values.size(); i++) if (values[i] == v) { box.setSelectedItemIndex((int)i, juce::dontSendNotification); return; } box.setSelectedItemIndex(-1, juce::dontSendNotification); }
    void resized() override { auto r = getLocalBounds(); l.setBounds(r.removeFromTop(16)); r.removeFromTop(3); box.setBounds(r.removeFromTop(CTL())); }
    static int height() { return 16 + 3 + CTL(); }
};
// a small line display (.envgfx): wave shapes, filter curves, envelopes. paint gets the inner area.
struct Gfx : juce::Component {
    std::function<void(juce::Graphics&, juce::Rectangle<float>)> draw; int h = 42;
    void paint(juce::Graphics& g) override { auto r = getLocalBounds().toFloat(); g.setColour(BG); g.fillRoundedRectangle(r, RAD() * 0.6f); if (draw) { g.saveState(); g.reduceClipRegion(getLocalBounds()); draw(g, r.reduced(6, 2)); g.restoreState(); } }
    // a line through points -1..1 across the width
    static void plot(juce::Graphics& g, juce::Rectangle<float> r, const std::vector<float>& pts, juce::Colour c = ACCENT) {
        if (pts.size() < 2) return; juce::Path p; for (size_t i = 0; i < pts.size(); i++) { const float x = r.getX() + r.getWidth() * i / (pts.size() - 1), y = r.getCentreY() - pts[i] * r.getHeight() * 0.43f; if (i == 0) p.startNewSubPath(x, y); else p.lineTo(x, y); }
        g.setColour(c); g.strokePath(p, juce::PathStrokeType(2, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
};
// a waveform display (.wave), min / max per pixel column
struct Wave : juce::Component {
    std::vector<float> data; juce::Colour col = ACCENT; float marker = -1; std::function<void(juce::Graphics&, juce::Rectangle<float>)> over;
    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat(); g.setColour(BG); g.fillRoundedRectangle(r, RAD() * 0.6f); if (data.empty()) { if (over) over(g, r); return; }
        g.setColour(col); const int W = getWidth(); const float H = (float)getHeight(); const double st = data.size() / (double)std::max(1, W);
        for (int x = 0; x < W; x++) { float lo = 1, hi = -1; const size_t a = (size_t)(x * st), b = std::min(data.size(), (size_t)((x + 1) * st) + 1), inc = std::max<size_t>(1, (b - a) / 64); for (size_t i = a; i < b; i += inc) { lo = std::min(lo, data[i]); hi = std::max(hi, data[i]); } if (hi < lo) continue;
            g.fillRect((float)x, (1 - hi) * H / 2, 1.f, std::max(1.f, (hi - lo) * H / 2)); }
        if (marker >= 0) { g.setColour(TEXT); g.fillRect(marker * W, 0.f, 1.5f, H); }
        if (over) over(g, r);
    }
};

class CardGrid : public juce::Component {
public:
    int minCol = 290; std::function<void()> onLayout;
    struct Entry { juce::Component* c = nullptr; int w = 0, h = 0, kind = 0; };   // kind: 0 inline, 1 full width, 2 line break
    struct Card { juce::String title; Btn* led = nullptr; std::vector<Entry> e; juce::Rectangle<int> area; juce::Component* head = nullptr; int headW = 0; };
    std::vector<Card> cards;
    void clear() { cards.clear(); removeAllChildren(); owned.clear(); }
    // a card; with ledOn / onLed it gets a power LED before its title
    void card(const juce::String& title, std::optional<bool> ledOn = std::nullopt, std::function<void(bool)> onLed = {}) {
        Card c; c.title = title;
        if (ledOn) { auto* b = add0(new Btn()); b->setOn(*ledOn); b->setAlpha(0.01f); b->setTooltip("On / off"); b->onClick = [b, onLed] { b->setOn(!b->on()); if (onLed) onLed(b->on()); b->getParentComponent()->repaint(); }; c.led = b; }
        cards.push_back(c);
    }
    void titleRight(juce::Component* comp, int w) { addAndMakeVisible(comp); cards.back().head = comp; cards.back().headW = w; }
    template <class T> T* add(T* c, int w, int h) { add0(c); cards.back().e.push_back({ c, w, h, 0 }); return c; }
    template <class T> T* full(T* c, int h) { add0(c); cards.back().e.push_back({ c, 0, h, 1 }); return c; }
    void br() { cards.back().e.push_back({ nullptr, 0, 0, 2 }); }
    Cap* note(const juce::String& t, float px = FS * 0.88f) { auto* c = new Cap(t, px); c->setJustificationType(juce::Justification::topLeft); c->setMinimumHorizontalScale(1.f); add0(c); cards.back().e.push_back({ c, 0, -1, 1 }); return c; }
    Knob* knob(const juce::String& label, double lo, double hi, double def, double value, std::function<void(double)> live, std::function<void()> done, bool expo = false, std::function<juce::String(double)> fmt = {}, double step = 0, Knob::Size sz = Knob::Sm) {
        auto* k = new Knob(label, lo, hi, def, expo, step); k->sized(sz); if (fmt) k->fmt = fmt; k->set(value); k->onChange = live; k->onDone = done; return add(k, k->idealWidth(), k->idealHeight());
    }
    Pick* pick(const juce::String& label, std::vector<std::pair<juce::String, juce::String>> opts, const juce::String& cur, std::function<void(const juce::String&)> fn, int w = 120) {
        auto* p = new Pick(label, opts, cur); p->onPick = fn; return add(p, w, Pick::height());
    }
    Btn* button(const juce::String& t, std::function<void()> fn, bool primary = false, bool sm = true) { auto* b = new Btn(t); b->primary = primary; b->sm = sm; b->onClick = fn; return add(b, b->idealWidth(), CTL()); }
    Btn* toggle(const juce::String& t, bool on, std::function<void(bool)> fn, const juce::String& tip = {}) { auto* b = new Btn(t); b->sm = true; b->setOn(on); b->setTooltip(tip); b->onClick = [b, fn] { b->setOn(!b->on()); fn(b->on()); }; return add(b, b->idealWidth(), CTL()); }
    juce::ToggleButton* check(const juce::String& t, bool on, std::function<void(bool)> fn) { auto* b = new juce::ToggleButton(t); b->setToggleState(on, juce::dontSendNotification); b->onClick = [b, fn] { fn(b->getToggleState()); }; return add(b, textW(font(FS * 0.92f), t) + 32, CTL()); }
    Gfx* gfx(std::function<void(juce::Graphics&, juce::Rectangle<float>)> draw, int h = 42) { auto* g = new Gfx(); g->draw = draw; g->h = h; return full(g, h); }

    // lays the cards out for a width; returns the height
    int layout(int W) {
        const int gap = 8, cols = std::max(1, (W - gap) / (minCol + gap)), cw = (W - gap * (cols + 1)) / cols;
        int y = gap; size_t i = 0;
        while (i < cards.size()) {
            int rowH = 0; std::vector<int> hs;
            for (int k = 0; k < cols && i + k < cards.size(); k++) { hs.push_back(place(cards[i + (size_t)k], gap + k * (cw + gap), y, cw, false)); rowH = std::max(rowH, hs.back()); }
            for (int k = 0; k < cols && i + k < cards.size(); k++) { auto& c = cards[i + (size_t)k]; place(c, gap + k * (cw + gap), y, cw, true); c.area.setHeight(rowH); }
            i += (size_t)cols; y += rowH + gap;
        }
        setSize(W, y); repaint(); if (onLayout) onLayout(); return y;
    }
    void paint(juce::Graphics& g) override {
        for (auto& c : cards) {
            ui::card(g, c.area.toFloat(), PANEL); int tx = c.area.getX() + 10;
            if (c.led) { auto l = c.led->getBounds().toFloat(); g.setColour(c.led->on() ? ACCENT : BG); g.fillEllipse(l); g.setColour(c.led->on() ? ACCENT.darker(0.4f) : LINE); g.drawEllipse(l.reduced(1), 2); if (c.led->on() && style().glow) { g.setColour(ACCENT.withAlpha(0.3f)); g.drawEllipse(l.expanded(2), 2); } tx = c.led->getRight() + 7; }
            cardTitle(g, c.title, { tx, c.area.getY() + 10, c.area.getRight() - tx - 10 - c.headW, 16 });
        }
    }
private:
    juce::OwnedArray<juce::Component> owned;
    template <class T> T* add0(T* c) { owned.add(c); addAndMakeVisible(c); return c; }
    int place(Card& c, int x, int y, int w, bool apply) {
        const int pad = 10, inner = w - 2 * pad, gapV = 9, gapH = 6; int cy = y + pad;
        if (apply && c.led) c.led->setBounds(x + pad, cy + 1, 14, 14);
        if (apply && c.head) c.head->setBounds(x + w - pad - c.headW, cy - 4, c.headW, CTL() - 6);
        cy += 16 + gapV;
        // rows of inline items (wrapping, bottom aligned) and full-width items
        std::vector<Entry*> row; int rx = 0;
        auto flush = [&] { if (row.empty()) return; int rh = 0; for (auto* e : row) rh = std::max(rh, e->h); int xx = x + pad; for (auto* e : row) { if (apply) e->c->setBounds(xx, cy + rh - e->h, e->w, e->h); xx += e->w + gapH; } cy += rh + gapV; row.clear(); rx = 0; };
        for (auto& e : c.e) {
            if (e.kind == 2) { flush(); continue; }
            if (e.kind == 1) { flush(); int h = e.h; if (h < 0) { auto* cap = static_cast<Cap*>(e.c); h = (textW(cap->getFont(), cap->getText()) / std::max(1, inner) + 1 + cap->getText().length() / 300) * (int)std::ceil(cap->getFont().getHeight() * 1.15f) + 2; } if (apply) e.c->setBounds(x + pad, cy, inner, h); cy += h + gapV; continue; }
            const int ew = std::min(e.w, inner); if (rx + ew > inner && !row.empty()) flush();
            e.w = ew; row.push_back(&e); rx += ew + gapH;
        }
        flush();
        if (apply) c.area = { x, y, w, cy - gapV + pad - y };
        return cy - gapV + pad - y;
    }
};

// a CardGrid in a scrolling view, with an optional fixed bar (.vbar) above it
class CardView : public juce::Component {
public:
    CardGrid grid; Flow bar; int barH = 0; std::function<void(Flow&)> makeBar; juce::Component* top = nullptr; int topH = 0;
    CardView() { view.setViewedComponent(&grid, false); view.setScrollBarsShown(true, false); addAndMakeVisible(view); }
    void resized() override {
        auto r = getLocalBounds(); barH = 0;
        if (makeBar) { Flow f; makeBar(f); barH = f.layout(r); r.removeFromTop(barH); }
        if (top) { top->setBounds(r.removeFromTop(topH).reduced(8, 0).withTrimmedTop(8)); }
        view.setBounds(r); grid.layout(std::max(300, view.getWidth() - 10));
    }
    void paint(juce::Graphics& g) override { g.fillAll(BG2); if (barH) vbarLine(g, barH, getWidth()); }
    void relayout() { grid.layout(std::max(300, view.getWidth() - 10)); }
    juce::Viewport view;
};

}   // namespace ui
