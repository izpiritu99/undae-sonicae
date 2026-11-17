// Look of the native edition, ported from the app's stylesheet (app/css/app.css): its themes (Slate by default), 13 px
// text, 30 px controls (40 px on touch screens), 10 px corners, and its widgets: .btn, .tb icon buttons, .seg switches,
// .mini toggles, the conic knob, .grp boxes, cards, pill tabs, menus and the toast. Colours are variables: a theme
// change sets them and repaints the window.
#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include "Touch.h"

namespace ui {

// --- theme ---------------------------------------------------------------------------------------------------------
inline juce::Colour BG{ 0xff16181c }, BG2{ 0xff1e2126 }, BG3{ 0xff2a2e35 }, LINE{ 0xff3b414b }, TEXT{ 0xffe7eaef }, DIM{ 0xff8b93a1 },
    ACCENT{ 0xff6ea8ff }, ACCENT2{ 0xffffb86b }, ONACC{ 0xff16120f }, PANEL{ 0xff23272d }, TABBG{ 0xff1a1c20 };
const juce::Colour GOOD{ 0xff3ecf8e }, WARN{ 0xfff5c542 }, BAD{ 0xffe5484d }, RECLT{ 0xffff6b70 };
const juce::Colour PALETTE[] = { juce::Colour(0xffff7a59), juce::Colour(0xffffb84d), juce::Colour(0xffe6d15a), juce::Colour(0xff7bd88f), juce::Colour(0xff45d0c4), juce::Colour(0xff5aa9ff), juce::Colour(0xff9d8cff), juce::Colour(0xfff277c6) };

struct ThemeDef { const char* id; const char* name; juce::uint32 v[8]; bool light; };   // bg bg2 bg3 line text dim accent accent2
inline const ThemeDef THEMES[] = {
    { "ember", "Ember", { 0xff15131a, 0xff1e1b25, 0xff2a2633, 0xff3a3545, 0xffece8f1, 0xff9089a1, 0xffff7a59, 0xffffc857 }, false },
    { "tidal", "Tidal", { 0xff0c1618, 0xff122023, 0xff1a2d31, 0xff284349, 0xffdff3f1, 0xff7fa3a3, 0xff3ddbc0, 0xff7cc4ff }, false },
    { "orchid", "Orchid", { 0xff17121c, 0xff211a29, 0xff2e2439, 0xff45365a, 0xfff1e9f7, 0xffa290b5, 0xffd77bff, 0xffff8fb8 }, false },
    { "moss", "Moss", { 0xff11150f, 0xff191f16, 0xff242c1f, 0xff36422e, 0xffe9f0df, 0xff93a283, 0xffa6e05a, 0xfff2d95c }, false },
    { "slate", "Slate", { 0xff16181c, 0xff1e2126, 0xff2a2e35, 0xff3b414b, 0xffe7eaef, 0xff8b93a1, 0xff6ea8ff, 0xffffb86b }, false },
    { "mono", "Mono", { 0xff0d0d0d, 0xff161616, 0xff222222, 0xff353535, 0xfff0f0f0, 0xff8c8c8c, 0xfff0f0f0, 0xff9a9a9a }, false },
    { "paper", "Paper", { 0xffe9e2d6, 0xfff7f2e9, 0xffe2d9ca, 0xffcbbfab, 0xff2b2620, 0xff7d7364, 0xffd9480f, 0xff1c6fd1 }, true },
    { "frost", "Frost", { 0xffdde4ea, 0xfff3f6f9, 0xffd5dde5, 0xffb9c5d1, 0xff1d2530, 0xff66758a, 0xff2b6fe0, 0xffe0542b }, true },
};

// sizes and options from Settings › Theme (density, corner radius, scale, glow, step shape, grid contrast, note colours)
struct Style {
    float scale = 1, radius = 10, gridK = 1; int ctl = 30, pad = 6; bool glow = true, motion = true, light = false;
    std::string density = "cozy", stepShape = "lozenge", noteColor = "channel", theme = "slate"; bool bgOn = false;   // a background photo: panels see-through
};
inline Style& style() { static Style s; return s; }
inline bool isTouch() {
#if JUCE_ANDROID || JUCE_IOS
    return true;
#else
    return false;
#endif
}
inline int CTL() { return juce::roundToInt(style().ctl * style().scale); }   // control height (CSS --ctl)
inline int PAD() { return juce::roundToInt(style().pad * style().scale); }
inline float RAD() { return style().radius; }
inline int ROWH() { return CTL() + 2 * PAD(); }   // a toolbar row (.vbar)
#define ROW CTL()

inline juce::Colour mix(juce::Colour a, juce::Colour b, float aShare) { return b.interpolatedWith(a, aShare); }   // color-mix(in srgb, a x%, b)

inline void applyTheme(const std::string& id, const std::string& accentHex, const std::string& density, float radius, float scale) {
    const ThemeDef* t = &THEMES[4]; for (auto& x : THEMES) if (id == x.id) t = &x;
    BG = juce::Colour(t->v[0]); BG2 = juce::Colour(t->v[1]); BG3 = juce::Colour(t->v[2]); LINE = juce::Colour(t->v[3]); TEXT = juce::Colour(t->v[4]); DIM = juce::Colour(t->v[5]);
    ACCENT = juce::Colour(t->v[6]); ACCENT2 = juce::Colour(t->v[7]);
    if (accentHex.size() == 7 && accentHex[0] == '#') ACCENT = juce::Colour((juce::uint32)(0xff000000u | (juce::uint32)std::stoul(accentHex.substr(1), nullptr, 16)));
    const float lum = (0.299f * ACCENT.getRed() + 0.587f * ACCENT.getGreen() + 0.114f * ACCENT.getBlue()) / 255.f;
    ONACC = lum > 0.55f ? juce::Colour(0xff16120f) : juce::Colours::white;
    PANEL = mix(BG3, BG2, 0.45f); TABBG = mix(BG, BG2, 0.55f);
    auto& s = style(); s.theme = t->id; s.light = t->light; s.density = density; s.radius = radius; s.scale = scale;
    const bool touch = isTouch();
    s.ctl = density == "compact" ? (touch ? 36 : 25) : density == "roomy" ? (touch ? 46 : 36) : (touch ? 40 : 30);
    s.pad = density == "compact" ? (touch ? 6 : 4) : density == "roomy" ? (touch ? 10 : 9) : (touch ? 8 : 6);
}

inline juce::Colour hex(const std::string& s, juce::Colour def = ACCENT) {
    if (s.size() == 7 && s[0] == '#') return juce::Colour((juce::uint32)(0xff000000u | (juce::uint32)std::stoul(s.substr(1), nullptr, 16)));
    return def;
}
inline std::string toHex(juce::Colour c) { return ("#" + c.toDisplayString(false).toLowerCase()).toStdString(); }
inline juce::String u8(const char* s) { return juce::String::fromUTF8(s); }

// --- fonts: CSS px -> JUCE height (a JUCE font's height includes ascent and descent, about 1.17x the CSS size) -------
inline juce::String& sansName() { static juce::String n; return n; }
inline juce::Font font(float cssPx, int style_ = juce::Font::plain) { return juce::Font(juce::FontOptions(sansName(), cssPx * 1.17f * style().scale, style_)); }
inline int textW(const juce::Font& f, const juce::String& t) { return juce::GlyphArrangement::getStringWidthInt(f, t); }
inline juce::Font fontB(float cssPx) { return font(cssPx, juce::Font::bold); }
inline juce::Font mono(float cssPx, bool bold = true) { return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), cssPx * 1.17f * style().scale, bold ? juce::Font::bold : juce::Font::plain)); }
constexpr float FS = 13.f;   // the base size (CSS 13px)

