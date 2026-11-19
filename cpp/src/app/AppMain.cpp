// Undae Sonicae, native edition: the window (app/js/main.js). The top bar (project menu, transport, tempo, time
// signature, pattern, position, master meter and pitch, load, undo / redo, MIDI, keyboard) over one to four docked
// decks and a floating one, each showing any view; the on-screen keyboard; every shortcut; MIDI controller links;
// recording (notes, audio from armed buses, automation), exports, autosave. Plays through JACK / ALSA (PipeWire).
#include <iostream>
#include <fstream>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#if JUCE_ANDROID
#include <juce_opengl/juce_opengl.h>
#endif
#include "Doc.h"
#include "Keys.h"
#include "engine/Mp3.h"
#include "MixerView.h"
#include "DesignView.h"
#include "InstView.h"
#include "LabView.h"
#include "LibView.h"
#include "RackView.h"
#include "SettingsView.h"
#include "RollView.h"
#include "SongView.h"
#include "Theme.h"
#if __has_include("BinaryData.h")
#include "BinaryData.h"
#endif
#if JUCE_LINUX || JUCE_ANDROID
#include <unistd.h>
#include <dirent.h>
#endif
#if JUCE_WINDOWS
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#endif
#if JUCE_MAC
#include <libproc.h>
#include <mach/mach_time.h>
#include <unistd.h>
#endif

namespace ui {

// --- the app's views ------------------------------------------------------------------------------------------------
struct ViewDef { const char* id; const char* label; const char* icon; };
inline const std::vector<ViewDef>& views() {
    static const std::vector<ViewDef> v = { { "rack", "Rack", "rack" }, { "roll", "Piano Roll", "roll" }, { "song", "Song", "song" }, { "mixer", "Mixer", "mixer" }, { "inst", "Instrument", "inst" },
        { "design", "Sound Design", "design" }, { "lab", "Audio Lab", "lab" }, { "lib", "Library", "lib" }, { "set", "Settings", "set" } };
    return v;
}

// --- process load: CPU (share of all cores) and memory of the app and the plug-in hosts it started ---------------------
struct Load {
    double lastT = 0; long long lastTicks = 0; double cpu = -1, ramMB = -1;
    static long long ticksOf(int pid, int& ppid, long long& rssPages) {
#if JUCE_LINUX || JUCE_ANDROID
        std::ifstream f("/proc/" + std::to_string(pid) + "/stat"); std::string s; std::getline(f, s); const auto rp = s.rfind(')'); if (rp == std::string::npos) return -1;
        std::istringstream is(s.substr(rp + 2)); std::string st; long long v; std::vector<long long> x; is >> st; while (is >> v) x.push_back(v);
        if (x.size() < 21) return -1; ppid = (int)x[0]; rssPages = x[20]; return x[10] + x[11];   // utime + stime (reading stops after rss: the next field overflows)
#else
        juce::ignoreUnused(pid, ppid, rssPages); return -1;
#endif
    }
    void update() {
#if JUCE_LINUX || JUCE_ANDROID
        const int me = getpid(); std::map<int, int> parent; std::map<int, std::pair<long long, long long>> info;
        if (DIR* d = opendir("/proc")) { while (auto* e = readdir(d)) { const int pid = atoi(e->d_name); if (pid <= 0) continue; int pp = 0; long long rss = 0; const auto t = ticksOf(pid, pp, rss); if (t >= 0) { parent[pid] = pp; info[pid] = { t, rss }; } } closedir(d); }
        std::set<int> tree{ me }; bool grew = true; while (grew) { grew = false; for (auto& [pid, pp] : parent) if (tree.count(pp) && tree.insert(pid).second) grew = true; }
        long long ticks = 0, rss = 0; for (int pid : tree) { ticks += info[pid].first; rss += info[pid].second; }
        const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0, hz = (double)sysconf(_SC_CLK_TCK), cores = std::max(1, juce::SystemStats::getNumCpus());
        if (lastT > 0 && now > lastT) cpu = juce::jlimit(0.0, 100.0, (ticks - lastTicks) / hz / (now - lastT) / cores * 100);
        lastT = now; lastTicks = ticks; ramMB = rss * (double)sysconf(_SC_PAGESIZE) / 1048576.0;
#elif JUCE_WINDOWS
        // the app and the processes it started (the VST hosts): CPU time in 100 ns units, working set in bytes
        const DWORD me = GetCurrentProcessId(); std::map<DWORD, DWORD> parent;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) { PROCESSENTRY32W e{}; e.dwSize = sizeof e; for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) parent[e.th32ProcessID] = e.th32ParentProcessID; CloseHandle(snap); }
        std::set<DWORD> tree{ me }; bool grew = true; while (grew) { grew = false; for (auto& [pid, pp] : parent) if (pid != pp && tree.count(pp) && tree.insert(pid).second) grew = true; }
        long long ticks = 0; double bytes = 0;
        for (DWORD pid : tree) {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); if (!h) continue;
            FILETIME c, x, k, u; if (GetProcessTimes(h, &c, &x, &k, &u)) ticks += (long long)(((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime));
            PROCESS_MEMORY_COUNTERS pm{}; if (GetProcessMemoryInfo(h, &pm, sizeof pm)) bytes += (double)pm.WorkingSetSize;
            CloseHandle(h);
        }
        const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0, cores = std::max(1, juce::SystemStats::getNumCpus());
        if (lastT > 0 && now > lastT) cpu = juce::jlimit(0.0, 100.0, (ticks - lastTicks) / 1e7 / (now - lastT) / cores * 100);
        lastT = now; lastTicks = ticks; ramMB = bytes / 1048576.0;
#elif JUCE_MAC
        // the app and the processes it started: CPU time from proc_pidinfo (Mach time units: on Apple Silicon these are
        // not nanoseconds, so mach_timebase_info converts), resident memory in bytes
        std::vector<pid_t> tree{ getpid() };
        for (size_t i = 0; i < tree.size() && tree.size() < 256; i++) {
            pid_t kids[64]; const int n = proc_listchildpids(tree[i], kids, (int)sizeof kids);
            for (int k = 0; k < n && k < 64; k++) if (kids[k] > 0) tree.push_back(kids[k]);
        }
        static const double nsPerTick = [] { mach_timebase_info_data_t tb{}; mach_timebase_info(&tb); return tb.denom ? (double)tb.numer / tb.denom : 1.0; }();
        double cpuNs = 0, bytes = 0;
        for (pid_t pid : tree) { proc_taskinfo ti{}; if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, (int)sizeof ti) == (int)sizeof ti) { cpuNs += (double)(ti.pti_total_user + ti.pti_total_system) * nsPerTick; bytes += (double)ti.pti_resident_size; } }
        const long long ticks = (long long)(cpuNs / 100);   // 100 ns units, like the Windows branch
        const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0, cores = std::max(1, juce::SystemStats::getNumCpus());
        if (lastT > 0 && now > lastT) cpu = juce::jlimit(0.0, 100.0, (ticks - lastTicks) / 1e7 / (now - lastT) / cores * 100);
        lastT = now; lastTicks = ticks; ramMB = bytes / 1048576.0;
#endif
    }
};

// --- one deck: a row of tabs and the chosen view under it ---------------------------------------------------------------
class Main;
class Deck : public juce::Component {
public:
    std::string id, cur; std::function<std::unique_ptr<juce::Component>(const std::string&)> make; std::function<void(Deck&, const std::string&)> onTab;
    bool floating = false, minimized = false; std::function<void()> onClose, onMinimize; std::function<void(juce::Point<int>)> onDrag;
    Deck(std::string i) : id(std::move(i)) {
        tabView.setViewedComponent(&tabRow, false); tabView.setScrollBarsShown(false, false, false, true); tabView.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::all); addAndMakeVisible(tabView);
        for (auto& v : views()) { auto* b = tabs.add(new Btn(v.label, Btn::Tab, v.icon)); b->setTooltip(v.label); const std::string vid = v.id; b->onClick = [this, vid] { if (onTab) onTab(*this, vid); }; tabRow.addAndMakeVisible(b); }
        addChildComponent(closeB); closeB.setTooltip("Close the floating deck"); closeB.onClick = [this] { if (onClose) onClose(); };
        // minimize sits at the far left, away from close at the far right, so a slip of the finger does not close the deck
        addChildComponent(minB); minB.setTooltip("Minimize the floating deck to its tab bar (tap again to restore)"); minB.onClick = [this] { if (onMinimize) onMinimize(); };
        tabRow.addMouseListener(this, false);
    }
    void place(const std::string& v) {
        cur = v; for (int i = 0; i < tabs.size(); i++) tabs[i]->setOn(views()[(size_t)i].id == v);
        content.reset(); if (make) content = make(v); if (content) addAndMakeVisible(*content); resized();
    }
    juce::Component* view() { return content.get(); }
    void showActiveTab() {   // a row wider than the deck: scroll the open view's tab into sight if it is out of it
        for (auto* b : tabs) if (b->on()) { const int vx = tabView.getViewPositionX(), vw = tabView.getViewWidth();
            if (b->getX() < vx || b->getRight() > vx + vw) tabView.setViewPosition(std::max(0, b->getX() + b->getWidth() / 2 - vw / 2), 0); }
    }
    template <class T> T* as() { return dynamic_cast<T*>(content.get()); }
    void resized() override {
        auto r = getLocalBounds(); auto t = r.removeFromTop(CTL() + 6);
        if (floating) { closeB.setVisible(true); closeB.setBounds(t.removeFromRight(32).reduced(3)); minB.setVisible(true); minB.setButtonText(minimized ? u8("▢") : u8("—")); minB.setBounds(t.removeFromLeft(32).reduced(3)); t.removeFromRight(10); }
        // narrow decks: tabs shrink to icons (the active one keeps its label), the row scrolls sideways
        const bool iconsOnly = getWidth() < 900 && !isTouch(), iconsAll = getWidth() < 560 && !isTouch();
        int x = 4; for (int i = 0; i < tabs.size(); i++) { auto* b = tabs[i]; b->showLabel = !iconsOnly || (b->on() && !iconsAll); const int w = b->idealWidth(); b->setBounds(x, 0, w, t.getHeight() - 8); x += w + 2; }
        tabRow.setSize(std::max(x + 4, t.getWidth()), t.getHeight() - 8); tabView.setBounds(t.reduced(0, 4)); showActiveTab();
        tabBar = t; if (content) { content->setVisible(!minimized); content->setBounds(r); }
    }
    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat(); juce::Path clip; clip.addRoundedRectangle(r, RAD()); g.saveState(); g.reduceClipRegion(clip);
        const float al = panelAlpha(); g.setColour(BG2.withAlpha(al)); g.fillRect(r); g.setColour(TABBG.withAlpha(al)); g.fillRect(tabBar.toFloat()); g.restoreState();
        if (floating && !minimized) { g.setColour(DIM); for (int i = 0; i < 3; i++) g.drawLine((float)getWidth() - 4 - i * 4, (float)getHeight() - 2, (float)getWidth() - 2, (float)getHeight() - 4 - i * 4, 1); }
    }
    void paintOverChildren(juce::Graphics& g) override { g.setColour(floating ? ACCENT : LINE); g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), RAD(), 1); }
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override {   // the wheel scrolls the tab row sideways
        if (e.eventComponent == &tabRow || e.eventComponent->getParentComponent() == &tabRow) { tabView.setViewPosition(tabView.getViewPositionX() - (int)(w.deltaY * 200), 0); return; }
        juce::Component::mouseWheelMove(e, w);
    }
    std::function<void(int, int)> onResize; bool sizing = false;
    void mouseDown(const juce::MouseEvent& e) override {
        sizing = floating && e.eventComponent == this && e.x > getWidth() - 18 && e.y > getHeight() - 18;
        if (!sizing && floating && (e.eventComponent == &tabRow || e.eventComponent == this) && e.getEventRelativeTo(this).y < tabBar.getBottom()) dragFrom = getPosition() - e.getScreenPosition(); else dragFrom.reset();
    }
    void mouseDrag(const juce::MouseEvent& e) override { if (sizing && onResize) { auto p = e.getEventRelativeTo(this).getPosition(); onResize(std::max(340, p.x + 4), std::max(220, p.y + 4)); return; } if (dragFrom && onDrag) onDrag(e.getScreenPosition() + *dragFrom); }
    void mouseMove(const juce::MouseEvent& e) override { setMouseCursor(floating && e.eventComponent == this && e.x > getWidth() - 18 && e.y > getHeight() - 18 ? juce::MouseCursor::BottomRightCornerResizeCursor : juce::MouseCursor::NormalCursor); }
private:
    juce::OwnedArray<Btn> tabs; juce::Viewport tabView; juce::Component tabRow; std::unique_ptr<juce::Component> content; juce::Rectangle<int> tabBar; Btn closeB{ u8("✕"), Btn::Tb }, minB{ u8("—"), Btn::Tb };
    std::optional<juce::Point<int>> dragFrom;
};

