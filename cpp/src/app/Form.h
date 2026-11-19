// A scrolling panel of cards holding knobs, lists, toggles and buttons, laid out to flow and wrap. Used by the Audio Lab
// and Sound Design. Each control has a getter (pulled on refresh) and a setter; `changed(final)` runs after a change.
#pragma once
#include "Theme.h"

namespace ui {

class Form : public juce::Component {
public:
    std::function<void(bool final)> changed;   // final: knob released, list picked, toggle clicked
    std::function<void(juce::Graphics&, juce::Rectangle<int>)> paintTop; int topH = 0;   // e.g. a waveform above the cards
    Form() { view.setViewedComponent(&panel, false); view.setScrollBarsShown(true, false); addAndMakeVisible(view); panel.o = this; }
    void resized() override { view.setBounds(getLocalBounds()); layout(); }
    void clear() { cards.clear(); panel.removeAllChildren(); }
    int card(const juce::String& title) { cards.push_back({ title, {}, {} }); return (int)cards.size() - 1; }
    void knob(int c, const juce::String& label, double lo, double hi, double def, std::function<double()> get, std::function<void(double)> set, bool expo = false, std::function<juce::String(double)> fmt = {}, double step = 0) {
        auto k = std::make_unique<Knob>(label, lo, hi, def, expo); if (step > 0) k->s.setRange(lo, hi, step);
        k->fmt = fmt ? fmt : [step](double v) { return step >= 1 ? juce::String((int)std::round(v)) : juce::String(v, std::abs(v) < 10 ? 2 : 0); };
        k->onChange = [this, set](double v) { set(v); if (changed) changed(false); }; k->onDone = [this] { if (changed) changed(true); };
        auto* raw = k.get(); raw->set(get()); add(c, std::move(k), [raw, get] { raw->set(get()); }, 62, 76);
    }
    void combo(int c, const juce::String& label, std::vector<std::pair<std::string, juce::String>> opts, std::function<std::string()> get, std::function<void(const std::string&)> set, int w = 120) {
        struct Box : juce::Component { juce::Label l; juce::ComboBox b; Box() { addAndMakeVisible(l); addAndMakeVisible(b); l.setColour(juce::Label::textColourId, DIM); l.setFont(11.f); } void resized() override { auto r = getLocalBounds(); l.setBounds(r.removeFromTop(16)); b.setBounds(r.removeFromTop(34)); } };
        auto box = std::make_unique<Box>(); box->l.setText(label, juce::dontSendNotification); for (size_t i = 0; i < opts.size(); i++) box->b.addItem(opts[i].second, (int)i + 1);
        auto* b = &box->b; auto pull = [b, opts, get] { const std::string v = get(); for (size_t i = 0; i < opts.size(); i++) if (opts[i].first == v) { b->setSelectedItemIndex((int)i, juce::dontSendNotification); return; } b->setSelectedItemIndex(-1, juce::dontSendNotification); };
        pull(); b->onChange = [this, b, opts, set] { const int i = b->getSelectedItemIndex(); if (i >= 0) { set(opts[(size_t)i].first); if (changed) changed(true); } };
        add(c, std::move(box), pull, w, 56);
    }
    void toggle(int c, const juce::String& label, std::function<bool()> get, std::function<void(bool)> set, int w = 110) {
        auto t = std::make_unique<juce::ToggleButton>(label); t->setColour(juce::ToggleButton::textColourId, TEXT); t->setColour(juce::ToggleButton::tickColourId, ACCENT);
        auto* raw = t.get(); raw->setToggleState(get(), juce::dontSendNotification); raw->onClick = [this, raw, set] { set(raw->getToggleState()); if (changed) changed(true); };
        add(c, std::move(t), [raw, get] { raw->setToggleState(get(), juce::dontSendNotification); }, w, 36);
    }
    juce::TextButton* button(int c, const juce::String& label, std::function<void()> fn, int w = 140) {
        auto b = std::make_unique<juce::TextButton>(label); b->onClick = fn; auto* raw = b.get(); add(c, std::move(b), [] {}, w, ROW); return raw;
    }
    juce::Label* text(int c, int w = 300, int h = 60) {
        auto l = std::make_unique<juce::Label>(); l->setColour(juce::Label::textColourId, TEXT); l->setJustificationType(juce::Justification::topLeft); auto* raw = l.get(); add(c, std::move(l), [] {}, w, h); return raw;
    }
    void add(int c, std::unique_ptr<juce::Component> comp, std::function<void()> pull, int w, int h) { panel.addAndMakeVisible(*comp); cards[(size_t)c].ctls.push_back({ std::move(comp), std::move(pull), w, h }); }
    void pull() { for (auto& cd : cards) for (auto& ct : cd.ctls) ct.pull(); }
    void layout() {
        const int W = std::max(320, view.getWidth() - 14); int x = 8, y = 8 + topH, rowH = 0;
        for (auto& c : cards) {
            int cw = 20; for (auto& ct : c.ctls) cw += ct.w + 6; cw = std::min(std::max(cw, 160), W - 16);
            auto place = [&](bool apply, int ox, int oy) { int cx = 10, cy = 26, lineH = 0; for (auto& ct : c.ctls) { if (cx + ct.w > cw - 6) { cx = 10; cy += lineH + 6; lineH = 0; } if (apply) ct.comp->setBounds(ox + cx, oy + cy, ct.w, ct.h); cx += ct.w + 6; lineH = std::max(lineH, ct.h); } return cy + lineH + 10; };
            const int ch = place(false, 0, 0);
            if (x + cw > W) { x = 8; y += rowH + 8; rowH = 0; }
            c.area = { x, y, cw, ch }; place(true, x, y); x += cw + 8; rowH = std::max(rowH, ch);
        }
        panel.setSize(W, y + rowH + 16); panel.repaint();
    }
    void refreshTop() { panel.repaint(0, 0, panel.getWidth(), topH + 8); }
private:
    struct Ctl { std::unique_ptr<juce::Component> comp; std::function<void()> pull; int w, h; };
    struct Card { juce::String title; std::vector<Ctl> ctls; juce::Rectangle<int> area; };
    struct Panel : juce::Component {
        Form* o = nullptr;
        void paint(juce::Graphics& g) override {
            fillPanel(g);
            if (o->paintTop && o->topH) o->paintTop(g, { 8, 8, getWidth() - 16, o->topH - 8 });
            for (auto& c : o->cards) { g.setColour(PANEL.withMultipliedAlpha(panelAlpha())); g.fillRoundedRectangle(c.area.toFloat(), 9); g.setColour(DIM); g.setFont(juce::FontOptions(11.f, juce::Font::bold)); g.drawText(c.title.toUpperCase(), c.area.getX() + 10, c.area.getY() + 6, c.area.getWidth() - 20, 16, juce::Justification::centredLeft); }
        }
    };
    juce::Viewport view; Panel panel; std::vector<Card> cards;
};

inline void drawWave(juce::Graphics& g, juce::Rectangle<int> r, const std::vector<float>& x, juce::Colour col) {
    g.setColour(BG); g.fillRoundedRectangle(r.toFloat(), 8); if (x.empty()) return;
    g.setColour(col); const int W = r.getWidth(); const size_t per = std::max<size_t>(1, x.size() / (size_t)std::max(1, W));
    for (int i = 0; i < W; i++) { float pk = 0; for (size_t k = (size_t)i * per; k < std::min(x.size(), (size_t)(i + 1) * per); k++) pk = std::max(pk, std::abs(x[k])); const float h = pk * r.getHeight() * 0.46f; g.fillRect((float)(r.getX() + i), r.getCentreY() - h, 1.f, 2 * h + 1); }
}

}   // namespace ui
