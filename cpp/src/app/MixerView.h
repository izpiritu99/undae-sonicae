// Mixer (app/js/ui/mixer.js): a strip per bus with free routing (sends to any bus, an optional link to the master),
// inputs with arm and monitor, a live waveform, effect LEDs, pan / width, mute / solo / polarity / mono, fader and meter
// with level and peak; "+" adds an insert bus (right-click: a send bus). On the right, the selected bus's effect slots.
// Mixer hotkeys: Ctrl+L links the selected rack channels, F2 renames, S solos, Alt+arrows move, Alt+R renders armed buses.
#pragma once
#include "Doc.h"
#include "Keys.h"
#include "MetersPanel.h"
#include "PlugUI.h"
#include "Theme.h"

namespace ui {

inline juce::String dBs(double v) { return v <= 0.001 ? u8("-∞ dB") : juce::String(20 * std::log10(v), 1) + " dB"; }
inline juce::String pkS(double v) { return v <= 0.001 ? u8("-∞") : (v >= 1 ? "+" : "") + juce::String(20 * std::log10(v), 1); }
inline float meterPos(float v) { return v <= 0.001f ? 0.f : juce::jlimit(0.f, 1.f, (20 * std::log10(v) + 60) / 60); }

class MixerView : public juce::Component, private juce::ChangeListener, private juce::Timer {
public:
    std::function<void()> renderArmed;   // the window renders each armed bus to a WAV (Alt+R)
    std::function<juce::StringArray()> inputNames;   // the audio device's input channels
    explicit MixerView(Doc& d) : doc(d) {
        doc.addChangeListener(this); setWantsKeyboardFocus(true);
        stripsView.setViewedComponent(&strips, false); stripsView.setScrollBarsShown(true, true); addAndMakeVisible(stripsView);
        fxView.setViewedComponent(&fx, false); fxView.setScrollBarsShown(true, false); addAndMakeVisible(fxView);
        strips.o = this; fx.o = this; strips.addAndMakeVisible(addBusB);
        addBusB.setTooltip("Click: add an insert bus - Right-click: add a send bus"); addBusB.onClick = [this] { newInsert(); }; addBusB.onRight = [this] { cur = newSend(); doc.commit(); };
        rebuild(); startTimerHz(30);
    }
    ~MixerView() override { doc.removeChangeListener(this); }
    void resized() override {
        auto r = getLocalBounds(); const int fw = juce::jlimit(250, 480, getWidth() * 2 / 5);
        if (isTouch() && getWidth() < 760) { fxView.setBounds(r.removeFromBottom(getHeight() * 45 / 100)); } else fxView.setBounds(r.removeFromRight(fw));
        stripsView.setBounds(r); layoutStrips(); fx.layout();
    }
    void paint(juce::Graphics& g) override { g.fillAll(BG2); g.setColour(LINE); if (fxView.getX() > 0) g.fillRect(fxView.getX(), 0, 1, getHeight()); else g.fillRect(0, fxView.getY(), getWidth(), 1); }
    bool keyPressed(const juce::KeyPress& k) override {
        const auto id = actionFor(k, "mixer"); auto* b = doc.bus(cur); if (id.isEmpty() || !b) return false;
        if (id == "mix.rename") renameBus(cur);
        else if (id == "mix.solo") { if (b->kind != "insert") toast("Only insert buses can be soloed"); else { b->solo = !b->solo; doc.commit(); } }
        else if (id == "mix.left") swap(cur, -1); else if (id == "mix.right") swap(cur, 1);
        else if (id == "mix.link") { auto ids = doc.selectedChannels(); for (auto& c : doc.p.channels) if (ids.count(c.id)) c.bus = cur; doc.commit(); toast(juce::String((int)ids.size()) + " channel" + (ids.size() == 1 ? "" : "s") + " now play through " + juce::String(b->name)); }
        else if (id == "mix.render") { if (renderArmed) renderArmed(); }
        else return false;
        return true;
    }

private:
    Doc& doc; std::string cur = "master"; juce::String sig;
    juce::Viewport stripsView, fxView; Btn addBusB{ "+", Btn::Dashed };