// --- the on-screen keyboard: docked under the decks (drag its top edge) or a floating panel ------------------------------
class Keys : public juce::Component {
public:
    std::function<void(int, bool, double)> play; std::function<void()> changed; std::function<void(juce::Point<int>)> onDrag;
    int octave = 3; bool floating = false; std::set<int> down; std::map<int, int> held;   // finger / mouse -> note
    Keys() { for (auto* b : { &upB, &dnB, &floatB }) { addAndMakeVisible(b); b->sm = true; } addAndMakeVisible(octL);
        upB.setTooltip("Octave up"); dnB.setTooltip("Octave down"); floatB.setTooltip("Dock the keyboard / make it a floating, resizable panel");
        upB.onClick = [this] { octave = std::min(7, octave + 1); if (changed) changed(); repaint(); }; dnB.onClick = [this] { octave = std::max(0, octave - 1); if (changed) changed(); repaint(); };
        floatB.onClick = [this] { floating = !floating; if (changed) changed(); }; octL.setJustificationType(juce::Justification::centred); }
    void resized() override { const int sw = 34; auto s = getLocalBounds().removeFromLeft(sw).reduced(0, 4); upB.setBounds(s.removeFromTop(24)); floatB.setBounds(s.removeFromBottom(24)); s.removeFromBottom(4); dnB.setBounds(s.removeFromBottom(24)); octL.setBounds(s); }
    juce::Rectangle<int> box() const { return getLocalBounds().withTrimmedLeft(40).withTrimmedTop(floating ? 6 : 6).withTrimmedBottom(floating ? 6 : 0).withTrimmedRight(floating ? 6 : 0); }
    void paint(juce::Graphics& g) override {
        octL.setText("C" + juce::String(octave), juce::dontSendNotification); floatB.setButtonText(floating ? u8("⇲") : u8("⇱"));
        if (!floating) { g.setColour(LINE); g.fillRoundedRectangle((float)getWidth() * 0.46f, 1, (float)getWidth() * 0.08f, 3, 2); }   // the grip
        if (floating) { g.setColour(BG2); g.fillRoundedRectangle(getLocalBounds().toFloat(), RAD()); g.setColour(LINE); g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), RAD(), 1); g.fillRect(37, 6, 1, getHeight() - 12); }
        auto b = box(); juce::Path clip; clip.addRoundedRectangle(b.toFloat(), RAD()); g.saveState(); g.reduceClipRegion(clip);
        layoutKeys(); for (auto& k : keys) if (!k.black) { g.setColour(down.count(k.n) ? ACCENT : juce::Colour(0xffece8e2)); g.fillRoundedRectangle(k.r.withTrimmedTop(-6), 6);
            if (k.n % 12 == 0) { g.setColour(juce::Colour(0xff77706a)); g.setFont(font(FS * 0.7f)); g.drawText("C" + juce::String(k.n / 12 - 1), k.r.withTrimmedBottom(4), juce::Justification::centredBottom); } }
        for (auto& k : keys) if (k.black) { g.setColour(down.count(k.n) ? ACCENT : juce::Colour(0xff221f28)); g.fillRoundedRectangle(k.r.withTrimmedTop(-5), 5); }
        g.restoreState();
    }
    int noteAt(juce::Point<float> p, double& vel) { layoutKeys(); for (auto& k : keys) if (k.black && k.r.contains(p)) { vel = juce::jlimit(0.2, 1.0, 0.35 + 0.65 * (p.y - k.r.getY()) / k.r.getHeight()); return k.n; }
        for (auto& k : keys) if (!k.black && k.r.contains(p)) { vel = juce::jlimit(0.2, 1.0, 0.35 + 0.65 * (p.y - k.r.getY()) / k.r.getHeight()); return k.n; } return -1; }
    std::function<void(int, int, int)> onResize; int sizing = 0;   // 1 docked height, 2 floating corner
    void mouseMove(const juce::MouseEvent& e) override { setMouseCursor(!floating && e.y < 6 ? juce::MouseCursor::UpDownResizeCursor : floating && e.x > getWidth() - 16 && e.y > getHeight() - 16 ? juce::MouseCursor::BottomRightCornerResizeCursor : juce::MouseCursor::NormalCursor); }
    void mouseDown(const juce::MouseEvent& e) override {
        sizing = !floating && e.y < 6 ? 1 : floating && e.x > getWidth() - 16 && e.y > getHeight() - 16 ? 2 : 0; if (sizing) { h0 = getHeight(); y0 = e.getScreenY(); return; }
        if (e.x < 38) { if (floating) dragFrom = getPosition() - e.getScreenPosition(); return; }
        double v; const int n = noteAt(e.position, v); if (n < 0) return; held[e.source.getIndex()] = n; down.insert(n); if (play) play(n, true, v); repaint();
    }
    int h0 = 0, y0 = 0;
    void mouseDrag(const juce::MouseEvent& e) override {
        if (sizing == 1) { if (onResize) onResize(1, 0, h0 - (e.getScreenY() - y0)); return; }
        if (sizing == 2) { if (onResize) onResize(2, std::max(260, e.x + 4), std::max(80, e.y + 4)); return; }
        if (dragFrom) { if (onDrag) onDrag(e.getScreenPosition() + *dragFrom); return; }
        auto it = held.find(e.source.getIndex()); if (it == held.end()) return; double v; const int n = noteAt(e.position, v);
        if (n >= 0 && n != it->second) { if (play) play(it->second, false, 0); down.erase(it->second); it->second = n; down.insert(n); if (play) play(n, true, v); repaint(); }
    }
    void mouseUp(const juce::MouseEvent& e) override { dragFrom.reset(); auto it = held.find(e.source.getIndex()); if (it == held.end()) return; if (play) play(it->second, false, 0); down.erase(it->second); held.erase(it); repaint(); }
    void light(int n, bool on) { if (on) down.insert(n); else down.erase(n); repaint(); }
private:
    Btn upB{ "+" }, dnB{ u8("−") }, floatB{ u8("⇱") }; Cap octL{ "C3", FS * 0.88f }; std::optional<juce::Point<int>> dragFrom;
    struct K { int n; bool black; juce::Rectangle<float> r; }; std::vector<K> keys;
    void layoutKeys() {
        keys.clear(); auto b = box().toFloat(); const float wk = juce::jlimit(16.f, 64.f, b.getHeight() * 0.3f); const int octs = juce::jlimit(1, 8, (int)(b.getWidth() / (wk * 7))), base = (octave + 1) * 12;
        int whites = 0; for (int n = base; n <= base + octs * 12; n++) if (!juce::MidiMessage::isMidiNoteBlack(n)) whites++;
        const float w = b.getWidth() / whites; float x = b.getX();
        for (int n = base; n <= base + octs * 12; n++) { if (juce::MidiMessage::isMidiNoteBlack(n)) continue; keys.push_back({ n, false, { x + 1, b.getY(), w - 2, b.getHeight() } });
            if (n < base + octs * 12 && juce::MidiMessage::isMidiNoteBlack(n + 1)) keys.push_back({ n + 1, true, { x + w * 0.68f, b.getY(), w * 0.62f, b.getHeight() * 0.6f } }); x += w; }
    }
};

// --- the toast: a pill at the bottom of the window --------------------------------------------------------------------------
class Toast : public juce::Component, private juce::Timer {
public:
    Toast() { setInterceptsMouseClicks(false, false); setAlwaysOnTop(true); }
    void show(const juce::String& m, int ms = 2600) { msg = m; auto* p = getParentComponent(); if (!p) return; const int w = std::min(p->getWidth() - 32, textW(font(FS), msg) + 34); setBounds((p->getWidth() - w) / 2, p->getHeight() - 26 - 38, w, 38); setVisible(true); toFront(false); repaint(); startTimer(ms); }
    void paint(juce::Graphics& g) override { g.setColour(TEXT); g.fillRoundedRectangle(getLocalBounds().toFloat(), getHeight() / 2.f); g.setColour(BG); g.setFont(font(FS, juce::Font::plain)); g.drawFittedText(msg, getLocalBounds().reduced(16, 0), juce::Justification::centred, 2); }
private:
    juce::String msg; void timerCallback() override { stopTimer(); setVisible(false); }
};

// --- help (F1): the shortcut list ------------------------------------------------------------------------------------------
class HelpPanel : public juce::Component {
public:
    HelpPanel() { addAndMakeVisible(view); view.setViewedComponent(&inner, false); view.setScrollBarsShown(true, false); setSize(560, 560); }
    void resized() override { view.setBounds(getLocalBounds().reduced(8)); int y = 4; const int w = view.getWidth() - 14; inner.rows.clear();
        for (auto& [k, t] : scopes()) { inner.rows.push_back({ true, t, {} }); y += 30; for (auto& a : actions()) if (juce::String(a.scope) == k) { inner.rows.push_back({ false, keyOf(a.id), a.label }); y += 22; } }
        inner.rows.push_back({ true, "Mouse and touch", {} }); y += 30; for (auto& [m, t] : mouseHelp()) { inner.rows.push_back({ false, m, t }); y += 22; }
        inner.rows.push_back({ false, {}, "Change any of the keys in Settings > Hotkeys. Single-letter keys play notes while the typing keyboard is on; click the Song, Piano Roll, Rack or Mixer first to use their letters." }); y += 50;
        inner.setSize(w, y + 8); }
    void paint(juce::Graphics& g) override { g.fillAll(BG2); }
private:
    struct Inner : juce::Component { struct R { bool head; juce::String a, b; }; std::vector<R> rows;
        void paint(juce::Graphics& g) override { int y = 4; for (auto& r : rows) { if (r.head) { y += 10; g.setColour(TEXT); g.setFont(fontB(FS)); g.drawText(r.a, 4, y, getWidth(), 20, juce::Justification::centredLeft); y += 20; continue; }
            if (r.a.isEmpty()) { g.setColour(DIM); g.setFont(font(FS * 0.88f)); g.drawFittedText(r.b, 4, y + 6, getWidth() - 8, 40, juce::Justification::topLeft, 3); y += 50; continue; }
            g.setColour(ACCENT2); g.setFont(mono(FS * 0.82f)); g.drawText(r.a, 4, y, 150, 22, juce::Justification::centredLeft); g.setColour(DIM); g.setFont(font(FS * 0.9f)); g.drawText(r.b, 160, y, getWidth() - 164, 22, juce::Justification::centredLeft, true); y += 22; } } };
    juce::Viewport view; Inner inner;
};

// --- the record dialog: what to capture --------------------------------------------------------------------------------------
class RecordPanel : public juce::Component {
public:
    std::function<void()> onArm; std::function<void()> onCancel;
    RecordPanel(Doc& d) : doc(d) {
        auto add = [this](juce::ToggleButton& t, const char* key, bool def, const char* label) { t.setButtonText(label); t.setToggleState(doc.pb(key, def), juce::dontSendNotification); addAndMakeVisible(t); };
        add(midi, "recMidi", true, "Notes (MIDI)"); add(audio, "recAudio", true, "Microphone / line audio"); add(autoB, "recAuto", true, "Automation"); add(loop, "loopRec", true, "Loop record");
        for (auto* b : { &cancelB, &armB }) addAndMakeVisible(b); armB.primary = true;
        cancelB.onClick = [this] { if (onCancel) onCancel(); };
        armB.onClick = [this] { if (!midi.getToggleState() && !audio.getToggleState() && !autoB.getToggleState()) { toast("Tick at least one thing to record"); return; }
            doc.setp("recMidi", midi.getToggleState()); doc.setp("recAudio", audio.getToggleState()); doc.setp("recAuto", autoB.getToggleState()); doc.setp("loopRec", loop.getToggleState()); if (onArm) onArm(); };
        setSize(460, 330);
    }
    void resized() override {
        int y = 46; for (auto* t : { &midi, &audio, &autoB }) { t->setBounds(16, y, getWidth() - 32, 22); y += 64; } loop.setBounds(16, y + 6, getWidth() - 32, 22);
        armB.setBounds(getWidth() - 16 - 130, getHeight() - 16 - CTL(), 130, CTL()); cancelB.setBounds(armB.getX() - 8 - 90, armB.getY(), 90, CTL());
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(BG2); g.setColour(TEXT); g.setFont(fontB(FS * 1.1f)); g.drawText("Record", 16, 12, 200, 24, juce::Justification::centredLeft);
        g.setColour(DIM); g.setFont(font(FS * 0.88f)); const char* hints[] = { "What you play on a MIDI keyboard, the on-screen keys or the typing keys, into the selected channel. PAT mode.",
            "Every mixer bus with an input chosen and its red dot armed, onto the song timeline. SONG mode.", "Fader, pan and knob moves in the mixer and rack while the song plays. SONG mode." };
        int y = 70; for (auto* h : hints) { g.drawFittedText(h, 42, y, getWidth() - 58, 34, juce::Justification::topLeft, 2); y += 64; }
        g.setColour(LINE); g.fillRect(16, loop.getY() - 6, getWidth() - 32, 1);
        g.setColour(DIM); g.drawFittedText("Keep cycling and layer each pass (audio passes become stacked takes). Off: record one pass, then stop.", 42, loop.getBottom() + 2, getWidth() - 58, 34, juce::Justification::topLeft, 2);
    }
private:
    Doc& doc; juce::ToggleButton midi, audio, autoB, loop; Btn cancelB{ "Cancel" }, armB{ "Arm recording" };
};