// --- icons (util.js ICONS, 24x24 paths; stroked 2 px, or filled) ----------------------------------------------------
inline const std::map<std::string, std::pair<const char*, bool>>& iconDefs() {
    static const std::map<std::string, std::pair<const char*, bool>> m = {
        { "play", { "M8 5v14l11-7z", true } }, { "stop", { "M6 6h12v12H6z", true } }, { "rec", { "M12 6a6 6 0 1 0 0 12 6 6 0 0 0 0-12z", true } },
        { "metro", { "M9 3h6l4 18H5zM12 16l5-10", false } }, { "undo", { "M8 5L3 10l5 5M3 10h11a5 5 0 0 1 0 10h-4", false } }, { "redo", { "M16 5l5 5-5 5M21 10H10a5 5 0 0 0 0 10h4", false } },
        { "keys", { "M3 5h18v14H3zM8 5v8M12 5v8M16 5v8M9 13v6M15 13v6", false } }, { "menu", { "M4 6h16M4 12h16M4 18h16", false } }, { "plus", { "M12 5v14M5 12h14", false } },
        { "draw", { "M4 20l1-4L16 5l3 3L8 19z", false } }, { "select", { "M4 4h4M12 4h4M20 4v4M20 12v4M20 20h-4M12 20H8M4 20v-4M4 12V8", false } },
        { "slice", { "M5 19L19 5M14 5h5v5", false } }, { "erase", { "M7 20h11M5 13l7-8 7 6-6 8H9z", false } },
        { "pan", { "M12 3v18M3 12h18M9 6l3-3 3 3M9 18l3 3 3-3M6 9l-3 3 3 3M18 9l3 3-3 3", false } },
        { "midi", { "M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM8 13h.01M16 13h.01M12 16h.01M9 9h.01M15 9h.01", false } },
        { "rack", { "M4 6h3v3H4zM10.5 6h3v3h-3zM17 6h3v3h-3zM4 15h3v3H4zM10.5 15h3v3h-3zM17 15h3v3h-3z", false } },
        { "roll", { "M3 5h18v14H3zM8 5v14M11 9h6M13 15h6", false } }, { "song", { "M3 6h8M13 6h8M3 12h12M17 12h4M3 18h5M10 18h11", false } },
        { "mixer", { "M6 3v18M12 3v18M18 3v18M4 8h4M10 15h4M16 10h4", false } }, { "inst", { "M2 12c2-8 4-8 6 0s4 8 6 0 4-8 6 0", false } },
        { "lab", { "M12 3a3 3 0 0 0-3 3v6a3 3 0 0 0 6 0V6a3 3 0 0 0-3-3zM6 11a6 6 0 0 0 12 0M12 17v4", false } },
        { "lib", { "M3 6h6l2 2h10v11H3z", false } }, { "design", { "M12 3v4M12 17v4M3 12h4M17 12h4M6 6l2.5 2.5M15.5 15.5L18 18M18 6l-2.5 2.5M8.5 15.5L6 18", false } },
        { "set", { "M4 7h10M18 7h2M4 17h2M10 17h10M16 5v4M8 15v4", false } },
        // extra glyphs the stylesheet drew with text characters
        { "mute", { "M4 9h4l5-4v14l-5-4H4zM17 9l4 6M21 9l-4 6", false } }, { "zoom", { "M10 4a6 6 0 1 0 0 12 6 6 0 0 0 0-12zM14.5 14.5L20 20", false } },
        { "paint", { "M18 4l2 2-8 8-3-3zM9 11l-3 3c-1 1-1 3-3 4 3 1 5 0 6-1l3-3", false } }, { "slip", { "M3 12h18M7 8l-4 4 4 4M17 8l4 4-4 4", false } },
        { "playfrom", { "M6 4v16M10 6v12l9-6z", false } }, { "up", { "M12 19V5M6 11l6-6 6 6", false } }, { "down", { "M12 5v14M6 13l6 6 6-6", false } },
        { "x", { "M6 6l12 12M18 6L6 18", false } }, { "more", { "M5 12h.01M12 12h.01M19 12h.01", false } }, { "float", { "M4 14V4h10M10 10l10 10M20 12v8h-8", false } },
        { "layout", { "M3 4h18v16H3zM12 4v16", false } }, { "help", { "M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM9.5 9a2.5 2.5 0 1 1 3.5 2.3c-.6.3-1 .9-1 1.6V14M12 17h.01", false } },
        { "steps", { "M4 8h4v8H4zM10 8h4v8h-4zM16 8h4v8h-4z", false } }, { "notes", { "M4 7h6M8 12h8M12 17h8", false } }, { "wave", { "M3 12h2l2-6 3 12 3-9 2 6 2-3h4", false } },
        { "folder", { "M3 6h6l2 2h10v11H3z", false } }, { "search", { "M10 4a6 6 0 1 0 0 12 6 6 0 0 0 0-12zM14.5 14.5L20 20", false } },
        { "check", { "M5 12l5 5 9-10", false } }, { "dot", { "M12 8a4 4 0 1 0 0 8 4 4 0 0 0 0-8z", true } },
    };
    return m;
}
inline const juce::Path& iconPath(const std::string& name) {
    static std::map<std::string, juce::Path> cache;
    auto it = cache.find(name); if (it != cache.end()) return it->second;
    const auto& d = iconDefs(); auto f = d.find(name); juce::Path p = juce::Drawable::parseSVGPath(f != d.end() ? f->second.first : d.at("plus").first);
    return cache[name] = p;
}
inline bool iconFilled(const std::string& name) { const auto& d = iconDefs(); auto f = d.find(name); return f != d.end() && f->second.second; }
// draws the 24x24 icon fitted to `size` px, centred in r
inline void drawIcon(juce::Graphics& g, const std::string& name, juce::Rectangle<float> r, juce::Colour c, float size = 18) {
    size *= style().scale; const float k = size / 24.f;
    auto t = juce::AffineTransform::scale(k).translated(r.getCentreX() - size / 2, r.getCentreY() - size / 2);
    g.setColour(c); const auto& p = iconPath(name);
    if (iconFilled(name)) g.fillPath(p, t);
    else g.strokePath(p, juce::PathStrokeType(2.f * k < 1.2f ? 1.2f : 2.f * k, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), t);
}

