// The engine: sequencer (the JavaScript collect()), channel strips with their voices, the mixer graph (inserts, sends,
// master, solo / mute, effects), song automation and audio clips. Renders offline (export) or in real time (the app).
#pragma once
#include <array>
#include <map>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <functional>
#include <juce_audio_basics/juce_audio_basics.h>
#include "AtomicShared.h"
#include "Effects.h"
#include "Project.h"
#include "Samples.h"
#include "Voices.h"
#include "PluginHost.h"

namespace us {

enum class Mode { Pattern, Song };

struct Event { const Channel* c; int n; double v, d, tick, sw; double pan, fine, rel, cut, res; bool slide, hasCut, hasRes; };
// Every note that starts in the linear tick window [a, b), looping the pattern (or its Piano Roll loop) or the song.
std::vector<Event> collect(const Project& P, Mode mode, const Pattern* pt, double a, double b, bool once = false);
double swingShift(const Project& P, double t);
// Pitch-preserving time stretch (WSOLA), as app/js/audio/stretch.js.
std::shared_ptr<Sample> stretchSample(const Sample& s, double ratio);

class Engine {
public:
    explicit Engine(std::shared_ptr<SampleBank> bank);
    ~Engine();
    void setProject(std::shared_ptr<Project> p);            // message thread: a new project (stops playback)
    // message thread: an edited snapshot. Level, pan, effect settings, notes and clips apply on the next audio block
    // without locking; a change of channels, buses, effects or routing builds a new graph here and swaps it in.
    void update(std::shared_ptr<Project> p);
    void collectGarbage();
    void reloadPlugins();   // load every plug-in again (after switching to or from a plug-in server)                                  // message thread, now and then: frees what the audio thread let go
    // plug-ins: instances for the live graph are kept across edits (loading one is slow), keyed "ch:<channel>" or
    // "fx:<bus>:<effect>". The UI reads their parameters and events through these.
    std::shared_ptr<PluginInstance> plugin(const std::string& key) const { std::lock_guard<std::mutex> l(poolLock); auto it = pool.find(key); return it == pool.end() ? nullptr : it->second.inst; }
    std::string pluginStatus(const std::string& key) const { std::lock_guard<std::mutex> l(poolLock); auto it = pool.find(key); return it == pool.end() ? std::string() : *it->second.status; }
    float busPeak(size_t index) const { return index < busPeaks.size() ? busPeaks[index].load() : 0.f; }
    // the lowest and highest sample on a bus since the last call (the mixer's scrolling waveforms)
    std::pair<float, float> busWave(size_t index) { if (index >= busLo.size()) return { 0.f, 0.f }; return { busLo[index].exchange(0.f), busHi[index].exchange(0.f) }; }
    float inputPeak(int ch) const { return ch >= 0 && ch < 16 ? inPeaks[(size_t)ch].load() : 0.f; }   // device input level (record arm buttons)
    void setPattern(const std::string& id) { std::lock_guard<std::mutex> q(queueLock); pendingPat = id; patChanged = true; }
    std::shared_ptr<const Project> project() const { return proj; }
    void prepare(double sampleRate, int maxBlock);

    // Offline: the whole song, or the pattern `loops` times, plus 2.5 s of tails (engine.render in the browser version).
    juce::AudioBuffer<float> render(Mode mode, const std::string& patId, int loops, double sampleRate, std::function<void(double)> progress = {});