// --- the window's content ---------------------------------------------------------------------------------------------------
class Main : public juce::Component, public juce::AudioIODeviceCallback, public juce::FileDragAndDropTarget, public juce::DragAndDropContainer,
             public juce::MidiInputCallback, private juce::Timer, private juce::ChangeListener {
public:
    Main() : bank(std::make_shared<us::SampleBank>()), engine(bank), doc(engine, *bank), ctx(makeCtx()) {
        doc.props = props.get(); applyPrefs(); setLookAndFeel(&look); juce::LookAndFeel::setDefaultLookAndFeel(&look);
        // keymap, prefs
        keymap() = juce::JSON::parse(props->getValue("keymap", "{}")); if (!keymap().isObject()) keymap() = juce::var(new juce::DynamicObject());
        saveKeymap() = [this] { props->setValue("keymap", juce::JSON::toString(keymap(), true)); props->saveIfNeeded(); };
        doc.prefsChanged = [this] { applyPrefs(); look.refresh(); sendLookAndFeelChange(); layoutDecks(); resized(); repaint(); };
        doc.gotoView = [this](const std::string& to, const std::string& from) { gotoView(to, from); };
        knobHooks() = [this](Menu& m, const std::string& t, double v, std::function<void(double)>) { linkItems(m, t, v); };
        toastSink() = [this](const juce::String& s) { toastC.show(s); };
        SongView::renderArmed() = [this] { renderArmed(); };
        buildTopBar();
        for (auto& id : { "A", "B", "C", "D", "F" }) { auto* d = decks.add(new Deck(id)); d->make = [this](const std::string& v) { return makeView(v); }; d->onTab = [this](Deck& dk, const std::string& v) { show(dk, v); }; addChildComponent(d); }
        auto* F = decks[4]; F->floating = true; F->onClose = [this] { props->setValue("float", false); layoutDecks(); };
        F->minimized = props->getBoolValue("floatMin", false); F->onMinimize = [this, F] { F->minimized = !F->minimized; props->setValue("floatMin", F->minimized); layoutDecks(); F->resized(); };
        F->onResize = [this](int w, int h) { props->setValue("floatW", w); props->setValue("floatH", h); layoutDecks(); };
        F->onDrag = [this, F](juce::Point<int> screen) { auto p = getLocalPoint(nullptr, screen); props->setValue("floatX", juce::jlimit(0, getWidth() - 120, p.x)); props->setValue("floatY", juce::jlimit(0, getHeight() - 60, p.y)); F->setTopLeftPosition(props->getIntValue("floatX"), props->getIntValue("floatY")); };
        addAndMakeVisible(splitter); splitter.o = this;
        addChildComponent(keysC); keysC.octave = props->getIntValue("octave", 3); keysC.floating = props->getBoolValue("keysFloat", false);
        keysC.play = [this](int n, bool on, double v) { liveNote(n, on, v); };
        keysC.changed = [this] { props->setValue("octave", keysC.octave); props->setValue("keysFloat", keysC.floating); resized(); };
        keysC.onResize = [this](int kind, int w, int h) { if (kind == 1) props->setValue("keysH", juce::jlimit(60, getHeight() / 2, h)); else { props->setValue("keysW", w); props->setValue("keysFH", h); } layoutDecks(); };
        keysC.onDrag = [this](juce::Point<int> s) { auto p = getLocalPoint(nullptr, s); props->setValue("keysX", p.x); props->setValue("keysY", p.y); resized(); };
        addChildComponent(toastC);
        doc.addChangeListener(this);
        doc.setProject(blankProject());
        startup();
#if JUCE_ANDROID
        juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio, [this](bool granted) { devices.initialise(granted ? 2 : 0, 2, nullptr, true); });
#else
        { std::unique_ptr<juce::XmlElement> st(props->getXmlValue("audio")); devices.initialise(8, 2, st.get(), true); }   // the last device setup, or the default
#endif
        devices.addChangeListener(this); devices.addAudioCallback(this);
        for (auto& m : juce::MidiInput::getAvailableDevices()) if (!props->getBoolValue("midiOff:" + m.identifier, false)) devices.setMidiInputDeviceEnabled(m.identifier, true);
        devices.addMidiInputDeviceCallback({}, this);
        recFifoBuf.setSize(8, 48000 * 8);
        us::HostSet::get().start(us::HostSet::findBinDir(), !props->getBoolValue("threads", true));   // Settings › Audio › Multi-threaded processing
        if (props->getBoolValue("plugServerOn", false)) {
            const auto a = props->getValue("plugServer").toStdString(), c = props->getValue("plugCode").toStdString();
            std::thread([this, a, c, sp = juce::Component::SafePointer<Main>(this)] { const auto err = us::HostSet::get().useServer(a, c); juce::MessageManager::callAsync([sp, err] { if (sp && err.empty()) sp->engine.reloadPlugins(); }); }).detach();
        }
        engine.metronome = false;
        backupDir().createDirectory();
        layoutDecks();
        setWantsKeyboardFocus(true); setSize(1360, 860);
        startTimerHz(30);
#if JUCE_ANDROID
        gl.attachTo(*this);   // Android redraws the whole view in software for any change (about 0.2 s a frame at phone density); the GPU renderer repaints only what changed
#endif
    }
    ~Main() override {
#if JUCE_ANDROID
        gl.detach();
#endif
        stopTimer(); us::HostSet::get().stop(); devices.removeChangeListener(this); devices.removeMidiInputDeviceCallback({}, this); devices.removeAudioCallback(this); doc.removeChangeListener(this);
        knobHooks() = nullptr; toastSink() = nullptr; SongView::renderArmed() = nullptr; decks.clear(); juce::LookAndFeel::setDefaultLookAndFeel(nullptr); setLookAndFeel(nullptr); }

    void open(const juce::File& f) {
        try { doc.load(f); doc.song = doc.p.songBars() > 0; doc.sendChangeMessage(); toast("Opened " + juce::String(doc.p.name)); }
        catch (const std::exception& e) { message("Could not open", e.what()); }
    }
    bool okToQuit() {
        if (!doc.dirty) return true;
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, "Unsaved changes", "Save before quitting?", "Save", "Quit without saving", nullptr,
            juce::ModalCallbackFunction::create([this](int r) { if (r) { if (save(false)) juce::JUCEApplication::quit(); } else juce::JUCEApplication::quit(); }));
        return false;
    }

    // --- audio ---
    void audioDeviceIOCallbackWithContext(const float* const* in, int nIn, float* const* out, int nOut, int n, const juce::AudioIODeviceCallbackContext&) override {
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        engine.setInput(in, nIn, n);
        if (recAudioNow && nIn > 0 && engine.isPlaying() && !engine.inCountIn()) {   // audio recording: every input channel into a FIFO the UI thread empties
            int s1, n1, s2, n2; recFifo.prepareToWrite(n, s1, n1, s2, n2);
            for (int c = 0; c < std::min(nIn, 8); c++) { if (!in[c]) continue; if (n1) recFifoBuf.copyFrom(c, s1, in[c], n1); if (n2) recFifoBuf.copyFrom(c, s2, in[c] + n1, n2); }
            recFifo.finishedWrite(n1 + n2); recIns = std::min(nIn, 8);
        }
        if (nOut >= 2) engine.process(out[0], out[1], n); else if (nOut == 1) { tmp.resize((size_t)n); engine.process(out[0], tmp.data(), n); }
        callbacks++; const double took = juce::Time::getMillisecondCounterHiRes() - t0; if (took > worstMs) worstMs = took; totalMs = totalMs + took; lastLoad = (float)(took / (n * 1000.0 / sr));
        if (nOut >= 2) for (int i = 0; i < n; i++) maxPeak = std::max({ maxPeak.load(), std::abs(out[0][i]), std::abs(out[1][i]) });
    }
    void audioDeviceAboutToStart(juce::AudioIODevice* d) override { sr = d->getCurrentSampleRate(); engine.prepare(d->getCurrentSampleRate(), d->getCurrentBufferSizeSamples()); tmp.resize(8192); }
    void audioDeviceStopped() override {}
    // test hooks
    void startPlaying() { togglePlay(); }
    void armAudioRecording() { doc.rec = true; props->setValue("recAudio", true); for (auto& b : doc.p.buses) if (b.kind != "master") { b.arm = true; if (b.input.empty()) b.input = "1"; break; } doc.song = true; recB.setOn(true); }
    void stopAndReport() {
        if (engine.isPlaying()) stopTransport();
        int takes = 0; double secs = 0; for (auto& a : doc.p.audio) if (a.name.rfind("Take", 0) == 0) { takes++; if (auto s = bank->get(a.sample)) secs = s->seconds(); }
        auto* d = devices.getCurrentAudioDevice();
        std::cout << "input channels: " << (d ? d->getActiveInputChannels().countNumberOfSetBits() : 0) << ", recorded takes on the playlist: " << takes << ", last take " << secs << " s\n";
    }
    Doc& document() { return doc; }
    // the cost of one whole-window repaint at phone density (Android redraws the whole view for any change)
    void benchPaint() {
        const float sc = (float)juce::SystemStats::getEnvironmentVariable("BENCH_SCALE", "2.625").getDoubleValue(); juce::Image img(juce::Image::ARGB, juce::roundToInt(getWidth() * sc), juce::roundToInt(getHeight() * sc), true, juce::SoftwareImageType()); const bool was = style().glow;
        for (bool glow : { true, false }) { style().glow = glow; double total = 0;
            for (int i = 0; i < 12; i++) { juce::Graphics g(img); g.addTransform(juce::AffineTransform::scale(sc)); const double a = juce::Time::getMillisecondCounterHiRes(); paintEntireComponent(g, true); if (i >= 2) total += juce::Time::getMillisecondCounterHiRes() - a; }
            std::cout << "whole repaint " << img.getWidth() << "x" << img.getHeight() << (glow ? " with glow: " : " without glow: ") << total / 10 << " ms" << std::endl; }
        style().glow = was;
    }
    void showViews(int a, int b) { if (a >= 0 && a < 9) show(*decks[0], views()[(size_t)a].id); if (b >= 0 && b < 9) show(*decks[1], views()[(size_t)b].id); }
    void report() {
        auto* d = devices.getCurrentAudioDevice();
        std::cout << "device: " << (d ? (d->getTypeName() + " / " + d->getName() + " @ " + juce::String(d->getCurrentSampleRate()) + " Hz, buffer " + juce::String(d->getCurrentBufferSizeSamples())).toStdString() : std::string("none"))
                  << "\ncallbacks: " << callbacks.load() << ", peak " << juce::Decibels::gainToDecibels(maxPeak.load()) << " dBFS, slowest callback " << worstMs.load() << " ms, average " << totalMs.load() / std::max(1, callbacks.load()) << " ms (budget "
                  << (d ? d->getCurrentBufferSizeSamples() * 1000.0 / d->getCurrentSampleRate() : 0) << " ms), xruns " << (d ? d->getXRunCount() : -1) << ", position " << engine.positionTicks() << " ticks\n";
    }

    bool isInterestedInFileDrag(const juce::StringArray&) override { return true; }
    void filesDropped(const juce::StringArray& f, int x, int y) override {
        if (f.size() == 1 && f[0].endsWithIgnoreCase(".json")) { open(juce::File(f[0])); return; }
        if (f.size() == 1 && (f[0].endsWithIgnoreCase(".mid") || f[0].endsWithIgnoreCase(".midi"))) { importMidi(juce::File(f[0])); return; }
        // onto the song: audio clips there; elsewhere a sampler channel each
        int bad = 0; std::vector<std::string> ids; for (auto& p : f) { auto id = doc.importAudio(juce::File(p)); if (id.empty()) bad++; else ids.push_back(id); }
        auto* target = getComponentAt(x, y); bool onSong = false; for (auto* c = target; c; c = c->getParentComponent()) if (dynamic_cast<SongView*>(c)) onSong = true;
        if (onSong) { double at = doc.songStart; for (auto& id : ids) { doc.placeAudio(id, at); if (auto s = bank->get(id)) at += s->seconds() / (doc.p.bar() * doc.p.secondsPerTick()); } }
        else for (auto& id : ids) doc.addSampleChannel(id);
        if (bad) message("Import", juce::String(bad) + " file(s) could not be read as audio.");
    }
    // MIDI input (MIDI thread): controller links first, then notes for the current channel
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& m) override {
        if (m.isController() && m.getControllerNumber() != 64 && m.getControllerNumber() != 1) { const int ch = m.getChannel() - 1, cc = m.getControllerNumber(), v = m.getControllerValue();
            juce::MessageManager::callAsync([sp = juce::Component::SafePointer<Main>(this), ch, cc, v] { if (sp) sp->onControl(ch, cc, v); }); }
        if (m.isController() && m.getControllerNumber() == 1) { const int ch = m.getChannel() - 1, v = m.getControllerValue(); juce::MessageManager::callAsync([sp = juce::Component::SafePointer<Main>(this), ch, v] { if (sp && !sp->onControl(ch, 1, v)) sp->engine.modWheel(v / 127.0); }); return; }
        const int midiCh = props->getIntValue("midiCh", 0); if (midiCh > 0 && m.getChannel() != midiCh) return;
        if (m.isNoteOn()) { const int n = m.getNoteNumber(); const double v = velCurve(m.getFloatVelocity()); juce::MessageManager::callAsync([sp = juce::Component::SafePointer<Main>(this), n, v] { if (sp) sp->liveNote(n, true, v); }); }
        else if (m.isNoteOff()) { const int n = m.getNoteNumber(); juce::MessageManager::callAsync([sp = juce::Component::SafePointer<Main>(this), n] { if (sp) sp->liveNote(n, false, 0); }); }
        else if (m.isPitchWheel()) engine.pitchBend((m.getPitchWheelValue() - 8192) / 8192.0);
        else if (m.isSustainPedalOn()) { if (props->getBoolValue("pedal", true)) engine.sustain(true); } else if (m.isSustainPedalOff()) engine.sustain(false);
        else if (m.isAllNotesOff() || m.isAllSoundOff()) engine.sustain(false);
        midiAct = true;
    }
    double velCurve(double v) const { const auto c = props->getValue("velCurve", "linear"); return c == "soft" ? std::sqrt(v) : c == "hard" ? v * v : c == "fixed" ? 0.8 : v; }
    // a played note (MIDI, typing keys, on-screen keys): the current channel; starts a waiting recording
    void liveNote(int n, bool on, double v) {
        if (on && waiting) { waiting = false; startTransport(true); }
        doc.audition(n, on, v); keysC.light(n, on); if (auto* r = deckWith<RollView>()) r->keyDown(n, on);
    }

    // the computer keyboard as a piano: Z-M (and S D G H J) one octave, Q-P (and 2 3 5 6 7 9 0) the next, , L . above
    static int keyNote(int code) {
        static const std::map<int, int> m = { { 'Z', 0 }, { 'S', 1 }, { 'X', 2 }, { 'D', 3 }, { 'C', 4 }, { 'V', 5 }, { 'G', 6 }, { 'B', 7 }, { 'H', 8 }, { 'N', 9 }, { 'J', 10 }, { 'M', 11 }, { ',', 12 }, { 'L', 13 }, { '.', 14 },
            { 'Q', 12 }, { '2', 13 }, { 'W', 14 }, { '3', 15 }, { 'E', 16 }, { 'R', 17 }, { '5', 18 }, { 'T', 19 }, { '6', 20 }, { 'Y', 21 }, { '7', 22 }, { 'U', 23 }, { 'I', 24 }, { '9', 25 }, { 'O', 26 }, { '0', 27 }, { 'P', 28 } };
        auto it = m.find(juce::CharacterFunctions::toUpperCase((juce::juce_wchar)code)); return it == m.end() ? -1 : it->second;
    }
    bool keyStateChanged(bool down) override {
        if (down) return false;
        for (auto it = typed.begin(); it != typed.end();) { if (!juce::KeyPress::isKeyCurrentlyDown(it->first)) { liveNote(it->second, false, 0); it = typed.erase(it); } else ++it; }
        return false;
    }
    bool keyPressed(const juce::KeyPress& k) override {
        if (dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent())) return false;
        const auto m = k.getModifiers(); const bool mod = m.isCtrlDown() || m.isCommandDown() || m.isAltDown(); const bool typing = props->getBoolValue("typing", true);
        juce::String id = actionFor(k, "global");
        const int kn = keyNote(k.getKeyCode());
        if (id.isNotEmpty() && !mod && !m.isShiftDown() && typing && kn >= 0 && k.getKeyCode() < 128 && k.getKeyCode() != ' ') id = {};   // a letter that plays a note while typing is on
        if (id.isNotEmpty()) { runAction(id); return true; }
        if (!mod && typing && kn >= 0) { if (!typed.count(k.getKeyCode())) { const int note = juce::jlimit(0, 127, (keysC.octave + 1) * 12 + kn); typed[k.getKeyCode()] = note; liveNote(note, true, 0.8); } return true; }
        return false;
    }
    void runAction(const juce::String& id) {
        if (id.startsWith("v.")) { gotoView(id.substring(2).toStdString(), {}); return; }
        if (id == "play") togglePlay(); else if (id == "pause") pauseToggle(); else if (id == "mode") setSong(!doc.song);
        else if (id == "rec") recQuick(); else if (id == "metro") { engine.metronome = !engine.metronome.load(); metB.setOn(engine.metronome); }
        else if (id == "countIn") { const bool v = !props->getBoolValue("countIn", false); props->setValue("countIn", v); toast(v ? "Count-in on: one bar of clicks before recording" : "Count-in off"); }
        else if (id == "waitInput") { const bool v = !props->getBoolValue("waitInput", false); props->setValue("waitInput", v); toast(v ? "Recording waits for your first note" : "Recording starts when you press play"); }
        else if (id == "panic") { if (engine.isPlaying()) stopTransport(); else engine.panic(); toast("All sound stopped"); }
        else if (id == "save") save(false); else if (id == "saveAs") saveAs(); else if (id == "saveNew") saveNew(); else if (id == "open") openDialog();
        else if (id == "exportWav") exportAudio(doc.song, "wav"); else if (id == "exportComp") exportAudio(doc.song, "mp3"); else if (id == "exportMidi") exportMidi();
        else if (id == "undo") { if (undoMark == histMark) { doc.redo(); undoMark = -1; } else { doc.undo(); undoMark = ++histMark; } }
        else if (id == "undoStep") { doc.undo(); undoMark = -1; } else if (id == "redo" || id == "redo2") doc.redo();
        else if (id == "time") { props->setValue("timeFmt", props->getValue("timeFmt", "bst") == "msc" ? "bst" : "msc"); }
        else if (id == "keys") { props->setValue("keys", !props->getBoolValue("keys", false)); keysB.setOn(props->getBoolValue("keys", false)); resized(); }
        else if (id == "prevPat" || id == "nextPat") { auto& P = doc.p.patterns; for (size_t i = 0; i < P.size(); i++) if (P[i].id == doc.pat) { const int j = (int)i + (id == "nextPat" ? 1 : -1); if (j >= 0 && j < (int)P.size()) doc.selectPattern(P[(size_t)j].id); break; } }
        else if (id == "picker") choosePlugin(&brandB, true, [this](const juce::var& slot) { auto c = newChannel(doc.p, "plugin", slot.getProperty("name", "Plug-in").toString().substring(0, 18).toStdString()); c.plug = slot; doc.p.channels.push_back(c); doc.ch = c.id; doc.commit(); toast(slot.getProperty("name", "").toString() + " added to the rack"); });
        else if (id == "midiSet") { SettingsView::startTab() = "midi"; gotoView("set", {}); } else if (id == "songInfo") { SettingsView::startTab() = "project"; gotoView("set", {}); }
        else if (id == "manual") showHelp();
        else if (id == "closeAll") { juce::PopupMenu::dismissAllActiveMenus(); props->setValue("float", false); keysC.floating = false; props->setValue("keysFloat", false); layoutDecks(); resized(); }
        else if (id == "closeWin") { learning.clear(); if (juce::PopupMenu::dismissAllActiveMenus()) return; if (props->getBoolValue("float", false)) { props->setValue("float", false); layoutDecks(); } }
    }

    juce::Image bgImg; juce::String bgPath;
    void paint(juce::Graphics& g) override {
        g.fillAll(BG);
        const auto bp = props->getValue("bgImage"); if (bp != bgPath) { bgPath = bp; bgImg = bp.isNotEmpty() ? juce::ImageFileFormat::loadFrom(juce::File(bp)) : juce::Image(); if (bgImg.isValid() && std::max(bgImg.getWidth(), bgImg.getHeight()) > 1920) bgImg = bgImg.rescaled(bgImg.getWidth() * 1920 / std::max(bgImg.getWidth(), bgImg.getHeight()), bgImg.getHeight() * 1920 / std::max(bgImg.getWidth(), bgImg.getHeight())); }
        style().bgOn = bgImg.isValid();
        if (bgImg.isValid()) { g.drawImage(bgImg, getLocalBounds().toFloat(), juce::RectanglePlacement::fillDestination); g.setColour(BG.withAlpha((float)props->getDoubleValue("bgDim", 0.6))); g.fillAll(); }
        else if (style().glow) {   // the soft accent glows behind the panels
            juce::ColourGradient a(ACCENT.withAlpha(0.13f), getWidth() * 0.12f, -getHeight() * 0.1f, ACCENT.withAlpha(0.f), getWidth() * 0.12f + 1200 * 0.7f, -getHeight() * 0.1f, true); g.setGradientFill(a); g.fillAll();
            juce::ColourGradient b(ACCENT2.withAlpha(0.09f), (float)getWidth(), getHeight() * 1.1f, ACCENT2.withAlpha(0.f), getWidth() - 900 * 0.7f, getHeight() * 1.1f, true); g.setGradientFill(b); g.fillAll();
        }
        for (auto& r : grpR) paintGrp(g, r);
        if (brandB.isMouseOver()) { g.setColour(BG3); g.fillRoundedRectangle(brandB.getBounds().toFloat(), RAD() * 0.7f); }
        // brand: wordmark, project name, ▾
        if (logo.isValid()) { const auto b = brandB.getBounds(); const float h = std::min((float)b.getHeight(), CTL() * (narrow ? 1.35f : 1.75f)); const float w = logo.getWidth() * h / logo.getHeight(); g.drawImage(logo, juce::Rectangle<float>((float)b.getX() + 4, b.getCentreY() - h / 2, w, h), juce::RectanglePlacement::centred); }
        { auto b = brandB.getBounds(); g.setColour(DIM); g.setFont(font(FS)); const int lw = logoW(); auto t = b.withTrimmedLeft(lw + 10); if (t.getWidth() > 30 && getWidth() >= 1240) { g.drawText(juce::String(doc.p.name), t.withTrimmedRight(18), juce::Justification::centredLeft, true); }
          if (getWidth() >= 960) g.drawText(u8("▾"), t.removeFromRight(18), juce::Justification::centred); }
        // position (.pos), master meter (.mmeter), load (.stats)
        { auto r = posB.getBounds().toFloat(); g.setColour(BG); g.fillRoundedRectangle(r, RAD() * 0.6f); g.setColour(LINE); g.drawRoundedRectangle(r.reduced(0.5f), RAD() * 0.6f, 1);
          g.setColour(ACCENT2); g.setFont(mono(FS * 1.02f)); g.drawText(posText, r.withTrimmedBottom(r.getHeight() * 0.36f).toNearestInt(), juce::Justification::centredBottom);
          g.setColour(DIM); auto f = mono(FS * 0.58f, false); f.setExtraKerningFactor(0.12f); g.setFont(f); g.drawText(props->getValue("timeFmt", "bst") == "msc" ? "M:S:CS" : "B:S:T", r.withTrimmedTop(r.getHeight() * 0.62f).toNearestInt(), juce::Justification::centredTop); }
        if (meterR.getWidth() > 0) {
            auto bar = meterR.withHeight(9).withY(meterR.getCentreY() - 10).toFloat(); g.setColour(BG); g.fillRoundedRectangle(bar, 4); g.setColour(LINE); g.drawRoundedRectangle(bar.reduced(0.5f), 4, 1);
            const float k = meterPos(mLv); g.saveState(); g.reduceClipRegion(bar.withWidth(bar.getWidth() * k).toNearestInt()); juce::Path cl; cl.addRoundedRectangle(bar, 4); g.reduceClipRegion(cl);
            if (mLv >= 1) { g.setColour(BAD); g.fillRect(bar); } else { g.setColour(GOOD); g.fillRect(bar.withWidth(bar.getWidth() * 0.8f)); g.setColour(WARN); g.fillRect(bar.withX(bar.getX() + bar.getWidth() * 0.8f).withWidth(bar.getWidth() * 0.1f)); g.setColour(BAD); g.fillRect(bar.withX(bar.getX() + bar.getWidth() * 0.9f).withWidth(bar.getWidth() * 0.1f)); }
            g.restoreState();
            const juce::String db = mHold <= 0.001f ? u8("-∞ dB") : (mHold >= 1 ? "+" : "") + juce::String(20 * std::log10(mHold), 1) + " dB";
            auto t = meterR.withTop((int)bar.getBottom() + 2).withHeight(13); g.setFont(mono(FS * 0.74f)); const int w1 = textW(mono(FS * 0.64f), "MASTER  "), w2 = textW(mono(FS * 0.74f), db); int x = t.getCentreX() - (w1 + w2) / 2;
            if (getWidth() >= 1120) { g.setColour(DIM); g.setFont(mono(FS * 0.64f)); g.drawText("MASTER  ", x, t.getY(), w1, t.getHeight(), juce::Justification::centredLeft); } else x = t.getCentreX() - w2 / 2, w1 == 0;
            g.setColour(mHold >= 1 ? RECLT : DIM); g.setFont(mono(FS * 0.74f)); g.drawText(db, getWidth() >= 1120 ? x + w1 : x, t.getY(), w2 + 2, t.getHeight(), juce::Justification::centredLeft);
        }
        if (statsR.getWidth() > 0) {
            auto col = [&](juce::Rectangle<int> r, const char* lab, const juce::String& val) { g.setColour(DIM); auto f = font(FS * 0.62f); f.setExtraKerningFactor(0.06f); g.setFont(f); g.drawText(lab, r.removeFromTop(r.getHeight() / 2), juce::Justification::centredBottom);
                g.setColour(ACCENT2); g.setFont(mono(FS * 0.82f)); g.drawText(val, r, juce::Justification::centredTop); };
            auto r = statsR.reduced(8, 0); const juce::String cpu = load.cpu < 0 ? u8("—") : juce::String(juce::roundToInt(load.cpu)) + "%", ram = load.ramMB < 0 ? u8("—") : load.ramMB >= 1024 ? juce::String(load.ramMB / 1024, 1) + " GB" : juce::String(juce::roundToInt(load.ramMB)) + " MB";
            col(r.removeFromLeft(34), "CPU", cpu); r.removeFromLeft(8);
            if (statsHist) { auto h = r.removeFromLeft(60).withSizeKeepingCentre(60, 22); g.setColour(BG); g.fillRoundedRectangle(h.toFloat(), 4);
                for (size_t i = 0; i < cpuHist.size(); i++) { const float v = cpuHist[i]; g.setColour(v > 80 ? BAD : v > 50 ? WARN : GOOD); const float y = juce::jlimit(1.f, 22.f, v / 100 * 22); g.fillRect((float)h.getX() + i + 60 - cpuHist.size(), (float)h.getBottom() - y, 1.f, y); }
                r.removeFromLeft(8); }
            col(r.removeFromLeft(56), "RAM", ram); r.removeFromLeft(8); col(r.removeFromLeft(40), "VOICES", juce::String(engine.voiceCount()));
        }
    }
    void resized() override {
        narrow = getWidth() < 760 || (isTouch() && getHeight() <= 560 && getWidth() < 1000);
        auto r = safeArea(); laidOut = r; layoutTop(r); layoutDecks();
    }
    juce::Rectangle<int> safeArea() const {
        auto r = getLocalBounds();
#if JUCE_ANDROID || JUCE_IOS
        if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()) r = d->safeAreaInsets.subtractedFrom(r);
