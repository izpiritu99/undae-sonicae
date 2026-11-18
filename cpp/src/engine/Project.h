// The project as the engine sees it, read from the same JSON the JavaScript version saves (.undae.json files, or a bare
// project object). Field names and defaults follow app/js/state.js.
#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <juce_core/juce_core.h>

namespace us {

constexpr int PPQ = 96, STEP = 24;

struct Note { std::string id; double t = 0, d = STEP, v = 0.8; int n = 60; double pan = 0, fine = 0, rel = 0, cut = 0, res = 0; bool mute = false, slide = false, hasCut = false, hasRes = false; };
struct Loop { bool on = false; double a = 0, b = 0; };
struct Pattern { std::string id, name, color; double bars = 1; std::map<std::string, std::vector<Note>> notes; Loop loop; };

struct EnvP { double a = 0.005, d = 0.25, s = 0.7, r = 0.2; };
struct OscP { bool on = true; std::string wave = "sawtooth", wave2 = "square"; double oct = 0, semi = 0, fine = 0, level = 0.8, morph = 0, pan = 0; };
struct FilterP { bool on = true; std::string type = "lowpass"; double cutoff = 6000, q = 1, env = 0, key = 0, drive = 0; };
struct LfoP { std::string wave = "sine"; double rate = 5, sync = 0, fade = 0; };
struct ModP { std::string src = "none", dst = "none"; double amt = 0; };
// Physical model voice source, beside the oscillators: "pluck" (Karplus-Strong string: a noise burst circulating in a
// tuned delay with a damping filter) or "mallet" (struck modal resonators of a bar, glass, wood or bell).
struct ModelP { std::string type = "off", mat = "bar"; double decay = 2, bright = 0.6, pos = 0.2, hard = 0.5, level = 0.8; };
struct SynthP {
    OscP osc[3];
    int uni = 1; double detune = 14, spread = 0.5, drift = 0, sub = 0; std::string subWave = "sine"; double subOct = -1, noise = 0, noiseTone = 1, fm = 0, ring = 0;
    FilterP f1, f2; std::string froute = "serial"; std::string shaperType = "off"; double shaperAmt = 0.3;
    EnvP aenv, fenv{ 0.005, 0.3, 0.2, 0.3 }, menv{ 0.01, 0.4, 0, 0.3 }, menv2{ 0.3, 1, 0.5, 0.5 };
    LfoP lfo[3]; std::vector<ModP> mod; double macro[4] = { 0, 0, 0, 0 };
    ModelP pm;
    std::string mode = "poly"; double glide = 0; int voices = 8; double vel = 0.6, gain = 0.7, bendRange = 2;
};

// Granular playback of a sample channel: grains of `size` s, `density` per second, taken around `pos` (0..1 of the
// sample) which moves at `scan` (sample lengths per second), scattered by `jitter` (fraction of the sample), each
// re-pitched by up to `spray` semitones and reversed with probability `rev`, spread across the stereo field.
// A multi-sample zone: notes lo..hi at velocities vlo..vhi (0-127) play `sample`, re-pitched from `root`, `gain` dB.
// Zones covering the same notes and velocities take turns (round robin).
struct Zone { std::string sample; int lo = 0, hi = 127, root = 60, vlo = 0, vhi = 127; double gain = 0; };
struct GranP { bool on = false; double pos = 0.3, scan = 0, size = 0.08, density = 24, jitter = 0.03, spray = 0, spread = 0.6, rev = 0; };

struct Channel {
    std::string id, name, type = "sample", color = "#5aa9ff";   // type: sample | synth | plugin
    double vol = 0.8, pan = 0; bool mute = false, solo = false;
    std::string bus = "master"; int root = 60; std::string sample = "inst:keys";
    double tune = 0, fine = 0, start = 0, att = 0, rel = 0.08; bool gate = false, rev = false, cut = false; int choke = 0;
    GranP gran; std::vector<Zone> zones;   // zones, when present, replace `sample`
    std::optional<SynthP> synth;
    juce::var plug;   // plug-in slot { path, pid, name, fmt, state, v<id>... }
    std::string group;   // rack group (Alt+G)
};

struct FxSlot { std::string id, type; bool on = true; juce::NamedValueSet p; double num(const char* k, double def) const; std::string str(const char* k, const std::string& def) const; };
struct Send { std::string to; double amt = 0; };
struct Bus {
    std::string id, name, kind = "insert";   // master | insert | send
    double vol = 1, pan = 0, width = 1; bool mute = false, solo = false, mono = false, inv = false, toMaster = true;
    std::vector<Send> sends; std::vector<FxSlot> fx;
    std::string input; bool arm = false, mon = false, stereo = false;   // live input: device channel ("" none, "default"), record arm, monitor
};
struct Clip { std::string id, pat, color; int track = 0; double start = 0, len = 1, off = 0; bool mute = false; std::string group; };
struct AudioClip { std::string id, sample, name, bus = "master", color, group; int track = 0; double start = 0, len = 1, off = 0, gain = 1, bpm = 0; bool stretch = true, mute = false; };
struct AutoPt { double t = 0, v = 0; };
struct AutoLane { std::string id, target; std::vector<AutoPt> pts; };
struct Marker { std::string id, name; double t = 0; };   // bars
struct Link { std::string id, target; int ch = 0, cc = 0; };   // a MIDI controller linked to an automation target

struct Project {
    std::string name = "Untitled";
    double bpm = 120, swing = 0, masterPitch = 0; int keyRoot = 0; std::string keyScale = "chromatic"; int swingDiv = 16, sigN = 4, sigD = 4;
    std::vector<Channel> channels; std::vector<Pattern> patterns; std::vector<Bus> buses;
    int tracks = 6; std::vector<Clip> clips; std::vector<AudioClip> audio; std::vector<AutoLane> autos; std::vector<Marker> markers;
    std::vector<Link> links; std::string rackGroup;
    struct Arr { std::string id, name; juce::var song; };   // arrangements: the current one is the song above, the others keep their song as JSON
    std::vector<Arr> arrs; std::string arrCur;   // rack filter: "" all, "-" unsorted, or a group name
    // samples carried inside the file (id -> encoded audio file bytes, and their names); shared by every snapshot
    struct Embedded { std::map<std::string, juce::MemoryBlock> data; std::map<std::string, std::string> names; };
    std::shared_ptr<Embedded> embedded = std::make_shared<Embedded>();
    juce::var raw;   // the project object as read: fields the native edition does not use yet are written back from it

    int beat() const { return PPQ * 4 / sigD; }
    int bar() const { return sigN * beat(); }
    int songBars() const;
    const Pattern* pattern(const std::string& id) const;
    const Bus* findBus(const std::string& id) const;
    const Channel* findChannel(const std::string& id) const;
    double secondsPerTick() const { return 60.0 / bpm / PPQ; }
};

// Throws std::runtime_error with a readable message when the file is not a project.
Project loadProject(const juce::File& file);
Project parseProject(const juce::var& root);
// The project as the app's JSON: every field the engine knows, merged into what was read (unknown fields kept, matched by id).
juce::var projectToVar(const Project& p);
void saveProject(const Project& p, const juce::File& file);   // .undae.json with the embedded samples
std::string newId();   // like util.js uid()
juce::var songToVar(const Project& p);                 // the current song (tracks, clips, audio, automation, markers) as JSON
void songFromVar(Project& p, const juce::var& song);   // replace the current song

}   // namespace us
