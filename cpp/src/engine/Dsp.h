// DSP building blocks with the same behaviour as the Web Audio nodes the JavaScript version is built from, so a project
// sounds the same in both: biquad filters (Web Audio's cookbook formulas, Q in dB for low/high-pass), the equal-power
// stereo panner, wave shapers, band-limited oscillators (normalised like PeriodicWave), and AudioParam-style timelines.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace us {

constexpr double PI = 3.14159265358979323846;
inline double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
inline double mtof(double n) { return 440.0 * std::pow(2.0, (n - 69.0) / 12.0); }
inline double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

// Small fast random source (xorshift); audio code must not lock or allocate, which std::rand may.
struct Rng {
    uint64_t s = 0x9E3779B97F4A7C15ull;
    explicit Rng(uint64_t seed = 0) { if (seed) s = seed * 0x9E3779B97F4A7C15ull + 1; }
    double next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (double)(s >> 11) * (1.0 / 9007199254740992.0); }   // 0..1
    double bi() { return next() * 2.0 - 1.0; }
};
Rng& globalRng();

// --- biquad --------------------------------------------------------------------------------------------------------
enum class FilterType { Lowpass, Highpass, Bandpass, Lowshelf, Highshelf, Peaking, Notch, Allpass };
FilterType filterTypeFrom(const std::string& s);

struct Biquad {
    FilterType type = FilterType::Lowpass;
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    // frequency in Hz (detune in cents already applied), q as Web Audio's Q, gain in dB (shelves / peaking)
    void set(double sr, double freq, double q, double gainDb);
    inline double tick(double x) {
        double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        if (std::abs(y) < 1e-30) y1 = 0;   // no denormals
        return y;
    }
    void reset() { x1 = x2 = y1 = y2 = 0; }
};
// A stereo biquad (one state per channel, shared coefficients).
struct Biquad2 {
    Biquad l, r;
    void set(double sr, double f, double q, double g) { l.set(sr, f, q, g); r.b0 = l.b0; r.b1 = l.b1; r.b2 = l.b2; r.a1 = l.a1; r.a2 = l.a2; r.type = l.type; }
    void setType(FilterType t) { l.type = r.type = t; }
    void tick(float& L, float& R) { L = (float)l.tick(L); R = (float)r.tick(R); }
};

// --- stereo panner (Web Audio StereoPannerNode, equal power) ---------------------------------------------------------
inline void panMono(float in, double pan, float& L, float& R) {
    double x = (clampd(pan, -1, 1) + 1) * 0.5;
    L = (float)(in * std::cos(x * PI / 2)); R = (float)(in * std::sin(x * PI / 2));
}
inline void panStereo(float& L, float& R, double pan) {
    pan = clampd(pan, -1, 1);
    if (pan <= 0) { double x = pan + 1; float l = L + R * (float)std::cos(x * PI / 2), r = R * (float)std::sin(x * PI / 2); L = l; R = r; }
    else { double x = pan; float l = L * (float)std::cos(x * PI / 2), r = R + L * (float)std::sin(x * PI / 2); L = l; R = r; }
}

// --- wave shaper (Web Audio WaveShaperNode curve lookup with linear interpolation) ------------------------------------
struct Shaper {
    std::vector<float> curve;
    inline float apply(float x) const {
        if (curve.empty()) return x;
        const size_t n = curve.size();
        double v = (n - 1) * 0.5 * (x + 1.0);
        if (v <= 0) return curve[0];
        if (v >= n - 1) return curve[n - 1];
        size_t k = (size_t)v; double f = v - k;
        return (float)((1 - f) * curve[k] + f * curve[k + 1]);
    }
};
// The synth's drive / shaper curves (synth.js curve()).
std::vector<float> shapeCurve(const std::string& type, double amt);