    std::string newSend() { int n = 0; for (auto& b : doc.p.buses) n += b.kind == "send"; const char* L = "ABCDEFGHIJKL"; const auto id = doc.addBus(std::string("Send ") + (n < 12 ? std::string(1, L[n]) : std::string("+")), "send"); if (auto* b = doc.bus(id)) b->fx.push_back(newFx("reverb")), b->fx.back().p.set("mix", 1.0); return id; }
    void newInsert() { int n = 0; for (auto& b : doc.p.buses) n += b.kind == "insert"; cur = doc.addBus("Insert " + std::to_string(n + 1)); doc.commit(); }
    void renameBus(const std::string& id) { if (auto* b = doc.bus(id)) ask("Bus name", juce::String(b->name), [this, id](juce::String v) { if (auto* x = doc.bus(id); x && v.isNotEmpty()) { x->name = v.toStdString(); doc.commit(); } }); }
    void swap(const std::string& id, int d) {
        auto& B = doc.p.buses; int i = -1; for (int k = 0; k < (int)B.size(); k++) if (B[(size_t)k].id == id) i = k; if (i < 0 || B[(size_t)i].kind == "master") return;
        int j = i + d; while (j >= 0 && j < (int)B.size() && B[(size_t)j].kind != B[(size_t)i].kind) j += d; if (j < 0 || j >= (int)B.size()) return; std::swap(B[(size_t)i], B[(size_t)j]); doc.commit();
    }
    // mixer strip presets: level, pan, width, flags and the effect chain (kept in the settings, like the browser's lw.buspresets)
    juce::var presets() const { auto v = juce::JSON::parse(doc.ps("busPresets", "{}")); return v.isObject() ? v : juce::var(new juce::DynamicObject()); }
    void savePreset(const juce::String& name, const us::Bus* b) {
        auto all = presets(); auto* o = all.getDynamicObject();
        if (!b) o->removeProperty(name); else { auto pv = us::projectToVar(doc.p); for (auto& x : *pv.getProperty("buses", juce::var()).getArray()) if (x.getProperty("id", "") == juce::String(b->id)) {
            juce::var keep(new juce::DynamicObject()); for (auto k : { "vol", "pan", "width", "mono", "inv", "fx" }) keep.getDynamicObject()->setProperty(k, x.getProperty(k, juce::var())); o->setProperty(name, keep); } }
        doc.setp("busPresets", juce::JSON::toString(all, true));
    }
    void loadPreset(const juce::String& name, const std::string& id) {
        auto pr = presets().getProperty(name, juce::var()); auto* b = doc.bus(id); if (!pr.isObject() || !b) return;
        b->vol = pr.getProperty("vol", b->vol); b->pan = pr.getProperty("pan", b->pan); b->width = pr.getProperty("width", b->width); b->mono = pr.getProperty("mono", b->mono); b->inv = pr.getProperty("inv", b->inv);
        b->fx.clear(); if (auto* arr = pr.getProperty("fx", juce::var()).getArray()) for (auto& f : *arr) { us::FxSlot s; s.id = us::newId(); s.type = f.getProperty("type", "").toString().toStdString(); s.on = f.getProperty("on", true); if (auto* po = f.getProperty("p", juce::var()).getDynamicObject()) s.p = po->getProperties(); b->fx.push_back(s); }
        doc.commit(); toast(u8("Loaded “") + name + u8("” onto ") + juce::String(b->name));
    }
    void stripMenu(const std::string& id) {
        auto* b = doc.bus(id); if (!b) return; const bool fixed = b->kind == "master"; auto pres = presets(); juce::StringArray names; if (auto* o = pres.getDynamicObject()) for (auto& kv : o->getProperties()) names.add(kv.name.toString());
        int k = 0, nSame = 0; for (auto& x : doc.p.buses) if (x.kind == b->kind) { if (x.id == id) k = nSame; nSame++; }
        Menu m; m.head(doc.busLabel(id) + u8(" · ") + juce::String(b->name));
        m.item(u8("Rename…"), [this, id] { renameBus(id); }, true, false, "F2");
        m.item("Move left", [this, id] { swap(id, -1); }, !fixed && k > 0, false, "Alt+Left"); m.item("Move right", [this, id] { swap(id, 1); }, !fixed && k < nSame - 1, false, "Alt+Right");
        m.item("Reset level & pan", [this, id] { if (auto* x = doc.bus(id)) { x->vol = x->kind == "master" ? 0.9 : 1; x->pan = 0; x->width = 1; x->mono = x->inv = false; doc.commit(); } });
        m.sep();
        m.item(u8("Save as preset…"), [this, id] { if (auto* x = doc.bus(id)) ask("Preset name", juce::String(x->name), [this, id](juce::String v) { if (v.isEmpty()) return; savePreset(v, doc.bus(id)); toast(u8("Saved mixer preset “") + v + u8("”")); }); });
        for (auto& n : names) m.item("Load preset: " + n, [this, n, id] { loadPreset(n, id); });
        if (!names.isEmpty()) { Menu del; for (auto& n : names) del.item(n, [this, n] { savePreset(n, nullptr); toast("Preset deleted"); }); m.sub(u8("Delete a preset…"), del); }
        m.sep(); m.item("Add insert bus", [this] { newInsert(); }); m.item("Add send bus", [this] { cur = newSend(); doc.commit(); });
        m.item("Delete bus", [this, id] {
            auto& P = doc.p; const auto name = doc.bus(id)->name;
            for (auto& c : P.channels) if (c.bus == id) c.bus = "master"; for (auto& a : P.audio) if (a.bus == id) a.bus = "master";
            for (auto& z : P.buses) { z.sends.erase(std::remove_if(z.sends.begin(), z.sends.end(), [&](auto& s) { return s.to == id; }), z.sends.end()); for (auto& f : z.fx) if (f.str("src", "") == id) f.p.set("src", juce::String()); }
            P.autos.erase(std::remove_if(P.autos.begin(), P.autos.end(), [&](auto& l) { return l.target.rfind("bus:" + id + ":", 0) == 0 || l.target.rfind("fx:" + id + ":", 0) == 0; }), P.autos.end());
            P.buses.erase(std::remove_if(P.buses.begin(), P.buses.end(), [&](auto& z) { return z.id == id; }), P.buses.end()); cur = "master"; doc.commit();
            toast(u8("Deleted “") + juce::String(name) + u8("” - its channels now go to the master")); }, !fixed);
        m.show(atMouse());
    }
    void inputMenu(const std::string& id, juce::Component* atC) {
        auto* b = doc.bus(id); if (!b) return; auto names = inputNames ? inputNames() : juce::StringArray();
        auto pick = [this, id](const std::string& in) { if (auto* x = doc.bus(id)) { x->input = in; if (in.empty()) x->arm = false; doc.commit(); } };
        Menu m; m.head(u8("Audio input · ") + juce::String(b->name));
        m.item("(none)", [pick] { pick(""); }, true, b->input.empty());
        m.item("Default input", [pick] { pick("1"); }, true, b->input == "1" && names.size() < 2);
        for (int i = 0; i < names.size(); i++) { const auto v = std::to_string(i + 1); m.item(names[i], [pick, v] { pick(v); }, true, b->input == v); }
        m.sep();
        m.item("Stereo (In 1 + In 2)", [this, id] { if (auto* x = doc.bus(id)) { x->stereo = !x->stereo; doc.commit(); } }, true, b->stereo);
        m.item("Monitor through this bus", [this, id] { if (auto* x = doc.bus(id)) { x->mon = !x->mon; doc.commit(); } }, true, b->mon);
        if (names.isEmpty()) m.head(u8("No input device open (Settings › Audio)"));
        m.show(at(atC));
    }
    void sendMenu(const std::string& id, juce::Component* atC) {
        auto* b = doc.bus(id); if (!b) return; Menu m; m.head("Send " + juce::String(b->name) + " to"); int ok = 0;
        for (auto& t : doc.p.buses) { if (t.kind == "master" || t.id == id) continue; bool has = false; for (auto& s : b->sends) has |= s.to == t.id; if (has || doc.reaches(t.id, id)) continue; ok++;
            const auto tid = t.id; m.item(doc.busLabel(t.id) + u8(" · ") + juce::String(t.name), [this, id, tid] { if (auto* x = doc.bus(id)) { x->sends.push_back({ tid, 0.5 }); doc.commit(); } }); }
        if (!ok) m.head("No bus available (a send may not loop back)");
        m.sep(); m.item("New send bus", [this, id] { const auto t = newSend(); if (auto* x = doc.bus(id)) x->sends.push_back({ t, 0.5 }); doc.commit(); });
        m.show(at(atC));
    }
    void faderMenu(const std::string& id) {
        auto* b = doc.bus(id); if (!b) return; const std::string t = "bus:" + id + ":vol";
        auto setV = [this, id](double v) { if (auto* x = doc.bus(id)) { x->vol = juce::jlimit(0.0, 1.25, v); doc.commit(); } };
        Menu m; m.head(juce::String(b->name) + u8(" level · ") + dBs(b->vol));
        m.item("Reset (0 dB)", [setV] { setV(1); });
        m.item(u8("Type in level (dB)…"), [b, setV] { ask("Level in dB (up to +1.9)", juce::String(20 * std::log10(std::max(1e-4, b->vol)), 1), [setV](juce::String v) { if (v.trim().isNotEmpty()) setV(std::pow(10.0, v.getDoubleValue() / 20)); }); });
        if (knobHooks()) knobHooks()(m, t, b->vol, setV);
        m.show(atMouse());
    }

