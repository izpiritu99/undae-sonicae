// The open document: the project being edited, its file, undo history and the link to the engine.
// Views edit `p` and then call commit() (an undoable step) or live() (while dragging: the engine hears it at once).
#pragma once
#include <deque>
#include <set>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <optional>
#include "engine/Engine.h"

namespace ui {

// what the mixer shows for each effect parameter (app/js/audio/fx.js params)
struct FxParam { const char* k; const char* label; double lo, hi, def; bool expo; std::vector<std::string> opts; };
struct FxSpec { const char* type; const char* name; std::vector<FxParam> params; };
const std::vector<FxSpec>& fxSpecs();
const FxSpec* fxSpec(const std::string& type);
us::FxSlot newFx(const std::string& type);
us::SynthP defSynth();
us::Channel newChannel(const us::Project& p, const std::string& type, const std::string& name, const std::string& sample = "inst:keys");
us::Project blankProject();

class Doc : public juce::ChangeBroadcaster {
public:
    explicit Doc(us::Engine& e, us::SampleBank& b) : eng(e), bank(b) {}
    us::Engine& eng; us::SampleBank& bank;
    us::Project p; juce::File file; bool dirty = false;
    std::string ch, pat; bool song = false;   // current channel, pattern, transport mode
    // live UI state (state.js S): channels picked with Ctrl+click, the song's play marker (ticks), record / metronome flags
    std::set<std::string> chSel; double songStart = 0; bool rec = false, recAuto = false;
    juce::PropertiesFile* props = nullptr;   // app preferences (state.js prefs)
    bool pb(const char* k, bool def) const { return props ? props->getBoolValue(k, def) : def; }
    double pd(const char* k, double def) const { return props ? props->getDoubleValue(k, def) : def; }
    juce::String ps(const char* k, const juce::String& def) const { return props ? props->getValue(k, def) : def; }
    void setp(const char* k, const juce::var& v) { if (props) { props->setValue(k, v); props->saveIfNeeded(); } }
    std::function<void(const std::string& to, const std::string& from)> gotoView;   // show a view in a deck (bus 'goto')
    void go(const std::string& to, const std::string& from = {}) { if (gotoView) gotoView(to, from); }
    std::function<void()> prefsChanged;   // theme / layout preferences changed: the window re-applies them
    std::set<std::string> selectedChannels() const { std::set<std::string> s; for (auto& id : chSel) for (auto& c : p.channels) if (c.id == id) s.insert(id); if (s.empty() && !ch.empty()) s.insert(ch); return s; }
    std::string addBus(const std::string& name, const std::string& kind = "insert");   // inserts go before the sends
    bool reaches(const std::string& from, const std::string& to) const;   // would audio from `from` arrive at `to` (sends)?
    // automation / MIDI link targets: "bus:<id>:vol|pan", "ch:<id>:vol|pan", "fx:<bus>:<fx>:<param>", "plug:<ch>:v<id>", "master:pitch"
    struct Target { juce::String label; double min = 0, max = 1; std::function<double()> get; std::function<void(double)> set; };
    std::optional<Target> target(const std::string& t);
    void autoWrite(const std::string& target, double v);   // while recording automation in song mode: a point at the play position
    us::AutoLane* lane(const std::string& target) { for (auto& l : p.autos) if (l.target == target) return &l; return nullptr; }
    void deleteChannels(const std::set<std::string>& ids);

    void load(const juce::File& f);           // throws on a bad file
    void setProject(us::Project np, const juce::File& f = {});
    void commit();                            // an edit is done: undo step, engine, views
    void live();                              // an edit is under way (dragging): engine only
    void undo(); void redo();
    bool canUndo() const { return hi > 0; } bool canRedo() const { return hi + 1 < (int)hist.size(); }
    void save(const juce::File& f);           // throws on failure

    us::Channel* chan(); us::Pattern* patt();
    us::Pattern* pattern(const std::string& id) { for (auto& x : p.patterns) if (x.id == id) return &x; return nullptr; }
    us::Bus* bus(const std::string& id) { for (auto& b : p.buses) if (b.id == id) return &b; return nullptr; }
    juce::String busLabel(const std::string& id) const;
    void selectChannel(const std::string& id) { ch = id; sendChangeMessage(); }
    void selectPattern(const std::string& id) { pat = id; eng.setPattern(id); sendChangeMessage(); }
    // audio files: stored inside the project (so they save with it) and decoded into the sample bank
    std::string importAudio(const juce::File& f);           // sample id, or "" when the file is not audio
    std::string addSampleData(const std::string& name, const juce::MemoryBlock& bytes);   // recordings
    void addSampleChannel(const std::string& sampleId);
    void placeAudio(const std::string& sampleId, double startBar = -1, int track = -1);   // -1: after everything / first free track
    // a channel's plug-in slot is shared between undo snapshots: copy it before changing it
    static juce::var& editablePlug(us::Channel& c) { if (auto* o = c.plug.getDynamicObject()) c.plug = juce::var(new juce::DynamicObject(*o)); else c.plug = juce::var(new juce::DynamicObject()); return c.plug; }
    void audition(int note, bool on, double vel = 0.8) { if (auto* c = chan()) { if (on) eng.noteOn(c->id, note, vel); else eng.noteOff(c->id, note); } }
private:
    std::vector<std::shared_ptr<us::Project>> hist; int hi = -1; bool uncommitted = false;
    std::map<std::string, double> lastAutoTick;
    std::deque<std::shared_ptr<us::Project>> recent;   // keeps live snapshots alive, so the audio thread never frees one
    void push(std::shared_ptr<us::Project> s);
};

}   // namespace ui