// --- band-limited oscillators ----------------------------------------------------------------------------------------
// A shape as Fourier coefficients (cos in re, sin in im), rendered into one table per harmonic budget so no harmonic
// passes the Nyquist frequency, and normalised to a peak of 1 like Web Audio's PeriodicWave.
class WaveTable {
public:
    static constexpr int SIZE = 2048;
    static const WaveTable& get(const std::string& name);   // sine, triangle, sawtooth, square, pulse25, softsaw, organ…
    static void warmUp();                                    // build every shape now, so the audio thread never has to
    // phase 0..1, freq in Hz, sample rate: picks the table with as many harmonics as fit below Nyquist
    inline float read(double phase, double freq, double sr) const {
        int maxH = freq > 0 ? (int)(sr * 0.5 / freq) : 1024;
        int k = 0; while (k + 1 < (int)tables.size() && harm[k] > maxH) k++;
        const float* t = tables[k].data();
        double p = phase * SIZE; int i = (int)p; double f = p - i; i &= SIZE - 1;
        return (float)(t[i] + f * (t[(i + 1) & (SIZE - 1)] - t[i]));
    }
    WaveTable(const std::vector<double>& re, const std::vector<double>& im);
private:
    std::vector<std::array<float, SIZE>> tables; std::vector<int> harm;
};

// --- AudioParam timeline (setValueAtTime, linear / exponential ramps, setTargetAtTime) ---------------------------------
// Used where the JavaScript schedules envelopes as automation events: the drum kit, the keys sound, sweeps.
class Timeline {
public:
    explicit Timeline(double def = 0) : def(def) {}
    Timeline& set(double v, double t) { ev.push_back({ 0, t, v, 0 }); return *this; }
    Timeline& lin(double v, double t) { ev.push_back({ 1, t, v, 0 }); return *this; }
    Timeline& exp(double v, double t) { ev.push_back({ 2, t, v, 0 }); return *this; }
    Timeline& target(double v, double t, double tau) { ev.push_back({ 3, t, v, tau }); return *this; }
    double at(double t);   // call with increasing t for speed
private:
    struct Ev { int kind; double t, v, tau; double start = 0; };
    std::vector<Ev> ev; double def; bool ready = false; size_t cur = 0;
    void prepare();
    double valueBefore(size_t i);
};

// --- ADSR as the synth schedules it (synth.js adsr()) -------------------------------------------------------------------
// Attack: linear ramp to peak; decay: exponential approach to sustain (time constant D/3); release: exponential approach to
// the base (R/4). dur < 0 means held until release().
struct Adsr {
    double a = 0.005, d = 0.25, s = 0.7, r = 0.2, peak = 1, base = 0;
    double dur = -1, relAt = -1, relFrom = 0;   // seconds since note start
    void start(double A, double D, double S, double R, double pk, double b, double duration) {
        a = std::max(A, 0.002); d = std::max(D, 0.005); r = std::max(R, 0.005); s = S; peak = pk; base = b; dur = duration; relAt = -1;
        if (dur >= 0) { relAt = dur; relFrom = shapeAt(dur); }
    }
    double shapeAt(double t) const {   // the attack / decay part
        if (t <= 0) return base;
        if (dur >= 0 && dur <= a) return base + peak * std::min(t, dur) / a;
        if (t < a) return base + peak * t / a;
        return base + peak * (s + (1 - s) * std::exp(-(t - a) / (d / 3)));
    }
    double at(double t) const {
        if (relAt >= 0 && t >= relAt) return base + (relFrom - base) * std::exp(-(t - relAt) / (r / 4));
        return shapeAt(t);
    }
    void release(double t) { if (relAt >= 0 && relAt <= t) return; relFrom = at(t); relAt = t; }
    double endTime() const { return relAt >= 0 ? relAt + r * 1.5 : 1e30; }
};

// --- delay line ----------------------------------------------------------------------------------------------------
struct DelayLine {
    std::vector<float> buf; size_t w = 0;
    void init(size_t maxSamples) { buf.assign(maxSamples + 4, 0.f); w = 0; }
    inline void push(float x) { buf[w] = x; w = (w + 1) % buf.size(); }
    inline float read(double delaySamples) const {   // linear interpolation, delaySamples >= 1
        double p = (double)w - delaySamples; while (p < 0) p += buf.size();
        size_t i = (size_t)p; double f = p - i; size_t j = (i + 1) % buf.size();
        return (float)(buf[i % buf.size()] * (1 - f) + buf[j] * f);
    }
};

}   // namespace us