    // --- one strip ---
    struct Strip : juce::Component {
        MixerView* o = nullptr; std::string id; bool master = false, send = false, sel = false;
        Btn name{ {}, Btn::Text }, inB{ u8("IN ▾"), Btn::Mini }, armB{ u8("●"), Btn::Mini }, monB{ "Mon", Btn::Mini }, muteB{ "M", Btn::Mini }, soloB{ "S", Btn::Mini }, invB{ u8("Ø"), Btn::Mini }, monoB{ "1", Btn::Mini }, toM{ u8("→ Master"), Btn::Mini }, addSend{ "+ send", Btn::Dashed };
        Knob pan{ "Pan", -1, 1, 0 }, width{ "Width", 0, 2, 1 }; juce::Slider fader{ juce::Slider::LinearVertical, juce::Slider::NoTextBox };
        juce::OwnedArray<Btn> leds; juce::OwnedArray<Knob> sends; std::vector<std::string> sendTo;
        float lv = 0, hold = 0; int quiet = 0; std::vector<float> cols = std::vector<float>(240, 0.f); juce::String num, title;
        Strip() {
            for (juce::Component* c : std::initializer_list<juce::Component*>{ &name, &pan, &width, &muteB, &soloB, &invB, &monoB, &fader }) addAndMakeVisible(c);
            pan.sized(Knob::Xs); width.sized(Knob::Xs); width.fmt = [](double v) { return juce::String(juce::roundToInt(v * 100)) + "%"; };
            name.setAlpha(0.01f); muteB.onCol = BAD; muteB.setTooltip("Mute"); soloB.setTooltip("Solo"); invB.setTooltip("Reverse polarity"); monoB.setTooltip("Mono (sum left + right)");
            inB.setTooltip("Audio input (microphone / line in) for this bus"); armB.setTooltip("Arm for recording: with Rec on, playing in SONG mode records this input onto the song timeline"); monB.setTooltip("Monitor: hear the input through this bus");
            toM.setTooltip("Link to the master bus. Switch off to hear this bus only through its sends."); addSend.setTooltip("Add a send to another bus"); name.setTooltip("Bus options: rename, move, presets, delete (or right-click the strip)");
            armB.onCol = BAD; armB.onText = juce::Colours::white; toM.onCol = ACCENT; toM.onText = ONACC;
            fader.setRange(0, 1.25, 0.005); fader.setDoubleClickReturnValue(true, 1.0); fader.setPopupMenuEnabled(false); fader.addMouseListener(this, false);
        }
        juce::Rectangle<int> scopeR, ledsR, meterR, dbR;
        int layoutH(int H) {   // lays out the strip for a height; returns the height it needs
            const int w = getWidth(), in = 6, c = 22; int y = 6; name.setBounds(in, y, w - 2 * in, 20); y += 24;
            scopeR = { in, y, w - 2 * in, 30 }; y += 36;
            ledsR = { in, y, w - 2 * in, std::max(22, leds.size() * 19) }; for (int i = 0; i < leds.size(); i++) leds[i]->setBounds(in, y + i * 19, w - 2 * in, 17); y += ledsR.getHeight() + 6;
            const int kw = pan.idealWidth(), kh = pan.idealHeight(); pan.setBounds(w / 2 - kw - 1, y, kw, kh); width.setBounds(w / 2 + 1, y, kw, kh); y += kh + 6;
            { const int bw = 18; int n = soloB.isVisible() ? 4 : 3, x = (w - (n * bw + (n - 1) * 2)) / 2; for (auto* b : { &muteB, &soloB, &invB, &monoB }) { if (!b->isVisible()) continue; b->setBounds(x, y, bw, c); x += bw + 2; } y += c + 6; }
            if (!master) { const int iw = 32, aw = 20, mw = 30, x0 = (w - (iw + aw + mw + 4)) / 2; inB.setBounds(x0, y, iw, c); armB.setBounds(x0 + iw + 2, y, aw, c); monB.setBounds(x0 + iw + aw + 4, y, mw, c); y += c + 6; }
            int routesH = 0; if (!master) routesH = 6 + 18 + 4 + sends.size() * (width.idealHeight() + 4) + 20;
            const int frowH = std::max(110, H - y - 30 - routesH - 6);
            const int fx = (w - (26 + 8 + 10)) / 2; fader.setBounds(fx, y, 26, frowH); meterR = { fx + 34, y, 10, frowH }; y += frowH + 6;
            dbR = { in, y, w - 2 * in, 26 }; y += 30;
            if (!master) { y += 6; toM.setBounds(in, y, w - 2 * in, 18); y += 22; for (int i = 0; i < sends.size(); i++) { sends[i]->setBounds((w - kw) / 2, y, kw, width.idealHeight()); y += width.idealHeight() + 4; } addSend.setBounds(in, y, w - 2 * in, 18); y += 22; }
            return y + 4;
        }
        void paint(juce::Graphics& g) override {
            auto r = getLocalBounds().toFloat().reduced(0.5f); const juce::Colour bg = master ? mix(ACCENT, BG2, 0.09f) : send ? mix(ACCENT2, BG2, 0.07f) : PANEL;
            g.setColour(bg); g.fillRoundedRectangle(r, RAD()); g.setColour(sel ? ACCENT : LINE); g.drawRoundedRectangle(r, RAD(), 1);
            auto nr = name.getBounds(); g.setColour(ACCENT2); g.setFont(mono(FS * 0.82f * 0.86f)); const int nw = textW(mono(FS * 0.82f * 0.86f), num) + 5; g.drawText(num, nr.removeFromLeft(nw), juce::Justification::centredLeft);
            g.setColour(TEXT); g.setFont(fontB(FS * 0.86f)); g.drawText(title, nr, juce::Justification::centredLeft, true);
            // scope: a scrolling waveform, one column per frame (lowest / highest sample), scaled to the loudest column shown
            g.setColour(BG); g.fillRoundedRectangle(scopeR.toFloat(), 6); g.setColour(juce::Colours::white.withAlpha(0.06f)); g.fillRect((float)scopeR.getX(), scopeR.getCentreY() - 0.f, (float)scopeR.getWidth(), 1.f);
            { const int W = scopeR.getWidth(); const float H = (float)scopeR.getHeight(); float pk = 0; for (float v : cols) pk = std::max(pk, std::abs(v)); const float k = (H / 2 - 1) / std::max(0.125f, pk); g.setColour(ACCENT);
              for (int x = 0; x < W && x * 2 + 1 < (int)cols.size(); x++) { const size_t i = cols.size() - (size_t)(W - x) * 2; const float a = H / 2 - cols[i + 1] * k, b = H / 2 - cols[i] * k; g.fillRect((float)scopeR.getX() + x, (float)scopeR.getY() + a, 1.f, std::max(1.f, b - a)); } }
            // meter: -60..0 dB, green to -12, yellow to -6, red above
            g.setColour(BG); g.fillRoundedRectangle(meterR.toFloat(), 5);
            { const float hh = (float)meterR.getHeight(), k = meterPos(lv); auto mr = meterR.toFloat(); g.saveState(); g.reduceClipRegion(meterR.withTop(meterR.getBottom() - (int)std::ceil(k * hh)));
              if (lv > 1) { g.setColour(BAD); g.fillRect(mr); } else { g.setColour(GOOD); g.fillRect(mr.withTop(mr.getBottom() - 0.8f * hh)); g.setColour(WARN); g.fillRect(mr.withTop(mr.getBottom() - 0.9f * hh).withHeight(0.1f * hh)); g.setColour(BAD); g.fillRect(mr.withHeight(0.1f * hh)); }
              g.restoreState(); }
            g.setFont(mono(FS * 0.78f, false)); g.setColour(TEXT); g.drawText(dBs(fader.getValue()), dbR.withHeight(13), juce::Justification::centred);
            g.setColour(DIM); const juce::String pt = "peak "; const int pw = textW(mono(FS * 0.78f, false), pt), vw = textW(mono(FS * 0.78f, false), pkS(hold)); const int px = dbR.getCentreX() - (pw + vw) / 2;
            g.drawText(pt, px, dbR.getY() + 13, pw, 13, juce::Justification::centredLeft); g.setColour(hold >= 1 ? RECLT : ACCENT2); g.drawText(pkS(hold), px + pw, dbR.getY() + 13, vw + 2, 13, juce::Justification::centredLeft);
            if (!master) { g.setColour(LINE); g.fillRect(6, toM.getY() - 5, getWidth() - 12, 1); }
            if (armB.isVisible() && armB.on()) { const float l = std::min(1.f, std::sqrt(o->doc.eng.inputPeak(juce::String(o->doc.bus(id) ? o->doc.bus(id)->input : "1").getIntValue() - 1))); g.setColour(BAD.withAlpha(0.45f)); g.drawRoundedRectangle(armB.getBounds().toFloat().expanded(l * 3), 7, l * 5); }
        }
        void mouseDown(const juce::MouseEvent& e) override {
            if (e.eventComponent == &fader) { if (e.mods.isPopupMenu()) o->faderMenu(id); return; }
            if (o->cur != id) { o->cur = id; o->selectionChanged(); }
            if (e.mods.isPopupMenu() || touchMenu()) o->stripMenu(id);
            else if (dbR.contains(e.getPosition()) && e.y > dbR.getY() + 12) { hold = 0; repaint(); }
        }
    };
    struct Strips : juce::Component { MixerView* o = nullptr; };
    Strips strips; juce::OwnedArray<Strip> list;