#endif
        return r;
    }

private:
    Look look; TouchHelper touch; std::shared_ptr<us::SampleBank> bank; us::Engine engine; Doc doc; juce::AudioDeviceManager devices; std::vector<float> tmp; double sr = 48000;
    std::unique_ptr<juce::PropertiesFile> props = [] { juce::PropertiesFile::Options o; o.applicationName = "settings"; o.filenameSuffix = ".xml"; o.folderName = "undae-sonicae/native"; o.osxLibrarySubFolder = "Application Support"; return std::make_unique<juce::PropertiesFile>(o); }();
    AppCtx ctx;
    AppCtx makeCtx() { AppCtx a; a.devices = &devices; a.props = props.get(); a.templatesDir = [] { return appDir().getChildFile("templates"); };
        a.newFromTemplate = [this](const juce::File& f) { try { doc.load(f); doc.file = juce::File(); doc.p.name = "Untitled"; doc.dirty = false; doc.sendChangeMessage(); } catch (const std::exception& e) { message("Template", e.what()); } };
        a.backupDir = [] { return backupDir(); }; a.openFile = [this](const juce::File& f) { open(f); };
        return a; }

    // --- top bar ---
    Btn brandB{ {}, Btn::Tb }, playB{ {}, Btn::Big, "play" }, recB{ {}, Btn::Tb, "rec" }, metB{ {}, Btn::Tb, "metro" }, tapB{ "Tap" }, sigB{ "4/4" }, patMore{ u8("⋯"), Btn::Tb }, posB{ {}, Btn::Tb },
        undoB{ {}, Btn::Tb, "undo" }, redoB{ {}, Btn::Tb, "redo" }, midiB{ {}, Btn::Tb, "midi" }, keysB{ {}, Btn::Tb, "keys" };
    Seg modeSeg{ { "PAT", "SONG" } }; Field bpm{ true, true }; Cap bpmL{ "BPM" }; juce::ComboBox patBox; Knob pitch{ "Pitch", -12, 12, 0, false, 0.01 };
#if JUCE_ANDROID
    juce::OpenGLContext gl;
