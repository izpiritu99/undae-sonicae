// Plug-in UI: the chooser (scans every host once, then from a cache; "Rescan" refreshes), and the panel for a loaded
// plug-in: status, its own window, and knobs for its parameters (filter by name; big plug-ins have thousands).
#pragma once
#include <thread>
#include "Doc.h"
#include "Theme.h"

namespace ui {

// slot fields from a scan result: { path, pid, name, fmt, windows, bits, inst }
inline juce::var slotFromScan(const juce::var& pl, bool inst) {
    auto* o = new juce::DynamicObject();
    o->setProperty("path", pl.getProperty("path", "")); o->setProperty("pid", pl.getProperty("id", "")); o->setProperty("name", pl.getProperty("name", "")); o->setProperty("fmt", pl.getProperty("format", ""));
    if ((bool)pl.getProperty("windows", false)) o->setProperty("windows", true); if ((int)pl.getProperty("bits", 64) == 32) o->setProperty("bits", 32); if (inst) o->setProperty("inst", true);
    return juce::var(o);
}

class PluginList {
public:
    static PluginList& get() { static PluginList l; return l; }
    // calls back on the message thread with the list (scanning first when needed)
    void fetch(std::function<void(const std::vector<juce::var>&)> cb, bool rescan = false) {
        { std::lock_guard<std::mutex> lk(m); if (!rescan && scanned) { auto copy = list; juce::MessageManager::callAsync([cb, copy] { cb(copy); }); return; } }
        std::thread([this, cb] { auto l = us::scanPlugins(); { std::lock_guard<std::mutex> lk(m); list = l; scanned = !us::HostSet::get().winPending(); }
            juce::MessageManager::callAsync([cb, l] { cb(l); }); }).detach();
    }
private:
    std::mutex m; std::vector<juce::var> list; bool scanned = false;
};

// a menu of plug-ins at `at`; want: 1 instruments, 0 effects
inline void choosePlugin(juce::Component* at, bool instruments, std::function<void(const juce::var& slot)> onPick, bool rescan = false) {
    juce::Component::SafePointer<juce::Component> sp(at);
    PluginList::get().fetch([sp, instruments, onPick](const std::vector<juce::var>& all) {
        juce::PopupMenu m; std::vector<juce::var> shown; if (us::HostSet::get().remote()) m.addSectionHeader("On the plug-in server (windows open there)");
        auto group = [&](const char* title, std::function<bool(const juce::var&)> test) {
            bool head = false;
            for (auto& p : all) { if ((bool)p.getProperty("instrument", false) != instruments || !test(p)) continue; if (!head) { m.addSectionHeader(title); head = true; }
                shown.push_back(p); m.addItem((int)shown.size(), p.getProperty("name", "").toString() + (p.getProperty("vendor", "").toString().isNotEmpty() ? u8(" · ") + p.getProperty("vendor", "").toString() : juce::String())); }
        };
        group("CLAP", [](auto& p) { return p.getProperty("format", "") == "clap"; });
        group("VST3 (Linux)", [](auto& p) { return p.getProperty("format", "") == "vst3" && !(bool)p.getProperty("windows", false); });
        group("Windows VST (through Wine)", [](auto& p) { return (bool)p.getProperty("windows", false) && (int)p.getProperty("bits", 64) != 32; });
        group("Windows VST, 32-bit (through Wine)", [](auto& p) { return (bool)p.getProperty("windows", false) && (int)p.getProperty("bits", 64) == 32; });
        if (shown.empty()) m.addItem(-1, us::HostSet::get().winPending() ? "Wine is still starting: try again in a moment" : juce::String("No ") + (instruments ? "instrument" : "effect") + " plug-ins found (~/.clap, ~/.vst3, ~/.vst-win)", false);
        m.addSeparator(); m.addItem(9999, "Rescan plug-in folders");
        m.showMenuAsync(sp ? juce::PopupMenu::Options().withTargetComponent(sp.getComponent()) : juce::PopupMenu::Options(), [sp, instruments, onPick, shown](int r) {
            if (r == 9999) { if (sp) choosePlugin(sp.getComponent(), instruments, onPick, true); return; }
            if (r > 0 && r <= (int)shown.size()) onPick(slotFromScan(shown[(size_t)r - 1], instruments));
        });
    }, rescan);
}

// The panel for one plug-in slot (plugui.js pluginBody): its name (click to choose another), "Open window", status, a
// filter, and knobs for up to 64 matching parameters. autoKey: the automation / MIDI link prefix ("fx:<bus>:<slot>:" or
// "plug:<channel>:"), so knob moves can be recorded and linked.
class PlugPanel : public juce::Component, private juce::Timer {
public:
    std::function<juce::var()> slot;                                   // current slot (read)
    std::function<void(const juce::Identifier&, const juce::var&)> put; // write one field (the owner clones shared data first)
    std::function<void()> live, commit, change;                         // change: pick another plug-in
    PlugPanel(Doc& d, std::string key, std::string autoKey = {}) : doc(d), k(std::move(key)), ak(std::move(autoKey)) {
        for (juce::Component* c : std::initializer_list<juce::Component*>{ &nameB, &winB, &status, &filter, &more }) addAndMakeVisible(c);
        nameB.sm = winB.sm = true; nameB.setTooltip("Pick a CLAP, VST3 or Windows VST plug-in installed on this computer"); winB.setTooltip("Show the plug-in's own window");
        filter.setTextToShowWhenEmpty("Filter parameters", DIM); filter.onTextChange = [this] { rebuild(); };
        winB.onClick = [this] { if (auto pi = doc.eng.plugin(k)) { const bool want = !open; std::thread([pi, want, sp = juce::Component::SafePointer<PlugPanel>(this)] { const bool o = pi->showWindow(want); juce::MessageManager::callAsync([sp, o, want] { if (sp) { sp->open = o; sp->winB.setButtonText(o ? "Close window" : "Open window"); sp->winB.primary = o; sp->winB.repaint(); if (want && !o) toast("The plug-in's window could not be opened"); } }); }).detach(); } };
        nameB.onClick = [this] { if (change) change(); };
        startTimerHz(4); rebuild();
    }
    void resized() override { layout(); }
    int idealHeight() const { return height; }
    bool open = false;
private:
    Doc& doc; std::string k, ak; Btn nameB{ u8("Choose plug-in…") }, winB{ "Open window" }; Cap status{ {}, FS * 0.88f }, more{ {}, FS * 0.88f }; Field filter;
    std::vector<std::unique_ptr<Knob>> knobs; std::vector<int> ids; bool built = false; int height = 60; juce::String lastStatus = "?";
    void rebuild() {
        knobs.clear(); ids.clear(); auto pi = doc.eng.plugin(k); built = pi && pi->ready(); int total = 0;
        if (built) {
            const auto f = filter.getText().toLowerCase(); int shown = 0;
            for (auto& q : pi->info().params) {
                if (q.hidden || q.readonly) continue; total++; if (f.isNotEmpty() && !juce::String(q.name).toLowerCase().contains(f)) continue; if (shown >= 64) continue; shown++;
                const double lo = q.min, hi = q.max > q.min ? q.max : q.min + 1; const int id = q.id;
                auto kn = std::make_unique<Knob>(juce::String(q.name).substring(0, 14), lo, hi, q.def, false, q.stepped ? 1 : 0); kn->sized(Knob::Sm); kn->set(currentValue(id, q.value));
                const std::string vk = "v" + std::to_string(id); if (!ak.empty()) kn->setLink(ak + vk);
                kn->onChange = [this, id, vk](double v) { if (put) put(juce::Identifier("v" + juce::String(id)), v); if (live) live(); if (!ak.empty()) doc.autoWrite(ak + vk, v); };
                kn->onDone = [this] { if (commit) commit(); };
                addAndMakeVisible(*kn); knobs.push_back(std::move(kn)); ids.push_back(id);
            }
        }
        more.setText(total > 64 && filter.getText().isEmpty() ? "Showing 64 of " + juce::String(total) + " parameters; filter them, or open the plug-in's window for the rest." : juce::String(), juce::dontSendNotification);
        layout();
    }
    double currentValue(int id, double def) const { if (!slot) return def; const auto v = slot().getProperty(juce::Identifier("v" + juce::String(id)), juce::var()); return v.isDouble() || v.isInt() ? (double)v : def; }
    void layout() {
        const int W = std::max(200, getWidth()), c = CTL(); int x = 0, y = 0;
        nameB.setBounds(0, 0, std::min(W - 120, nameB.idealWidth()), c - 4); winB.setBounds(nameB.getRight() + 6, 0, winB.idealWidth(), c - 4); winB.setVisible(built);
        status.setBounds(0, c, W, 18); y = c + 20;
        if (built) { filter.setBounds(0, y, std::min(220, W), c); y += c + 6; }
        for (auto& kn : knobs) { const int kw = kn->idealWidth(), kh = kn->idealHeight(); if (x + kw > W) { x = 0; y += kh + 6; } kn->setBounds(x, y, kw, kh); x += kw + 6; }
        if (!knobs.empty()) y += knobs[0]->idealHeight() + 6;
        if (more.getText().isNotEmpty()) { more.setBounds(0, y, W, 34); more.setJustificationType(juce::Justification::topLeft); y += 36; }
        height = y;
    }
    void timerCallback() override {
        auto pi = doc.eng.plugin(k); const auto sv = slot ? slot() : juce::var();
        const auto nm = sv.getProperty("name", "").toString(); if (nameB.getButtonText() != (nm.isEmpty() ? u8("Choose plug-in…") : nm)) { nameB.setButtonText(nm.isEmpty() ? u8("Choose plug-in…") : nm); layout(); }
        juce::String st = doc.eng.pluginStatus(k);
        if (!pi) st = nm.isEmpty() ? juce::String() : juce::String("Not loaded"); else if (pi->ready()) st = (pi->info().vendor.empty() ? juce::String() : juce::String(pi->info().vendor) + u8(" · ")) + juce::String((int)pi->info().params.size()) + " parameters" + (pi->info().gui ? u8(" · has its own window") : "") + (sv.getProperty("windows", false) ? u8(" · Windows, through Wine") : "");
        status.setText(st, juce::dontSendNotification); winB.setEnabled(pi && pi->ready() && pi->info().gui);
        if (pi && pi->ready() && !built) rebuild();
        if (built) for (size_t i = 0; i < knobs.size(); i++) knobs[i]->set(currentValue(ids[i], knobs[i]->get()));
        if (st != lastStatus && getParentComponent()) { lastStatus = st; getParentComponent()->resized(); }
    }
};

}   // namespace ui
