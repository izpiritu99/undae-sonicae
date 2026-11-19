// Settings (app/js/ui/settings.js): tabs for Theme, General, Audio, MIDI, Hotkeys, File, Plug-ins and Project. Each page is
// a grid of cards; a card is rows of "label + hint" on the left and a control on the right.
#pragma once
#include <thread>
#include "Doc.h"
#include "Keys.h"
#include "engine/Analysis.h"
#include "engine/PluginHost.h"
#include "engine/Mp3.h"
#include "PlugUI.h"

namespace ui {

// app-wide services the window owns and the settings change
struct AppCtx {
    juce::AudioDeviceManager* devices = nullptr; juce::PropertiesFile* props = nullptr;
    std::function<juce::File()> templatesDir, backupDir;
    std::function<void(const juce::File&)> newFromTemplate, openFile;
};
inline juce::File templateFile(const AppCtx& a, const juce::String& name) { return a.templatesDir().getChildFile(juce::File::createLegalFileName(name) + ".undae.json"); }
us::Project demoProject();   // Doc.cpp: the small finished example (state.js demoProject)

// a button that takes the next key combination (Settings › Hotkeys)
class KeyCapture : public Btn {
public:
    std::function<void(const juce::String&)> onKey; bool listening = false;
    KeyCapture(const juce::String& t) : Btn(t) { sm = true; monoFont = true; setWantsKeyboardFocus(true); setTooltip("Click, then press the new key combination (Esc cancels)"); }
    void clicked() override { listening = true; old = getButtonText(); setButtonText(u8("Press keys…")); setOn(true); grabKeyboardFocus(); }
    bool keyPressed(const juce::KeyPress& k) override {
        if (!listening) return false; listening = false; setOn(false);
        if (k == juce::KeyPress::escapeKey) { setButtonText(old); return true; }
        const auto c = comboOf(k); if (c.isEmpty()) { setButtonText(old); return true; } if (onKey) onKey(c); return true;
    }
    void focusLost(FocusChangeType) override { if (listening) { listening = false; setOn(false); setButtonText(old); } }
private:
    juce::String old;
};

class SettingsView : public juce::Component, private juce::ChangeListener, private juce::Timer {
public:
    static juce::String& startTab() { static juce::String t = "theme"; return t; }   // F10 / F11 open a tab directly
    SettingsView(Doc& d, AppCtx& a) : doc(d), app(a) {
        doc.addChangeListener(this);
        static const std::pair<const char*, const char*> tabs[] = { { "theme", "Theme" }, { "general", "General" }, { "audio", "Audio" }, { "midi", "MIDI" }, { "hotkeys", "Hotkeys" }, { "file", "File" }, { "plugins", "Plug-ins" }, { "project", "Project" } };
        for (auto& [id, l] : tabs) { auto* b = tabBtns.add(new Tab(l)); const juce::String tid = id; b->onClick = [this, tid] { tab = tid; startTab() = tid; build(); }; addAndMakeVisible(b); ids.add(id); }
        view.setViewedComponent(&page, false); view.setScrollBarsShown(true, false); addAndMakeVisible(view);
        tab = startTab(); build(); startTimerHz(2);
    }
    ~SettingsView() override { doc.removeChangeListener(this); }
    void resized() override { layoutTabs(); view.setBounds(getLocalBounds().withTrimmedTop(tabH)); layout(); }
    void paint(juce::Graphics& g) override { fillPanel(g); g.setColour(LINE); g.fillRect(0, tabH - 1, getWidth(), 1); }

private:
    struct Tab : juce::Button { Tab(const juce::String& t) : juce::Button(t) { setMouseCursor(juce::MouseCursor::PointingHandCursor); }
        void paintButton(juce::Graphics& g, bool over, bool) override { auto r = getLocalBounds().toFloat(); juce::Path p; p.addRoundedRectangle(r.getX(), r.getY(), r.getWidth(), r.getHeight() + 6, RAD() * 0.7f, RAD() * 0.7f, true, true, false, false);
            g.setColour(getToggleState() ? ACCENT : over ? mix(BG3, TEXT, 0.85f) : BG3); g.fillPath(p); g.setColour(getToggleState() ? ONACC : DIM); g.setFont(fontB(FS)); g.drawText(getButtonText(), getLocalBounds(), juce::Justification::centred); } };
    struct Row { juce::String label, hint; juce::Component* ctl = nullptr; int w = 160, h = -1; bool full = false; };
    struct Page : juce::Component { SettingsView* o = nullptr;
        void paint(juce::Graphics& g) override {
            fillPanel(g);
            for (auto& c : o->cards) { ui::card(g, c.area.toFloat(), PANEL); cardTitle(g, c.title, { c.area.getX() + 10, c.area.getY() + 8, c.area.getWidth() - 20, 16 });
                for (auto& r : c.rows) { if (r.full) { g.setColour(DIM); g.setFont(font(FS * 0.88f)); g.drawFittedText(r.hint, r.labelR, juce::Justification::topLeft, 12); continue; }
                    g.setColour(TEXT); g.setFont(font(FS)); g.drawText(r.label, r.labelR.withHeight(18), juce::Justification::centredLeft, true);
                    if (r.hint.isNotEmpty()) { g.setColour(DIM); g.setFont(font(FS * 0.88f)); g.drawFittedText(r.hint, r.labelR.withTrimmedTop(19), juce::Justification::topLeft, 6); } } }
        }
    };
    struct RowR : Row { juce::Rectangle<int> labelR; };
    friend struct Page;