    void selectionChanged() { for (auto* s : list) { s->sel = s->id == cur; s->repaint(); } fx.build(); }
    void rebuild() {
        list.clear(); if (!doc.bus(cur)) cur = "master";
        for (auto& b : doc.p.buses) {
            auto* s = list.add(new Strip()); s->o = this; s->id = b.id; s->master = b.kind == "master"; s->send = b.kind == "send"; s->sel = b.id == cur; s->num = doc.busLabel(b.id); s->title = juce::String(b.name);
            const std::string id = b.id;
            s->name.setButtonText(juce::String(b.name)); s->name.onClick = [this, id] { stripMenu(id); };
            for (auto& f : b.fx) { auto* l = s->leds.add(new Btn(juce::String(fxName(f.type, f)).substring(0, 4), Btn::FxLed)); const auto fid = f.id; l->setOn(f.on); l->setTooltip(juce::String(fxName(f.type, f)) + " (click to bypass)");
                l->onClick = [this, id, fid] { if (auto* x = doc.bus(id)) for (auto& q : x->fx) if (q.id == fid) q.on = !q.on; doc.commit(); }; s->addAndMakeVisible(l); }
            s->pan.set(b.pan); s->pan.setLink("bus:" + id + ":pan"); s->pan.onChange = [this, id](double v) { if (auto* x = doc.bus(id)) { x->pan = v; doc.live(); doc.autoWrite("bus:" + id + ":pan", v); } }; s->pan.onDone = [this] { doc.commit(); };
            s->width.set(b.width); s->width.onChange = [this, id](double v) { if (auto* x = doc.bus(id)) { x->width = v; doc.live(); } }; s->width.onDone = [this] { doc.commit(); };
            auto tog = [this, id](Btn& bt, bool v, std::function<bool&(us::Bus&)> f) { bt.setOn(v); bt.onClick = [this, id, f] { if (auto* x = doc.bus(id)) { f(*x) = !f(*x); doc.commit(); } }; };
            tog(s->muteB, b.mute, [](us::Bus& x) -> bool& { return x.mute; }); tog(s->soloB, b.solo, [](us::Bus& x) -> bool& { return x.solo; }); tog(s->invB, b.inv, [](us::Bus& x) -> bool& { return x.inv; }); tog(s->monoB, b.mono, [](us::Bus& x) -> bool& { return x.mono; });
            s->soloB.setVisible(b.kind == "insert");
            s->fader.setValue(b.vol, juce::dontSendNotification); s->fader.getProperties().set("link", juce::String("bus:" + id + ":vol"));
            s->fader.onValueChange = [this, id, s] { if (auto* x = doc.bus(id)) { x->vol = s->fader.getValue(); doc.live(); doc.autoWrite("bus:" + id + ":vol", x->vol); s->repaint(); } };
            s->fader.onDragEnd = [this] { doc.commit(); };
            if (!s->master) {
                for (auto* c : std::initializer_list<juce::Component*>{ &s->inB, &s->armB, &s->monB, &s->toM, &s->addSend }) s->addAndMakeVisible(c);
                s->inB.setOn(!b.input.empty()); s->inB.onCol = BG3; s->inB.onText = TEXT; s->inB.onClick = [this, id, s] { inputMenu(id, &s->inB); };
                s->armB.setOn(b.arm); s->armB.onClick = [this, id] { if (auto* x = doc.bus(id)) { x->arm = !x->arm; if (x->arm && x->input.empty()) x->input = "1"; doc.commit(); } };
                s->monB.setOn(b.mon); s->monB.onClick = [this, id] { if (auto* x = doc.bus(id)) { x->mon = !x->mon; doc.commit(); } };
                s->toM.setOn(b.toMaster); s->toM.onClick = [this, id] { if (auto* x = doc.bus(id)) { x->toMaster = !x->toMaster; doc.commit(); } };
                s->addSend.onClick = [this, id, s] { sendMenu(id, &s->addSend); };
                for (auto& sd : b.sends) {
                    auto* k = s->sends.add(new Knob(u8("→ ") + doc.busLabel(sd.to), 0, 1.25, 0.5)); k->sized(Knob::Xs); k->fmt = [](double v) { return juce::String(juce::roundToInt(v * 100)) + "%"; }; k->set(sd.amt);
                    const auto to = sd.to; juce::String tn; if (auto* t = doc.bus(to)) tn = juce::String(t->name);
                    k->s.setTooltip("Send to " + tn + " - right-click to remove");
                    k->onChange = [this, id, to](double v) { if (auto* x = doc.bus(id)) for (auto& q : x->sends) if (q.to == to) q.amt = v; doc.live(); }; k->onDone = [this] { doc.commit(); };
                    k->s.addMouseListener(new SendRemover(this, id, to), false);
                    s->addAndMakeVisible(k);
                }
            }
            strips.addAndMakeVisible(s);
        }
        layoutStrips(); fx.build();
    }
    // right-click a send knob: remove that send (owned by the knob's slider listener list; leaks one tiny object per rebuild at most)
    struct SendRemover : juce::MouseListener { MixerView* o; std::string id, to; SendRemover(MixerView* m, std::string i, std::string t) : o(m), id(std::move(i)), to(std::move(t)) {}
        void mouseDown(const juce::MouseEvent& e) override { if (!e.mods.isPopupMenu()) return; juce::MessageManager::callAsync([o = o, id = id, to = to] { if (auto* x = o->doc.bus(id)) { x->sends.erase(std::remove_if(x->sends.begin(), x->sends.end(), [&](auto& s) { return s.to == to; }), x->sends.end()); o->doc.commit(); } }); } };
    static std::string fxName(const std::string& type, const us::FxSlot& f) { if (type == "plugin") return f.str("name", "Plug-in"); if (auto* s = fxSpec(type)) return s->name; return type; }
    void layoutStrips() {
        const int H = std::max(200, stripsView.getHeight() - (stripsView.isHorizontalScrollBarShown() ? 10 : 0) - 16); const int sw = juce::roundToInt(96 * style().scale); int x = 8, maxH = H;
        for (auto* s : list) { s->setSize(sw, H); const int need = s->layoutH(H); if (need > H) { s->setSize(sw, need); s->layoutH(need); maxH = std::max(maxH, need); } s->setTopLeftPosition(x, 8); x += sw + 6; }
        addBusB.setBounds(x, 8, 38, std::min(H, 220)); x += 46;
        strips.setSize(std::max(x, stripsView.getWidth()), maxH + 16);
    }