#endif
    std::vector<juce::Rectangle<int>> grpR; juce::Rectangle<int> meterR, statsR, laidOut; int safeTick = 0; juce::Image logo; juce::String posText = "1:01:00"; bool narrow = false;
    float mLv = 0, mHold = 0; Load load; std::deque<float> cpuHist; std::vector<double> taps; std::atomic<bool> midiAct{ false };

    int logoW() const { if (!logo.isValid()) return 0; const float h = CTL() * (narrow ? 1.35f : 1.75f); return (int)(logo.getWidth() * h / logo.getHeight()); }
    void buildTopBar() {
        for (auto f : { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory().getChildFile("wordmark.png"), juce::File::getCurrentWorkingDirectory().getChildFile("app/img/wordmark.png"), juce::File::getCurrentWorkingDirectory().getChildFile("../app/img/wordmark.png") })
            if (f.existsAsFile()) { logo = juce::ImageFileFormat::loadFrom(f); break; }
#if __has_include("BinaryData.h")
        if (!logo.isValid()) logo = juce::ImageCache::getFromMemory(BinaryData::wordmark_png, BinaryData::wordmark_pngSize);
#endif
        for (juce::Component* c : std::initializer_list<juce::Component*>{ &brandB, &playB, &recB, &metB, &modeSeg, &bpm, &bpmL, &tapB, &sigB, &patBox, &patMore, &posB, &pitch, &undoB, &redoB, &midiB, &keysB }) addAndMakeVisible(c);
        brandB.setAlpha(0.01f); brandB.setTooltip("Project menu"); brandB.onClick = [this] { projectMenu(); };
        playB.setTooltip("Play / stop (Space)"); playB.onClick = [this] { togglePlay(); };
        recB.rec = true; recB.setTooltip("Record - choose what to capture, then press play"); recB.onClick = [this] { if (doc.rec) { doc.rec = false; recB.setOn(false); return; } recordDialog(); };
        metB.setTooltip("Metronome (Ctrl+M)"); metB.onClick = [this] { runAction("metro"); };
        modeSeg.tip(0, "Play the current pattern or the whole song"); modeSeg.onChange = [this](int i) { setSong(i == 1); };
        bpm.setTooltip("Tempo (BPM) - type a number or scroll"); bpm.setInputRestrictions(5, "0123456789."); bpm.onDone = [this](const juce::String& t) { setBpm(juce::jlimit(40.0, 260.0, t.getDoubleValue() > 0 ? t.getDoubleValue() : 120.0)); };
        bpm.addMouseListener(this, false);
        tapB.sm = true; tapB.setTooltip("Tap tempo"); tapB.onClick = [this] { const double t = juce::Time::getMillisecondCounterHiRes(); taps.erase(std::remove_if(taps.begin(), taps.end(), [t](double x) { return t - x > 2500; }), taps.end()); taps.push_back(t);
            if (taps.size() > 2) setBpm(juce::jlimit(40.0, 260.0, std::round(60000 / ((t - taps[0]) / (taps.size() - 1))))); };
        sigB.sm = true; sigB.monoFont = true; sigB.setTooltip("Time signature"); sigB.onClick = [this] { sigMenu(); };
        patBox.setTooltip("Current pattern"); patBox.onChange = [this] { const int i = patBox.getSelectedItemIndex(); if (i >= 0 && i < (int)doc.p.patterns.size() && doc.p.patterns[(size_t)i].id != doc.pat) doc.selectPattern(doc.p.patterns[(size_t)i].id); };
        patMore.setTooltip("Pattern options"); patMore.onClick = [this] { patMenu(); };
        posB.setAlpha(0.01f); posB.setTooltip("Song position - click to switch between bars:steps:ticks and minutes:seconds:centiseconds"); posB.onClick = [this] { runAction("time"); };
        pitch.sized(Knob::Xs); pitch.setLink("master:pitch"); pitch.fmt = [](double v) { return (v > 0 ? "+" : "") + juce::String(v, std::abs(v - std::round(v)) < 0.005 ? 0 : 2) + " st"; };
        pitch.onChange = [this](double v) { doc.p.masterPitch = std::abs(v) < 0.02 ? 0 : v; doc.live(); doc.autoWrite("master:pitch", v); }; pitch.onDone = [this] { doc.commit(); };
        pitch.s.setTooltip("Master pitch: transposes every instrument (built-in in cents, plug-ins in whole semitones). Right-click for more.");
        undoB.setTooltip("Undo"); redoB.setTooltip("Redo"); undoB.onClick = [this] { doc.undo(); }; redoB.onClick = [this] { doc.redo(); };
        midiB.setTooltip("MIDI devices"); midiB.onClick = [this] { midiMenu(); };
        keysB.setTooltip("On-screen keyboard (Alt+K)"); keysB.setOn(props->getBoolValue("keys", false)); keysB.onClick = [this] { runAction("keys"); };
    }
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override {   // scroll over the tempo: 1 BPM per notch, 5 with Shift
        if (e.eventComponent == &bpm) { setBpm(juce::jlimit(40.0, 260.0, std::round(doc.p.bpm) + (w.deltaY > 0 ? 1 : -1) * (e.mods.isShiftDown() ? 5 : 1))); return; }
        juce::Component::mouseWheelMove(e, w);
    }
    void setBpm(double v) { if (doc.p.bpm == v) { bpm.setText(juce::String(v, v == std::round(v) ? 0 : 1), false); return; } doc.p.bpm = v; doc.commit(); }
    void layoutTop(juce::Rectangle<int>& r) {
        grpR.clear(); meterR = {}; statsR = {}; const int c = CTL(), pad = 3, W = r.getWidth(); const bool hideLabels = W < 1120;
        bpmL.setVisible(!hideLabels); const bool showMeter = W >= 860, showStats = W >= 1050 && !(isTouch() && W < 1360);
        // The bar's items, in .grp boxes: transport, tempo, pattern, and undo / redo / MIDI / keyboard at the right end. When they do
        // not fit on one row (narrow windows, phones) it takes two: logo, transport and tempo; then pattern and the tools. Before
        // wrapping, and to make two rows fit, it squeezes step by step (`tight` 1-3: gaps, play button, tempo box, Tap, logo, record
        // and metronome); on the narrowest screens it takes three rows.
        struct G { std::vector<std::pair<juce::Component*, int>> items; int w() const { int s = 2 * 3 - 4; for (auto& it : items) s += it.second + 4; return s; } };
        struct Bar { G t, m, p, u; int brandW = 0, logoH = 0, gap = 8, edge = 8; };
        auto make = [&](int tight) {
            Bar b; b.logoH = (int)(c * (narrow ? (tight >= 3 ? 1.15f : 1.35f) : 1.75f)); b.gap = tight >= 1 ? 5 : 8; b.edge = tight >= 1 || narrow ? 5 : 8;
            const int lw = logo.isValid() ? (int)(logo.getWidth() * (float)b.logoH / (float)logo.getHeight()) : 0;
            b.brandW = lw + (tight >= 1 ? 4 : 10) + (W >= 1240 ? std::min(120, textW(font(FS), juce::String(doc.p.name)) + 8) : 0) + (W >= 960 ? 20 : 0);
            const int small = tight >= 3 ? juce::roundToInt(c * 0.85f) : c;
            b.t.items = { { &playB, tight >= 2 ? juce::roundToInt(c * 1.1f) : playB.idealWidth() }, { &recB, small }, { &metB, small }, { &modeSeg, modeSeg.idealWidth() } };
            b.m.items = { { &bpm, (W < 960 ? 50 : 62) - (tight >= 2 ? 6 : 0) } }; if (!hideLabels) b.m.items.push_back({ &bpmL, bpmL.idealWidth() });
            if (W >= 810 && tight < 3) b.m.items.push_back({ &tapB, tapB.idealWidth() }); b.m.items.push_back({ &sigB, sigB.idealWidth() });
            b.p.items = { { &patBox, W < 860 ? 72 : W < 960 ? 84 : W < 1440 ? 104 : 130 }, { &patMore, c }, { &posB, W < 960 ? 56 : 92 } };
            if (showMeter) b.p.items.push_back({ nullptr, W < 960 ? 58 : W < 1120 ? 72 : W < 1440 ? 100 : 118 }); b.p.items.push_back({ &pitch, pitch.idealWidth() });
            b.u.items = { { &undoB, c }, { &redoB, c }, { &midiB, c }, { &keysB, c } };
            return b; };
        auto rowW = [](const Bar& b, const std::vector<G*>& gs, bool brand) { int w = 2 * b.edge - b.gap + (brand ? b.brandW + b.gap : 0); for (auto* g : gs) w += g->w() + b.gap; return w; };
        Bar b; std::vector<std::vector<G*>> rows; int tight = 0;
        for (; tight <= (narrow ? 3 : 0); tight++) { b = make(tight); if (rowW(b, { &b.t, &b.m, &b.p, &b.u }, true) <= W) break; }
        if (tight <= (narrow ? 3 : 0)) rows = { { &b.t, &b.m, &b.p, &b.u } };
        else { for (tight = 0; tight <= 3; tight++) { b = make(tight); if (rowW(b, { &b.t, &b.m }, true) <= W && rowW(b, { &b.p, &b.u }, false) <= W) break; }
            if (tight <= 3) rows = { { &b.t, &b.m }, { &b.p, &b.u } }; else { b = make(3); rows = { { &b.t }, { &b.m, &b.u }, { &b.p } }; } }
        tapB.setVisible(std::any_of(b.m.items.begin(), b.m.items.end(), [this](auto& it) { return it.first == &tapB; }));
        const int rowH = c + 2 * pad + 2; int y = r.getY() + 4;
        auto put = [&](G& g, int x, int lineH) { grpR.push_back({ x, y + (lineH - rowH) / 2, g.w(), rowH }); int ix = x + pad;
            for (auto& [cp, iw] : g.items) { const int h = cp == &pitch ? pitch.idealHeight() : c; if (cp) cp->setBounds(ix, y + (lineH - h) / 2, iw, h); else meterR = { ix, y + (lineH - c) / 2, iw, c }; ix += iw + 4; }
            return g.w(); };
        for (size_t ri = 0; ri < rows.size(); ri++) {
            auto& row = rows[ri]; const bool first = ri == 0; const int lineH = first ? std::max(rowH, b.logoH) : rowH; int x = r.getX() + b.edge;
            if (first) { brandB.setBounds(x, y + (lineH - b.logoH) / 2, b.brandW, b.logoH); x += b.brandW + b.gap; }
            if (rows.size() == 1) {   // one row: the tools at the right end, the load meters before them when there is room
                for (auto* g : { &b.t, &b.m, &b.p }) x += put(*g, x, lineH) + b.gap;
                const int uw = b.u.w(); if (showStats) { const int room = r.getRight() - b.edge - uw - b.gap - x; const int full = 34 + 8 + 60 + 8 + 56 + 8 + 40 + 16, part = 34 + 8 + 56 + 8 + 40 + 16;
                    statsHist = room >= full && W >= 1240; const int sw = statsHist ? full : part; if (sw <= room) { grpR.push_back({ x, y + (lineH - rowH) / 2, sw, rowH }); statsR = grpR.back(); } }
                put(b.u, r.getRight() - b.edge - uw, lineH);
            } else {   // rows of two or three: the pattern box takes some of the spare width; a little left over lines the last box up on the right
                int slack = r.getWidth() - rowW(b, row, first);
                if (std::find(row.begin(), row.end(), &b.p) != row.end() && slack > 0) {
                    const int pw = std::min(slack, std::max(0, 92 - b.p.items[2].second)); b.p.items[2].second += pw; slack -= pw;   // the whole position first (1:01:00)
                    const int add = rows.size() == 3 ? slack : std::min(slack, 60); b.p.items[0].second += add; slack -= add; }
                for (size_t gi = 0; gi < row.size(); gi++) { if (gi > 0 && gi + 1 == row.size() && slack > 0 && slack <= 48) x += slack; x += put(*row[gi], x, lineH) + b.gap; }
            }
            y += lineH + 4;
        }
        topBottom = y; r.setTop(topBottom);
    }
    int topBottom = 60; bool statsHist = true;

    // --- decks ---
    juce::OwnedArray<Deck> decks;
    struct Splitter : juce::Component { Main* o = nullptr; bool side = false;
        void paint(juce::Graphics& g) override { g.setColour(LINE); auto r = getLocalBounds().toFloat(); if (side) g.fillRoundedRectangle(r.reduced(3, r.getHeight() * 0.42f), 4); else g.fillRoundedRectangle(r.reduced(r.getWidth() * 0.42f, 3), 4); }
        void mouseEnter(const juce::MouseEvent&) override { setMouseCursor(side ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::UpDownResizeCursor); }
        void mouseDrag(const juce::MouseEvent& e) override { auto p = o->getLocalPoint(this, e.getPosition()); auto r = o->decksArea; const double v = side ? (p.x - r.getX()) / (double)r.getWidth() : (p.y - r.getY()) / (double)r.getHeight(); o->props->setValue("split", juce::jlimit(0.15, 0.85, v)); o->layoutDecks(); }
    } splitter;
    juce::Rectangle<int> decksArea; Keys keysC; Toast toastC;
    // the workspace layout: what the user chose in Settings, on any screen (a phone cast to a TV or docked to a monitor
    // can use several decks); until they choose, one deck on a narrow screen and two elsewhere
    juce::String layoutPref() const { return props->containsKey("layout") ? props->getValue("layout") : juce::String(narrow ? "single" : "stack"); }
    std::vector<Deck*> active() { const auto l = layoutPref(); const int n = l == "single" ? 1 : l == "tri" ? 3 : l == "quad" ? 4 : 2;
        std::vector<Deck*> v; for (int i = 0; i < n; i++) v.push_back(decks[i]); if (props->getBoolValue("float", false)) v.push_back(decks[4]); return v; }
    std::string freeView() { auto act = active(); for (auto& v : views()) { bool used = false; for (auto* d : act) used |= d->cur == v.id; if (!used) return v.id; } return "rack"; }
    void place(Deck& d, const std::string& v) { d.place(v); props->setValue("deck" + juce::String(d.id), juce::String(v)); afterPlace(d); }
    void show(Deck& d, const std::string& v) {
        Deck* other = nullptr; for (auto* o : active()) if (o != &d && o->cur == v) other = o; const std::string prev = d.cur;
        if (d.cur == v) return; place(d, v);
        if (other) place(*other, !prev.empty() && prev != v ? prev : freeView());   // the two decks swap views
    }
    void gotoView(const std::string& to, const std::string& from) {
        auto act = active(); for (auto* d : act) if (d->cur == to) return;
        Deck* src = nullptr; for (auto* d : act) if (d->cur == from) src = d;
        Deck* target = act.size() == 1 ? act[0] : nullptr; if (!target) for (auto* d : act) if (d != src && d->id == "B") target = d; if (!target) for (auto* d : act) if (d != src) { target = d; break; }
        if (target) show(*target, to);
    }
    void layoutDecks() {
        auto act = active(); for (auto* d : decks) { const bool on = std::find(act.begin(), act.end(), d) != act.end(); d->setVisible(on); if (!on && !d->cur.empty()) { d->place(""); d->cur.clear(); } }
        for (auto* d : act) { std::string want = d->cur.empty() ? props->getValue("deck" + juce::String(d->id), d->id == "A" ? "rack" : d->id == "B" ? "roll" : d->id == "C" ? "song" : d->id == "D" ? "mixer" : "inst").toStdString() : d->cur;
            bool taken = false; for (auto* o : act) if (o != d && o->cur == want) taken = true; bool known = false; for (auto& v : views()) known |= v.id == want;
            if (d->cur.empty() || taken) place(*d, known && !taken ? want : freeView()); }
        auto r = safeArea().withTop(topBottom); r = r.withTrimmedLeft(narrow ? 4 : 6).withTrimmedRight(narrow ? 4 : 6).withTrimmedBottom(narrow ? 4 : 6);
        const bool showKeys = props->getBoolValue("keys", false); keysC.setVisible(showKeys);
        if (showKeys) { if (keysC.floating) { const int w = props->getIntValue("keysW", 760), h = props->getIntValue("keysFH", 170); keysC.setBounds(juce::jlimit(0, std::max(0, getWidth() - 200), props->getIntValue("keysX", 120)), juce::jlimit(0, std::max(0, getHeight() - 80), props->getIntValue("keysY", 140)), std::min(w, getWidth()), h); keysC.toFront(false); }
            else { const int h = juce::jlimit(60, getHeight() / 2, props->getIntValue("keysH", isTouch() ? 104 : 118)); keysC.setBounds(r.removeFromBottom(h)); r.removeFromBottom(6); } }
        decksArea = r; const auto l = layoutPref(); const double split = props->getDoubleValue("split", 0.45);
        splitter.setVisible(l == "stack" || l == "side"); splitter.side = l == "side";
        if (l == "single") decks[0]->setBounds(r);
        else if (l == "stack") { const int h = (int)((r.getHeight() - 8) * split); decks[0]->setBounds(r.removeFromTop(h)); splitter.setBounds(r.removeFromTop(8)); decks[1]->setBounds(r); }
        else if (l == "side") { const int w = (int)((r.getWidth() - 8) * split); decks[0]->setBounds(r.removeFromLeft(w)); splitter.setBounds(r.removeFromLeft(8)); decks[1]->setBounds(r); }
        else { auto top = r.removeFromTop((r.getHeight() - 6) / 2); r.removeFromTop(6);
            if (l == "tri") { decks[0]->setBounds(top); decks[1]->setBounds(r.removeFromLeft((r.getWidth() - 6) / 2)); r.removeFromLeft(6); decks[2]->setBounds(r); }
            else { decks[0]->setBounds(top.removeFromLeft((top.getWidth() - 6) / 2)); top.removeFromLeft(6); decks[1]->setBounds(top); decks[2]->setBounds(r.removeFromLeft((r.getWidth() - 6) / 2)); r.removeFromLeft(6); decks[3]->setBounds(r); } }
        if (props->getBoolValue("float", false)) {   // on any screen; kept inside the window (a small screen gets a smaller panel)
            auto* F = decks[4]; const int fw = std::min(props->getIntValue("floatW", 620), std::max(240, getWidth() - 8)), fh = F->minimized ? CTL() + 8 : std::min(props->getIntValue("floatH", 400), std::max(180, getHeight() - topBottom - 8));
            F->setBounds(juce::jlimit(0, std::max(0, getWidth() - fw), props->getIntValue("floatX", 140)), juce::jlimit(topBottom, std::max(topBottom, getHeight() - fh), props->getIntValue("floatY", 110)), fw, fh); F->toFront(false); }
        toastC.toFront(false);
    }
    std::unique_ptr<juce::Component> makeView(const std::string& v) {
        if (v == "rack") return std::make_unique<RackView>(doc);
        if (v == "roll") return std::make_unique<RollView>(doc);
        if (v == "song") return std::make_unique<SongView>(doc);
        if (v == "mixer") { auto m = std::make_unique<MixerView>(doc); m->renderArmed = [this] { renderArmed(); }; m->inputNames = [this] { auto* d = devices.getCurrentAudioDevice(); juce::StringArray s; if (d) { auto names = d->getInputChannelNames(); auto act = d->getActiveInputChannels(); for (int i = 0; i < names.size(); i++) if (act[i]) s.add(names[i]); } return s; }; return m; }
        if (v == "inst") return std::make_unique<InstView>(doc);
        if (v == "design") return std::make_unique<DesignView>(doc);
        if (v == "lab") return std::make_unique<LabView>(doc);
        if (v == "lib") return std::make_unique<LibView>(doc);
        if (v == "set") return std::make_unique<SettingsView>(doc, ctx);
        return {};
    }
    void afterPlace(Deck&) {}
    template <class T> T* deckWith() { for (auto* d : decks) if (d->isVisible()) if (auto* t = d->as<T>()) return t; return nullptr; }

    // --- preferences: theme, density, corners, scale ---
    void applyPrefs() {
        applyTheme(props->getValue("theme", "slate").toStdString(), props->getValue("accent", "").toStdString(), props->getValue("density", "cozy").toStdString(), (float)props->getDoubleValue("radius", 10), (float)props->getDoubleValue("scale", 1));
        auto& s = style(); s.glow = props->getBoolValue("glow", true); s.motion = props->getBoolValue("motion", true); s.stepShape = props->getValue("stepShape", "lozenge").toStdString(); s.noteColor = props->getValue("noteColor", "channel").toStdString(); s.gridK = (float)props->getDoubleValue("gridContrast", 1);
        s.deckAlpha = (float)juce::jlimit(0.15, 1.0, props->getDoubleValue("deckAlpha", 0.78));
        if (auto* w = dynamic_cast<juce::DocumentWindow*>(getTopLevelComponent())) w->setBackgroundColour(BG);
    }

    // --- MIDI controller links (links.js) ---
    std::string learning;
    void linkItems(Menu& m, const std::string& target, double v) {
        auto* lane = doc.lane(target);
        m.sep(); m.head("Automation");
        m.item(lane ? "Show its automation lane" : "Create automation lane", [this, target, v] { if (!doc.lane(target)) { us::AutoLane l; l.id = us::newId(); l.target = target; l.pts.push_back({ 0, v }); doc.p.autos.push_back(l); doc.commit(); } gotoView("song", {}); });
        m.item("Start the song at this value", [this, target, v] { if (auto* l = doc.lane(target)) { l->pts.erase(std::remove_if(l->pts.begin(), l->pts.end(), [](auto& p) { return p.t <= 0; }), l->pts.end()); l->pts.insert(l->pts.begin(), { 0, v }); doc.commit(); } }, lane != nullptr);
        m.head("Remote control");
        m.item(u8("Link to MIDI controller…"), [this, target] { learning = target; toast(u8("Move a knob or fader on your MIDI controller… (Esc cancels)")); });
        for (auto& l : doc.p.links) if (l.target == target) m.item("Remove link (CC " + juce::String(l.cc) + ", channel " + juce::String(l.ch + 1) + ")", [this, target] { doc.p.links.erase(std::remove_if(doc.p.links.begin(), doc.p.links.end(), [&](auto& x) { return x.target == target; }), doc.p.links.end()); doc.commit(); toast("Controller link removed"); });
    }
    bool onControl(int ch, int cc, int value) {
        if (!learning.empty()) { const auto t = learning; learning.clear(); us::Link l; l.id = us::newId(); l.target = t; l.ch = ch; l.cc = cc; doc.p.links.push_back(l); doc.commit();
            auto tg = doc.target(t); toast("Linked CC " + juce::String(cc) + " (channel " + juce::String(ch + 1) + ") to " + (tg ? tg->label : juce::String(t))); applyLink(t, value); return true; }
        bool used = false; for (auto& l : doc.p.links) if (l.cc == cc && l.ch == ch) { applyLink(l.target, value); used = true; } return used;
    }
    void applyLink(const std::string& t, int value) {
        auto tg = doc.target(t); if (!tg) return; const double v = tg->min + (tg->max - tg->min) * value / 127.0; tg->set(v); doc.autoWrite(t, v); doc.live(); setLinked(t, v);
        linkDirty = true;
    }
    bool linkDirty = false;

    // --- menus ---
    void projectMenu() {
        Menu m; m.head(juce::String(doc.p.name));
        m.item(u8("Rename project…"), [this] { ask("Project name", juce::String(doc.p.name), [this](juce::String v) { if (v.isNotEmpty()) { doc.p.name = v.toStdString(); doc.commit(); } }); });
        m.item("New project", [this] { newProject(); }); m.item("Reopen the autosaved session", [this] { reopenAutosave(); }, appDir().getChildFile("autosave.undae.json").existsAsFile());
        m.item(u8("Open project…"), [this] { openDialog(); }, true, false, "Ctrl+O");
        auto tf = appDir().getChildFile("templates").findChildFiles(juce::File::findFiles, false, "*.undae.json"); for (auto& f : tf) m.item("New from template: " + f.getFileName().upToFirstOccurrenceOf(".", false, false), [this, f] { ctx.newFromTemplate(f); toast(u8("New project from “") + f.getFileName().upToFirstOccurrenceOf(".", false, false) + u8("”")); });
        m.item(u8("Save as template…"), [this] { ask("Template name (a new project can start from it, also at start-up)", doc.p.name == "Untitled" ? juce::String("My template") : juce::String(doc.p.name), [this](juce::String v) { if (v.isEmpty()) return; appDir().getChildFile("templates").createDirectory();
            try { us::saveProject(doc.p, appDir().getChildFile("templates").getChildFile(juce::File::createLegalFileName(v) + ".undae.json")); toast(u8("Template “") + v + u8("” saved")); } catch (const std::exception& e) { message("Template", e.what()); } }); });
        m.item("Save project file", [this] { save(false); }, true, false, "Ctrl+S"); m.item(u8("Save as…"), [this] { saveAs(); }, true, false, "Ctrl+Shift+S"); m.item("Save a new version", [this] { saveNew(); }, true, false, "Ctrl+N");
        m.sep();
        const bool song = doc.p.songBars() > 0;
        m.item(u8("Export WAV · pattern ×4"), [this] { exportAudio(false, "wav"); }); m.item(u8("Export WAV · song"), [this] { exportAudio(true, "wav"); }, song, false, "Ctrl+R");
        m.item(us::mp3Available() ? u8("Export MP3 · song") : u8("Export MP3 · song (needs LAME, libmp3lame)"), [this] { exportAudio(true, "mp3"); }, song && us::mp3Available(), false, "Ctrl+Shift+R");
        m.item(u8("Export MP3 · pattern ×4"), [this] { exportAudio(false, "mp3"); }, us::mp3Available());
        m.item(u8("Export FLAC · song"), [this] { exportAudio(true, "flac"); }, song); m.item(u8("Export Ogg Vorbis · song"), [this] { exportAudio(true, "ogg"); }, song);
        m.item(juce::String(u8("Export MIDI · ")) + (doc.song ? "song" : "pattern"), [this] { exportMidi(); }, true, false, "Ctrl+Shift+M"); m.item(u8("Import MIDI into channel…"), [this] { importMidiDialog(); });
        m.sep(); m.item("Open the backups folder", [] { backupDir().revealToUser(); });
        m.item("Undo", [this] { doc.undo(); }, doc.canUndo(), false, "Ctrl+Z"); m.item("Redo", [this] { doc.redo(); }, doc.canRedo(), false, "Ctrl+Shift+Z");
        m.show(at(&brandB));
    }
    void patMenu() {
        Menu m;
        m.item("New pattern", [this] { ask("Pattern name", "Pattern " + juce::String((int)doc.p.patterns.size() + 1), [this](juce::String v) { if (v.isEmpty()) return; us::Pattern p; p.id = us::newId(); p.name = v.toStdString(); p.bars = 1; doc.p.patterns.push_back(p); doc.commit(); doc.selectPattern(p.id); }); });
        m.item("Duplicate pattern", [this] { if (auto* p = doc.patt()) { us::Pattern n = *p; n.id = us::newId(); n.name += " copy"; for (auto& [k, ns] : n.notes) for (auto& z : ns) z.id = us::newId(); doc.p.patterns.push_back(n); doc.commit(); doc.selectPattern(n.id); } });
        m.item(u8("Rename…"), [this] { if (auto* p = doc.patt()) ask("Pattern name", juce::String(p->name), [this](juce::String v) { if (auto* q = doc.patt(); q && v.isNotEmpty()) { q->name = v.toStdString(); doc.commit(); } }); });
        m.item("Split by channel", [this] { auto* src = doc.patt(); if (!src) return; std::vector<us::Pattern> made;
            for (auto& c : doc.p.channels) if (src->notes.count(c.id) && !src->notes[c.id].empty()) { us::Pattern n; n.id = us::newId(); n.name = src->name + u8s(" · ") + c.name; n.bars = src->bars; n.notes[c.id] = src->notes[c.id]; for (auto& z : n.notes[c.id]) z.id = us::newId(); made.push_back(n); }
            if (made.size() < 2) { toast("This pattern only uses one channel"); return; } for (auto& n : made) doc.p.patterns.push_back(n); doc.commit(); toast("Made " + juce::String((int)made.size()) + " patterns, one per channel (the original is kept)"); });
        m.item("Clear notes", [this] { if (auto* p = doc.patt()) { p->notes.clear(); doc.commit(); } });
        m.item("Delete pattern", [this] { const auto id = doc.pat; auto& P = doc.p; P.patterns.erase(std::remove_if(P.patterns.begin(), P.patterns.end(), [&](auto& x) { return x.id == id; }), P.patterns.end()); P.clips.erase(std::remove_if(P.clips.begin(), P.clips.end(), [&](auto& c) { return c.pat == id; }), P.clips.end()); doc.commit(); doc.selectPattern(P.patterns[0].id); }, doc.p.patterns.size() > 1);
        m.show(at(&patMore));
    }
    static std::string u8s(const char* s) { return s; }
    void sigMenu() {
        Menu m; m.head("Time signature"); const std::pair<int, int> sigs[] = { { 2, 4 }, { 3, 4 }, { 4, 4 }, { 5, 4 }, { 6, 4 }, { 7, 4 }, { 3, 8 }, { 5, 8 }, { 6, 8 }, { 7, 8 }, { 9, 8 }, { 12, 8 } };
        for (auto [n, d] : sigs) m.item(juce::String(n) + " / " + juce::String(d), [this, n, d] { const bool was = engine.isPlaying(); if (was) stopTransport(); const double old = doc.p.bar(); doc.p.sigN = n; doc.p.sigD = d; for (auto& a : doc.p.audio) { a.start *= old / doc.p.bar(); a.len *= old / doc.p.bar(); } doc.commit(); if (was) togglePlay(); }, true, doc.p.sigN == n && doc.p.sigD == d);
        m.show(at(&sigB));
    }
    void midiMenu() {
        Menu m; auto list = juce::MidiInput::getAvailableDevices(); m.head(list.isEmpty() ? juce::String("MIDI on - no devices found") : juce::String("Connected inputs"));
        for (auto& d : list) { const auto id = d.identifier; m.item(d.name, [this, id] { const bool on = !devices.isMidiInputDeviceEnabled(id); devices.setMidiInputDeviceEnabled(id, on); props->setValue("midiOff:" + id, !on); }, true, devices.isMidiInputDeviceEnabled(id)); }
        m.sep(); m.item("Rescan", [this] { for (auto& d : juce::MidiInput::getAvailableDevices()) if (!props->getBoolValue("midiOff:" + d.identifier, false)) devices.setMidiInputDeviceEnabled(d.identifier, true); toast("MIDI devices rescanned"); });
        m.item(u8("MIDI settings…"), [this] { runAction("midiSet"); });
        m.show(at(&midiB));
    }
    void recordDialog() {
        auto* p = new RecordPanel(doc); juce::DialogWindow::LaunchOptions o; o.content.setOwned(p); o.dialogTitle = "Record"; o.dialogBackgroundColour = BG2; o.useNativeTitleBar = true; o.resizable = false;
        auto* w = o.launchAsync(); juce::Component::SafePointer<juce::DialogWindow> sw(w);
        p->onCancel = [sw] { if (sw) sw->exitModalState(0); };
        p->onArm = [this, sw] { doc.rec = true; recB.setOn(true); armRecording(); toast("Armed - press play to start recording"); if (sw) sw->exitModalState(1); };
    }
    void recQuick() {   // R: arm with the last choice, without the dialog
        if (!doc.rec && !props->getBoolValue("recMidi", true) && !props->getBoolValue("recAudio", true) && !props->getBoolValue("recAuto", true)) { recordDialog(); return; }
        doc.rec = !doc.rec; recB.setOn(doc.rec); armRecording(); toast(doc.rec ? "Recording armed - press play" : "Recording off");
    }
    void armRecording() { engine.setRecording(doc.rec && props->getBoolValue("recMidi", true)); doc.recAuto = props->getBoolValue("recAuto", true); }
    void showHelp() { juce::DialogWindow::LaunchOptions o; o.content.setOwned(new HelpPanel()); o.dialogTitle = "Keyboard shortcuts"; o.dialogBackgroundColour = BG2; o.useNativeTitleBar = true; o.resizable = true; o.launchAsync(); }

    // --- files ---
    std::unique_ptr<juce::FileChooser> chooser;
    std::map<int, int> typed; int undoMark = -1, histMark = 0;
    static juce::File appDir() { return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("undae-sonicae").getChildFile("native"); }
    static juce::File backupDir() { return appDir().getChildFile("backups"); }
    void newProject() { if (doc.dirty) try { us::saveProject(doc.p, appDir().getChildFile("autosave.undae.json")); } catch (...) {} doc.setProject(blankProject()); doc.song = false; doc.sendChangeMessage(); }
    void reopenAutosave() { auto f = appDir().getChildFile("autosave.undae.json"); try { doc.load(f); doc.file = juce::File(); doc.dirty = true; doc.sendChangeMessage(); toast("Reopened " + juce::String(doc.p.name)); } catch (const std::exception& e) { message("Could not open", e.what()); } }
    void openDialog() {
        chooser = std::make_unique<juce::FileChooser>("Open a project", doc.file.exists() ? doc.file.getParentDirectory() : juce::File::getSpecialLocation(juce::File::userHomeDirectory), "*.json");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) open(fc.getResult()); });
    }
    bool save(bool as) {
        if (!as && doc.file != juce::File()) { try { doc.save(doc.file); toast("Saved " + doc.file.getFileName()); return true; } catch (const std::exception& e) { message("Could not save", e.what()); return false; } }
        chooser = std::make_unique<juce::FileChooser>("Save project", juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(juce::File::createLegalFileName(juce::String(doc.p.name)) + ".undae.json"), "*.json");
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this](const juce::FileChooser& fc) {
            auto f = fc.getResult(); if (f == juce::File()) return;
            try { if (doc.p.name == "Untitled") doc.p.name = f.getFileNameWithoutExtension().upToFirstOccurrenceOf(".", false, false).toStdString(); doc.save(f); toast("Saved " + f.getFileName()); } catch (const std::exception& e) { message("Could not save", e.what()); }
        });
        return false;
    }
    void saveAs() { ask("Save as (project name)", juce::String(doc.p.name), [this](juce::String v) { if (v.isEmpty()) return; doc.p.name = v.toStdString(); doc.file = juce::File(); save(true); }); }
    void saveNew() {   // "Name v2", "Name v3"... next to the current file
        juce::String n = doc.p.name; int ver = 1; auto at = n.lastIndexOf(" v"); if (at > 0 && n.substring(at + 2).containsOnly("0123456789") && n.substring(at + 2).isNotEmpty()) { ver = n.substring(at + 2).getIntValue(); n = n.substring(0, at); }
        doc.p.name = (n + " v" + juce::String(ver + 1)).toStdString();
        if (doc.file != juce::File()) { auto f = doc.file.getSiblingFile(juce::File::createLegalFileName(juce::String(doc.p.name)) + ".undae.json"); try { doc.save(f); toast("Saved as " + juce::String(doc.p.name)); } catch (const std::exception& e) { message("Could not save", e.what()); } }
        else save(true);
    }
    // WAV 24-bit, FLAC 24-bit, Ogg Vorbis (quality 8), MP3 (LAME), at the export rate
    void exportAudio(bool song, const std::string& fmt) {
        if (song && doc.p.songBars() == 0) { toast("Nothing to export yet: the song is empty"); return; }
        const juce::String ext = fmt == "flac" ? ".flac" : fmt == "ogg" ? ".ogg" : fmt == "mp3" ? ".mp3" : ".wav";
        const juce::String base = juce::File::createLegalFileName(juce::String(doc.p.name)) + "-" + (song ? juce::String("song") : juce::File::createLegalFileName(juce::String(doc.patt() ? doc.patt()->name : "pattern")));
        chooser = std::make_unique<juce::FileChooser>("Export", juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile(base + ext), "*" + ext);
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this, song, fmt](const juce::FileChooser& fc) {
            auto f = fc.getResult(); if (f == juce::File()) return; toast(u8("Rendering audio…"));
            juce::MessageManager::callAsync([this, f, song, fmt] { try { exportTo(f, song, fmt); toast(juce::String(fmt == "mp3" ? "MP3" : fmt == "flac" ? "FLAC" : fmt == "ogg" ? "Ogg Vorbis" : "WAV") + " exported: " + f.getFileName()); } catch (const std::exception& e) { message("Export failed", e.what()); } });
        });
    }