    // Real time
    void play(Mode mode, const std::string& patId, double fromTick = 0, double countInBars = 0);   // count-in: that many bars of clicks first
    void panic();                    // silence every voice and plug-in now
    std::atomic<bool> metronome{ false };   // a click on every beat while playing (accent on the bar)
    // live input (audio thread, before process): buses with an input and monitoring on hear it
    // the input tap (Audio Lab: recording the microphone, the live tuner): channel 1 (or 1 + 2), mono, while switched on
    std::atomic<bool> tapOn{ false };
    int readTap(float* dst, int max) { int s1, n1, s2, n2; const int r = std::min(max, tapFifo.getNumReady()); tapFifo.prepareToRead(r, s1, n1, s2, n2); std::copy_n(tapBuf.data() + s1, n1, dst); std::copy_n(tapBuf.data() + s2, n2, dst + n1); tapFifo.finishedRead(n1 + n2); return n1 + n2; }
    void setInput(const float* const* in, int nIn, int n) { inPtr = in; inCount = nIn;
        if (tapOn && nIn > 0 && in[0]) { int s1, n1, s2, n2; tapFifo.prepareToWrite(n, s1, n1, s2, n2); for (int i = 0; i < n1 + n2; i++) { const float v = nIn > 1 && in[1] ? 0.5f * (in[0][i] + in[1][i]) : in[0][i]; tapBuf[(size_t)(i < n1 ? s1 + i : s2 + i - n1)] = v; } tapFifo.finishedWrite(n1 + n2); }
        for (int c = 0; c < nIn && c < 16; c++) { float pk = 0; if (in[c]) for (int i = 0; i < n; i++) pk = std::max(pk, std::abs(in[c][i])); inPeaks[(size_t)c] = std::max(pk, inPeaks[(size_t)c].load() * 0.9f); } }
    bool inCountIn() const { return countingIn.load(); }
    void stop();
    bool isPlaying() const { return playing.load(); }
    double positionTicks() const { return posTicks.load(); }
    void noteOn(const std::string& chanId, int note, double vel);   // live playing (keyboard, MIDI)
    void noteOff(const std::string& chanId, int note);
    void pitchBend(double v);        // -1..1 (the channel's bend range: a synth's own, 2 semitones for samplers)
    void modWheel(double v);         // 0..1
    void sustain(bool down);         // held notes keep sounding until the pedal comes up
    void previewSample(const std::string& sampleId);   // the library's audition: plays once on the master
    // MIDI recording: notes played while playing land in the current pattern (taken by the UI with takeRecorded)
    struct RecNote { std::string ch; int note; double vel, tick, dur; };
    void setRecording(bool on) { recording = on; }
    bool isRecording() const { return recording.load(); }
    std::vector<RecNote> takeRecorded();
    void process(float* L, float* R, int n);                         // audio thread
    float peak(int ch) const { return peaks[ch].load(); }
    int voiceCount() const { return voices.load(); }

private:
    struct Strip; struct BusNode; struct ClipPlay; struct Graph;
    std::shared_ptr<SampleBank> bank;
    std::shared_ptr<Project> proj;            // as last set
    std::unique_ptr<Graph> live;              // graph used by the audio thread
    std::mutex liveLock;                      // taken by the audio thread with try_lock only
    double sr = 48000; int maxBlock = 512; double quietFrames = 0;   // frames of silence while nothing could sound (see process)
    std::atomic<bool> playing{ false }; std::atomic<double> posTicks{ 0 };
    std::atomic<float> peaks[2]{ 0.f, 0.f }; std::atomic<int> voices{ 0 };
    std::array<std::atomic<float>, 64> busPeaks{}, busLo{}, busHi{}; std::array<std::atomic<float>, 16> inPeaks{};
    AtomicShared<Project> pendingProj; std::atomic<Graph*> pendingGraph{ nullptr }, retiredGraph{ nullptr };
    std::string liveKey; std::string pendingPat; bool patChanged = false;
    static std::string structureKey(const Project& p);
    struct Pooled { std::shared_ptr<PluginInstance> inst; std::string sig; std::shared_ptr<std::string> status; };
    std::map<std::string, Pooled> pool; mutable std::mutex poolLock;
    void syncPlugins(const Project& p);   // message thread: load what is new, drop what is gone
    // transport (audio thread)
    Mode mode = Mode::Pattern; std::string patId; double tNow = 0, aTick = 0, aTime = 0, nextTick = 0; bool startReq = false, stopReq = false; double startFrom = 0;
    struct LiveNote { std::string ch; int note; double vel; int kind; };   // kind: 0 off, 1 on, 2 bend, 3 wheel, 4 sustain, 5 preview
    std::atomic<bool> recording{ false }; std::vector<RecNote> recDone; std::map<std::pair<std::string, int>, std::pair<double, double>> recOpen;
    double liveBend = 0, liveWheel = 0; bool sustainDown = false;
    std::vector<LiveNote> liveQueue; std::mutex queueLock;
    juce::AbstractFifo tapFifo{ 1 << 18 }; std::vector<float> tapBuf = std::vector<float>(1 << 18);
    const float* const* inPtr = nullptr; int inCount = 0; std::atomic<bool> countingIn{ false }; double countIn = 0;
    struct Click { double t0; bool accent; }; std::vector<Click> clicks; double nextBeat = 0;
    void renderClicks(float* L, float* R, int n, double t);
    std::unique_ptr<Graph> build(std::shared_ptr<Project> p, double sampleRate, int block);
};

}   // namespace us