    // --- the effect slots of the selected bus ---
    struct FxRack : juce::Component {
        MixerView* o = nullptr; Btn addFx{ "+ Add effect" }; juce::OwnedArray<juce::Component> parts; std::vector<juce::Rectangle<int>> cards; juce::String headText; int H = 100;
        FxRack() { addAndMakeVisible(addFx); addFx.primary = true; addFx.sm = true; addFx.onClick = [this] { addMenu(); }; }
        void addMenu() {
            Menu m; for (auto& s : fxSpecs()) { const std::string t = s.type; m.item(s.name, [this, t] { if (auto* b = o->doc.bus(o->cur)) { b->fx.push_back(newFx(t)); o->doc.commit(); } }); }
            m.item(u8("Plug-in (CLAP / VST)…"), [this] { choosePlugin(&addFx, false, [this](const juce::var& slot) { if (auto* b = o->doc.bus(o->cur)) { us::FxSlot f; f.id = us::newId(); f.type = "plugin"; f.on = true; if (auto* so = slot.getDynamicObject()) f.p = so->getProperties(); b->fx.push_back(f); o->doc.commit(); } }); });
            m.show(at(&addFx));
        }
        template <class T> T* addPart(T* p) { parts.add(p); return p; }
        void build();
        void layout();
        void refresh() {   // values changed elsewhere (automation, MIDI links, undo): update the knobs
            auto* b = o->doc.bus(o->cur); if (!b) return;
            for (auto* p : parts) if (auto* k = dynamic_cast<Knob*>(p)) { const int s = p->getProperties()["slot"]; if (s < (int)b->fx.size()) k->set(b->fx[(size_t)s].num(p->getProperties()["k"].toString().toRawUTF8(), k->get())); }
        }
        void paintOverChildren(juce::Graphics& g) override {   // the slot LEDs (.led)
            for (auto* p : parts) if (p->getProperties()["role"] == "led") { auto* b = static_cast<Btn*>(p); auto l = b->getBounds().toFloat(); g.setColour(b->on() ? ACCENT : BG); g.fillEllipse(l); g.setColour(b->on() ? ACCENT.darker(0.4f) : LINE); g.drawEllipse(l.reduced(1), 2); }
        }
        void paint(juce::Graphics& g) override {
            g.fillAll(BG2); g.setColour(TEXT); g.setFont(fontB(FS)); g.drawText(headText, 8, 8, getWidth() - addFx.getWidth() - 24, CTL() - 4, juce::Justification::centredLeft, true);
            for (auto& c : cards) card(g, c.toFloat(), mix(BG3, BG2, 0.5f));
            if (cards.empty()) { g.setColour(DIM); g.setFont(font(FS * 0.88f)); g.drawFittedText("No effects on this bus. Add EQ, compression, reverb, delay, a peak controller for ducking and more, then toggle each slot on or off.", juce::Rectangle<int>(12, CTL() + 18, getWidth() - 24, 60), juce::Justification::topLeft, 4); }
        }
    };
    FxRack fx; friend struct FxRack;