public:
    void exportTo(const juce::File& f, bool song, const std::string& fmt) {
        us::Engine off(bank); auto snap = std::make_shared<us::Project>(doc.p); off.setProject(snap);
        const double rate = props ? props->getDoubleValue("exportRate", 48000) : 48000; const int bits = props ? props->getIntValue("bitDepth", 24) : 24;
        auto buf = off.render(song ? us::Mode::Song : us::Mode::Pattern, doc.pat, song ? 1 : 4, rate);
        writeAudio(f, buf, rate, fmt, bits);
    }
private:
    void writeAudio(const juce::File& f, const juce::AudioBuffer<float>& buf, double rate, const std::string& fmt, int bits = 24) {
        f.deleteFile();
        if (fmt == "mp3") { auto mb = us::encodeMp3(buf, (int)rate, props ? props->getIntValue("mp3Kbps", 320) : 320); if (!f.replaceWithData(mb.getData(), mb.getSize())) throw std::runtime_error("Cannot write " + f.getFullPathName().toStdString()); return; }
        std::unique_ptr<juce::AudioFormat> af; int quality = 0;
        if (fmt == "flac") { af = std::make_unique<juce::FlacAudioFormat>(); bits = 24; } else if (fmt == "ogg") { af = std::make_unique<juce::OggVorbisAudioFormat>(); bits = 16; quality = 8; } else af = std::make_unique<juce::WavAudioFormat>();
        auto os = std::make_unique<juce::FileOutputStream>(f); if (os->failedToOpen()) throw std::runtime_error("Cannot write " + f.getFullPathName().toStdString());
        std::unique_ptr<juce::AudioFormatWriter> w(af->createWriterFor(os.get(), rate, 2, bits, {}, quality));
        if (!w) throw std::runtime_error("This format cannot be written here"); os.release();
        w->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
    }
    // Alt+R in the Mixer: one WAV per armed bus, with only the channels and audio clips that reach that bus playing
    void renderArmed() {
        std::vector<std::string> armed; for (auto& b : doc.p.buses) if (b.arm && b.kind != "master") armed.push_back(b.id);
        if (armed.empty()) { toast("Arm one or more buses first (the red dot on a strip)"); return; }
        chooser = std::make_unique<juce::FileChooser>("Render armed buses into a folder", juce::File::getSpecialLocation(juce::File::userMusicDirectory));
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this, armed](const juce::FileChooser& fc) {
            auto dir = fc.getResult(); if (!dir.isDirectory()) return; int done = 0;
            for (auto& id : armed) {
                us::Project P = doc.p; for (auto& c : P.channels) c.mute = c.mute || !doc.reaches(c.bus.empty() ? "master" : c.bus, id); for (auto& a : P.audio) a.mute = a.mute || !doc.reaches(a.bus.empty() ? "master" : a.bus, id);
                try { us::Engine off(bank); off.setProject(std::make_shared<us::Project>(P)); auto buf = off.render(doc.song ? us::Mode::Song : us::Mode::Pattern, doc.pat, doc.song ? 1 : 4, 48000);
                    auto* b = doc.bus(id); writeAudio(dir.getChildFile(juce::File::createLegalFileName(juce::String(doc.p.name) + "-" + juce::String(b ? b->name : id)) + ".wav"), buf, 48000, "wav"); done++; }
                catch (const std::exception& e) { message("Render failed", e.what()); }
            }
            toast("Rendered " + juce::String(done) + " bus" + (done == 1 ? "" : "es"));
        });
    }
    // --- MIDI files ---
    static void vlq(std::vector<uint8_t>& o, uint32_t v) { uint8_t b[5]; int n = 0; b[n++] = v & 127; while (v >>= 7) b[n++] = (uint8_t)((v & 127) | 128); while (n) o.push_back(b[--n]); }
    void exportMidi() {
        const double L = doc.song ? (double)doc.p.songBars() * doc.p.bar() : (doc.patt() ? doc.patt()->bars * doc.p.bar() : 0); if (L <= 0) { toast("Nothing to export yet"); return; }
        std::map<std::string, std::vector<us::Event>> by; for (auto& e : us::collect(doc.p, doc.song ? us::Mode::Song : us::Mode::Pattern, doc.patt(), 0, L, true)) by[e.c->id].push_back(e);
        std::vector<uint8_t> out; auto u32 = [&](uint32_t v) { for (int s = 24; s >= 0; s -= 8) out.push_back((uint8_t)(v >> s)); };
        auto chunk = [&](const std::vector<uint8_t>& body) { for (char c : std::string("MTrk")) out.push_back((uint8_t)c); u32((uint32_t)body.size()); out.insert(out.end(), body.begin(), body.end()); };
        for (char c : std::string("MThd")) out.push_back((uint8_t)c); u32(6); out.push_back(0); out.push_back(1); out.push_back(0); out.push_back((uint8_t)(by.size() + 1)); out.push_back(us::PPQ >> 8); out.push_back(us::PPQ & 255);
        const uint32_t us_ = (uint32_t)std::round(60e6 / doc.p.bpm); chunk({ 0, 0xff, 0x51, 3, (uint8_t)(us_ >> 16), (uint8_t)(us_ >> 8), (uint8_t)us_, 0, 0xff, 0x2f, 0 });
        int i = 0; for (auto& [cid, evs] : by) {
            const int ch = i++ % 16; std::vector<std::array<int, 5>> ev; for (auto& e : evs) { ev.push_back({ (int)std::round(e.tick + e.sw), 1, 0x90 | ch, e.n, std::max(1, (int)std::round(e.v * 127)) }); ev.push_back({ (int)std::round(e.tick + e.sw + e.d), 0, 0x80 | ch, e.n, 0 }); }
            std::sort(ev.begin(), ev.end(), [](auto& a, auto& b) { return a[0] < b[0] || (a[0] == b[0] && a[1] < b[1]); });
            std::vector<uint8_t> body; const auto* c = doc.p.findChannel(cid); std::string name = c ? c->name : "Track"; body.push_back(0); body.push_back(0xff); body.push_back(3); vlq(body, (uint32_t)name.size()); for (char x : name) body.push_back((uint8_t)(x & 127));
            int last = 0; for (auto& e : ev) { vlq(body, (uint32_t)std::max(0, e[0] - last)); body.push_back((uint8_t)e[2]); body.push_back((uint8_t)e[3]); body.push_back((uint8_t)e[4]); last = e[0]; }
            body.push_back(0); body.push_back(0xff); body.push_back(0x2f); body.push_back(0); chunk(body);
        }
        chooser = std::make_unique<juce::FileChooser>("Export MIDI", juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile(juce::File::createLegalFileName(juce::String(doc.p.name)) + ".mid"), "*.mid");
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this, out](const juce::FileChooser& fc) { auto f = fc.getResult(); if (f == juce::File()) return; f.replaceWithData(out.data(), out.size()); toast("MIDI exported: " + f.getFileName()); });
    }
    void importMidiDialog() {
        chooser = std::make_unique<juce::FileChooser>("Import MIDI into the current channel", juce::File::getSpecialLocation(juce::File::userHomeDirectory), "*.mid;*.midi");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) importMidi(fc.getResult()); });
    }
    void importMidi(const juce::File& f) {
        auto* c = doc.chan(); if (!c) { toast("Add a channel in the Rack first"); return; }
        juce::FileInputStream in(f); juce::MidiFile mf; if (!in.openedOk() || !mf.readFrom(in)) { toast("Could not read that MIDI file"); return; }
        const int tpq = mf.getTimeFormat() > 0 ? mf.getTimeFormat() : 96; const double k = (double)us::PPQ / tpq; std::vector<us::Note> ns;
        for (int t = 0; t < mf.getNumTracks(); t++) { juce::MidiMessageSequence s(*mf.getTrack(t)); s.updateMatchedPairs();
            for (int i = 0; i < s.getNumEvents(); i++) { auto* e = s.getEventPointer(i); if (!e->message.isNoteOn()) continue; const double t0 = e->message.getTimeStamp(), t1 = e->noteOffObject ? e->noteOffObject->message.getTimeStamp() : t0 + tpq / 4.0;
                if (t0 * k >= 64.0 * doc.p.bar()) continue; us::Note n; n.id = us::newId(); n.t = std::round(t0 * k); n.d = std::max(1.0, std::round((t1 - t0) * k)); n.n = e->message.getNoteNumber(); n.v = e->message.getFloatVelocity(); ns.push_back(n); } }
        if (ns.empty()) { toast("No notes in that file"); return; }
        auto* p = doc.patt(); p->notes[c->id] = ns; double end = 0; for (auto& n : ns) end = std::max(end, n.t + n.d); double b = p->bars; while (b * doc.p.bar() < end && b < 64) b *= 2; p->bars = b;
        doc.commit(); gotoView("roll", {}); toast("Imported " + juce::String((int)ns.size()) + u8(" notes into “") + juce::String(c->name) + u8("”"));
    }

    // --- recording: notes, audio (one take per armed bus), automation ---
    juce::AudioBuffer<float> recFifoBuf; juce::AbstractFifo recFifo{ 48000 * 8 }; std::atomic<bool> recAudioNow{ false }; std::atomic<int> recIns{ 0 };
    juce::AudioBuffer<float> take; int takeLen = 0; double takeStartTick = 0; double takeRate = 48000; int takeNo = 1; bool waiting = false; double resumeAt = 0; double lastPos = 0;
    void drainRecording() {
        auto notes = engine.takeRecorded();
        if (!notes.empty()) if (auto* pt = doc.patt()) { const int rq = props->getIntValue("recQuant", 0); for (auto& r : notes) { us::Note n; n.id = us::newId(); n.t = rq > 0 ? std::round(r.tick / rq) * rq : r.tick; n.d = r.dur; n.n = r.note; n.v = r.vel; pt->notes[r.ch].push_back(n); } doc.commit(); }
        int s1, n1, s2, n2; const int ready = recFifo.getNumReady();
        if (ready > 0) {
            recFifo.prepareToRead(ready, s1, n1, s2, n2);
            if (take.getNumSamples() < takeLen + ready) take.setSize(8, takeLen + ready + 48000 * 10, true, true);
            for (int c = 0; c < 8; c++) { if (n1) take.copyFrom(c, takeLen, recFifoBuf, c, s1, n1); if (n2) take.copyFrom(c, takeLen + n1, recFifoBuf, c, s2, n2); }
            takeLen += n1 + n2; recFifo.finishedRead(n1 + n2);
        }
    }
    void finishTake() {   // one take per armed bus: from its input channel(s), onto the first free track, playing through that bus
        drainRecording();
        if (takeLen < 2400) { takeLen = 0; return; }
        for (auto& b : doc.p.buses) {
            if (!b.arm || b.input.empty() || b.kind == "master") continue; const int ch = juce::jlimit(0, 7, juce::String(b.input).getIntValue() - 1); const int ch2 = b.stereo ? std::min(7, ch + 1) : ch;
            juce::AudioBuffer<float> one(2, takeLen); one.copyFrom(0, 0, take, ch, 0, takeLen); one.copyFrom(1, 0, take, ch2, 0, takeLen);
            juce::MemoryBlock mb; { auto mo = std::make_unique<juce::MemoryOutputStream>(mb, false); juce::WavAudioFormat wav; std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(mo.get(), takeRate, 2, 24, {}, 0)); if (w) { mo.release(); w->writeFromAudioSampleBuffer(one, 0, takeLen); } }
            auto id = doc.addSampleData("Take " + std::to_string(takeNo++) + " " + b.name, mb);
            if (!id.empty()) { doc.placeAudio(id, takeStartTick / doc.p.bar()); if (!doc.p.audio.empty()) { doc.p.audio.back().bus = b.id; doc.commit(); } }
        }
        takeLen = 0;
    }
    bool armedAudio() const { if (!doc.rec || !props->getBoolValue("recAudio", true) || !doc.song) return false; for (auto& b : doc.p.buses) if (b.arm && !b.input.empty()) return true; return false; }
    void startTransport(bool force = false) {
        if (doc.rec && props->getBoolValue("waitInput", false) && !force && !engine.isPlaying()) { waiting = true; toast(u8("Waiting for you to play a note…")); return; }
        double from = doc.song ? doc.songStart * doc.p.bar() : 0;
        if (armedAudio()) { recFifo.reset(); takeLen = 0; takeRate = devices.getCurrentAudioDevice() ? devices.getCurrentAudioDevice()->getCurrentSampleRate() : 48000; recAudioNow = true; takeStartTick = from; }
        armRecording(); lastPos = from;
        engine.play(doc.song ? us::Mode::Song : us::Mode::Pattern, doc.pat, from, doc.rec && props->getBoolValue("countIn", false) ? 1 : 0);
    }
    void stopTransport() { engine.stop(); waiting = false; if (recAudioNow) { recAudioNow = false; finishTake(); } }
    void togglePlay() { if (waiting) { waiting = false; return; } if (engine.isPlaying()) stopTransport(); else startTransport(); }
    void pauseToggle() { if (engine.isPlaying()) { resumeAt = engine.positionTicks(); stopTransport(); } else { armRecording(); engine.play(doc.song ? us::Mode::Song : us::Mode::Pattern, doc.pat, resumeAt); } }
    void setSong(bool s) { if (doc.song == s) return; const bool was = engine.isPlaying(); if (was) stopTransport(); doc.song = s; modeSeg.set(s ? 1 : 0); doc.sendChangeMessage(); if (was) startTransport(true); }
    void startup() {
        const auto st = props->getValue("startup", "blank");
        juce::File f = st == "last" ? appDir().getChildFile("autosave.undae.json") : st.startsWith("tpl:") ? appDir().getChildFile("templates").getChildFile(st.substring(4)) : juce::File();
        if (f.existsAsFile()) { try { doc.load(f); doc.file = juce::File(); if (st != "last") doc.p.name = "Untitled"; doc.song = doc.p.songBars() > 0; doc.sendChangeMessage(); } catch (...) {} }
    }
    void changeListenerCallback(juce::ChangeBroadcaster* src) override {
        if (src == &devices) { std::unique_ptr<juce::XmlElement> x(devices.createStateXml()); if (x) { props->setValue("audio", x.get()); props->saveIfNeeded(); } return; }
        patBox.clear(juce::dontSendNotification); int i = 1; for (auto& p : doc.p.patterns) patBox.addItem(juce::String(p.name), i++);
        for (size_t k = 0; k < doc.p.patterns.size(); k++) if (doc.p.patterns[k].id == doc.pat) patBox.setSelectedItemIndex((int)k, juce::dontSendNotification);
        if (!bpm.hasKeyboardFocus(true)) bpm.setText(juce::String(doc.p.bpm, doc.p.bpm == std::round(doc.p.bpm) ? 0 : 1), false);
        sigB.setButtonText(juce::String(doc.p.sigN) + "/" + juce::String(doc.p.sigD)); modeSeg.set(doc.song ? 1 : 0); pitch.set(doc.p.masterPitch);
        undoB.setEnabled(doc.canUndo()); redoB.setEnabled(doc.canRedo());
        if (auto* w = dynamic_cast<juce::DocumentWindow*>(getTopLevelComponent())) w->setName((doc.dirty ? "* " : "") + juce::String(doc.p.name) + u8(" — Undae Sonicae"));
        resized(); repaint();
    }
    double lastAutosave = 0, lastBackup = 0, lastStats = 0; std::atomic<int> callbacks{ 0 }; std::atomic<double> worstMs{ 0 }, totalMs{ 0 }; std::atomic<float> maxPeak{ 0 }, lastLoad{ 0 };
    void timerCallback() override {
        engine.collectGarbage(); drainRecording(); pollPlugins();
#if JUCE_ANDROID || JUCE_IOS
        if (++safeTick >= 15) { safeTick = 0; if (safeArea() != laidOut) resized(); }   // the notch and system bars moved without the window changing size (a rotation)
#endif
        if (linkDirty) { linkDirty = false; doc.commit(); }
        // loop record off: one pass, then stop; on, each audio pass becomes its own take (stacked)
        if (engine.isPlaying()) { const double pos = engine.positionTicks(); if (doc.rec && pos + 1 < lastPos && !engine.inCountIn()) { if (!props->getBoolValue("loopRec", true)) stopTransport(); else if (recAudioNow) { finishTake(); takeStartTick = doc.songStart * doc.p.bar(); } } lastPos = pos; }
        if (recAudioNow && !engine.isPlaying()) { recAudioNow = false; finishTake(); }
        // autosave every 2 minutes while there are unsaved changes, and a dated backup every 10 minutes (the last 20 kept)
        const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
        if (doc.dirty && props->getBoolValue("autosave", true) && now - lastAutosave > 120) {
            lastAutosave = now;
            try { us::saveProject(doc.p, appDir().getChildFile("autosave.undae.json"));
                if (props->getBoolValue("backups", true) && now - lastBackup > 600) { lastBackup = now; us::saveProject(doc.p, backupDir().getChildFile(juce::Time::getCurrentTime().formatted("%Y-%m-%d %H-%M ") + juce::File::createLegalFileName(juce::String(doc.p.name)) + ".undae.json"));
                    auto files = backupDir().findChildFiles(juce::File::findFiles, false, "*.undae.json"); std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.getLastModificationTime() > b.getLastModificationTime(); });
                    for (int i = 20; i < files.size(); i++) files.getReference(i).deleteFile(); }
            } catch (...) {}
        }
        // transport, meter, position, load
        // only what changed is redrawn: phones draw in software, and the whole bar 30 times a second kept a core busy
        const bool pl = engine.isPlaying(); const std::string ico = pl ? "stop" : "play"; if (playB.ico != ico || playB.getToggleState() != (pl || waiting)) { playB.ico = ico; playB.setOn(pl || waiting); playB.repaint(); }
        const float oLv = mLv, oHold = mHold, lv = std::max(engine.peak(0), engine.peak(1)); mLv = std::max(lv, mLv * 0.82f); mHold = std::max(lv, mHold * 0.97f); if (mLv < 1e-4f) mLv = 0; if (mHold < 1e-4f) mHold = 0;
        const juce::String oPos = posText;
        const double t = pl ? engine.positionTicks() : (doc.song ? doc.songStart * doc.p.bar() : 0);
        if (props->getValue("timeFmt", "bst") == "msc") { const double s = t * doc.p.secondsPerTick(); posText = juce::String((int)(s / 60)) + ":" + juce::String((int)std::fmod(s, 60)).paddedLeft('0', 2) + ":" + juce::String((int)(s * 100) % 100).paddedLeft('0', 2); }
        else posText = juce::String((int)(t / doc.p.bar()) + 1) + ":" + juce::String((int)(std::fmod(t, doc.p.bar()) / us::STEP) + 1).paddedLeft('0', 2) + ":" + juce::String((int)std::fmod(t, us::STEP)).paddedLeft('0', 2);
        if (midiAct.exchange(false)) { midiB.setOn(true); juce::Timer::callAfterDelay(150, [sp = juce::Component::SafePointer<Main>(this)] { if (sp) sp->midiB.setOn(false); }); }
        bool statsNew = false; if (now - lastStats > 1) { statsNew = true; lastStats = now; load.update(); if (load.cpu >= 0) { cpuHist.push_back((float)load.cpu); if (cpuHist.size() > 60) cpuHist.pop_front(); } }
        if (pl && doc.song && !doc.p.autos.empty()) { for (auto& l : doc.p.autos) if (auto tg = doc.target(l.target)) setLinked(l.target, tg->get()); }
        if (posText != oPos) repaint(posB.getBounds());
        if ((mLv != oLv || mHold != oHold) && meterR.getWidth() > 0) repaint(meterR.expanded(2, 6));
        if (statsNew && statsR.getWidth() > 0) repaint(statsR);
    }
    // what plug-ins report: parameters moved in their own windows (kept in the project), state changes (read and kept)
    void pollPlugins() {
        bool changed = false;
        auto apply = [&](const std::string& key, std::function<void(const juce::Identifier&, const juce::var&)> put) {
            auto pi = engine.plugin(key); if (!pi) return;
            for (auto& e : pi->takeEvents()) {
                if (e.kind == 0) { for (auto& [id, v] : e.changes) { put(juce::Identifier("v" + juce::String(id)), v); const std::string pref = key.rfind("ch:", 0) == 0 ? "plug:" + key.substr(3) + ":" : key + ":"; doc.autoWrite(pref + "v" + std::to_string(id), v); } changed = true; }
                else if (e.kind == 1) std::thread([pi, key, sp = juce::Component::SafePointer<Main>(this)] { const auto st = pi->readState(); if (st.empty()) return;
                    juce::MessageManager::callAsync([sp, key, st] { if (sp) sp->putState(key, st); }); }).detach();
            }
        };
        for (auto& c : doc.p.channels) if (c.type == "plugin") { const std::string id = c.id; apply("ch:" + id, [this, id](const juce::Identifier& k, const juce::var& v) { for (auto& x : doc.p.channels) if (x.id == id) Doc::editablePlug(x).getDynamicObject()->setProperty(k, v); }); }
        for (auto& b : doc.p.buses) for (auto& f : b.fx) if (f.type == "plugin") { const std::string bid = b.id, fid = f.id; apply("fx:" + bid + ":" + fid, [this, bid, fid](const juce::Identifier& k, const juce::var& v) { if (auto* bb = doc.bus(bid)) for (auto& x : bb->fx) if (x.id == fid) x.p.set(k, v); }); }
        if (changed) { doc.dirty = true; doc.live(); }
    }
    void putState(const std::string& key, const std::string& st) {
        if (key.rfind("ch:", 0) == 0) { for (auto& x : doc.p.channels) if ("ch:" + x.id == key) Doc::editablePlug(x).getDynamicObject()->setProperty("state", juce::String(st)); }
        else for (auto& b : doc.p.buses) for (auto& f : b.fx) if ("fx:" + b.id + ":" + f.id == key) f.p.set("state", juce::String(st));
        doc.dirty = true; doc.live();
    }
};

}   // namespace ui