// --- painting helpers --------------------------------------------------------------------------------------------------
inline void card(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour bg = PANEL) { g.setColour(bg); g.fillRoundedRectangle(r, RAD()); g.setColour(LINE); g.drawRoundedRectangle(r.reduced(0.5f), RAD(), 1); }
inline void cardTitle(juce::Graphics& g, const juce::String& t, juce::Rectangle<int> r) {   // .card h4: uppercase, spaced, dim
    g.setColour(DIM); auto f = fontB(FS * 0.82f); f.setExtraKerningFactor(0.09f); g.setFont(f); g.drawText(t.toUpperCase(), r, juce::Justification::centredLeft, true);
}
inline void vbarLine(juce::Graphics& g, int y, int w) { g.setColour(LINE); g.fillRect(0, y - 1, w, 1); }

// --- the look and feel -------------------------------------------------------------------------------------------------
class Look : public juce::LookAndFeel_V4 {
public:
    Look() {
        for (const char* n : { "Noto Sans", "Ubuntu", "Cantarell", "Roboto", "DejaVu Sans" }) if (juce::Font::findAllTypefaceNames().contains(n)) { sansName() = n; break; }
        if (sansName().isNotEmpty()) setDefaultSansSerifTypefaceName(sansName());
        refresh();
    }
    void refresh() {
        setColourScheme({ BG2, BG, BG2, LINE, TEXT, ACCENT, ONACC, BG3, TEXT });
        setColour(juce::TextButton::buttonColourId, BG3); setColour(juce::TextButton::buttonOnColourId, ACCENT);
        setColour(juce::TextButton::textColourOffId, TEXT); setColour(juce::TextButton::textColourOnId, ONACC);
        setColour(juce::ComboBox::backgroundColourId, BG3); setColour(juce::ComboBox::outlineColourId, LINE); setColour(juce::ComboBox::textColourId, TEXT); setColour(juce::ComboBox::arrowColourId, DIM);
        setColour(juce::PopupMenu::backgroundColourId, BG2); setColour(juce::PopupMenu::highlightedBackgroundColourId, BG3); setColour(juce::PopupMenu::textColourId, TEXT); setColour(juce::PopupMenu::highlightedTextColourId, TEXT); setColour(juce::PopupMenu::headerTextColourId, DIM);
        setColour(juce::Label::textColourId, TEXT); setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack); setColour(juce::Slider::textBoxTextColourId, TEXT);
        setColour(juce::ScrollBar::thumbColourId, LINE); setColour(juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);
        setColour(juce::TextEditor::backgroundColourId, BG3); setColour(juce::TextEditor::outlineColourId, LINE); setColour(juce::TextEditor::focusedOutlineColourId, ACCENT2); setColour(juce::TextEditor::textColourId, TEXT); setColour(juce::TextEditor::highlightColourId, ACCENT.withAlpha(0.35f)); setColour(juce::CaretComponent::caretColourId, TEXT);
        setColour(juce::ToggleButton::textColourId, TEXT); setColour(juce::ToggleButton::tickColourId, ONACC); setColour(juce::ToggleButton::tickDisabledColourId, DIM);
        setColour(juce::AlertWindow::backgroundColourId, BG2); setColour(juce::AlertWindow::textColourId, TEXT); setColour(juce::AlertWindow::outlineColourId, LINE);
        setColour(juce::TooltipWindow::backgroundColourId, BG3); setColour(juce::TooltipWindow::textColourId, TEXT); setColour(juce::TooltipWindow::outlineColourId, LINE);
        setColour(juce::ResizableWindow::backgroundColourId, BG); setColour(juce::DocumentWindow::textColourId, TEXT);
        setColour(juce::ListBox::backgroundColourId, BG2); setColour(juce::ListBox::textColourId, TEXT);
        setColour(juce::Slider::trackColourId, ACCENT); setColour(juce::Slider::backgroundColourId, BG); setColour(juce::Slider::thumbColourId, TEXT);
        setColour(juce::GroupComponent::outlineColourId, LINE); setColour(juce::GroupComponent::textColourId, DIM);
        setColour(juce::ProgressBar::backgroundColourId, BG); setColour(juce::ProgressBar::foregroundColourId, ACCENT);
    }
    juce::Font getTextButtonFont(juce::TextButton& b, int) override { return b.getToggleState() ? fontB(FS) : font(FS); }
    juce::Font getLabelFont(juce::Label& l) override { return l.getFont().getHeight() == 15.f ? font(FS) : l.getFont(); }   // JUCE's default 15 -> our 13 px
    juce::Font getComboBoxFont(juce::ComboBox&) override { return font(FS); }
    juce::Font getPopupMenuFont() override { return font(FS); }
    juce::Font getAlertWindowMessageFont() override { return font(FS); }
    juce::Font getAlertWindowTitleFont() override { return fontB(FS * 1.1f); }
    juce::Font getAlertWindowFont() override { return font(FS); }
    void getIdealPopupMenuItemSize(const juce::String& t, bool sep, int, int& w, int& h) override {
        if (sep) { w = 50; h = 9; return; }
        w = textW(font(FS), t) + 60; h = isTouch() ? 44 : juce::roundToInt(30 * style().scale);
    }
    void drawPopupMenuBackground(juce::Graphics& g, int w, int h) override {
        g.fillAll(BG2); g.setColour(LINE); g.drawRect(0, 0, w, h, 1);
    }
    int getPopupMenuBorderSize() override { return 4; }
    void drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area, bool sep, bool active, bool hl, bool ticked, bool hasSub, const juce::String& text, const juce::String& key, const juce::Drawable*, const juce::Colour*) override {
        if (sep) { g.setColour(LINE); g.fillRect(area.getX() + 2, area.getCentreY(), area.getWidth() - 4, 1); return; }
        auto r = area.toFloat().reduced(2, 1);
        if (hl && active) { g.setColour(BG3); g.fillRoundedRectangle(r, RAD() * 0.6f); }
        g.setColour(!active ? TEXT.withAlpha(0.4f) : ticked ? ACCENT : TEXT); g.setFont(font(FS));
        auto t = area.reduced(12, 0); if (ticked) { drawIcon(g, "check", t.removeFromRight(18).toFloat(), ACCENT, 14); }
        if (hasSub) { g.setColour(DIM); g.drawText(u8("›"), t.removeFromRight(16), juce::Justification::centredRight); g.setColour(active ? (ticked ? ACCENT : TEXT) : TEXT.withAlpha(0.4f)); }
        if (key.isNotEmpty()) { g.setColour(DIM); g.setFont(font(FS * 0.88f)); g.drawText(key, t, juce::Justification::centredRight); g.setFont(font(FS)); g.setColour(active ? (ticked ? ACCENT : TEXT) : TEXT.withAlpha(0.4f)); }
        g.drawFittedText(text, t, juce::Justification::centredLeft, 1);
    }
    void drawPopupMenuSectionHeader(juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& t) override {   // .mhead
        g.setColour(DIM); auto f = font(FS * 0.76f); f.setExtraKerningFactor(0.08f); g.setFont(f); g.drawText(t.toUpperCase(), area.reduced(12, 0).withTrimmedTop(4), juce::Justification::centredLeft, true);
    }
    void drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour& bg, bool over, bool down) override {   // .btn
        auto r = b.getLocalBounds().toFloat().reduced(0.5f).translated(0, down ? 1.f : 0.f); const float rad = RAD() * 0.7f;
        const bool on = b.getToggleState();
        juce::Colour c = on ? b.findColour(juce::TextButton::buttonOnColourId) : bg; if (over && !on) c = mix(c, TEXT, 0.8f);
        g.setColour(b.isEnabled() ? c : c.withMultipliedAlpha(0.5f)); g.fillRoundedRectangle(r, rad);
        if (!on) { g.setColour(LINE); g.drawRoundedRectangle(r, rad, 1); }
    }
    void drawButtonText(juce::Graphics& g, juce::TextButton& b, bool, bool down) override {
        g.setFont(getTextButtonFont(b, b.getHeight())); g.setColour(b.findColour(b.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId).withMultipliedAlpha(b.isEnabled() ? 1 : 0.5f));
        g.drawFittedText(b.getButtonText(), b.getLocalBounds().reduced(6, 0).translated(0, down ? 1 : 0), juce::Justification::centred, 1);
    }
    void drawComboBox(juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& b) override {   // select
        auto r = juce::Rectangle<float>(0, 0, (float)w, (float)h).reduced(0.5f); const float rad = RAD() * 0.7f;
        g.setColour(b.findColour(juce::ComboBox::backgroundColourId)); g.fillRoundedRectangle(r, rad); g.setColour(LINE); g.drawRoundedRectangle(r, rad, 1);
        juce::Path a; const float cx = w - 12.f, cy = h * 0.5f; a.startNewSubPath(cx - 4, cy - 2); a.lineTo(cx, cy + 2); a.lineTo(cx + 4, cy - 2);
        g.setColour(TEXT.withAlpha(b.isEnabled() ? 0.85f : 0.4f)); g.strokePath(a, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    void positionComboBoxText(juce::ComboBox& b, juce::Label& l) override { l.setBounds(4, 0, b.getWidth() - 22, b.getHeight()); l.setFont(getComboBoxFont(b)); }
    juce::PopupMenu::Options getOptionsForComboBoxPopupMenu(juce::ComboBox& b, juce::Label& l) override { return LookAndFeel_V4::getOptionsForComboBoxPopupMenu(b, l).withMinimumWidth(b.getWidth()); }
    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& b, bool over, bool) override {   // checkbox + label
        const float s = 17 * style().scale; auto box = juce::Rectangle<float>(2, (b.getHeight() - s) / 2, s, s);
        g.setColour(b.getToggleState() ? ACCENT : BG3); g.fillRoundedRectangle(box, 4);
        if (!b.getToggleState()) { g.setColour(over ? DIM : LINE); g.drawRoundedRectangle(box.reduced(0.5f), 4, 1); }
        else drawIcon(g, "check", box, ONACC, s * 0.8f);
        g.setColour(b.findColour(juce::ToggleButton::textColourId).withMultipliedAlpha(b.isEnabled() ? 1 : 0.5f)); g.setFont(font(FS * 0.92f));
        g.drawFittedText(b.getButtonText(), b.getLocalBounds().withTrimmedLeft((int)s + 9), juce::Justification::centredLeft, 2);
    }
    void drawScrollbar(juce::Graphics& g, juce::ScrollBar&, int x, int y, int w, int h, bool vert, int tp, int ts, bool over, bool) override {
        juce::Rectangle<float> t = vert ? juce::Rectangle<float>((float)x + 1, (float)tp, (float)w - 2, (float)ts) : juce::Rectangle<float>((float)tp, (float)y + 1, (float)ts, (float)h - 2);
        g.setColour(over ? DIM : LINE); g.fillRoundedRectangle(t.reduced(1), std::min(t.getWidth(), t.getHeight()) / 2);
    }
    int getDefaultScrollbarWidth() override { return isTouch() ? 10 : 9; }
    void fillTextEditorBackground(juce::Graphics& g, int w, int h, juce::TextEditor& e) override { g.setColour(e.findColour(juce::TextEditor::backgroundColourId)); g.fillRoundedRectangle(0, 0, (float)w, (float)h, RAD() * 0.7f); }
    void drawTextEditorOutline(juce::Graphics& g, int w, int h, juce::TextEditor& e) override { g.setColour(e.hasKeyboardFocus(true) ? ACCENT2 : LINE); g.drawRoundedRectangle(0.5f, 0.5f, w - 1.f, h - 1.f, RAD() * 0.7f, 1); }
    void drawAlertBox(juce::Graphics& g, juce::AlertWindow& a, const juce::Rectangle<int>& text, juce::TextLayout& tl) override {
        auto r = a.getLocalBounds().toFloat(); g.setColour(BG2); g.fillRoundedRectangle(r, RAD() * 1.3f); g.setColour(LINE); g.drawRoundedRectangle(r.reduced(0.5f), RAD() * 1.3f, 1);
        g.setColour(TEXT); tl.draw(g, text.toFloat());
    }
    int getAlertWindowButtonHeight() override { return CTL(); }
    void drawTooltip(juce::Graphics& g, const juce::String& t, int w, int h) override {
        g.setColour(BG3); g.fillRoundedRectangle(0, 0, (float)w, (float)h, 6); g.setColour(LINE); g.drawRoundedRectangle(0.5f, 0.5f, w - 1.f, h - 1.f, 6, 1);
        g.setColour(TEXT); g.setFont(font(FS * 0.9f)); g.drawFittedText(t, juce::Rectangle<int>(w, h).reduced(8, 4), juce::Justification::centredLeft, 6);
    }
    juce::Rectangle<int> getTooltipBounds(const juce::String& t, juce::Point<int> p, juce::Rectangle<int> parent) override {
        const int w = std::min(360, textW(font(FS * 0.9f), t) + 20), lines = textW(font(FS * 0.9f), t) / 340 + 1, h = lines * 18 + 10;
        return juce::Rectangle<int>(p.x > parent.getCentreX() ? p.x - w - 8 : p.x + 12, p.y > parent.getCentreY() ? p.y - h - 6 : p.y + 18, w, h).constrainedWithin(parent);
    }
    // knob: conic ring from 225° (the start, or the middle for bipolar), 4 px wide, the dial face inset, a pointer line
    void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float, float, juce::Slider& s) override {
        const float d = (float)std::min(w, h), cx = x + w * 0.5f, cy = y + h * 0.5f, r = d / 2, ring = d <= 26 ? 3.f : 4.f;
        const float a0 = juce::MathConstants<float>::pi * 1.25f, span = juce::MathConstants<float>::pi * 1.5f;   // JUCE angles: 0 = up, clockwise
        auto arc = [&](float from, float to, juce::Colour c) {
            if (to <= from) return; juce::Path p; p.addPieSegment(cx - r, cy - r, d, d, a0 + from * span, a0 + to * span, 1 - ring / r * 1.6f); g.setColour(c); g.fillPath(p);
        };
        const bool bi = s.getProperties().getWithDefault("bipolar", s.getMinimum() < 0 && s.getMaximum() > 0);
        const float st = bi ? std::min(pos, 0.5f) : 0.f, en = bi ? std::max(pos, 0.5f) : pos;
        arc(0, st, LINE); arc(st, en, s.isEnabled() ? ACCENT : DIM); arc(en, 1, LINE);
        const float in = ring; auto face = juce::Rectangle<float>(cx - r + in, cy - r + in, d - 2 * in, d - 2 * in);
        g.setColour(juce::Colours::black.withAlpha(0.25f)); g.fillEllipse(face.translated(0, 1.5f));
        g.setColour(BG3); g.fillEllipse(face);
        g.setColour(juce::Colours::white.withAlpha(0.07f)); g.drawEllipse(face.reduced(0.5f).withTrimmedBottom(face.getHeight() * 0.5f), 0.8f);
        juce::Path p; p.addRoundedRectangle(-1.f, 0.f, 2.f, d * 0.4f, 1.f);
        g.setColour(TEXT); g.fillPath(p, juce::AffineTransform::rotation(juce::MathConstants<float>::pi * 0.25f + pos * span).translated(cx, cy));
    }
    // horizontal slider = <input type=range>, vertical = the mixer fader
    void drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float, float, juce::Slider::SliderStyle st, juce::Slider& s) override {
        if (st == juce::Slider::LinearVertical) {
            const float cx = x + w * 0.5f; g.setColour(BG); g.fillRoundedRectangle(cx - 3, (float)y, 6, (float)h, 3);
            g.setColour(ACCENT); g.fillRoundedRectangle(cx - 3, pos, 6, y + h - pos, 3);
            g.setColour(TEXT); g.fillEllipse(cx - 8, pos - 8, 16, 16); g.setColour(juce::Colours::black.withAlpha(0.25f)); g.drawEllipse(cx - 8, pos - 8, 16, 16, 1);
            return;
        }
        if (st == juce::Slider::LinearHorizontal || st == juce::Slider::LinearBar) {
            const float cy = y + h * 0.5f; g.setColour(BG3); g.fillRoundedRectangle((float)x, cy - 3, (float)w, 6, 3); g.setColour(LINE); g.drawRoundedRectangle((float)x, cy - 3, (float)w, 6, 3, 1);
            g.setColour(ACCENT); g.fillRoundedRectangle((float)x, cy - 3, pos - x, 6, 3);
            g.setColour(ACCENT); g.fillEllipse(pos - 8, cy - 8, 16, 16); g.setColour(TEXT); g.fillEllipse(pos - 4, cy - 4, 8, 8);
            return;
        }
        LookAndFeel_V4::drawLinearSlider(g, x, y, w, h, pos, 0, 0, st, s);
    }
    void drawProgressBar(juce::Graphics& g, juce::ProgressBar&, int w, int h, double p, const juce::String& t) override {
        g.setColour(BG); g.fillRoundedRectangle(0, h / 2.f - 3, (float)w, 6, 3); if (p >= 0) { g.setColour(ACCENT); g.fillRoundedRectangle(0, h / 2.f - 3, (float)(w * p), 6, 3); }
        if (t.isNotEmpty()) { g.setColour(DIM); g.setFont(font(FS * 0.8f)); g.drawText(t, 0, 0, w, h / 2 - 3, juce::Justification::centredLeft); }
    }
    void drawCornerResizer(juce::Graphics&, int, int, bool, bool) override {}
    void drawDocumentWindowTitleBar(juce::DocumentWindow& w, juce::Graphics& g, int wd, int ht, int, int, const juce::Image*, bool) override { g.fillAll(BG); g.setColour(TEXT); g.setFont(font(FS)); g.drawText(w.getName(), 0, 0, wd, ht, juce::Justification::centred); }
};