    // the tabs: one row when they fit, else as many rows as needed (a phone in portrait), each row stretched to the full width
    int tabH = 0;
    void layoutTabs() {
        const int W = getWidth() - 16, h = CTL(), n = tabBtns.size(); std::vector<int> w;
        for (auto* b : tabBtns) w.push_back(textW(fontB(FS), b->getButtonText()) + 28);
        int rows = 1; while (rows < n) {   // the fewest rows where an even share of tabs per row fits
            const int per = (n + rows - 1) / rows; bool fits = true;
            for (int s = 0; s < n && fits; s += per) { int rw = 0; for (int i = s; i < std::min(n, s + per); i++) rw += w[(size_t)i] + 2; fits = rw - 2 <= W; }
            if (fits) break; rows++; }
        const int per = (n + rows - 1) / rows; int y = 8;
        for (int s = 0; s < n; s += per, y += h + 4) {
            const int e = std::min(n, s + per); int rw = 0; for (int i = s; i < e; i++) rw += w[(size_t)i];
            const float k = rows > 1 ? (float)(W - 2 * (e - s - 1)) / (float)std::max(1, rw) : 1.f; float x = 8;
            for (int i = s; i < e; i++) { const int bw = i == e - 1 && rows > 1 ? 8 + W - juce::roundToInt(x) : juce::roundToInt(w[(size_t)i] * k); tabBtns[i]->setBounds(juce::roundToInt(x), y, bw, h); x += w[(size_t)i] * k + 2; }
        }
        tabH = y - 4;
    }

    Doc& doc; AppCtx& app; juce::String tab = "theme"; juce::OwnedArray<Tab> tabBtns; juce::StringArray ids; juce::Viewport view; Page page;
    juce::OwnedArray<juce::Component> owned; std::unique_ptr<juce::FileChooser> chooser;
    struct CardX { juce::String title; std::vector<RowR> rows; juce::Rectangle<int> area; };
    std::vector<CardX> cards;
    juce::PropertiesFile& P() { return *app.props; }

    // --- building blocks ---
    void sec(const juce::String& title) { cards.push_back({ title, {}, {} }); }
    template <class T> T* own(T* c) { owned.add(c); page.addAndMakeVisible(c); return c; }
    void row(const juce::String& label, juce::Component* ctl, const juce::String& hint = {}, int w = 170, int h = -1) { RowR r; r.label = label; r.hint = hint; r.ctl = ctl; r.w = w; r.h = h; cards.back().rows.push_back(r); }
    void note(const juce::String& text) { RowR r; r.full = true; r.hint = text; cards.back().rows.push_back(r); }
    void up(const char* k, const juce::var& v, bool reapply = true) { P().setValue(k, v); P().saveIfNeeded(); if (reapply && doc.prefsChanged) doc.prefsChanged(); }
    juce::ComboBox* sel(std::vector<std::pair<juce::String, juce::String>> opts, const juce::String& cur, std::function<void(const juce::String&)> fn) {
        auto* c = own(new juce::ComboBox()); for (size_t i = 0; i < opts.size(); i++) c->addItem(opts[i].second, (int)i + 1);
        for (size_t i = 0; i < opts.size(); i++) if (opts[i].first == cur) c->setSelectedItemIndex((int)i, juce::dontSendNotification);
        c->onChange = [c, opts, fn] { const int i = c->getSelectedItemIndex(); if (i >= 0) fn(opts[(size_t)i].first); }; return c;
    }
    juce::ToggleButton* check(const char* k, bool def, std::function<void(bool)> after = {}) {
        auto* t = own(new juce::ToggleButton()); t->setToggleState(P().getBoolValue(k, def), juce::dontSendNotification); const std::string key = k;
        t->onClick = [this, t, key, after] { up(key.c_str(), t->getToggleState()); if (after) after(t->getToggleState()); }; return t;
    }
    juce::Slider* range(const char* k, double lo, double hi, double step, double def) {
        auto* s = own(new juce::Slider(juce::Slider::LinearHorizontal, juce::Slider::NoTextBox)); s->setRange(lo, hi, step); s->setValue(P().getDoubleValue(k, def), juce::dontSendNotification); const std::string key = k;
        s->onDragEnd = [this, s, key] { up(key.c_str(), s->getValue()); }; s->onValueChange = [this, s, key] { if (!s->isMouseButtonDown()) up(key.c_str(), s->getValue()); }; return s;
    }
    Btn* btn(const juce::String& t, std::function<void()> fn, bool primary = false) { auto* b = own(new Btn(t)); b->primary = primary; b->sm = true; b->onClick = fn; return b; }
    Cap* small(const juce::String& t) { auto* c = own(new Cap(t)); c->setJustificationType(juce::Justification::centredRight); return c; }
    struct HRow : juce::Component { std::vector<juce::Component*> items; void resized() override { int x = getWidth(); for (auto it = items.rbegin(); it != items.rend(); ++it) { auto* c = *it; const int w = c->getProperties().getWithDefault("w", 80); x -= w; c->setBounds(x, 0, w, getHeight()); x -= 6; } } };
    HRow* hrow(std::vector<std::pair<juce::Component*, int>> items) { auto* h = own(new HRow()); for (auto& [c, w] : items) { c->getProperties().set("w", w); owned.removeObject(c, false); h->addAndMakeVisible(c); h->items.push_back(c); owned.add(c); } return h; }