class App : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "Undae Sonicae"; }
    const juce::String getApplicationVersion() override { return "0.3.0"; }
    void initialise(const juce::String& cmd) override {
        win = std::make_unique<Window>();
        auto* m = static_cast<ui::Main*>(win->getContentComponent());
        juce::StringArray args = juce::StringArray::fromTokens(cmd, true); bool play = false, recTest = false; double quitAfter = 0, playDelay = 0.3; juce::File snap; int va = -1, vb = -1; std::vector<std::pair<int, int>> clicks;
        for (int i = 0; i < args.size(); i++) {
            const juce::String a = args[i].unquoted();
            if (a == "--play") play = true;
            else if (a == "--bench-paint") juce::Timer::callAfterDelay(1500, [m] { m->benchPaint(); });
            else if (a == "--play-at" && i + 1 < args.size()) { play = true; playDelay = args[++i].getDoubleValue(); }   // e.g. after the engine has gone idle
            else if (a == "--record-audio") { m->armAudioRecording(); recTest = true; }
            else if (a == "--quit-after" && i + 1 < args.size()) quitAfter = args[++i].getDoubleValue();
            else if (a == "--snapshot" && i + 1 < args.size()) snap = juce::File::getCurrentWorkingDirectory().getChildFile(args[++i].unquoted());
            else if (a == "--settings-tab" && i + 1 < args.size()) ui::SettingsView::startTab() = args[++i].unquoted();   // e.g. audio (snapshots, tests)
            else if (a == "--size" && i + 1 < args.size()) { auto s = args[++i]; win->setSize(s.upToFirstOccurrenceOf("x", false, false).getIntValue(), s.fromFirstOccurrenceOf("x", false, false).getIntValue()); }
            else if (a == "--channel" && i + 1 < args.size()) { const auto nm = args[++i].unquoted().toStdString(); for (auto& c : m->document().p.channels) if (c.name == nm) m->document().selectChannel(c.id); }
            else if (a == "--export" && i + 1 < args.size()) { auto f = juce::File::getCurrentWorkingDirectory().getChildFile(args[++i].unquoted()); try { m->exportTo(f, true, f.getFileExtension().substring(1).toLowerCase().toStdString()); std::cout << "exported " << f.getFullPathName() << " (" << f.getSize() / 1024 << " KB)\n"; } catch (const std::exception& e) { std::cout << "export failed: " << e.what() << "\n"; } }
            else if (a == "--views" && i + 1 < args.size()) { auto v = args[++i]; va = v.substring(0, 1).getIntValue(); vb = v.substring(1, 2).getIntValue(); }   // e.g. 23: Song over Mixer (rack, roll, song, mixer, inst, design, lab, lib, set)
            else { auto f = juce::File::getCurrentWorkingDirectory().getChildFile(a); if (f.existsAsFile()) m->open(f); }
        }
        if (va >= 0) m->showViews(va, vb);
        if (play) juce::Timer::callAfterDelay((int)(playDelay * 1000), [m] { m->startPlaying(); });
        if (quitAfter > 0) juce::Timer::callAfterDelay((int)(quitAfter * 1000), [this, m, snap, recTest] {
            if (recTest) m->stopAndReport();
            if (snap != juce::File()) { auto img = m->createComponentSnapshot(m->getLocalBounds(), true, 1.0f); snap.deleteFile(); juce::FileOutputStream os(snap); juce::PNGImageFormat().writeImageToStream(img, os); }
            m->report(); quit(); });
    }
    void shutdown() override { win = nullptr; }
    void systemRequestedQuit() override { if (static_cast<ui::Main*>(win->getContentComponent())->okToQuit()) quit(); }
    void anotherInstanceStarted(const juce::String&) override {}
    struct Window : juce::DocumentWindow {
        Window() : DocumentWindow("Undae Sonicae", ui::BG, allButtons) {
            setUsingNativeTitleBar(true); setContentOwned(new ui::Main(), true);
#if JUCE_ANDROID || JUCE_IOS
            setFullScreen(true);
#else
            setResizable(true, true); setResizeLimits(640, 480, 8000, 5000); centreWithSize(getWidth(), getHeight());
#endif
            setVisible(true); }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
        // the screen's details changed (on a phone turning round, the notch and system bars move; Android can report that after
        // the new size, so the window's size alone may not change): lay the app out again for the new safe area
        void parentSizeChanged() override { DocumentWindow::parentSizeChanged(); if (auto* c = getContentComponent()) c->resized(); }
    };
    std::unique_ptr<Window> win;
};

START_JUCE_APPLICATION(App)