// --- widgets ------------------------------------------------------------------------------------------------------------
// A button in the app's styles: Btn (.btn: text, bordered), Tb (.tb: an icon, transparent until hovered), Big (.tb.big,
// the play button), Mini (.mini: a small square toggle), BusNo (.busno: a mixer bus number), Tab (a deck tab), SegItem.
class Btn : public juce::Button {
public:
    static bool longPress(juce::Button* b) { if (auto* x = dynamic_cast<Btn*>(b); x && x->onRight) { x->onRight(); return true; } return false; }
    enum Kind { Text, Tb, Big, Mini, BusNo, Tab, Chip, Dashed, FxLed };
    Kind kind = Text; std::string ico; bool primary = false, rec = false, sm = false, monoFont = false, showLabel = true;
    juce::Colour onCol, onText; float iconSize = 18;   // .mini.on colour (accent2 by default; mute uses red)
    std::function<void()> onRight;   // right-click / long press
    Btn(const juce::String& text = {}, Kind k = Text, const std::string& icon = {}) : juce::Button(text), kind(k), ico(icon) {
        setMouseCursor(juce::MouseCursor::PointingHandCursor); setWantsKeyboardFocus(false);
        onCol = k == Mini ? ACCENT2 : ACCENT;
    }
    Btn& tip(const juce::String& t) { setTooltip(t); return *this; }
    void setOn(bool on) { setToggleState(on, juce::dontSendNotification); }
    bool on() const { return getToggleState(); }
    int idealWidth() const {
        const int c = CTL();
        switch (kind) {
        case Tb: return ico.empty() ? std::max(c, textW(font(FS), getButtonText()) + 14) : c;
        case Big: return juce::roundToInt(c * 1.35f);
        case Mini: return std::max(juce::roundToInt(22 * style().scale), (ico.empty() ? textW(font(FS * 0.82f, juce::Font::bold), getButtonText()) : 14) + 8);
        case BusNo: return std::max(juce::roundToInt(24 * style().scale), textW(mono(FS * 0.78f), getButtonText()) + 10);
        case Tab: return (ico.empty() ? 0 : juce::roundToInt(15 * style().scale) + 6) + (showLabel ? textW(font(FS * 0.9f), getButtonText()) : 0) + (showLabel ? 20 : 18);
        default: return (ico.empty() ? 0 : 22) + (getButtonText().isEmpty() ? 0 : (monoFont ? textW(mono(FS), getButtonText()) : textW(on() || primary ? fontB(FS) : font(FS), getButtonText()))) + (sm ? 16 : 24);
        }
    }
    void mouseDown(const juce::MouseEvent& e) override { if (e.mods.isPopupMenu() && onRight) { onRight(); return; } juce::Button::mouseDown(e); }
    void clicked() override { if (wantsMenu() && onRight) { onRight(); return; } }
    void paintButton(juce::Graphics& g, bool over, bool down) override {
        auto r = getLocalBounds().toFloat(); const float rad = RAD() * 0.7f; const bool o = on(), en = isEnabled(); const float a = en ? 1.f : 0.4f;
        switch (kind) {
        case Text: case Chip: case Dashed: {
            if (sm) r = r.withSizeKeepingCentre(r.getWidth(), std::min(r.getHeight(), (float)CTL() - 4));
            r = r.reduced(0.5f).translated(0, down ? 1.f : 0);
            const bool fill = o || primary; juce::Colour bg = kind == Dashed ? juce::Colours::transparentBlack : fill ? (rec ? BAD : ACCENT) : BG3;
            if (over && !fill) bg = kind == Dashed ? BG3.withAlpha(0.4f) : mix(BG3, TEXT, 0.8f);
            g.setColour(bg.withMultipliedAlpha(a)); g.fillRoundedRectangle(r, kind == Chip ? r.getHeight() / 2 : rad);
            if (kind == Dashed) { juce::Path p; p.addRoundedRectangle(r, rad); juce::PathStrokeType(1).createDashedStroke(p, p, std::array<float, 2>{ 4.f, 3.f }.data(), 2); g.setColour(over ? ACCENT : LINE); g.fillPath(p); }
            else if (!fill) { g.setColour(rec ? BAD : LINE); g.drawRoundedRectangle(r, kind == Chip ? r.getHeight() / 2 : rad, 1); }
            const juce::Colour tc = (fill ? (rec ? juce::Colours::white : ONACC) : rec ? juce::Colour(0xffff8a8d) : kind == Dashed ? DIM : TEXT).withMultipliedAlpha(a);
            auto t = r.reduced(sm ? 8.f : 12.f, 0);
            if (!ico.empty()) { auto ir = getButtonText().isEmpty() ? t : t.removeFromLeft(18); drawIcon(g, ico, ir, tc, 16); if (getButtonText().isNotEmpty()) t.removeFromLeft(4); }
            g.setColour(tc); g.setFont(kind == Dashed && getButtonText() == "+" ? font(FS * 1.4f) : monoFont ? mono(FS) : fill ? fontB(FS) : font(FS)); if (getButtonText().isNotEmpty()) g.drawFittedText(getButtonText(), t.toNearestInt(), ico.empty() ? juce::Justification::centred : juce::Justification::centredLeft, 1, 0.8f);
            break;
        }
        case Tb: case Big: {
            const bool big = kind == Big; juce::Colour bg = o ? (rec ? BAD : ACCENT) : big ? BG3 : over ? BG3 : juce::Colours::transparentBlack;
            g.setColour(bg.withMultipliedAlpha(a)); g.fillRoundedRectangle(r, rad);
            const juce::Colour ic = (o ? (rec ? juce::Colours::white : ONACC) : rec ? RECLT : big || over ? TEXT : DIM).withMultipliedAlpha(a);
            if (!ico.empty()) drawIcon(g, ico, r, ic, iconSize); else { g.setColour(ic); g.setFont(font(FS * 1.1f)); g.drawText(getButtonText(), r, juce::Justification::centred); }
            break;
        }
        case Mini: {
            auto m = r.withSizeKeepingCentre(r.getWidth(), std::min(r.getHeight(), 22.f * style().scale));
            g.setColour((o ? onCol : BG3).withMultipliedAlpha(a)); g.fillRoundedRectangle(m, 6);
            const juce::Colour tc = (o ? (onText.isTransparent() ? (onCol == BAD ? juce::Colours::white : juce::Colour(0xff16120f)) : onText) : DIM).withMultipliedAlpha(a);
            if (!ico.empty()) drawIcon(g, ico, m, tc, 14); else { g.setColour(tc); g.setFont(fontB(FS * 0.82f)); g.drawText(getButtonText(), m, juce::Justification::centred); }
            break;
        }
        case FxLed: {   // .fxled: a mixer strip's effect switch
            g.setColour(o ? mix(ACCENT, BG, 0.3f) : BG); g.fillRoundedRectangle(r, 5);
            g.setColour(o ? TEXT : DIM); auto f = font(FS * 0.7f); f.setExtraKerningFactor(0.04f); g.setFont(f); g.drawText(getButtonText().toUpperCase(), r, juce::Justification::centred);
            break;
        }
        case BusNo: {
            auto m = r.withSizeKeepingCentre(r.getWidth(), std::min(r.getHeight(), 22.f * style().scale)).reduced(0.5f);
            g.setColour(BG); g.fillRoundedRectangle(m, 5); g.setColour(over ? ACCENT : LINE); g.drawRoundedRectangle(m, 5, 1);
            g.setColour(ACCENT2.withMultipliedAlpha(a)); g.setFont(mono(FS * 0.78f)); g.drawText(getButtonText(), m, juce::Justification::centred);
            break;
        }
        case Tab: {
            auto m = r.withSizeKeepingCentre(r.getWidth(), std::min(r.getHeight(), (float)CTL() - 2));
            if (o) { g.setColour(BG3); g.fillRoundedRectangle(m, m.getHeight() / 2); g.saveState(); g.reduceClipRegion(m.toNearestInt().removeFromBottom(2)); g.setColour(ACCENT); g.fillRoundedRectangle(m, m.getHeight() / 2); g.restoreState(); }
            auto t = m.reduced(10, 0); const juce::Colour tc = o || over ? TEXT : DIM;
            if (!ico.empty()) { drawIcon(g, ico, showLabel ? t.removeFromLeft(15 * style().scale) : t, o ? ACCENT : tc, 15); t.removeFromLeft(6); }
            if (showLabel) { g.setColour(tc); g.setFont(font(FS * 0.9f)); g.drawText(getButtonText(), t, juce::Justification::centredLeft, false); }
            break;
        }
        }
    }
};