    void build() {
        for (int i = 0; i < tabBtns.size(); i++) tabBtns[i]->setToggleState(ids[i] == tab, juce::dontSendNotification);
        cards.clear(); page.removeAllChildren(); owned.clear();
        if (tab == "theme") theme(); else if (tab == "general") general(); else if (tab == "audio") audio(); else if (tab == "midi") midi(); else if (tab == "hotkeys") hotkeys(); else if (tab == "file") file(); else if (tab == "plugins") plugins(); else project();
        page.o = this; layout(); page.repaint();
    }
    void layout() {
        const int W = std::max(340, view.getWidth() - 12), gap = 8; const int cols = std::max(1, (W - gap) / (330 + gap)); const int cw = (W - gap * (cols + 1)) / cols;
        std::vector<int> colY((size_t)cols, gap); int ci = 0;
        for (auto& c : cards) {
            const int x = gap + ci * (cw + gap); int y = colY[(size_t)ci] + 32; const int inner = cw - 20;
            for (auto& r : c.rows) {
                if (r.full) { const int lines = textW(font(FS * 0.88f), r.hint) / std::max(1, inner) + 1 + r.hint.length() / 400; const int h = lines * 16 + 6; r.labelR = { x + 10, y, inner, h }; y += h + 6; continue; }
                const int cwid = r.ctl ? std::min(r.w, inner * 3 / 5) : 0, lw = inner - cwid - 12;
                const int hintH = r.hint.isEmpty() ? 0 : (textW(font(FS * 0.88f), r.hint) / std::max(1, lw) + 1) * 15 + 2; const int ch = r.ctl ? (r.h > 0 ? r.h : CTL()) : 0; const int h = std::max({ CTL(), 18 + hintH, ch });
                r.labelR = { x + 10, y + (r.hint.isEmpty() ? (h - 18) / 2 : 0), lw, h };
                if (r.ctl) r.ctl->setBounds(x + 10 + inner - cwid, y + (h - ch) / 2, cwid, ch);
                y += h + 9;
            }
            c.area = { x, colY[(size_t)ci], cw, y - colY[(size_t)ci] + 2 }; colY[(size_t)ci] = c.area.getBottom() + gap;
            ci = (int)(std::min_element(colY.begin(), colY.end()) - colY.begin());
        }
        page.setSize(W, *std::max_element(colY.begin(), colY.end()) + 8); page.repaint();
    }

