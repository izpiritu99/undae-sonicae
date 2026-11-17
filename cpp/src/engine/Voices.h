// Voices: one playing note. SynthVoice is the subtractive / FM synth of app/js/audio/synth.js (three oscillators with
// morph and unison, sub, noise, FM, ring, two filters, shaper, four envelopes, three LFOs and an 8-slot mod matrix);
// SampleVoice is the pitched sample player. Times are seconds on the engine clock.
#pragma once
#include <memory>
#include "Dsp.h"
#include "Project.h"
#include "Samples.h"

namespace us {

struct NoteOpts {
    double bend = 0;          // cents (pitch wheel, master pitch, per-note fine pitch)
    double wheel = 0;         // mod wheel 0..1
    double pan = 0;           // per-note pan -1..1
    double rel = 0, cut = 0, res = 0;   // per-note release / cutoff / resonance offsets, -1..1
    int glideFrom = -1;       // previous note on the channel (synth glide)
    double bpm = 120;
};

class Voice {
public:
    virtual ~Voice() = default;
    int note = 60; double t0 = 0;          // start time
    double end = 1e30;                     // when it is silent for good
    bool released = false;
    virtual bool stereo() const = 0;
    // add n samples starting at engine time t into L/R (stereo voices) or M (mono voices)
    virtual void render(float* L, float* R, float* M, int n, double t, double sr) = 0;
    virtual void release(double t) = 0;
    virtual void kill(double t) = 0;
    virtual void slide(int to, double t, double d) = 0;
    virtual void setBend(double cents) {}   // pitch wheel (absolute offset in cents, master pitch included)
    virtual void setWheel(double w) {}      // mod wheel 0..1
};

// A pitch offset in cents that can glide (slide notes) from one value to another.
struct Glider {
    double v = 0, from = 0, to = 0, ts = -1, te = -1;
    double at(double t) const { if (ts < 0 || t <= ts) return ts < 0 ? v : from; if (t >= te) return to; return from + (to - from) * (t - ts) / (te - ts); }
    void glide(double target, double t, double d) { from = at(t); to = target; ts = t; te = t + std::max(0.01, d); v = target; }
};

std::unique_ptr<Voice> makeSynthVoice(const SynthP& P, int note, double vel, double t, double dur, const NoteOpts& o, double sr);
std::unique_ptr<Voice> makeSampleVoice(SampleRef smp, const Channel& C, int note, double vel, double t, double dur, const NoteOpts& o, double sr);

}   // namespace us