inline const bool btnLongPressHook = (longPressHook() = [](juce::Button* b) { return Btn::longPress(b); }, true);   // long press = the button's right-click action

// .seg: a group of options, one on (PAT | SONG, Steps | Notes | Wave, the tool row)
class Seg : public juce::Component {
public:
    std::function<void(int)> onChange; bool icons = false;
    Seg(std::vector<juce::String> labels, std::vector<std::string> icons_ = {}) {
        for (size_t i = 0; i < labels.size(); i++) { auto* b = items.add(new Item(labels[i], i < icons_.size() ? icons_[i] : std::string())); b->onClick = [this, i] { set((int)i); if (onChange) onChange((int)i); }; addAndMakeVisible(b); }
        icons = !icons_.empty();
    }
    void set(int i) { cur = i; for (int k = 0; k < items.size(); k++) items[k]->setToggleState(k == i, juce::dontSendNotification); }
    int get() const { return cur; }
    void tip(int i, const juce::String& t) { if (i < items.size()) items[i]->setTooltip(t); }
    juce::Button* item(int i) { return items[i]; }
    int idealWidth() const { int w = 4; for (auto* b : items) w += b->ideal() + 2; return w; }
    void paint(juce::Graphics& g) override { g.setColour(BG3); g.fillRoundedRectangle(getLocalBounds().toFloat(), RAD() * 0.7f); }
    void resized() override { auto r = getLocalBounds().reduced(2); for (auto* b : items) { b->setBounds(r.removeFromLeft(b->ideal())); r.removeFromLeft(2); } }
private:
    struct Item : juce::Button {
        std::string ico; Item(const juce::String& t, const std::string& i) : juce::Button(t), ico(i) { setMouseCursor(juce::MouseCursor::PointingHandCursor); setWantsKeyboardFocus(false); }
        int ideal() const { return ico.empty() ? std::max(CTL(), textW(fontB(FS * 0.86f), getButtonText()) + 16) : CTL() - 2; }
        void paintButton(juce::Graphics& g, bool over, bool) override {
            auto r = getLocalBounds().toFloat(); if (getToggleState()) { g.setColour(ACCENT); g.fillRoundedRectangle(r, RAD() * 0.55f); }
            const juce::Colour c = getToggleState() ? ONACC : over ? TEXT : DIM;
            if (!ico.empty()) drawIcon(g, ico, r, c, 16); else { g.setColour(c); g.setFont(fontB(FS * 0.86f)); g.drawText(getButtonText(), r, juce::Justification::centred); }
        }
    };
    juce::OwnedArray<Item> items; int cur = -1;
};