    // --- pages ---
    void theme() {
        sec("Look");
        struct Swatches : juce::Component { SettingsView* o = nullptr; void paint(juce::Graphics& g) override { const int n = 8, cols = 4, w = (getWidth() - 15) / cols; for (int i = 0; i < n; i++) { auto& t = THEMES[i]; juce::Rectangle<float> r((float)(i % cols) * (w + 5), (float)(i / cols) * 45, (float)w, 40);
                juce::Path p; p.addRoundedRectangle(r, RAD() * 0.6f); g.saveState(); g.reduceClipRegion(p); g.setColour(juce::Colour(t.v[1])); g.fillRect(r); juce::Path a; a.startNewSubPath(r.getX() + r.getWidth() * 0.55f + 40, r.getY()); a.lineTo(r.getRight(), r.getY()); a.lineTo(r.getRight(), r.getBottom()); a.lineTo(r.getX() + r.getWidth() * 0.55f, r.getBottom()); a.closeSubPath();
                g.setColour(juce::Colour(t.v[6])); g.fillPath(a); juce::Path b; b.startNewSubPath(r.getX() + r.getWidth() * 0.8f + 30, r.getY()); b.lineTo(r.getRight(), r.getY()); b.lineTo(r.getRight(), r.getBottom()); b.lineTo(r.getX() + r.getWidth() * 0.8f, r.getBottom()); b.closeSubPath(); g.setColour(juce::Colour(t.v[7])); g.fillPath(b); g.restoreState();
                if (style().theme == t.id) { g.setColour(TEXT); g.drawRoundedRectangle(r.reduced(1), RAD() * 0.6f, 2); }
                g.setColour(juce::Colours::white); g.setFont(font(FS * 0.68f)); g.drawText(t.name, r.reduced(5, 2).toNearestInt(), juce::Justification::bottomLeft); } }
            void mouseUp(const juce::MouseEvent& e) override { const int cols = 4, w = (getWidth() - 15) / cols; const int i = (e.y / 45) * cols + std::min(cols - 1, e.x / (w + 5)); if (i >= 0 && i < 8) { o->P().setValue("accent", ""); o->up("theme", juce::String(THEMES[i].id)); o->build(); } } };
        auto* sw = own(new Swatches()); sw->o = this; row("Theme", sw, {}, 400, 85);
        auto* acc = btn(u8("Choose…"), [this] { pickAccent(); }); auto* rst = btn("Reset", [this] { up("accent", ""); build(); });
        row("Accent colour", hrow({ { acc, 90 }, { rst, 70 } }), "Overrides the theme accent", 166);
        auto* ph = btn(u8("Upload photo…"), [this] { chooser = std::make_unique<juce::FileChooser>("Background photo", juce::File::getSpecialLocation(juce::File::userPicturesDirectory), "*.jpg;*.jpeg;*.png");
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) { if (juce::ImageFileFormat::loadFrom(fc.getResult()).isValid()) { up("bgImage", fc.getResult().getFullPathName()); toast("Background set"); build(); } else toast("That image could not be used (too large or not an image)"); } }); }, true);
        if (P().getValue("bgImage").isNotEmpty()) { auto* rm = btn("Remove", [this] { up("bgImage", ""); build(); }); row("Background photo", hrow({ { ph, 120 }, { rm, 80 } }), "Shown behind the panels", 206); row("Photo dimming", range("bgDim", 0.1, 0.95, 0.05, 0.6), "Higher = darker photo, easier reading");
            row("Deck transparency", range("deckAlpha", 0.15, 1, 0.05, 0.78), "How solid the decks are: lower shows more of the photo"); }
        else row("Background photo", ph, "Shown behind the panels", 120);
        row("Corners", range("radius", 0, 18, 1, 10)); row("Interface size", range("scale", 0.85, 1.3, 0.05, 1));
        row("Density", sel({ { "compact", "Compact" }, { "cozy", "Cozy" }, { "roomy", "Roomy" } }, P().getValue("density", "cozy"), [this](auto v) { up("density", v); }));
        row("Step buttons", sel({ { "lozenge", "Lozenge" }, { "square", "Square" }, { "dot", "Dot" } }, P().getValue("stepShape", "lozenge"), [this](auto v) { up("stepShape", v); }));
        row("Note colours", sel({ { "channel", "Channel colour" }, { "accent", "Accent" }, { "velocity", "Velocity heat" }, { "pitch", "Pitch class" } }, P().getValue("noteColor", "channel"), [this](auto v) { up("noteColor", v); }));
        row("Grid contrast", range("gridContrast", 0.3, 2, 0.1, 1)); row("Glow & gradients", check("glow", true), {}, 30); row("Animations", check("motion", true), {}, 30);
    }
    struct AccentPick : juce::ColourSelector { std::function<void(juce::Colour)> fn; AccentPick() : juce::ColourSelector(showColourspace | showSliders) { setSize(300, 280); } ~AccentPick() override { if (fn) fn(getCurrentColour()); } };
    void pickAccent() { auto cs = std::make_unique<AccentPick>(); cs->setCurrentColour(ACCENT); cs->fn = [this, sp = juce::Component::SafePointer<SettingsView>(this)](juce::Colour c) { juce::MessageManager::callAsync([sp, c] { if (sp) { sp->up("accent", juce::String(toHex(c))); sp->build(); } }); };
        juce::CallOutBox::launchAsynchronously(std::move(cs), getScreenBounds().withSizeKeepingCentre(10, 10), nullptr); }
    static bool narrowScreen() { if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()) return d->userArea.getWidth() < 760; return false; }   // the app's own default for phones
    void general() {
        sec("Layout");
        row("Workspace", sel({ { "single", "One deck" }, { "stack", "Two decks, stacked" }, { "side", "Two decks, side by side" }, { "tri", "Three decks (one wide, two below)" }, { "quad", "Four decks (2 x 2)" } }, P().getValue("layout", narrowScreen() ? "single" : "stack"), [this](auto v) { up("layout", v); }), "Any view can live in any deck. Phones always use one deck.", 210);
        row("Floating deck", check("float", false), "An extra deck in a panel you can drag by its tab bar", 30);
        row("On-screen keyboard", check("keys", false), {}, 30);
        sec("Editing");
        row("Song snap", sel({ { "1", "Bar" }, { "0.5", "1/2 bar" }, { "0.25", "Beat" }, { "0.125", "1/2 beat" }, { "0.0625", "Step" }, { "0", "Off" } }, juce::String(P().getDoubleValue("songSnap", 1)), [this](auto v) { up("songSnap", v.getDoubleValue()); doc.sendChangeMessage(); }), "Grid for placing and moving clips in the Song view. Alt while dragging ignores it.");
        row("Ghost notes", check("ghost", true, [this](bool) { doc.sendChangeMessage(); }), "Show other channels faintly in the piano roll", 30);
        row("Lock notes to scale", check("scaleLock", false, [this](bool) { doc.sendChangeMessage(); }), {}, 30);
        sec("Shortcuts"); row("Keyboard and mouse shortcuts", btn("Open Hotkeys", [this] { tab = "hotkeys"; build(); }), "See every shortcut and set your own keys", 120);
    }
    void audio() {
        sec("Input / output");
        if (auto* dm = app.devices) deviceRows(*dm); else note("No audio device manager.");
        note("Speakers, headphones or an audio interface. Recording inputs are chosen per bus in the Mixer (IN).");
        auto* st = small(statusText()); st->setJustificationType(juce::Justification::centredLeft); st->setFont(mono(FS * 0.8f, false)); row("Status", st, {}, 300);
        sec("Quality");
        row("Bit depth", sel({ { "16", "16-bit (CD quality, smallest files)" }, { "24", "24-bit (studio standard)" }, { "32", "32-bit float (highest, largest files)" } }, P().getValue("bitDepth", "24"), [this](auto v) { up("bitDepth", v.getIntValue(), false); }), "Used for WAV export", 200);
        row("Export sample rate", sel({ { "44100", "44.1 kHz" }, { "48000", "48 kHz" }, { "88200", "88.2 kHz" }, { "96000", "96 kHz" } }, P().getValue("exportRate", "48000"), [this](auto v) { up("exportRate", v.getIntValue(), false); }), "Rate of exported files. Recording uses the device rate shown under Status.");
        row("MP3 bitrate", sel({ { "128", "128 kbps" }, { "192", "192 kbps" }, { "256", "256 kbps" }, { "320", "320 kbps (highest)" } }, P().getValue("mp3Kbps", "320"), [this](auto v) { up("mp3Kbps", v.getIntValue(), false); }), us::mp3Available() ? "MP3 encoding uses LAME (libmp3lame, LGPL)" : 
           #if JUCE_WINDOWS
            "Needs libmp3lame.dll next to Undae Sonicae.exe"
           #elif JUCE_ANDROID
            "MP3 export is not available on Android yet"
           #else
            "Needs LAME: sudo apt install libmp3lame0"
           #endif
            );
        note("WAV is uncompressed, so it has no bitrate: quality is set by bit depth and sample rate. 24-bit / 48 kHz is about 2,304 kbps in stereo. FLAC is lossless and about half the size; Ogg Vorbis is lossy (quality 8).");
        sec("Drivers"); note(
           #if JUCE_WINDOWS
            "On Windows, Undae Sonicae uses Windows Audio (WASAPI) or DirectSound. For the lowest delay choose Windows Audio (Exclusive Mode) or (Low Latency Mode) above with a small buffer; exclusive mode keeps other apps from sharing the device while it plays."
           #elif JUCE_ANDROID
            "On Android, Undae Sonicae uses the system's low-latency audio where the device has it. Wired or USB headphones and interfaces have far less delay than Bluetooth, which adds its own buffering."
           #elif JUCE_MAC
            "On macOS, Undae Sonicae uses Core Audio. Class-compliant USB interfaces work without extra drivers."
           #else
            "Undae Sonicae uses the Linux audio system: PipeWire, ALSA or JACK. For the lowest latency, run PipeWire or JACK with a small buffer (quantum) and choose its device above. Class-compliant USB audio interfaces work without extra drivers."
           #endif
            );
        sec("CPU");
        row("Multi-threaded processing", check("threads", true, [](bool) { toast("Takes effect when you restart Undae Sonicae"); }), "On: plug-ins process in parallel, each in its own host thread. Off: plug-ins take turns on one thread, which uses less CPU. Applies after a restart.", 30);
        row("Meter refresh", sel({ { "0", "Off" }, { "15", "15 fps" }, { "30", "30 fps" }, { "60", "60 fps" } }, P().getValue("meterFps", "30"), [this](auto v) { up("meterFps", v.getIntValue(), false); }));
        note("If you hear crackles, raise the audio buffer size above. Plug-ins run in their own processes and answer each audio block directly, so they add no buffering of their own.");
    }
    // The audio device in the app's own rows (driver, output, input, sample rate, buffer, test tone), so it fits narrow
    // screens and matches the rest of Settings. Changes go through the device manager, which saves them; the page is
    // rebuilt afterwards (asynchronously: never from inside the combo box that is reporting the change).
    void rebuildSoon() { juce::MessageManager::callAsync([sp = juce::Component::SafePointer<SettingsView>(this)] { if (sp) sp->build(); }); }
    void applySetup(juce::AudioDeviceManager& dm, const juce::AudioDeviceManager::AudioDeviceSetup& st) { const auto err = dm.setAudioDeviceSetup(st, true); if (err.isNotEmpty()) toast(err); rebuildSoon(); }
    void deviceRows(juce::AudioDeviceManager& dm) {
        using Opts = std::vector<std::pair<juce::String, juce::String>>;
        auto& types = dm.getAvailableDeviceTypes();
        if (types.size() > 1) { Opts o; for (auto* t : types) o.push_back({ t->getTypeName(), t->getTypeName() });
            row("Driver", sel(o, dm.getCurrentAudioDeviceType(), [this, &dm](const juce::String& v) { dm.setCurrentAudioDeviceType(v, true); rebuildSoon(); }), driverHint(), 260); }
        auto* type = dm.getCurrentDeviceTypeObject(); auto st = dm.getAudioDeviceSetup();
        if (type) {
            Opts outs; for (auto& n : type->getDeviceNames(false)) outs.push_back({ n, n });
            if (!outs.empty()) row("Output", sel(outs, st.outputDeviceName, [this, &dm](const juce::String& v) { auto s2 = dm.getAudioDeviceSetup(); s2.outputDeviceName = v; applySetup(dm, s2); }), {}, 260);
            if (type->hasSeparateInputsAndOutputs()) { Opts ins{ { "", "(no input)" } }; for (auto& n : type->getDeviceNames(true)) ins.push_back({ n, n });
                row("Input", sel(ins, st.inputDeviceName, [this, &dm](const juce::String& v) { auto s2 = dm.getAudioDeviceSetup(); s2.inputDeviceName = v; s2.useDefaultInputChannels = true; applySetup(dm, s2); }), "For recording: a microphone, line input or interface", 260); }
        }
        if (auto* d = dm.getCurrentAudioDevice()) {
            Opts rates; for (double r : d->getAvailableSampleRates()) rates.push_back({ juce::String((int)r), juce::String((int)r) + " Hz" });
            if (!rates.empty()) row("Sample rate", sel(rates, juce::String((int)d->getCurrentSampleRate()), [this, &dm](const juce::String& v) { auto s2 = dm.getAudioDeviceSetup(); s2.sampleRate = v.getDoubleValue(); applySetup(dm, s2); }), {}, 260);
            Opts sizes; const double sr = d->getCurrentSampleRate() > 0 ? d->getCurrentSampleRate() : 48000;
            for (int b : d->getAvailableBufferSizes()) sizes.push_back({ juce::String(b), juce::String(b) + " samples (" + juce::String(b * 1000.0 / sr, 1) + " ms)" });
            if (!sizes.empty()) row("Buffer size", sel(sizes, juce::String(d->getCurrentBufferSizeSamples()), [this, &dm](const juce::String& v) { auto s2 = dm.getAudioDeviceSetup(); s2.bufferSize = v.getIntValue(); applySetup(dm, s2); }),
                                    "Smaller: less delay between playing and hearing. Larger: fewer crackles.", 260);
            const auto inNames = d->getInputChannelNames();
            if (!inNames.isEmpty()) {
                auto* t = own(new juce::ToggleButton()); t->setToggleState(!d->getActiveInputChannels().isZero(), juce::dontSendNotification);
                t->onClick = [this, t, &dm, n = inNames.size()] { auto s2 = dm.getAudioDeviceSetup(); s2.useDefaultInputChannels = false; s2.inputChannels.clear(); if (t->getToggleState()) s2.inputChannels.setRange(0, n, true); applySetup(dm, s2); };
                row("Use the input", t, juce::String(inNames.size()) + (inNames.size() == 1 ? " channel: " : " channels: ") + inNames.joinIntoString(", "), 30);
            }
            row("Test", btn("Play a test tone", [&dm] { dm.playTestSound(); }), "A short beep through the output", 160);
        }
    }
    static juce::String driverHint() {
       #if JUCE_WINDOWS
        return "Exclusive or Low Latency mode: the least delay";
       #elif JUCE_ANDROID
        return "Android's own audio; the low-latency one is used when the device supports it";
       #elif JUCE_MAC
        return "Core Audio";
       #else
        return "ALSA, or JACK / PipeWire for the lowest delay";
       #endif
    }
    juce::String statusText() {
        auto* d = app.devices ? app.devices->getCurrentAudioDevice() : nullptr; if (!d) return "No audio device open";
        const double sr = d->getCurrentSampleRate(); const int bs = d->getCurrentBufferSizeSamples();
        return d->getTypeName() + u8(" · ") + juce::String(sr, 0) + " Hz" + u8(" · buffer ") + juce::String(bs) + " smp (" + juce::String(bs * 1000.0 / sr, 1) + " ms)" + u8(" · output ") + juce::String(d->getOutputLatencyInSamples() * 1000.0 / sr, 1) + " ms" + u8(" · xruns ") + juce::String(d->getXRunCount());
    }
    void midi() {
        sec("Input devices");
        auto list = juce::MidiInput::getAvailableDevices();
        if (list.isEmpty()) note("No MIDI devices found yet - plug one in (USB-OTG on phones), then press Rescan.");
        for (auto& dv : list) { auto* t = own(new juce::ToggleButton()); const auto id = dv.identifier; t->setToggleState(app.devices && app.devices->isMidiInputDeviceEnabled(id), juce::dontSendNotification);
            t->onClick = [this, t, id] { if (app.devices) app.devices->setMidiInputDeviceEnabled(id, t->getToggleState()); P().setValue("midiOff:" + id, !t->getToggleState()); }; row(dv.name, t, {}, 30); }
        row("Connection", btn("Rescan", [this] { build(); }, true), "Ticked devices play the selected channel", 90);
        sec("Playing");
        std::vector<std::pair<juce::String, juce::String>> chs = { { "0", "Omni (all)" } }; for (int i = 1; i <= 16; i++) chs.push_back({ juce::String(i), "Channel " + juce::String(i) });
        row("Listen to channel", sel(chs, P().getValue("midiCh", "0"), [this](auto v) { up("midiCh", v.getIntValue(), false); }));
        row("Velocity response", sel({ { "soft", "Soft (light touch is louder)" }, { "linear", "Linear" }, { "hard", "Hard (needs a firmer touch)" }, { "fixed", "Fixed" } }, P().getValue("velCurve", "linear"), [this](auto v) { up("velCurve", v, false); }), {}, 210);
        row("Foot pedal sustains notes", check("pedal", true), {}, 30);
        row("Record quantize", sel({ { "0", "Off" }, { "48", "1/8" }, { "24", "1/16" }, { "12", "1/32" } }, P().getValue("recQuant", "0"), [this](auto v) { up("recQuant", v.getIntValue(), false); }), "Recorded notes snap to this grid");
        row("Typing keyboard", check("typing", true), "Z-M and Q-P rows play notes", 30);
        row("Typing octave", sel({ { "1", "1" }, { "2", "2" }, { "3", "3" }, { "4", "4" }, { "5", "5" } }, P().getValue("octave", "3"), [this](auto v) { up("octave", v.getIntValue(), false); toast("Applies to the typing keys and the on-screen keyboard"); }));
    }
    void hotkeys() {
        for (auto& [scope, title] : scopes()) {
            sec(title);
            for (auto& a : actions()) {
                if (juce::String(a.scope) != scope) continue; const juce::String id = a.id; const bool changed = keymap().getDynamicObject() && keymap().getDynamicObject()->hasProperty(id);
                auto* kc = own(new KeyCapture(keyOf(id))); if (changed) kc->rec = false;
                kc->onKey = [this, id](const juce::String& c) { const auto other = clash(id, c); rebind(id, c); build(); toast(other.isNotEmpty() ? c + u8(" is also used by “") + other + u8("” - change one of them") : "Set to " + c); };
                if (changed) { auto* r = btn(u8("↺"), [this, id] { rebind(id, {}); build(); }); r->setTooltip(juce::String("Back to the default: ") + a.key); row(a.label, hrow({ { kc, 130 }, { r, 30 } }), {}, 166); }
                else row(a.label, kc, {}, 130);
            }
        }
        sec("Mouse (fixed)"); for (auto& [k, d] : mouseHelp()) { auto* c = small(k); c->setFont(mono(FS * 0.8f)); c->setColour(juce::Label::textColourId, ACCENT2); row(d, c, {}, 150); }
        sec("Reset"); row("All shortcuts", btn("Restore defaults", [this] { keymap() = juce::var(new juce::DynamicObject()); if (saveKeymap()) saveKeymap()(); build(); toast("Shortcuts restored"); }), "Typing-keyboard notes (Z-M and Q-P rows) are set in the MIDI tab", 130);
    }
    void file() {
        sec("Backup");
        row("Autosave", check("autosave", true), "Every 2 minutes while there are unsaved changes, to the autosave file (project menu > Reopen the autosaved session)", 30);
        row("Keep timed backups", check("backups", true), "A dated copy at most every 10 minutes; the last 20 are kept", 30);
        row("Save now", btn("Save", [this] { if (doc.file != juce::File()) { try { doc.save(doc.file); toast("Saved"); } catch (const std::exception& e) { message("Could not save", e.what()); } } else toast("Use Save project file (Ctrl+S) to choose where") ; }), {}, 80);
        sec("Restore a backup");
        auto files = app.backupDir ? app.backupDir().findChildFiles(juce::File::findFiles, false, "*.undae.json") : juce::Array<juce::File>();
        std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.getLastModificationTime() > b.getLastModificationTime(); });
        if (files.isEmpty()) note("No backups yet.");
        for (int i = 0; i < std::min(10, files.size()); i++) { auto f = files[i]; row(f.getFileNameWithoutExtension().upToLastOccurrenceOf(".", false, false), btn("Restore", [this, f] { if (app.openFile) app.openFile(f); toast("Backup restored"); }), f.getLastModificationTime().toString(true, true), 80); }
        row("Backups folder", btn("Open", [this] { if (app.backupDir) app.backupDir().revealToUser(); }), {}, 80);
        sec("Sample folders");
        note("Folders listed here appear in the Library, where you can audition files and drag them onto the Rack or a Song track without importing the whole folder.");
        auto folders = juce::StringArray::fromLines(P().getValue("sampleFolders")); folders.removeEmptyStrings();
        for (auto& f : folders) { const juce::String path = f; auto* rm = btn(u8("✕"), [this, path] { auto l = juce::StringArray::fromLines(P().getValue("sampleFolders")); l.removeString(path); l.removeEmptyStrings(); up("sampleFolders", l.joinIntoString("\n"), false); doc.sendChangeMessage(); build(); }); rm->setTooltip("Forget this folder");
            row(u8("📁 ") + juce::File(f).getFileName(), rm, f, 30); }
        row("Add a location", btn(u8("Add sample folder…"), [this] { chooser = std::make_unique<juce::FileChooser>("Sample folder", juce::File::getSpecialLocation(juce::File::userMusicDirectory));
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this](const juce::FileChooser& fc) { auto d = fc.getResult(); if (!d.isDirectory()) return; auto l = juce::StringArray::fromLines(P().getValue("sampleFolders")); l.addIfNotAlreadyThere(d.getFullPathName()); l.removeEmptyStrings(); up("sampleFolders", l.joinIntoString("\n"), false); doc.sendChangeMessage(); build(); }); }, true), "Remembered between sessions", 150);
    }
    void plugins() {
        sec("Use plug-ins from another computer");
        note(u8("Run the desktop app on a Linux computer on the same Wi-Fi, switch on “Share this computer’s plug-ins” there, and enter its address and pairing code here. Its Linux and Windows plug-ins then work in this app: sound goes there and back over the network. Plug-in windows open on that computer’s screen; here you get their knobs."));
        auto* addr = own(new Field()); addr->setText(P().getValue("plugServer"), false); addr->setTextToShowWhenEmpty("e.g. 192.168.1.20", DIM); addr->onDone = [this](const juce::String& t) { up("plugServer", t.trim(), false); };
        auto* code = own(new Field()); code->setText(P().getValue("plugCode"), false); code->setTextToShowWhenEmpty("6 digits", DIM); code->setInputRestrictions(6, "0123456789"); code->onDone = [this](const juce::String& t) { up("plugCode", t.trim(), false); };
        row("Computer address", addr, "Shown on that computer under Settings > Plug-ins", 180); row("Pairing code", code, {}, 110);
        auto* status = small(us::HostSet::get().remote() ? "Connected to " + P().getValue("plugServer") : juce::String("Not connected")); status->setJustificationType(juce::Justification::centredLeft);
        if (us::HostSet::get().remote()) row({}, hrow({ { btn("Disconnect", [this] { us::HostSet::get().useServer("", ""); up("plugServerOn", false, false); doc.eng.reloadPlugins(); toast(u8("Using this device’s own plug-ins again")); build(); }), 100 } }), {}, 100);
        else row({}, btn("Connect", [this, status, addr, code] { up("plugServer", addr->getText().trim(), false); up("plugCode", code->getText().trim(), false); status->setText(u8("Connecting…"), juce::dontSendNotification);
            const auto a = addr->getText().trim().toStdString(), c = code->getText().trim().toStdString();
            std::thread([this, a, c, sp = juce::Component::SafePointer<SettingsView>(this)] { std::string name; const auto err = us::HostSet::get().useServer(a, c, &name);
                juce::MessageManager::callAsync([this, sp, err, name] { if (!sp) return; if (err.empty()) { up("plugServerOn", true, false); doc.eng.reloadPlugins(); toast("Connected to " + juce::String(name)); } else message("Plug-in server", err); build(); }); }).detach(); }, true), {}, 100);
        row("Status", status, {}, 220);
        sec(u8("Share this computer’s plug-ins")); note("Sharing runs in the desktop (Electron) edition for now: open Undae Sonicae there, Settings > Plug-ins > Share on the network. This native edition connects to it as above.");
        sec("Built-in effects"); { juce::StringArray n; for (auto& s : fxSpecs()) n.add(s.name); note(n.joinIntoString(u8(" · ")) + u8(". Add these on any bus in the Mixer with “+ Add effect”, and in Sound Design.")); }
        sec("CLAP and VST plug-ins");
        note(u8("Linux CLAP and VST3 plug-ins load directly and, when Wine is installed, Windows VST2 and VST3 plug-ins, 64-bit and 32-bit. Put .clap files in ~/.clap, Linux .vst3 folders in ~/.vst3 (or /usr/lib/vst3), and Windows .dll or .vst3 files in ~/.vst-win, then add an effect with “Plug-in (CLAP / VST)” on any bus in the Mixer, or an instrument with “+ Channel” > “Plug-in instrument” in the Rack. Parameters appear as knobs and can be automated and linked to MIDI controllers; “Open window” shows the plug-in’s own window. Plug-in instruments get pitch bend, mod wheel and sustain from your MIDI keyboard. Exported audio includes plug-ins."));
        row("Rescan plug-in folders", btn("Rescan", [] { PluginListRescan(); toast("Plug-in folders rescanned"); }), us::HostSet::get().winPending() ? juce::String("Wine is still starting") : juce::String(), 90);
        note("LV2 plug-ins cannot be loaded yet. On Android, plug-ins come from a computer sharing them (above).");
    }
    static void PluginListRescan() { PluginList::get().fetch([](const std::vector<juce::var>&) {}, true); }
    void project() {
        auto& p = doc.p;
        sec("Current project");
        auto* name = own(new Field()); name->setText(juce::String(p.name), false); name->onDone = [this](const juce::String& t) { const auto v = t.trim().isEmpty() ? std::string("Untitled") : t.trim().toStdString(); if (v != doc.p.name) { doc.p.name = v; doc.commit(); } };
        row("Name", name, {}, 200);
        row("Tempo", small(juce::String(p.bpm, p.bpm == std::round(p.bpm) ? 0 : 1) + " BPM"), {}, 120); row("Timebase", small("96 PPQ"), {}, 120);
        std::vector<std::pair<juce::String, juce::String>> ns, ds; for (int n : { 2, 3, 4, 5, 6, 7, 9, 11, 12, 13, 15 }) ns.push_back({ juce::String(n), juce::String(n) }); for (int d : { 2, 4, 8, 16 }) ds.push_back({ juce::String(d), juce::String(d) });
        auto changeSig = [this](int n, int d) { const double old = doc.p.bar(); doc.p.sigN = n; doc.p.sigD = d; for (auto& a : doc.p.audio) { a.start *= old / doc.p.bar(); a.len *= old / doc.p.bar(); } doc.commit(); };
        auto* sn = sel(ns, juce::String(p.sigN), [this, changeSig](auto v) { changeSig(v.getIntValue(), doc.p.sigD); }); auto* sd = sel(ds, juce::String(p.sigD), [this, changeSig](auto v) { changeSig(doc.p.sigN, v.getIntValue()); });
        row("Time signature", hrow({ { sn, 64 }, { sd, 64 } }), "Also on the top bar next to the tempo", 134);
        std::vector<std::pair<juce::String, juce::String>> roots; for (int i = 0; i < 12; i++) roots.push_back({ juce::String(i), us::NOTE_NAMES[i] });
        std::vector<std::pair<juce::String, juce::String>> sc; for (auto& [k, v] : us::scales()) sc.push_back({ juce::String(k), juce::String(k) });
        auto* kr = sel(roots, juce::String(p.keyRoot), [this](auto v) { doc.p.keyRoot = v.getIntValue(); doc.commit(); }); auto* ks = sel(sc, juce::String(p.keyScale), [this](auto v) { doc.p.keyScale = v.toStdString(); doc.commit(); });
        row("Key", hrow({ { kr, 64 }, { ks, 130 } }), "Used by the Piano Roll's scale tools and the Audio Lab", 200);
        row("Swing grid", sel({ { "16", "1/16 notes" }, { "8", "1/8 notes" } }, juce::String(p.swingDiv), [this](auto v) { doc.p.swingDiv = v.getIntValue(); doc.commit(); }));
        row("Contents", small(juce::String((int)p.channels.size()) + u8(" channels · ") + juce::String((int)p.patterns.size()) + u8(" patterns · ") + juce::String((int)p.buses.size()) + u8(" buses · ") + juce::String((int)p.clips.size()) + u8(" pattern clips · ") + juce::String((int)p.audio.size()) + " audio clips"), {}, 230, 40);
        sec("Starting up");
        std::vector<std::pair<juce::String, juce::String>> st = { { "blank", "Start with a blank project" }, { "last", "Reopen where I left off" } };
        auto tf = app.templatesDir().findChildFiles(juce::File::findFiles, false, "*.undae.json");
        for (auto& f : tf) st.push_back({ "tpl:" + f.getFileName(), "Start from template: " + f.getFileName().upToFirstOccurrenceOf(".", false, false) });
        row("When the program opens", sel(st, P().getValue("startup", "blank"), [this](auto v) { up("startup", v, false); }), "A blank project has an empty rack and only the master bus. The last session is kept under the project menu > Reopen the autosaved session.", 210);
        row("Demo song", btn("Load demo", [this] { doc.setProject(demoProject()); doc.song = true; doc.sendChangeMessage(); toast("Demo loaded"); }), "A small finished example with drums, keys, bass and lead", 100);
        row("Empty project", btn("Blank project", [this] { doc.setProject(blankProject()); doc.song = false; doc.sendChangeMessage(); }), {}, 110);
        row("Templates", btn(u8("Save this project as a template…"), [this] { saveTemplate(); }), "Your own starting points: channels, mixer, effects and patterns. Choose one above to open it at start-up.", 230);
        for (auto& f : tf) { const juce::String nm = f.getFileName().upToFirstOccurrenceOf(".", false, false);
            auto* a = btn("New from it", [this, f, nm] { if (app.newFromTemplate) app.newFromTemplate(f); toast(u8("New project from “") + nm + u8("”")); });
            auto* u = btn("Update", [this, f, nm] { try { us::saveProject(doc.p, f); toast(u8("“") + nm + u8("” now matches this project")); } catch (...) {} }); u->setTooltip("Replace the template with the current project");
            auto* dl = btn("Delete", [this, f] { f.deleteFile(); if (P().getValue("startup") == "tpl:" + f.getFileName()) up("startup", "blank", false); juce::MessageManager::callAsync([this] { build(); }); });
            row(nm, hrow({ { a, 92 }, { u, 66 }, { dl, 62 } }), {}, 232); }
    }
    void saveTemplate() {
        ask("Template name", doc.p.name == "Untitled" ? juce::String("My template") : juce::String(doc.p.name), [this](juce::String nm) { nm = nm.trim(); if (nm.isEmpty()) return;
            app.templatesDir().createDirectory(); try { us::saveProject(doc.p, templateFile(app, nm)); toast(u8("Template “") + nm + u8("” saved")); } catch (const std::exception& e) { message("Template", e.what()); }
            juce::MessageManager::callAsync([this] { build(); }); });
    }
    void changeListenerCallback(juce::ChangeBroadcaster*) override { if (tab == "project" && !isMouseButtonDownAnywhere()) { juce::MessageManager::callAsync([sp = juce::Component::SafePointer<SettingsView>(this)] { if (sp && !sp->hasKeyboardFocus(true)) sp->build(); }); } }
    static bool isMouseButtonDownAnywhere() { return juce::Desktop::getInstance().getMainMouseSource().isDragging(); }
    void timerCallback() override { if (tab == "audio") for (auto* c : owned) if (auto* l = dynamic_cast<Cap*>(c)) if (l->getText().contains("Hz")) l->setText(statusText(), juce::dontSendNotification); }
};

}   // namespace ui
