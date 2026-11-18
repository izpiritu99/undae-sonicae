#include "Dsp.h"
#include <juce_dsp/juce_dsp.h>
#include <map>
#include <mutex>

namespace us {

Rng& globalRng() { static Rng r(12345); return r; }

FilterType filterTypeFrom(const std::string& s) {
    if (s == "highpass") return FilterType::Highpass;
    if (s == "bandpass") return FilterType::Bandpass;
    if (s == "lowshelf") return FilterType::Lowshelf;
    if (s == "highshelf") return FilterType::Highshelf;
    if (s == "peaking") return FilterType::Peaking;
    if (s == "notch") return FilterType::Notch;
    if (s == "allpass") return FilterType::Allpass;
    return FilterType::Lowpass;
}

// Web Audio spec, BiquadFilterNode: frequency clamped to 0..Nyquist; low/high-pass take Q in dB.
void Biquad::set(double sr, double freq, double q, double gainDb) {
    const double nyq = sr * 0.5;
    freq = clampd(freq, 0, nyq);
    const double w0 = 2 * PI * freq / sr, cw = std::cos(w0), sw = std::sin(w0), A = std::pow(10.0, gainDb / 40.0);
    double nb0 = 1, nb1 = 0, nb2 = 0, na0 = 1, na1 = 0, na2 = 0;
    switch (type) {
    case FilterType::Lowpass: {
        if (freq >= nyq) { b0 = 1; b1 = b2 = a1 = a2 = 0; return; }
        if (freq <= 0) { b0 = b1 = b2 = a1 = a2 = 0; return; }
        const double al = sw / (2 * std::pow(10.0, q / 20.0));
        nb0 = (1 - cw) / 2; nb1 = 1 - cw; nb2 = (1 - cw) / 2; na0 = 1 + al; na1 = -2 * cw; na2 = 1 - al; break;
    }
    case FilterType::Highpass: {
        if (freq >= nyq) { b0 = b1 = b2 = a1 = a2 = 0; return; }
        if (freq <= 0) { b0 = 1; b1 = b2 = a1 = a2 = 0; return; }
        const double al = sw / (2 * std::pow(10.0, q / 20.0));
        nb0 = (1 + cw) / 2; nb1 = -(1 + cw); nb2 = (1 + cw) / 2; na0 = 1 + al; na1 = -2 * cw; na2 = 1 - al; break;
    }
    case FilterType::Bandpass: {
        if (freq <= 0 || freq >= nyq || q <= 0) { b0 = b1 = b2 = a1 = a2 = 0; if (q <= 0 && freq > 0 && freq < nyq) b0 = 1; return; }
        const double al = sw / (2 * q);
        nb0 = al; nb1 = 0; nb2 = -al; na0 = 1 + al; na1 = -2 * cw; na2 = 1 - al; break;
    }
    case FilterType::Notch: {
        if (freq <= 0 || freq >= nyq) { b0 = 1; b1 = b2 = a1 = a2 = 0; return; }
        if (q <= 0) { b0 = b1 = b2 = a1 = a2 = 0; return; }
        const double al = sw / (2 * q);
        nb0 = 1; nb1 = -2 * cw; nb2 = 1; na0 = 1 + al; na1 = -2 * cw; na2 = 1 - al; break;
    }
    case FilterType::Allpass: {
        if (freq <= 0 || freq >= nyq) { b0 = 1; b1 = b2 = a1 = a2 = 0; return; }
        if (q <= 0) { b0 = -1; b1 = b2 = a1 = a2 = 0; return; }
        const double al = sw / (2 * q);
        nb0 = 1 - al; nb1 = -2 * cw; nb2 = 1 + al; na0 = 1 + al; na1 = -2 * cw; na2 = 1 - al; break;
    }
    case FilterType::Peaking: {
        if (freq <= 0 || freq >= nyq) { b0 = 1; b1 = b2 = a1 = a2 = 0; return; }
        if (q <= 0) { b0 = A * A; b1 = b2 = a1 = a2 = 0; return; }
        const double al = sw / (2 * q);
        nb0 = 1 + al * A; nb1 = -2 * cw; nb2 = 1 - al * A; na0 = 1 + al / A; na1 = -2 * cw; na2 = 1 - al / A; break;
    }
    case FilterType::Lowshelf: {
        if (freq >= nyq) { b0 = A * A; b1 = b2 = a1 = a2 = 0; return; }
        if (freq <= 0) { b0 = 1; b1 = b2 = a1 = a2 = 0; return; }
        const double al = sw / 2 * std::sqrt(2.0), sa = 2 * std::sqrt(A) * al;   // S = 1
        nb0 = A * ((A + 1) - (A - 1) * cw + sa); nb1 = 2 * A * ((A - 1) - (A + 1) * cw); nb2 = A * ((A + 1) - (A - 1) * cw - sa);
        na0 = (A + 1) + (A - 1) * cw + sa; na1 = -2 * ((A - 1) + (A + 1) * cw); na2 = (A + 1) + (A - 1) * cw - sa; break;
    }
    case FilterType::Highshelf: {
        if (freq >= nyq) { b0 = 1; b1 = b2 = a1 = a2 = 0; return; }
        if (freq <= 0) { b0 = A * A; b1 = b2 = a1 = a2 = 0; return; }
        const double al = sw / 2 * std::sqrt(2.0), sa = 2 * std::sqrt(A) * al;
        nb0 = A * ((A + 1) + (A - 1) * cw + sa); nb1 = -2 * A * ((A - 1) + (A + 1) * cw); nb2 = A * ((A + 1) + (A - 1) * cw - sa);
        na0 = (A + 1) - (A - 1) * cw + sa; na1 = 2 * ((A - 1) - (A + 1) * cw); na2 = (A + 1) - (A - 1) * cw - sa; break;
    }
    }
    b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0; a1 = na1 / na0; a2 = na2 / na0;
}

std::vector<float> shapeCurve(const std::string& type, double amt) {
    std::vector<float> c(1024);
    const double a = std::round(amt * 24) / 24, k = 1 + a * 28;
    for (int i = 0; i < 1024; i++) {
        const double x = i / 511.5 - 1;
        double y;
        if (type == "hard") y = clampd(x * (1 + a * 12), -1, 1);
        else if (type == "fold") y = std::sin(x * (1 + a * 7) * PI / 2);
        else if (type == "asym") y = (std::tanh((x + 0.35 * a) * k * 0.4) - std::tanh(0.35 * a * k * 0.4)) / std::tanh(k * 0.4);
        else if (type == "crush") { const double q = 2 + (1 - a) * 22; y = std::round(x * q) / q; }
        else y = std::tanh(x * k) / std::tanh(k);
        c[i] = (float)y;
    }
    return c;
}

// --- wave tables ---------------------------------------------------------------------------------------------------
const std::vector<WaveSet>& waveSets() {
    static const std::vector<WaveSet> v = { { "wt:sweep", "Saw to square" }, { "wt:bright", "Opening" }, { "wt:pulse", "Pulse width" }, { "wt:formant", "Vowel" },
                                            { "wt:sync", "Sync" }, { "wt:fold", "Fold" }, { "wt:glass", "Glass" } };
    return v;
}
// One frame of a wavetable: harmonic amplitudes from a formula, or the Fourier series of a time-domain cycle.
static bool frameCoefs(const std::string& name, std::vector<double>& re, std::vector<double>& im) {
    const size_t c = name.rfind(':'); if (c == std::string::npos || c < 4) return false;
    const std::string set = name.substr(0, c); const int k = std::atoi(name.c_str() + c + 1); const double t = (double)k / (WT_FRAMES - 1);
    const int N = 256; re.assign(N + 1, 0); im.assign(N + 1, 0);
    auto fromCycle = [&](auto fn) {   // FFT of a 4096-point cycle, the first N harmonics
        const int M = 4096; juce::dsp::FFT fft(12); std::vector<float> x(2 * M, 0.f); for (int i = 0; i < M; i++) x[(size_t)i] = (float)fn((double)i / M);
        fft.performRealOnlyForwardTransform(x.data(), true);
        for (int n = 1; n <= N; n++) { re[n] = 2.0 * x[(size_t)(2 * n)] / M; im[n] = -2.0 * x[(size_t)(2 * n + 1)] / M; }
    };
    if (set == "wt:sweep") for (int n = 1; n <= N; n++) im[n] = (n % 2 ? 1 : 1 - t) / n;
    else if (set == "wt:bright") for (int n = 1; n <= N; n++) im[n] = std::exp(-(n - 1) / (0.6 + t * t * 60)) / n;
    else if (set == "wt:pulse") { const double d = 0.5 - t * 0.45; for (int n = 1; n <= N; n++) { re[n] = 2 * std::sin(2 * PI * n * d) / (PI * n); im[n] = 2 * (1 - std::cos(2 * PI * n * d)) / (PI * n); } }
    else if (set == "wt:formant") { const double cc = 2 + t * 16; for (int n = 1; n <= N; n++) im[n] = std::exp(-(n - cc) * (n - cc) / 9.7) + 0.6 * std::exp(-(n - 2.6 * cc) * (n - 2.6 * cc) / 30) * 0.5 + 0.25 / n; }
    else if (set == "wt:sync") { const double r = 1 + t * 5; fromCycle([&](double p) { const double q = p * r; return 2 * (q - std::floor(q)) - 1; }); }
    else if (set == "wt:fold") { const double g = 1 + t * 6; fromCycle([&](double p) { return std::asin(std::sin(g * std::sin(2 * PI * p) * PI / 2)) * 2 / PI; }); }
    else if (set == "wt:glass") { static const int part[] = { 1, 2, 3, 5, 8, 13, 21, 34 }; for (int j = 0; j < 8; j++) { const double c2 = t * 7; im[part[j]] = std::exp(-std::abs(j - c2) * 0.9) / (1 + j * 0.15); } }
    else return false;
    return true;
}
static void coefsFor(const std::string& name, std::vector<double>& re, std::vector<double>& im) {
    if (isWaveSet(name) && frameCoefs(name, re, im)) return;
    const int N = name == "sine" || name == "triangle" || name == "sawtooth" || name == "square" ? 1024 : 64;
    re.assign(N + 1, 0); im.assign(N + 1, 0);
    for (int n = 1; n <= N; n++) {
        if (name == "sine") { if (n == 1) im[n] = 1; }
        else if (name == "sawtooth") im[n] = 2.0 / (n * PI) * (n % 2 ? 1 : -1);   // Web Audio: rises through zero at phase 0
        else if (name == "square") im[n] = n % 2 ? 4.0 / (n * PI) : 0;
        else if (name == "triangle") im[n] = n % 2 ? 8.0 / (PI * PI * n * n) * ((n / 2) % 2 ? -1 : 1) : 0;
        else if (name == "pulse25" || name == "pulse10") { const double d = name == "pulse25" ? 0.25 : 0.1; re[n] = 2 * std::sin(2 * PI * n * d) / (PI * n); im[n] = 2 * (1 - std::cos(2 * PI * n * d)) / (PI * n); }
        else if (name == "softsaw") im[n] = 1 / std::pow(n, 1.7);
        else if (name == "organ") { static const double o[] = { 0, 1, 0.8, 0.6, 0.3, 0, 0.4, 0, 0.3 }; im[n] = n < 9 ? o[n] : 0; }
        else if (name == "bell") { static const double b[] = { 0, 1, 0, 0.5, 0, 0, 0.35, 0, 0, 0.25, 0, 0, 0.2, 0, 0, 0, 0.12 }; im[n] = n < 17 ? b[n] : 0; }
        else if (name == "vocal") im[n] = std::exp(-((n - 6.0) * (n - 6.0)) / 6) + 0.6 * std::exp(-((n - 13.0) * (n - 13.0)) / 10) + 0.5 / n;
        else if (name == "metal") im[n] = n % 2 ? (n % 4 == 1 ? 1 : -1) / std::sqrt((double)n) : 0.15 / n;
        else if (name == "stair") im[n] = n % 4 ? 1.0 / n : 0;
        else if (n == 1) im[n] = 1;   // unknown: sine
    }
}

WaveTable::WaveTable(const std::vector<double>& re, const std::vector<double>& im) {
    const int N = (int)re.size() - 1;
    std::vector<double> tmp(SIZE);
    double norm = 0;
    // normalise by the peak of the full-bandwidth shape (PeriodicWave normalisation), applied to every table alike
    for (int h = N; h >= 1; h = h / 2) {
        std::fill(tmp.begin(), tmp.end(), 0.0);
        for (int n = 1; n <= h && n <= SIZE / 2 - 1; n++) {
            if (re[n] == 0 && im[n] == 0) continue;
            const double dc = std::cos(2 * PI * n / SIZE), ds = std::sin(2 * PI * n / SIZE); double c = 1, s = 0;   // rotate a phasor
            for (int i = 0; i < SIZE; i++) { tmp[i] += re[n] * c + im[n] * s; const double c2 = c * dc - s * ds; s = s * dc + c * ds; c = c2; }
        }
        if (norm == 0) { for (double v : tmp) norm = std::max(norm, std::abs(v)); if (norm == 0) norm = 1; }
        std::array<float, SIZE> t{}; for (int i = 0; i < SIZE; i++) t[i] = (float)(tmp[i] / norm);
        tables.push_back(t); harm.push_back(h);
        if (h == 1) break;
    }
}

void WaveTable::warmUp() {
    for (const char* w : { "sine", "triangle", "sawtooth", "square", "pulse25", "pulse10", "softsaw", "organ", "bell", "vocal", "metal", "stair" }) get(w);
    for (auto& ws : waveSets()) for (int k = 0; k < WT_FRAMES; k++) get(frameName(ws.id, k));
}
const WaveTable& WaveTable::get(const std::string& name) {
    static std::mutex m; static std::map<std::string, std::unique_ptr<WaveTable>> cache;
    std::lock_guard<std::mutex> lk(m);
    auto it = cache.find(name);
    if (it != cache.end()) return *it->second;
    std::vector<double> re, im; coefsFor(name, re, im);
    auto& p = cache[name] = std::make_unique<WaveTable>(re, im);
    return *p;
}

// --- timeline ------------------------------------------------------------------------------------------------------
void Timeline::prepare() {
    std::stable_sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
    for (size_t i = 0; i < ev.size(); i++) ev[i].start = valueBefore(i);
    ready = true;
}
// The value the curve has just before event i takes over.
double Timeline::valueBefore(size_t i) {
    if (i == 0) return def;
    const Ev& p = ev[i - 1];
    if (p.kind == 3) return p.v + (p.start - p.v) * std::exp(-(ev[i].t - p.t) / std::max(1e-9, p.tau));
    return p.v;
}
double Timeline::at(double t) {
    if (!ready) prepare();
    if (ev.empty()) return def;
    while (cur < ev.size() && ev[cur].t <= t) cur++;   // cur: first event after t
    if (cur < ev.size()) {
        const Ev& e = ev[cur];
        if (e.kind == 1 || e.kind == 2) {   // inside a ramp that ends at e.t
            const double t0 = cur ? ev[cur - 1].t : 0, v0 = cur ? (ev[cur - 1].kind == 3 ? valueBefore(cur) : ev[cur - 1].v) : def;
            const double f = e.t > t0 ? (t - t0) / (e.t - t0) : 1;
            if (e.kind == 1) return v0 + (e.v - v0) * f;
            if (v0 * e.v <= 0) return v0;   // exponential ramps need same-sign, non-zero ends
            return v0 * std::pow(e.v / v0, f);
        }
    }
    if (cur == 0) return def;
    const Ev& p = ev[cur - 1];
    if (p.kind == 3) return p.v + (p.start - p.v) * std::exp(-(t - p.t) / std::max(1e-9, p.tau));
    return p.v;
}

}   // namespace us