    void changeListenerCallback(juce::ChangeBroadcaster*) override {
        juce::String s; for (auto& b : doc.p.buses) { s << b.id << b.name << b.kind << (int)b.mute << (int)b.solo << (int)b.inv << (int)b.mono << (int)b.toMaster << b.input << (int)b.arm << (int)b.mon << (int)b.stereo; for (auto& f : b.fx) s << f.id << (int)f.on << f.type; for (auto& x : b.sends) s << x.to; s << "|"; }
        if (s != sig) { sig = s; rebuild(); return; }
        for (auto* st : list) if (auto* b = doc.bus(st->id)) { if (!st->fader.isMouseButtonDown()) st->fader.setValue(b->vol, juce::dontSendNotification); st->pan.set(b->pan); st->width.set(b->width);
            for (int i = 0; i < st->sends.size() && i < (int)b->sends.size(); i++) st->sends[i]->set(b->sends[(size_t)i].amt); st->repaint(); }
        fx.refresh();
    }
    void timerCallback() override {
        if (!isShowing()) return;
        for (int i = 0; i < list.size() && i < (int)doc.p.buses.size(); i++) {
            auto* s = list[i]; const float oLv = s->lv, oHold = s->hold, lv = doc.eng.busPeak((size_t)i); s->lv = std::max(lv, s->lv * 0.82f); s->hold = std::max(lv, s->hold * 0.992f); if (s->lv < 1e-4f) s->lv = 0; if (s->hold < 1e-4f) s->hold = 0;
            auto [lo, hi] = doc.eng.busWave((size_t)i); s->quiet = std::abs(lo) < 1e-5f && std::abs(hi) < 1e-5f ? s->quiet + 1 : 0;
            const bool scrolling = s->quiet <= (int)s->cols.size() / 2;   // a silent bus stops redrawing once its trace has gone flat
            if (scrolling) { std::rotate(s->cols.begin(), s->cols.begin() + 2, s->cols.end()); s->cols[s->cols.size() - 2] = lo; s->cols[s->cols.size() - 1] = hi; }
            if (doc.eng.isPlaying() && doc.song) if (auto* b = doc.bus(s->id)) { if (!s->fader.isMouseButtonDown()) s->fader.setValue(b->vol, juce::dontSendNotification); s->pan.set(b->pan); }
            if (scrolling || s->lv != oLv || s->hold != oHold) s->repaint();
        }
    }
public:
    struct FxRackPublic;
};

// the effect panel: a card per slot (LED, name, move up / down, remove) with a knob or a list per parameter
inline void MixerView::FxRack::build() {
    parts.clear(); cards.clear(); auto* b = o->doc.bus(o->cur); if (!b) return;
    headText = u8("FX slots · ") + juce::String(b->name);
    const std::string bid = b->id;
    for (size_t i = 0; i < b->fx.size(); i++) {
        auto& f = b->fx[i]; const std::string fid = f.id;
        auto* led = addPart(new Btn({}, Btn::Text)); led->setOn(f.on); led->setTooltip("Enable / bypass"); led->getProperties().set("role", "led"); led->getProperties().set("slot", (int)i); led->setAlpha(0.01f);
        led->onClick = [this, bid, fid] { if (auto* x = o->doc.bus(bid)) for (auto& q : x->fx) if (q.id == fid) q.on = !q.on; o->doc.commit(); };
        auto* nm = addPart(new Cap(juce::String(fxName(f.type, f)), FS)); nm->setFont(fontB(FS)); nm->setColour(juce::Label::textColourId, TEXT); nm->getProperties().set("role", "name"); nm->getProperties().set("slot", (int)i);
        auto mv = [this, bid, fid](int d) { if (auto* x = o->doc.bus(bid)) { for (size_t k = 0; k < x->fx.size(); k++) if (x->fx[k].id == fid) { const int j = (int)k + d; if (j >= 0 && j < (int)x->fx.size()) { std::swap(x->fx[k], x->fx[(size_t)j]); o->doc.commit(); } return; } } };
        auto* up = addPart(new Btn(u8("↑"))); up->sm = true; up->setTooltip("Move earlier in the chain"); up->onClick = [mv] { mv(-1); }; up->getProperties().set("role", "up"); up->getProperties().set("slot", (int)i);
        auto* dn = addPart(new Btn(u8("↓"))); dn->sm = true; dn->setTooltip("Move later"); dn->onClick = [mv] { mv(1); }; dn->getProperties().set("role", "down"); dn->getProperties().set("slot", (int)i);
        auto* rm = addPart(new Btn(u8("✕"))); rm->sm = true; rm->setTooltip("Remove"); rm->getProperties().set("role", "rm"); rm->getProperties().set("slot", (int)i);
        rm->onClick = [this, bid, fid] { if (auto* x = o->doc.bus(bid)) { x->fx.erase(std::remove_if(x->fx.begin(), x->fx.end(), [&](auto& q) { return q.id == fid; }), x->fx.end()); o->doc.commit(); } };
        if (f.type == "plugin") {
            auto* pp = addPart(new PlugPanel(o->doc, "fx:" + bid + ":" + fid, "fx:" + bid + ":" + fid + ":")); pp->getProperties().set("role", "plug"); pp->getProperties().set("slot", (int)i);
            pp->slot = [this, bid, fid] { juce::var v(new juce::DynamicObject()); if (auto* x = o->doc.bus(bid)) for (auto& q : x->fx) if (q.id == fid) for (auto& kv : q.p) v.getDynamicObject()->setProperty(kv.name, kv.value); return v; };
            pp->put = [this, bid, fid](const juce::Identifier& k, const juce::var& v) { if (auto* x = o->doc.bus(bid)) for (auto& q : x->fx) if (q.id == fid) q.p.set(k, v); };
            pp->live = [this] { o->doc.live(); }; pp->commit = [this] { o->doc.commit(); };
            pp->change = [this, bid, fid, pp] { choosePlugin(pp, false, [this, bid, fid](const juce::var& slot) { if (auto* x = o->doc.bus(bid)) for (auto& q : x->fx) if (q.id == fid) { q.p.clear(); if (auto* so = slot.getDynamicObject()) q.p = so->getProperties(); } o->doc.commit(); }); };
        } else if (auto* sp = fxSpec(f.type)) {
            for (auto& q : sp->params) {
                const std::string k = q.k;
                if (!q.opts.empty()) {
                    struct Pick : juce::Component { Cap l; juce::ComboBox c; Pick() { addAndMakeVisible(l); addAndMakeVisible(c); l.setFont(font(FS * 0.78f)); } void resized() override { auto r = getLocalBounds(); l.setBounds(r.removeFromTop(16)); c.setBounds(r.removeFromTop(CTL())); } };
                    auto* pk = addPart(new Pick()); pk->l.setText(q.label, juce::dontSendNotification); pk->getProperties().set("role", "pick"); pk->getProperties().set("slot", (int)i);
                    std::vector<std::pair<std::string, juce::String>> opts;
                    if (q.opts[0] == "@buses") { opts.push_back({ "", f.type == "gate" ? "(this bus)" : f.type == "vocoder" ? "(built-in buzz)" : "(choose a bus)" }); for (auto& z : o->doc.p.buses) if (z.id != bid && z.kind != "master") opts.push_back({ z.id, o->doc.busLabel(z.id) + u8(" · ") + juce::String(z.name) }); }
                    else for (auto& op : q.opts) opts.push_back({ op, op == "full" ? "Whole signal" : op == "band" ? "One frequency band" : op == "split" ? "Only the highs" : op == "wide" ? "Whole signal" : op == "mono" ? "Mono below" : juce::String(op) });
                    for (size_t j = 0; j < opts.size(); j++) pk->c.addItem(opts[j].second, (int)j + 1);
                    const auto curV = f.str(q.k, ""); for (size_t j = 0; j < opts.size(); j++) if (opts[j].first == curV) pk->c.setSelectedItemIndex((int)j, juce::dontSendNotification);
                    pk->c.onChange = [this, bid, fid, k, opts, pk] { const int j = pk->c.getSelectedItemIndex(); if (j < 0) return; if (auto* x = o->doc.bus(bid)) for (auto& s : x->fx) if (s.id == fid) s.p.set(juce::Identifier(k), juce::String(opts[(size_t)j].first)); o->doc.commit(); };
                } else {
                    auto* kn = addPart(new Knob(q.label, q.lo, q.hi, q.def, q.expo)); kn->sized(Knob::Sm); kn->set(f.num(q.k, q.def)); kn->getProperties().set("role", "knob"); kn->getProperties().set("slot", (int)i); kn->getProperties().set("k", juce::String(k));
                    const std::string t = "fx:" + bid + ":" + fid + ":" + k; kn->setLink(t);
                    kn->onChange = [this, bid, fid, k, t](double v) { if (auto* x = o->doc.bus(bid)) for (auto& s : x->fx) if (s.id == fid) s.p.set(juce::Identifier(k), v); o->doc.live(); o->doc.autoWrite(t, v); };
                    kn->onDone = [this] { o->doc.commit(); };
                }
            }
            if (f.type == "meters") { auto* mp = addPart(new MetersPanel(fid, f.str("view", "spectrum"))); mp->getProperties().set("role", "meters"); mp->getProperties().set("slot", (int)i); mp->setTooltip("Click the readings to restart integrated loudness and the peak hold"); }
            if (f.type == "vocoder") { auto* n = addPart(new Cap(u8("Put this on the voice’s bus. Choose the bus with the synth that should sing as the Carrier, or keep the built-in buzz and set its note."), FS * 0.88f)); n->getProperties().set("role", "note"); n->getProperties().set("slot", (int)i); n->setJustificationType(juce::Justification::topLeft); }
            if (f.type == "duck") { auto* n = addPart(new Cap(u8("Put this on the sound that should move out of the way (bass, pad) and choose the bus that triggers it (kick). “One frequency band” ducks only around Band Hz, like linking the peak to an EQ band."), FS * 0.88f)); n->getProperties().set("role", "note"); n->getProperties().set("slot", (int)i); n->setJustificationType(juce::Justification::topLeft); }
        }
    }
    for (auto* p : parts) addAndMakeVisible(p);
    layout(); repaint();
}
inline void MixerView::FxRack::layout() {
    const int W = std::max(200, o->fxView.getWidth() - 10); const int c = CTL(); addFx.setBounds(W - addFx.idealWidth() - 8, 8, addFx.idealWidth(), c - 4);
    int y = 8 + c + 4; cards.clear(); int slot = -1; juce::Rectangle<int> cr; int x = 0, rowH = 0, cy = 0;
    auto close = [&] { if (slot >= 0) { cr.setHeight(cy + rowH + 8 - cr.getY()); cards.push_back(cr); y = cr.getBottom() + 8; } };
    for (auto* p : parts) {
        const int s = p->getProperties()["slot"]; const juce::String role = p->getProperties()["role"];
        if (s != slot) { close(); slot = s; cr = { 8, y, W - 16, 0 }; cy = y + 8 + c + 6; x = 16; rowH = 0; }
        if (role == "led") p->setBounds(16, y + 8 + (c - 4 - 14) / 2, 14, 14);
        else if (role == "name") p->setBounds(36, y + 8, W - 36 - 3 * 34 - 24, c - 4);
        else if (role == "up") p->setBounds(W - 16 - 3 * 32, y + 8, 30, c - 4);
        else if (role == "down") p->setBounds(W - 16 - 2 * 32, y + 8, 30, c - 4);
        else if (role == "rm") p->setBounds(W - 16 - 32, y + 8, 30, c - 4);
        else {
            int w = 50, h = 60;
            if (auto* k = dynamic_cast<Knob*>(p)) { w = k->idealWidth(); h = k->idealHeight(); }
            else if (role == "pick") { w = 150; h = 16 + c; }
            else if (role == "note") { w = W - 32; h = 70; }
            else if (role == "meters") { w = W - 32; h = static_cast<MetersPanel*>(p)->idealHeight(); }
            else if (role == "plug") { auto* pp = static_cast<PlugPanel*>(p); pp->setSize(W - 32, 100); w = W - 32; h = pp->idealHeight(); }
            if (x + w > W - 16 && x > 16) { x = 16; cy += rowH + 6; rowH = 0; }
            p->setBounds(x, cy, w, h); x += w + 6; rowH = std::max(rowH, h);
        }
    }
    close();
    setSize(W, std::max(y + 8, o->fxView.getHeight()));
    repaint();
}

}   // namespace ui