// .grp: the top bar's boxes
inline void paintGrp(juce::Graphics& g, juce::Rectangle<int> r) { g.setColour(style().bgOn ? BG2.withAlpha(0.78f) : BG2); g.fillRoundedRectangle(r.toFloat(), RAD()); g.setColour(LINE); g.drawRoundedRectangle(r.toFloat().reduced(0.5f), RAD(), 1); }

// A dim caption (label, small)
class Cap : public juce::Label {
public:
    Cap(const juce::String& t = {}, float px = FS * 0.88f) { setText(t, juce::dontSendNotification); setFont(ui::font(px)); setColour(textColourId, DIM); setInterceptsMouseClicks(false, false); setMinimumHorizontalScale(0.8f); setBorderSize({ 0, 1, 0, 1 }); }
    int idealWidth() const { return textW(getFont(), getText()) + 4; }
};

// Text field (.txt / .bpm)
class Field : public juce::TextEditor {
public:
    std::function<void(const juce::String&)> onDone;
    explicit Field(bool centred = false, bool bold = false) { setFont(bold ? fontB(FS) : font(FS)); setJustification(centred ? juce::Justification::centred : juce::Justification::centredLeft); setIndents(8, 0); setSelectAllWhenFocused(true);
        onReturnKey = [this] { if (onDone) onDone(getText()); unfocusAllComponents(); }; onFocusLost = [this] { if (onDone) onDone(getText()); }; onEscapeKey = [this] { unfocusAllComponents(); }; }
    void resized() override { juce::TextEditor::resized(); setIndents(8, juce::jmax(0, (getHeight() - (int)getFont().getHeight()) / 2)); }
};

// --- dialogs and the toast --------------------------------------------------------------------------------------------
inline std::function<void(const juce::String&)>& toastSink() { static std::function<void(const juce::String&)> f; return f; }
inline void toast(const juce::String& msg) { if (toastSink()) toastSink()(msg); }

// ask(): a text prompt (util.js ask)
inline void ask(const juce::String& title, const juce::String& value, std::function<void(juce::String)> done) {
    auto* w = new juce::AlertWindow(title, {}, juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("v", value); if (auto* e = w->getTextEditor("v")) { e->setFont(font(FS)); e->setSelectAllWhenFocused(true); }
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey)); w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([w, done](int ok) { if (ok) done(w->getTextEditorContents("v")); }), true);
}
inline void confirm(const juce::String& title, const juce::String& msg, const juce::String& yes, std::function<void()> fn) {
    juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::NoIcon, title, msg, yes, "Cancel", nullptr, juce::ModalCallbackFunction::create([fn](int r) { if (r) fn(); }));
}
inline void message(const juce::String& title, const juce::String& msg) { juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::NoIcon, title, msg); }

// menus at a component (menuAt) or at the mouse
inline juce::PopupMenu::Options at(juce::Component* c) { return juce::PopupMenu::Options().withTargetComponent(c).withMinimumWidth(190); }
inline juce::PopupMenu::Options atMouse() { return juce::PopupMenu::Options().withMousePosition().withMinimumWidth(190); }
// a menu built from lambdas: items carry their own action
struct Menu {
    juce::PopupMenu m; std::vector<std::function<void()>> fns;
    Menu& head(const juce::String& t) { m.addSectionHeader(t); return *this; }
    Menu& item(const juce::String& t, std::function<void()> fn, bool enabled = true, bool ticked = false, const juce::String& hint = {}) {
        fns.push_back(std::move(fn)); juce::PopupMenu::Item it(t); it.itemID = (int)fns.size(); it.isEnabled = enabled; it.isTicked = ticked; it.shortcutKeyDescription = hint; m.addItem(it); return *this; }
    Menu& sep() { m.addSeparator(); return *this; }
    Menu& sub(const juce::String& t, Menu& s, bool enabled = true) {   // the sub-menu's actions are moved in, ids offset
        juce::PopupMenu sm; const int base = (int)fns.size(); for (auto& f : s.fns) fns.push_back(std::move(f));
        renumber(s.m, sm, base); m.addSubMenu(t, sm, enabled); return *this; }
    void show(juce::PopupMenu::Options o) { auto f = std::make_shared<std::vector<std::function<void()>>>(std::move(fns)); m.showMenuAsync(o, [f](int r) { if (r > 0 && r <= (int)f->size() && (*f)[(size_t)r - 1]) (*f)[(size_t)r - 1](); }); }
    static void renumber(const juce::PopupMenu& src, juce::PopupMenu& dst, int base) {
        for (juce::PopupMenu::MenuItemIterator i(src); i.next();) { auto it = i.getItem(); if (it.itemID > 0) it.itemID += base;
            if (it.subMenu) { juce::PopupMenu s2; renumber(*it.subMenu, s2, base); it.subMenu = std::make_unique<juce::PopupMenu>(s2); } dst.addItem(std::move(it)); }
    }
};

// --- knob ---------------------------------------------------------------------------------------------------------------
// util.js knob(): drag up / down (Shift = fine), wheel nudges, double-click resets; the caption shows the value while it
// moves; right-click: reset, type a value, copy / paste, and for controls with a `link` target the app's items
// (automation lane, start value, MIDI controller link).
inline std::function<void(Menu&, const std::string& link, double v, std::function<void(double)> set)>& knobHooks() { static std::function<void(Menu&, const std::string&, double, std::function<void(double)>)> f; return f; }
inline std::map<std::string, std::vector<juce::Component::SafePointer<juce::Component>>>& linkedKnobs() { static std::map<std::string, std::vector<juce::Component::SafePointer<juce::Component>>> m; return m; }
inline double& knobClip() { static double v = std::numeric_limits<double>::quiet_NaN(); return v; }

class Knob : public juce::Component, private juce::Timer {
public:
    enum Size { Normal, Sm, Xs };
    juce::Slider s{ juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    std::function<void(double)> onChange;   // while dragging
    std::function<void()> onDone;           // released: commit
    std::function<juce::String(double)> fmt;
    std::string link;   // automation / MIDI target ("bus:<id>:vol" ...)
    bool showCaption = true; Size size = Normal;
    Knob(const juce::String& label, double lo, double hi, double def, bool expo = false, double step = 0) : caption(label), def(def) {
        s.setRange(lo, hi, step); if (expo && lo > 0) s.setSkewFactorFromMidPoint(std::sqrt(lo * hi)); s.setDoubleClickReturnValue(true, def); s.setValue(def, juce::dontSendNotification);
        s.setRotaryParameters(juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        s.setMouseDragSensitivity(170); s.setVelocityModeParameters(1.0, 1, 0.0, false, juce::ModifierKeys::shiftModifier);
        s.setScrollWheelEnabled(true); s.setPopupMenuEnabled(false);
        fmt = [step](double v) { return step >= 1 ? juce::String((int)std::round(v)) : std::abs(v) >= 100 ? juce::String(v, 0) : std::abs(v) >= 10 ? juce::String(v, 1) : juce::String(v, 2); };
        s.onValueChange = [this] { showVal(); if (onChange) onChange(s.getValue()); };
        s.onDragEnd = [this] { if (onDone) onDone(); };
        s.addMouseListener(this, false); s.setTooltip(label);
        addAndMakeVisible(s);
    }
    ~Knob() override { setLink({}); }
    Knob& sized(Size z) { size = z; return *this; }
    void setLink(const std::string& l) {
        if (!link.empty()) { auto& v = linkedKnobs()[link]; v.erase(std::remove_if(v.begin(), v.end(), [this](auto& p) { return p.getComponent() == this || !p; }), v.end()); }
        link = l; if (!link.empty()) linkedKnobs()[link].push_back(this);
    }
    void set(double v) { if (!s.isMouseButtonDown()) s.setValue(v, juce::dontSendNotification); }
    double get() const { return s.getValue(); }
    void setCaption(const juce::String& c) { caption = c; s.setTooltip(c); repaint(); }
    int idealWidth() const { return juce::roundToInt((size == Xs ? 30 : size == Sm ? 50 : isTouch() ? 60 : 54) * style().scale); }
    int dial() const { return juce::roundToInt((size == Xs ? 24 : size == Sm ? 32 : isTouch() ? 44 : 38) * style().scale); }
    int idealHeight() const { return dial() + (showCaption ? juce::roundToInt(FS * (size == Xs ? 0.68f : 0.78f) * 1.3f) + 3 : 0); }
    void resized() override { auto r = getLocalBounds(); const int d = std::min(dial(), std::min(r.getWidth(), showCaption ? r.getHeight() - capH() : r.getHeight())); s.setBounds(r.getCentreX() - d / 2, showCaption ? 0 : (r.getHeight() - d) / 2, d, d); }
    void paint(juce::Graphics& g) override {
        if (!showCaption) return;
        g.setColour(showing ? ACCENT2 : DIM); g.setFont(font(FS * (size == Xs ? 0.68f : 0.78f)));
        g.drawFittedText(showing ? fmt(s.getValue()) : caption, getLocalBounds().removeFromBottom(capH()), juce::Justification::centredTop, 1, 0.85f);
    }
    void mouseDown(const juce::MouseEvent& e) override { if (e.mods.isPopupMenu() || touchMenu()) { menu(); } }
    void mouseUp(const juce::MouseEvent& e) override { if (!e.mouseWasDraggedSinceMouseDown() && !e.mods.isPopupMenu()) showVal(); }
private:
    juce::String caption; double def; bool showing = false;
    int capH() const { return juce::roundToInt(FS * (size == Xs ? 0.68f : 0.78f) * 1.3f) + 3; }
    void showVal() { showing = true; repaint(); startTimer(900); }
    void timerCallback() override { stopTimer(); showing = false; repaint(); }
    void setV(double v) { s.setValue(juce::jlimit(s.getMinimum(), s.getMaximum(), v), juce::dontSendNotification); showVal(); if (onChange) onChange(s.getValue()); if (onDone) onDone(); }
    void menu() {
        Menu m; m.head(caption + u8(" · ") + fmt(s.getValue()));
        m.item("Reset", [this] { setV(def); });
        m.sep(); m.head("Value");
        m.item(u8("Type in value…"), [this] { ask(caption + " (" + fmt(s.getMinimum()) + " to " + fmt(s.getMaximum()) + ")", juce::String(s.getValue(), 4), [sp = juce::Component::SafePointer<Knob>(this)](juce::String t) { if (sp && t.trim().containsOnly("-+.0123456789eE") && t.trim().isNotEmpty()) sp->setV(t.getDoubleValue()); }); });
        m.item("Copy value", [this] { knobClip() = s.getValue(); });
        m.item("Paste value", [this] { setV(knobClip()); }, !std::isnan(knobClip()));
        if (!link.empty() && knobHooks()) knobHooks()(m, link, s.getValue(), [sp = juce::Component::SafePointer<Knob>(this)](double v) { if (sp) sp->setV(v); });
        m.show(atMouse());
    }
};

// MIDI-linked controls show a moved value (links.js updates knobs through [data-link])
inline void setLinked(const std::string& target, double v) {
    auto it = linkedKnobs().find(target); if (it == linkedKnobs().end()) return;
    for (auto& p : it->second) if (auto* k = dynamic_cast<Knob*>(p.getComponent())) k->set(v);
}

// --- flow layout for toolbars (.vbar / .hrow): items left to right, gap 8, optional right-aligned tail, wraps ------------
struct Flow {
    struct It { juce::Component* c; int w, h; bool right, brk; };
    std::vector<It> items; int gap = 8;
    Flow& add(juce::Component& c, int w, int h = -1) { items.push_back({ &c, w, h, false, false }); return *this; }
    Flow& right(juce::Component& c, int w, int h = -1) { items.push_back({ &c, w, h, true, false }); return *this; }
    Flow& space(int w) { items.push_back({ nullptr, w, 0, false, false }); return *this; }
    Flow& br() { items.push_back({ nullptr, 0, 0, false, true }); return *this; }
    // lays out inside r (padding PAD, 8); returns the height used. Items taller than a control (knobs) make their line taller
    // and everything on the line is centred on it, like .vbar's align-items: center.
    int layout(juce::Rectangle<int> r, bool apply = true, bool wrap = true) const {
        const int c = CTL(), pad = PAD(), x0 = r.getX() + 8, maxX = r.getRight() - 8;
        int rightW = 0, rightH = c; for (auto& i : items) if (i.right) { rightW += i.w + gap; rightH = std::max(rightH, i.h < 0 ? c : i.h); }
        // split into lines
        std::vector<std::vector<const It*>> lines(1); int x = x0;
        for (auto& i : items) {
            if (i.right) continue;
            if (i.brk) { lines.emplace_back(); x = x0; continue; }
            const int limit = maxX - (lines.size() == 1 ? rightW : 0);
            if (wrap && x + i.w > limit && x > x0) { lines.emplace_back(); x = x0; }
            lines.back().push_back(&i); x += i.w + gap;
        }
        int y = r.getY() + pad;
        for (size_t li = 0; li < lines.size(); li++) {
            int lh = li == 0 ? rightH : c; for (auto* i : lines[li]) lh = std::max(lh, i->h < 0 ? c : i->h);
            int xx = x0; for (auto* i : lines[li]) { const int h = i->h < 0 ? c : i->h; if (i->c && apply) i->c->setBounds(xx, y + (lh - h) / 2, i->w, h); xx += i->w + gap; }
            if (li == 0) { int rx = maxX; for (auto it = items.rbegin(); it != items.rend(); ++it) if (it->right) { const int h = it->h < 0 ? c : it->h; rx -= it->w; if (it->c && apply) it->c->setBounds(rx, y + (lh - h) / 2, it->w, h); rx -= gap; } }
            y += lh + (li + 1 < lines.size() ? pad : 0);
        }
        return y + pad - r.getY();
    }
};

}   // namespace ui
