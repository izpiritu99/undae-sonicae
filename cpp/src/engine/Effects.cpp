#include "Effects.h"
#include "Analysis.h"
#include <complex>
#include <map>
#include <mutex>
#include <juce_dsp/juce_dsp.h>

namespace us {

// --- compressor ----------------------------------------------------------------------------------------------------
void Compressor::prepare(double s) { sr = s; look = (size_t)std::round(0.006 * sr); dl.init(look + 2); dr.init(look + 2); gs = 0; qv.assign(look + 4, 0); qi.assign(look + 4, 0); qh = qt = 0; idx = 0; }
double Compressor::curve(double x) const {   // output level in dB for input level x dB
    const double T = thr, W = knee, R = std::max(1.0, ratio);
    if (2 * (x - T) < -W) return x;
    if (W > 0 && 2 * std::abs(x - T) <= W) { const double d = x - T + W / 2; return x + (1 / R - 1) * d * d / (2 * W); }
    return T + (x - T) / R;
}
void Compressor::process(float* L, float* R, int n) {
    const double makeup = -curve(0) * 0.6;   // dB
    // attack: most of the way there within the attack time; the detector sees the loudest sample of the next 6 ms, so the
    // gain is down before a peak reaches the (delayed) output
    const double ca = std::exp(-3 / (std::max(1e-4, att) * sr)), cr = std::exp(-1 / (std::max(1e-3, rel) * sr));
    const size_t Q = qv.size();
    for (int i = 0; i < n; i++) {
        const double in = std::max(std::abs(L[i]), std::abs(R[i]));
        while (qh != qt && qv[(qt + Q - 1) % Q] <= in) qt = (qt + Q - 1) % Q;
        qv[qt] = in; qi[qt] = idx; qt = (qt + 1) % Q;
        while (qi[qh] <= idx - (int64_t)look) qh = (qh + 1) % Q;
        idx++;
        const double lvl = qv[qh];
        const double x = lvl > 1e-9 ? 20 * std::log10(lvl) : -180;
        const double gc = curve(x) - x;   // <= 0
        gs = gc < gs ? ca * gs + (1 - ca) * gc : cr * gs + (1 - cr) * gc;
        dl.push(L[i]); dr.push(R[i]);
        const double g = dbToGain(gs + makeup);
        L[i] = (float)(dl.read((double)look) * g); R[i] = (float)(dr.read((double)look) * g);
    }
}

// --- meter taps ------------------------------------------------------------------------------------------------------
void MeterTap::last(int n, std::vector<float>& L, std::vector<float>& R) const {
    n = std::min(n, RING); L.resize((size_t)n); R.resize((size_t)n); const uint32_t e = w.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++) { const uint32_t k = (e - (uint32_t)n + (uint32_t)i) % RING; L[(size_t)i] = l[k]; R[(size_t)i] = r[k]; }
}
std::shared_ptr<MeterTap> MeterTap::forSlot(const std::string& id) {
    static std::mutex mu; static std::map<std::string, std::weak_ptr<MeterTap>> taps;
    std::lock_guard<std::mutex> lk(mu); auto& wp = taps[id]; auto sp = wp.lock(); if (!sp) { sp = std::make_shared<MeterTap>(); wp = sp; } return sp;
}

namespace {

// equal-power dry / wet, as fx.js wetDry()
struct WetDry { double dry = 1, wet = 0; void mix(double v) { dry = std::cos(v * PI / 2); wet = std::sin(v * PI / 2); } };

struct Smooth {   // live parameter changes glide (setTargetAtTime, 20 ms), so knob moves do not click
    double v = 0, target = 0; bool init = false;
    void set(double x) { target = x; if (!init) { v = x; init = true; } }
    inline double next(double k) { v += (target - v) * k; return v; }
};

class Eq : public Effect {
    Biquad2 a, b, c; double low = 0, mid = 0, midf = 1000, high = 0; bool dirty = true;
public:
    void set(const FxSlot& f) override {
        double l = f.num("low", 0), m = f.num("mid", 0), mf = f.num("midf", 1000), h = f.num("high", 0);
        if (l != low || m != mid || mf != midf || h != high) { low = l; mid = m; midf = mf; high = h; dirty = true; }
    }
    void process(float* L, float* R, int n) override {
        if (dirty) { a.setType(FilterType::Lowshelf); a.set(sr, 120, 1, low); b.setType(FilterType::Peaking); b.set(sr, midf, 0.9, mid); c.setType(FilterType::Highshelf); c.set(sr, 6000, 1, high); dirty = false; }
        for (int i = 0; i < n; i++) { a.tick(L[i], R[i]); b.tick(L[i], R[i]); c.tick(L[i], R[i]); }
    }
};

class Filter : public Effect {
    Biquad2 f; std::string type = "lowpass"; double cut = 2000, q = 1; bool dirty = true;
public:
    void set(const FxSlot& s) override { auto t = s.str("type", "lowpass"); double c = s.num("cut", 2000), qq = s.num("q", 1); if (t != type || c != cut || qq != q) { type = t; cut = c; q = qq; dirty = true; } }
    void process(float* L, float* R, int n) override { if (dirty) { f.setType(filterTypeFrom(type)); f.set(sr, cut, q, 0); dirty = false; } for (int i = 0; i < n; i++) f.tick(L[i], R[i]); }
};

class Comp : public Effect {
    Compressor c; double makeup = 1;
public:
    void prepare(double s, int b) override { Effect::prepare(s, b); c.prepare(s); }
    void set(const FxSlot& f) override { c.thr = f.num("thr", -18); c.ratio = f.num("ratio", 4); c.att = f.num("att", 0.01); c.rel = f.num("rel", 0.15); c.knee = f.num("knee", 12); makeup = dbToGain(f.num("gain", 0)); }
    void process(float* L, float* R, int n) override { c.process(L, R, n); if (makeup != 1) for (int i = 0; i < n; i++) { L[i] *= (float)makeup; R[i] *= (float)makeup; } }
};

class Limiter : public Effect {
    Compressor c; double in = 1;
public:
    void prepare(double s, int b) override { Effect::prepare(s, b); c.prepare(s); c.knee = 0; c.ratio = 20; c.att = 0.002; }
    void set(const FxSlot& f) override { in = dbToGain(f.num("in", 0)); c.thr = f.num("ceil", -1); c.rel = f.num("rel", 0.08); }
    void process(float* L, float* R, int n) override { if (in != 1) for (int i = 0; i < n; i++) { L[i] *= (float)in; R[i] *= (float)in; } c.process(L, R, n); }
};

// Uniformly partitioned convolution (overlap-save, FFT size 2B): LAT = B samples of latency, no threads, the same output
// on every run. The reverb's pre-delay absorbs the latency, so the tail still starts when the pre-delay says
// (10 ms by default ~ 512 samples at 48 kHz).
constexpr int LAT = 512;
class PartConv {
public:
    static constexpr int B = LAT, N = 2 * B;
    void load(const std::vector<std::vector<float>>& ir) {   // one impulse per channel (2)
        const size_t len = ir[0].size(); P = std::max<size_t>(1, (len + B - 1) / B);
        for (int c = 0; c < 2; c++) {
            H[c].assign(P * (B + 1), {}); fdl[c].assign(P * (B + 1), {});
            std::vector<float> t(2 * N);
            for (size_t k = 0; k < P; k++) {
                std::fill(t.begin(), t.end(), 0.f);
                for (int i = 0; i < B && k * B + i < len; i++) t[(size_t)i] = ir[(size_t)c][k * B + (size_t)i];
                fft.performRealOnlyForwardTransform(t.data(), true);
                for (int b = 0; b <= B; b++) H[c][k * (B + 1) + (size_t)b] = { t[(size_t)(2 * b)], t[(size_t)(2 * b + 1)] };
            }
            prev[c].assign(B, 0.f); in[c].assign(B, 0.f); out[c].assign(B, 0.f);
        }
        cur = 0; pos = 0;
    }
    bool loaded() const { return P > 0 && !H[0].empty(); }
    void process(float* L, float* R, int n) {   // in place: input in, wet out (delayed by B)
        float* io[2] = { L, R };
        for (int i = 0; i < n; i++) {
            for (int c = 0; c < 2; c++) { const float x = io[c][i]; io[c][i] = out[c][(size_t)pos]; in[c][(size_t)pos] = x; }
            if (++pos == B) { block(); pos = 0; }
        }
    }
private:
    juce::dsp::FFT fft{ 10 };   // 2^10 = N
    size_t P = 0, cur = 0; int pos = 0;
    std::vector<std::complex<float>> H[2], fdl[2]; std::vector<float> prev[2], in[2], out[2];
    void block() {
        std::vector<float>& t = scratch; t.assign(2 * N, 0.f);
        for (int c = 0; c < 2; c++) {
            std::copy(prev[c].begin(), prev[c].end(), t.begin()); std::copy(in[c].begin(), in[c].end(), t.begin() + B); std::fill(t.begin() + N, t.end(), 0.f);
            fft.performRealOnlyForwardTransform(t.data(), true);
            std::complex<float>* slot = &fdl[c][cur * (B + 1)];
            for (int b = 0; b <= B; b++) slot[b] = { t[(size_t)(2 * b)], t[(size_t)(2 * b + 1)] };
            acc.assign(B + 1, {});
            for (size_t k = 0; k < P; k++) {
                const std::complex<float>* X = &fdl[c][((cur + P - k) % P) * (B + 1)]; const std::complex<float>* h = &H[c][k * (B + 1)];
                for (int b = 0; b <= B; b++) acc[(size_t)b] += X[b] * h[b];
            }
            for (int b = 0; b <= B; b++) { t[(size_t)(2 * b)] = acc[(size_t)b].real(); t[(size_t)(2 * b + 1)] = acc[(size_t)b].imag(); }
            for (int b = B + 1; b < N; b++) { t[(size_t)(2 * b)] = acc[(size_t)(N - b)].real(); t[(size_t)(2 * b + 1)] = -acc[(size_t)(N - b)].imag(); }
            fft.performRealOnlyInverseTransform(t.data());
            for (int i = 0; i < B; i++) out[c][(size_t)i] = t[(size_t)(B + i)];   // overlap-save: the second half is valid
            prev[c] = in[c];
        }
        cur = (cur + 1) % P;
    }
    std::vector<float> scratch; std::vector<std::complex<float>> acc;
};

class Reverb : public Effect {
    WetDry m; double size = -1, damp = -1, pre = 10; DelayLine pl, pr; PartConv conv; std::vector<float> wl, wr;
    void gen() {
        const int n = std::max(64, (int)std::floor(sr * size)); std::vector<std::vector<float>> ir(2, std::vector<float>((size_t)n)); const double a = 1 - damp * 0.92; Rng r(1234 + (uint64_t)(size * 1000) + (uint64_t)(damp * 100));
        double pw = 0;
        for (int c = 0; c < 2; c++) { double lp = 0; for (int i = 0; i < n; i++) { lp += a * (r.bi() - lp); const float v = (float)(lp * std::pow(1.0 - (double)i / n, 2.6)); ir[(size_t)c][(size_t)i] = v; pw += (double)v * v; } }
        // ConvolverNode normalisation (Chromium Reverb::calculateNormalizationScale): unit power, then -58 dB, rate-compensated
        pw = std::sqrt(pw / (2.0 * n)); if (pw < 0.000125) pw = 0.000125;
        const float scale = (float)(1.0 / pw * 0.00125 * 44100.0 / sr);
        for (auto& ch : ir) for (auto& v : ch) v *= scale;
        conv.load(ir);
    }
public:
    void prepare(double s, int b) override { Effect::prepare(s, b); pl.init((size_t)(0.5 * s) + 4); pr.init((size_t)(0.5 * s) + 4); wl.assign((size_t)std::max(b, 4096), 0); wr.assign((size_t)std::max(b, 4096), 0); }
    void set(const FxSlot& f) override {
        m.mix(f.num("mix", 0.3)); pre = f.num("pre", 10);
        const double s = f.num("size", 2.2), d = f.num("damp", 0.5);
        if (s != size || d != damp) { size = s; damp = d; gen(); }
    }
    void process(float* L, float* R, int n) override {
        const double ps = std::max(0.0, pre / 1000.0 * sr - LAT);
        for (int i = 0; i < n; i++) { pl.push(L[i]); pr.push(R[i]); wl[(size_t)i] = ps < 0.5 ? L[i] : pl.read(ps); wr[(size_t)i] = ps < 0.5 ? R[i] : pr.read(ps); }
        conv.process(wl.data(), wr.data(), n);
        for (int i = 0; i < n; i++) { L[i] = (float)(L[i] * m.dry + wl[(size_t)i] * m.wet); R[i] = (float)(R[i] * m.dry + wr[(size_t)i] * m.wet); }
    }
};

class Delay : public Effect {
    WetDry m; double time = 0.375, fb = 0.4, tone = 5000, tset = -1; DelayLine dl, dr; Biquad2 lp;
public:
    void prepare(double s, int b) override { Effect::prepare(s, b); dl.init((size_t)(2 * s) + 4); dr.init((size_t)(2 * s) + 4); lp.setType(FilterType::Lowpass); }
    void set(const FxSlot& f) override { m.mix(f.num("mix", 0.3)); time = f.num("time", 375) / 1000.0; fb = f.num("fb", 0.4); tone = f.num("tone", 5000); }
    void process(float* L, float* R, int n) override {
        if (tone != tset) { lp.set(sr, tone, 1, 0); tset = tone; }
        const double d = std::max(128.0, time * sr);   // a delay inside a feedback loop is at least one render quantum in Web Audio
        for (int i = 0; i < n; i++) {
            float yl = dl.read(d), yr = dr.read(d); lp.tick(yl, yr);
            dl.push((float)(L[i] + fb * yl)); dr.push((float)(R[i] + fb * yr));
            L[i] = (float)(L[i] * m.dry + yl * m.wet); R[i] = (float)(R[i] * m.dry + yr * m.wet);
        }
    }
};

class Chorus : public Effect {
    WetDry m; double rate = 0.8, depth = 0.5, ph = 0; DelayLine d1, d2;
public:
    void prepare(double s, int b) override { Effect::prepare(s, b); d1.init((size_t)(0.1 * s) + 4); d2.init((size_t)(0.1 * s) + 4); }
    void set(const FxSlot& f) override { m.mix(f.num("mix", 0.5)); rate = f.num("rate", 0.8); depth = f.num("depth", 0.5); }
    void process(float* L, float* R, int n) override {
        for (int i = 0; i < n; i++) {
            const double lf = std::sin(2 * PI * ph); ph += rate / sr; ph -= std::floor(ph);
            const float mono = (L[i] + R[i]) * 0.5f;   // each delay feeds one side of a channel merger, which takes mono
            d1.push(mono); d2.push(mono);
            const float wl = d1.read((0.013 + lf * depth * 0.006) * sr), wr = d2.read((0.021 - lf * depth * 0.006) * sr);
            L[i] = (float)(L[i] * m.dry + wl * m.wet); R[i] = (float)(R[i] * m.dry + wr * m.wet);
        }
    }
};

// Four modulated all-pass stages with feedback. (In the browser version the feedback loop has no delay node, which Web
// Audio mutes; here it works as intended.)
class Phaser : public Effect {
    WetDry m; double rate = 0.4, depth = 0.7, fb = 0.4, ph = 0; Biquad ap[2][4]; float lastL = 0, lastR = 0;
public:
    void prepare(double s, int b) override { Effect::prepare(s, b); for (auto& c : ap) for (auto& a : c) a.type = FilterType::Allpass; }
    void set(const FxSlot& f) override { m.mix(f.num("mix", 0.5)); rate = f.num("rate", 0.4); depth = f.num("depth", 0.7); fb = f.num("fb", 0.4); }
    void process(float* L, float* R, int n) override {
        for (int i = 0; i < n; i++) {
            if ((i & 15) == 0) { const double fr = std::max(20.0, 900 + std::sin(2 * PI * ph) * depth * 750); for (auto& c : ap) for (auto& a : c) a.set(sr, fr, 0.6, 0); }
            ph += rate / sr; ph -= std::floor(ph);
            double l = L[i] + fb * lastL, r = R[i] + fb * lastR;
            for (int k = 0; k < 4; k++) { l = ap[0][k].tick(l); r = ap[1][k].tick(r); }
            lastL = (float)l; lastR = (float)r;
            L[i] = (float)(L[i] * m.dry + l * m.wet); R[i] = (float)(R[i] * m.dry + r * m.wet);
        }
    }
};

class Dist : public Effect {
    WetDry m; double pre = 1, tone = 8000, tset = -1, level = 0.7; Shaper ws; Biquad2 lp;
public:
    Dist() { ws.curve.resize(2048); for (int i = 0; i < 2048; i++) ws.curve[i] = (float)std::tanh((i / 1023.5 - 1) * 3); lp.setType(FilterType::Lowpass); }
    void set(const FxSlot& f) override { m.mix(f.num("mix", 1)); const double d = f.num("drive", 0.4); pre = 0.4 + d * d * 20; tone = f.num("tone", 8000); level = f.num("level", 0.7); }
    void process(float* L, float* R, int n) override {
        if (tone != tset) { lp.set(sr, tone, 1, 0); tset = tone; }
        for (int i = 0; i < n; i++) {
            float l = ws.apply((float)(L[i] * pre)), r = ws.apply((float)(R[i] * pre)); lp.tick(l, r);
            L[i] = (float)(L[i] * m.dry + l * level * m.wet); R[i] = (float)(R[i] * m.dry + r * level * m.wet);
        }
    }
};

class Crush : public Effect {
    WetDry m; double bits = -1, tone = 12000, tset = -1; Shaper ws; Biquad2 lp;
public:
    Crush() { lp.setType(FilterType::Lowpass); }
    void set(const FxSlot& f) override {
        m.mix(f.num("mix", 1)); tone = f.num("tone", 12000);
        const double b = f.num("bits", 8);
        if (b != bits) { bits = b; const double lv = std::pow(2.0, b - 1); ws.curve.resize(4096); for (int i = 0; i < 4096; i++) ws.curve[i] = (float)(std::round((i / 2047.5 - 1) * lv) / lv); }
    }
    void process(float* L, float* R, int n) override {
        if (tone != tset) { lp.set(sr, tone, 1, 0); tset = tone; }
        for (int i = 0; i < n; i++) { float l = ws.apply(L[i]), r = ws.apply(R[i]); lp.tick(l, r); L[i] = (float)(L[i] * m.dry + l * m.wet); R[i] = (float)(R[i] * m.dry + r * m.wet); }
    }
};

// Peak controller: an envelope follower on another bus pulls this one down, whole or in one EQ band.
class Duck : public Effect {
    std::string src, mode = "full"; double depth = 12, sens = 3, decay = 0.15, freq = 120, q = 1, e = 0; std::vector<float> env; Biquad2 band; bool haveSide = false;
public:
    void prepare(double s, int b) override { Effect::prepare(s, b); env.assign(std::max(b, 4096), 0.f); band.setType(FilterType::Peaking); }
    void set(const FxSlot& f) override { src = f.str("src", ""); mode = f.str("mode", "full"); depth = f.num("depth", 12); sens = f.num("sens", 3); decay = f.num("decay", 0.15); freq = f.num("freq", 120); q = f.num("q", 1); }
    std::string sideSource() const override { return src; }
    void side(const float* L, const float* R, int n) override {
        const double a = std::exp(-1 / (sr * 0.002)), r = std::exp(-1 / (sr * std::max(0.005, decay)));
        for (int i = 0; i < n; i++) { const double v = std::min(1.0, std::abs((L[i] + R[i]) * 0.5) * sens); e = v + (e - v) * (v > e ? a : r); env[i] = (float)e; }
        haveSide = true;
    }
    void process(float* L, float* R, int n) override {
        if (!haveSide) return;
        haveSide = false;
        if (mode == "band") {
            for (int i = 0; i < n; i++) { if ((i & 15) == 0) band.set(sr, freq, q, -depth * env[i]); band.tick(L[i], R[i]); }
        } else {
            const double k = 1 - std::pow(10.0, -depth / 20);
            for (int i = 0; i < n; i++) { const float g = (float)(1 - k * env[i]); L[i] *= g; R[i] *= g; }
        }
    }
};

// --- built-in replacements for third-party plug-in jobs (see HANDOVER.md, "Built-in instruments and effects") ---------

// Low/high-pass Q in Web Audio's dB form: Butterworth 2nd order (-3.01 dB), or the two stages of a 4th-order Butterworth.
constexpr double Q_BW2 = -3.0103, Q_BW4A = -5.3329, Q_BW4B = 2.3222;
inline int slopeOf(const std::string& s) { return s == "12 dB/oct" ? 1 : s == "24 dB/oct" ? 2 : 0; }

// Parametric EQ: high-pass, low shelf, three peaks, high shelf, low-pass (RBJ cookbook biquads), output trim.
class ParamEq : public Effect {
    enum { HP1, HP2, LS, P1, P2, P3, HS, LP1, LP2, NB };
    Biquad2 b[NB]; bool on[NB] = {}; double v[18] = {}; bool dirty = true; double out = 1;
    static constexpr const char* K[18] = { "hpf", "hps", "lsf", "lsg", "p1f", "p1g", "p1q", "p2f", "p2g", "p2q", "p3f", "p3g", "p3q", "hsf", "hsg", "lpf", "lps", "out" };
public:
    void set(const FxSlot& f) override {
        double n[18];
        for (int i = 0; i < 18; i++) n[i] = (i == 1 || i == 16) ? slopeOf(f.str(K[i], "off")) : f.num(K[i], 0);
        for (int i = 0; i < 18; i++) if (n[i] != v[i]) { std::copy(n, n + 18, v); dirty = true; break; }
    }
    void process(float* L, float* R, int n) override {
        if (dirty) {
            dirty = false;
            const int hs = (int)v[1], ls = (int)v[16];
            auto put = [&](int i, FilterType t, double fr, double q, double g, bool o) { on[i] = o; if (o) { b[i].setType(t); b[i].set(sr, fr, q, g); } };
            put(HP1, FilterType::Highpass, v[0], hs == 2 ? Q_BW4A : Q_BW2, 0, hs >= 1); put(HP2, FilterType::Highpass, v[0], Q_BW4B, 0, hs == 2);
            put(LS, FilterType::Lowshelf, v[2], 1, v[3], v[3] != 0);
            put(P1, FilterType::Peaking, v[4], v[6], v[5], v[5] != 0); put(P2, FilterType::Peaking, v[7], v[9], v[8], v[8] != 0); put(P3, FilterType::Peaking, v[10], v[12], v[11], v[11] != 0);
            put(HS, FilterType::Highshelf, v[13], 1, v[14], v[14] != 0);
            put(LP1, FilterType::Lowpass, v[15], ls == 2 ? Q_BW4A : Q_BW2, 0, ls >= 1); put(LP2, FilterType::Lowpass, v[15], Q_BW4B, 0, ls == 2);
            out = dbToGain(v[17]);
        }
        for (int k = 0; k < NB; k++) if (on[k]) for (int i = 0; i < n; i++) b[k].tick(L[i], R[i]);
        if (out != 1) for (int i = 0; i < n; i++) { L[i] *= (float)out; R[i] *= (float)out; }
    }
};

// De-esser: the level above `freq` (2nd-order high-pass of the mono sum, fast peak follower) drives a gain reduction of
// up to `range` dB above `thr` (4:1). "Split" turns down only the band above `freq`, split by a 4th-order Linkwitz-Riley
// crossover (the two bands sum back flat, only the phase turns); "wide" turns down everything. Listen plays the high band.
class DeEsser : public Effect {
    Biquad det; Biquad2 lo1, lo2, hi1, hi2; double freq = -1, thr = -30, range = 10, env = 0, gr = 0; bool split = true, listen = false;
public:
    double reductionDb() const { return gr; }
    void set(const FxSlot& f) override {
        const double fr = f.num("freq", 6000); thr = f.num("thr", -30); range = f.num("range", 10);
        split = f.str("mode", "split") != "wide"; listen = f.str("listen", "off") == "on";
        if (fr != freq) {
            freq = fr; det.type = FilterType::Highpass; det.set(sr, freq, Q_BW2, 0);
            for (Biquad2* q : { &lo1, &lo2 }) { q->setType(FilterType::Lowpass); q->set(sr, freq, Q_BW2, 0); }
            for (Biquad2* q : { &hi1, &hi2 }) { q->setType(FilterType::Highpass); q->set(sr, freq, Q_BW2, 0); }
        }
    }
    void process(float* L, float* R, int n) override {
        const double a = std::exp(-1 / (0.0005 * sr)), r = std::exp(-1 / (0.06 * sr)), gs = std::exp(-1 / (0.002 * sr));
        for (int i = 0; i < n; i++) {
            const double h = det.tick(0.5 * (L[i] + R[i])), x = std::abs(h);
            env = x > env ? a * env + (1 - a) * x : r * env + (1 - r) * x;
            const double lv = env > 1e-9 ? 20 * std::log10(env) : -180;
            const double want = std::min(range, std::max(0.0, (lv - thr) * 0.75));
            gr = gs * gr + (1 - gs) * want;
            const double g = dbToGain(-gr);
            if (split || listen) {
                float hl = L[i], hr = R[i], ll = L[i], lr = R[i];
                hi1.tick(hl, hr); hi2.tick(hl, hr); lo1.tick(ll, lr); lo2.tick(ll, lr);
                if (listen) { L[i] = hl; R[i] = hr; continue; }
                L[i] = (float)(ll + hl * g); R[i] = (float)(lr + hr * g);
            } else { L[i] = (float)(L[i] * g); R[i] = (float)(R[i] * g); }
        }
    }
};

// Saturator: four-times oversampled wave shaping (JUCE's half-band FIR oversampler, integer latency, so the dry path can
// be delayed to match). Modes: tube (biased tanh: even harmonics), tape (soft arctan with a low bump before and the top
// rolled off by drive), transformer (the lows saturate more than the highs), clip (cubic soft clip), exciter (only the
// band above `band` is driven, then added). Level is compensated so a -10 dBFS tone keeps about its level.
class Saturator : public Effect {
    std::unique_ptr<juce::dsp::Oversampling<float>> os; juce::AudioBuffer<float> buf; int lat = 0; DelayLine dl, dr;
    std::string mode = "tube"; double drive = 1, bias = 0.2, tone = -1, band = -1, comp = 1, out = 1; WetDry m;
    Biquad2 bump, toneLp, exHp, lowSplit, dc; double lastDrive = -1; std::string lastMode;
    double shape(double x, double lo) const {   // lo: the low band of x (transformer)
        if (mode == "tube") { const double k = bias * 0.6; return std::tanh(x + k) - std::tanh(k); }
        if (mode == "tape") return 2 / PI * std::atan(x * PI / 2);
        if (mode == "transformer") { (void)x; double y = clampd(lo * 1.6, -1.5, 1.5); y = y - y * y * y * 4 / 27; return y / 1.6; }   // the low band only: the caller adds the highs back undriven
        if (mode == "exciter") return std::tanh(x);
        const double c = clampd(x, -1, 1); return 1.5 * c - 0.5 * c * c * c;   // clip
    }
public:
    void prepare(double s, int b) override {
        Effect::prepare(s, b);
        os = std::make_unique<juce::dsp::Oversampling<float>>(2, 2, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
        os->initProcessing((size_t)std::max(b, 4096)); lat = (int)std::lround(os->getLatencyInSamples());
        buf.setSize(2, std::max(b, 4096)); dl.init((size_t)lat + 8); dr.init((size_t)lat + 8);
        bump.setType(FilterType::Peaking); toneLp.setType(FilterType::Lowpass); exHp.setType(FilterType::Highpass); lowSplit.setType(FilterType::Lowpass); dc.setType(FilterType::Highpass);
        lowSplit.set(s * 4, 250, Q_BW2, 0); dc.set(s, 12, Q_BW2, 0); bump.set(s * 4, 90, 0.8, 2.5);
    }
    void set(const FxSlot& f) override {
        mode = f.str("mode", "tube"); drive = dbToGain(f.num("drive", 6)); bias = f.num("bias", 0.2); m.mix(f.num("mix", 1)); out = dbToGain(f.num("out", 0));
        const double t = f.num("tone", 14000), bd = f.num("band", 3000);
        if (t != tone || drive != lastDrive || mode != lastMode) { tone = t; toneLp.set(sr, mode == "tape" ? std::min(t, 20000 / std::sqrt(drive)) : t, Q_BW2, 0); }
        if (bd != band) { band = bd; exHp.set(sr * 4, band, Q_BW2, 0); }
        if (drive != lastDrive || mode != lastMode) {   // level compensation, measured on a -10 dBFS sine through the curve
            lastDrive = drive; lastMode = mode; double in2 = 0, o2 = 0;
            for (int i = 0; i < 64; i++) { const double x = 0.316 * std::sin(2 * PI * i / 64); const double y = mode == "exciter" ? x : shape(x * drive, mode == "transformer" ? x * drive : 0); in2 += x * x; o2 += y * y; }
            comp = mode == "exciter" ? 1 : std::sqrt(in2 / std::max(1e-12, o2));
        }
    }
    void process(float* L, float* R, int n) override {
        juce::ScopedNoDenormals nd;
        float* io[2] = { buf.getWritePointer(0), buf.getWritePointer(1) };
        std::copy(L, L + n, io[0]); std::copy(R, R + n, io[1]);
        juce::dsp::AudioBlock<float> blk(io, 2, (size_t)n);
        auto up = os->processSamplesUp(blk);
        float* u[2] = { up.getChannelPointer(0), up.getChannelPointer(1) };
        const size_t N = up.getNumSamples(); const double exAmt = 0.35 * (1 - 1 / drive);
        for (size_t i = 0; i < N; i++) {
            float l = u[0][i], r = u[1][i];
            if (mode == "exciter") { float hl = l, hr = r; exHp.tick(hl, hr); l = (float)(l + exAmt * std::tanh(hl * drive)); r = (float)(r + exAmt * std::tanh(hr * drive)); }
            else {
                if (mode == "tape") bump.tick(l, r);
                float ll = 0, lr = 0; if (mode == "transformer") { ll = l; lr = r; lowSplit.tick(ll, lr); }
                if (mode == "transformer") { l = (float)(shape(0, ll * drive) * comp + (l - ll)); r = (float)(shape(0, lr * drive) * comp + (r - lr)); }
                else { l = (float)shape(l * drive, 0); r = (float)shape(r * drive, 0); }
            }
            u[0][i] = l; u[1][i] = r;
        }
        os->processSamplesDown(blk);
        const double g = (mode == "transformer" ? 1.0 : comp) * out;   // transformer: compensated inside, on the low band
        for (int i = 0; i < n; i++) {
            float l = io[0][i], r = io[1][i]; toneLp.tick(l, r); if (mode == "tube") dc.tick(l, r);
            dl.push(L[i]); dr.push(R[i]); const float xl = lat ? dl.read(lat) : L[i], xr = lat ? dr.read(lat) : R[i];
            L[i] = (float)(xl * m.dry + l * g * m.wet); R[i] = (float)(xr * m.dry + r * g * m.wet);
        }
    }
    int latency() const { return lat; }
};

// Algorithmic reverb: 8-line feedback delay network (Jot), orthonormal Hadamard mixing, mutually prime line lengths,
// four input diffusion all-passes, a one-pole low-pass in every line for the high-frequency decay, gentle modulation of
// four lines. Each line's gain gives the RT60 asked for: g = 10^(-3 len / (RT60 sr)). "Gated" holds the tail open while
// the input is loud and shuts it `decay` seconds after (capped at 1.5 s), the classic gated drum sound.
class FdnReverb : public Effect {
    static constexpr int NL = 8;
    DelayLine line[NL], diff[4], pre; double len[NL] = {}, g[NL] = {}, lp[NL] = {}, dlen[4] = {}, ph[NL] = {};
    std::string mode = "hall"; double decay = 2.5, size = 1, damp = 6000, preMs = 15, mod = 0.3, width = 1, dampK = 0, genv = 0, gate = 0, hold = 0;
    WetDry m; std::string lastMode; double lastSize = -1, lastDecay = -1;
    void layout() {
        static const double hall[NL] = { 29.7, 37.1, 41.1, 43.7, 53.3, 59.9, 67.7, 73.1 }, plate[NL] = { 11.3, 13.7, 17.9, 19.1, 23.3, 26.9, 31.1, 34.7 };
        const double* base = mode == "plate" ? plate : hall; const double sc = (mode == "room" ? 0.42 : mode == "gated" ? 0.6 : 1.0) * size;
        const double rt = mode == "gated" ? 3.0 : decay;
        for (int k = 0; k < NL; k++) { len[k] = std::max(8.0, base[k] * sc * 0.001 * sr); g[k] = std::pow(10.0, -3 * len[k] / (rt * sr)); }
        static const double dms[4] = { 4.77, 3.59, 12.73, 9.31 };
        for (int k = 0; k < 4; k++) dlen[k] = std::max(2.0, dms[k] * (mode == "plate" ? 1.3 : 1.0) * std::min(size, 1.5) * 0.001 * sr);
    }
public:
    void prepare(double s, int b) override {
        Effect::prepare(s, b);
        for (auto& d : line) d.init((size_t)(0.08 * 2.1 * s) + 64);
        for (auto& d : diff) d.init((size_t)(0.02 * s) + 8);
        pre.init((size_t)(0.25 * s) + 8);
        for (int k = 0; k < NL; k++) ph[k] = k * 0.137;
        layout();
    }
    void set(const FxSlot& f) override {
        mode = f.str("mode", "hall"); decay = f.num("decay", 2.5); size = clampd(f.num("size", 1), 0.3, 2); damp = f.num("damp", 6000);
        preMs = f.num("pre", 15); mod = f.num("mod", 0.3); width = f.num("width", 1); m.mix(f.num("mix", 0.25));
        dampK = 1 - std::exp(-2 * PI * std::min(damp, sr * 0.45) / sr);
        if (mode != lastMode || size != lastSize || decay != lastDecay) { lastMode = mode; lastSize = size; lastDecay = decay; layout(); }
    }
    void process(float* L, float* R, int n) override {
        juce::ScopedNoDenormals nd;
        const double pd = std::max(1.0, preMs * 0.001 * sr), md = mod * 0.0012 * sr, ga = std::exp(-1 / (0.001 * sr)), gr = std::exp(-1 / (0.03 * sr));
        const double holdS = std::min(decay, 1.5);
        for (int i = 0; i < n; i++) {
            const double in = 0.5 * (L[i] + R[i]);
            pre.push((float)in); double x = pre.read(pd);
            for (int k = 0; k < 4; k++) { const double d = diff[k].read(dlen[k]); const double v = x + 0.6 * d; diff[k].push((float)v); x = d - 0.6 * v; }   // Schroeder all-pass
            double y[NL];
            for (int k = 0; k < NL; k++) {
                double dd = len[k];
                if (k < 4 && md > 0) { ph[k] += (0.31 + 0.23 * k) / sr; ph[k] -= std::floor(ph[k]); dd += md * std::sin(2 * PI * ph[k]); }
                const double v = line[k].read(dd); lp[k] += dampK * (v - lp[k]); y[k] = lp[k] * g[k];
            }
            // 8-point fast Walsh-Hadamard transform, scaled to be orthonormal (energy kept, the decay comes from g)
            for (int h = 1; h < NL; h <<= 1) for (int a = 0; a < NL; a += h << 1) for (int b = a; b < a + h; b++) { const double u = y[b], w = y[b + h]; y[b] = u + w; y[b + h] = u - w; }
            for (int k = 0; k < NL; k++) line[k].push((float)(y[k] * 0.35355339059327373 + x * ((k & 1) ? -0.5 : 0.5)));
            double wl = 0, wr = 0;
            for (int k = 0; k < NL; k++) { const double v = lp[k]; if (k & 1) wr += (k & 2) ? -v : v; else wl += (k & 2) ? -v : v; }
            wl *= 0.5; wr *= 0.5;
            if (mode == "gated") {
                const double e = std::abs(in); genv = e > genv ? ga * genv + (1 - ga) * e : gr * genv + (1 - gr) * e;
                if (genv > 0.03) hold = holdS * sr; else if (hold > 0) hold--;
                const double tgt = hold > 0 ? 1.0 : 0.0; gate += (tgt - gate) * (tgt > gate ? 0.01 : 0.0015);
                wl *= gate; wr *= gate;
            }
            const double mid = 0.5 * (wl + wr), side = 0.5 * (wl - wr) * width;
            L[i] = (float)(L[i] * m.dry + (mid + side) * m.wet); R[i] = (float)(R[i] * m.dry + (mid - side) * m.wet);
        }
    }
};

// Gate / expander: the detector (peak follower on a high-passed copy of this bus, or of the key bus when one is chosen)
// opens the gate above `thr` and lets it close only below `thr - hyst`, after `hold`. Closed means `range` dB down
// (80 = shut, less = an expander). The gain moves in dB with the attack / release times.
class Gate : public Effect {
    std::string src; double thr = -40, hyst = 4, att = 0.001, hold = 0.05, rel = 0.15, range = 80, hpf = -1;
    Biquad hpS, hpK; double env = 0, gdb = -80, holdLeft = 0; bool open = false, haveKey = false; std::vector<float> key;
    double detect(Biquad& hp, double x) { const double h = std::abs(hp.tick(x)); const double a = std::exp(-1 / (0.0002 * sr)), r = std::exp(-1 / (0.01 * sr)); env = h > env ? a * env + (1 - a) * h : r * env + (1 - r) * h; return env; }
public:
    void prepare(double s, int b) override { Effect::prepare(s, b); key.assign((size_t)std::max(b, 4096), 0.f); hpf = -1; }
    void set(const FxSlot& f) override {
        src = f.str("src", ""); thr = f.num("thr", -40); hyst = f.num("hyst", 4); att = f.num("att", 0.001); hold = f.num("hold", 0.05); rel = f.num("rel", 0.15); range = f.num("range", 80);
        const double h = f.num("hpf", 20); if (h != hpf) { hpf = h; for (Biquad* q : { &hpS, &hpK }) { q->type = FilterType::Highpass; q->set(sr, hpf, Q_BW2, 0); } }
    }
    std::string sideSource() const override { return src; }
    void side(const float* L, const float* R, int n) override { for (int i = 0; i < n; i++) key[(size_t)i] = 0.5f * (L[i] + R[i]); haveKey = true; }
    void process(float* L, float* R, int n) override {
        const bool k = haveKey && !src.empty(); haveKey = false;
        const double ca = std::exp(-1 / (std::max(1e-4, att) * sr)), cr = std::exp(-1 / (std::max(1e-3, rel) * sr));
        const double openLin = dbToGain(thr), closeLin = dbToGain(thr - hyst);
        for (int i = 0; i < n; i++) {
            const double e = k ? detect(hpK, key[(size_t)i]) : detect(hpS, 0.5 * (L[i] + R[i]));
            if (e > openLin) { open = true; holdLeft = hold * sr; }
            else if (open && e < closeLin) { if (holdLeft > 0) holdLeft--; else open = false; }
            else if (open) holdLeft = hold * sr;
            const double tgt = open ? 0.0 : -range;
            gdb = tgt > gdb ? ca * gdb + (1 - ca) * tgt : cr * gdb + (1 - cr) * tgt;
            const float g = (float)dbToGain(gdb); L[i] *= g; R[i] *= g;
        }
    }
};

// Transient shaper: two pairs of envelope followers on the louder side. Attack: a fast-attack follower against a
// slow-attack one, their difference in dB is how far into a hit we are. Sustain: a slow-release follower against a
// fast-release one, the difference is how far into a tail. Gain (dB) = attack% x first + sustain% x second, +-24 dB.
class Transient : public Effect {
    double aAmt = 0, sAmt = 0, out = 1, fA = 0, sA = 0, fR = 0, sR = 0; WetDry m;
    static double fol(double& e, double x, double a, double r) { e = x > e ? a * e + (1 - a) * x : r * e + (1 - r) * x; return e; }
public:
    void set(const FxSlot& f) override { aAmt = f.num("attack", 0) / 100; sAmt = f.num("sustain", 0) / 100; out = dbToGain(f.num("out", 0)); m.mix(f.num("mix", 1)); }
    void process(float* L, float* R, int n) override {
        auto k = [&](double t) { return std::exp(-1 / (t * sr)); };
        const double a1 = k(0.0005), a20 = k(0.02), r20 = k(0.02), r60 = k(0.06), r400 = k(0.4);
        for (int i = 0; i < n; i++) {
            const double x = std::max(std::abs(L[i]), std::abs(R[i])) + 1e-9;
            const double d1 = 20 * std::log10(fol(fA, x, a1, r60) / (fol(sA, x, a20, r60) + 1e-9) + 1e-9);
            const double d2 = 20 * std::log10(fol(sR, x, a1, r400) / (fol(fR, x, a1, r20) + 1e-9) + 1e-9);
            const double gdb = clampd(aAmt * std::max(0.0, d1) + sAmt * std::max(0.0, d2), -24, 24);
            const double g = dbToGain(gdb) * out;
            L[i] = (float)(L[i] * (m.dry + g * m.wet)); R[i] = (float)(R[i] * (m.dry + g * m.wet));
        }
    }
};

// Stereo imager: mid / side width (0 % mono, 200 % double side), the side's low end removed below `mono` Hz (4th-order
// Linkwitz-Riley high-pass on the side only, so bass is mono), a Haas delay on the right for widening mono sources,
// and balance.
class Imager : public Effect {
    double width = 1, mono = -1, haas = 0, bal = 0; bool bassMono = false; Biquad s1, s2; DelayLine hd;
public:
    void prepare(double s, int b) override { Effect::prepare(s, b); hd.init((size_t)(0.04 * s) + 4); mono = -1; }
    void set(const FxSlot& f) override {
        width = f.num("width", 100) / 100; haas = f.num("haas", 0); bal = f.num("bal", 0); bassMono = f.str("bass", "off") == "mono";
        const double mf = f.num("mono", 120); if (mf != mono) { mono = mf; for (Biquad* q : { &s1, &s2 }) { q->type = FilterType::Highpass; q->set(sr, mono, Q_BW2, 0); } }
    }
    void process(float* L, float* R, int n) override {
        const double hs = haas * 0.001 * sr; const double gl = bal > 0 ? 1 - bal : 1, gr = bal < 0 ? 1 + bal : 1;
        for (int i = 0; i < n; i++) {
            const double mid = 0.5 * (L[i] + R[i]); double side = 0.5 * (L[i] - R[i]);
            if (bassMono) side = s2.tick(s1.tick(side));
            side *= width;
            double l = mid + side, r = mid - side;
            hd.push((float)r); if (hs >= 1) r = hd.read(hs);
            L[i] = (float)(l * gl); R[i] = (float)(r * gr);
        }
    }
};

// Note lengths in quarter notes for the tempo-synced effects ("free" = 0: use milliseconds instead).
inline double noteBeats(const std::string& s) {
    static const std::pair<const char*, double> t[] = { { "1/64", 1.0 / 16 }, { "1/32", 0.125 }, { "1/16", 0.25 }, { "1/8 triplet", 1.0 / 3 }, { "1/8", 0.5 }, { "1/8 dotted", 0.75 },
        { "1/4 triplet", 2.0 / 3 }, { "1/4", 1 }, { "1/4 dotted", 1.5 }, { "1/2", 2 }, { "1 bar", 4 } };
    for (auto& [k, v] : t) if (s == k) return v;
    return 0;
}

// Tempo delay: time free (ms) or a note length at the project tempo, stereo or ping-pong, the right side offset by
// `spread`, high- and low-pass and soft saturation inside the feedback loop, wow / flutter on the read position, and
// the echoes ducked while the dry signal is loud. Time changes glide (like a tape machine's speed) instead of clicking.
class EchoLine : public Effect {
    DelayLine dl, dr; Biquad2 hp, lp; WetDry m; std::string sync = "1/8", mode = "stereo";
    double ms = 375, spread = 0, fb = 0.45, lo = -1, hi = -1, drive = 0, wow = 0, duck = 0, curL = -1, curR = -1, ph1 = 0, ph2 = 0, env = 0; size_t cap = 0;
public:
    void prepare(double s, int b) override { Effect::prepare(s, b); cap = (size_t)(4.2 * s); dl.init(cap); dr.init(cap); hp.setType(FilterType::Highpass); lp.setType(FilterType::Lowpass); lo = hi = -1; curL = curR = -1; }
    void set(const FxSlot& f) override {
        sync = f.str("sync", "1/8"); mode = f.str("mode", "stereo"); ms = f.num("time", 375); spread = f.num("spread", 0); fb = f.num("fb", 0.45);
        drive = f.num("drive", 0); wow = f.num("wow", 0); duck = f.num("duck", 0); m.mix(f.num("mix", 0.3));
        const double l = f.num("lo", 120), h = f.num("hi", 6000);
        if (l != lo || h != hi) { lo = l; hi = h; hp.set(sr, lo, Q_BW2, 0); lp.set(sr, hi, Q_BW2, 0); }
    }
    double timeSec() const { const double nb = noteBeats(sync); return nb > 0 ? nb * 60 / std::max(20.0, bpm) : ms / 1000; }
    void process(float* L, float* R, int n) override {
        juce::ScopedNoDenormals nd;
        const double maxD = (double)cap - 0.06 * sr - 4;
        const double tl = clampd(timeSec() * sr, 16, maxD), tr = clampd(tl * (1 + spread), 16, maxD);
        if (curL < 0) { curL = tl; curR = tr; }
        const double gk = 1 - std::exp(-1 / (0.08 * sr)), ea = std::exp(-1 / (0.005 * sr)), er = std::exp(-1 / (0.25 * sr));
        const bool pp = mode == "ping-pong"; const double dg = 1 + drive * 4;
        for (int i = 0; i < n; i++) {
            curL += (tl - curL) * gk; curR += (tr - curR) * gk;
            ph1 += 0.55 / sr; ph1 -= std::floor(ph1); ph2 += 6.3 / sr; ph2 -= std::floor(ph2);
            const double wobble = wow * (0.004 * sr * std::sin(2 * PI * ph1) + 0.0006 * sr * std::sin(2 * PI * ph2));
            float yl = dl.read(std::max(2.0, curL + wobble)), yr = dr.read(std::max(2.0, curR + wobble));
            float fl = yl, fr = yr; hp.tick(fl, fr); lp.tick(fl, fr);
            if (drive > 0) { fl = (float)(std::tanh(fl * dg) / dg); fr = (float)(std::tanh(fr * dg) / dg); }
            if (pp) { dl.push((float)(0.5 * (L[i] + R[i]) + fb * fr)); dr.push((float)(fb * fl)); }
            else { dl.push((float)(L[i] + fb * fl)); dr.push((float)(R[i] + fb * fr)); }
            const double x = std::max(std::abs(L[i]), std::abs(R[i])); env = x > env ? ea * env + (1 - ea) * x : er * env + (1 - er) * x;
            const double wg = m.wet * (1 - duck * std::min(1.0, env * 2));
            L[i] = (float)(L[i] * m.dry + yl * wg); R[i] = (float)(R[i] * m.dry + yr * wg);
        }
    }
};

// Glitch: on every `grid` step of the song (or of its own clock when stopped) a seeded random choice, the same on
// every play and export, picks pass / repeat (loop the last `slice` before the step) / reverse (play the previous step
// backwards) / tape stop (the read speed falls to zero across the step) / gate (slices chopped on and off). 2 ms fades
// at every cut.
class Glitch : public Effect {
    std::vector<float> bl, br; size_t W = 0, wp = 0; WetDry m;
    double grid = 0.5, slice = 0.125, pRep = 0.3, pRev = 0.15, pStop = 0.1, pGate = 0.1, own = 0; uint64_t seed = 1;
    int64_t step = INT64_MIN; int act = 0; double stepStart = 0, readPos = 0;
    static double hash(uint64_t s, int64_t k) { uint64_t z = s * 0x9E3779B97F4A7C15ull + (uint64_t)k * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; z ^= z >> 31; return (double)(z >> 11) / 9007199254740992.0; }
    inline float rd(const std::vector<float>& b, double p) const { while (p < 0) p += (double)W; const size_t i = (size_t)p % W; const double f = p - std::floor(p); return (float)(b[i] * (1 - f) + b[(i + 1) % W] * f); }
public:
    void prepare(double s, int b) override { Effect::prepare(s, b); W = (size_t)(8.5 * s); bl.assign(W, 0.f); br.assign(W, 0.f); wp = 0; step = INT64_MIN; }
    void set(const FxSlot& f) override {
        grid = std::max(0.0625, noteBeats(f.str("grid", "1/8"))); slice = std::max(1.0 / 64, noteBeats(f.str("slice", "1/32")));
        pRep = f.num("repeat", 0.3); pRev = f.num("reverse", 0.15); pStop = f.num("stop", 0.1); pGate = f.num("gate", 0.1); seed = (uint64_t)std::lround(f.num("seed", 1)); m.mix(f.num("mix", 1));
    }
    void process(float* L, float* R, int n) override {
        const double spb = 60.0 / std::max(20.0, bpm) * sr;   // samples per beat
        double pos = beat >= 0 ? beat : own;                 // beats
        const double stepS = grid * spb, sliceS = std::min(slice * spb, stepS), fade = 0.002 * sr;
        for (int i = 0; i < n; i++, pos += 1 / spb) {
            bl[wp] = L[i]; br[wp] = R[i];
            const int64_t k = (int64_t)std::floor(pos / grid + 1e-9);
            const double off = (pos - (double)k * grid) * spb;   // samples into this step
            if (k != step) {
                step = k; stepStart = (double)wp - off;
                const double u = hash(seed, k); double c = 0; act = 0;
                if (u < (c += pRep)) act = 1; else if (u < (c += pRev)) act = 2; else if (u < (c += pStop)) act = 3; else if (u < (c += pGate)) act = 4;
                readPos = stepStart;
            }
            float yl = L[i], yr = R[i]; double g = 1;
            if (act == 1) { const double j = std::fmod(off, sliceS); const double p = stepStart - sliceS + j; yl = rd(bl, p); yr = rd(br, p); g = std::min({ 1.0, j / fade, (sliceS - j) / fade }); }
            else if (act == 2) { const double p = stepStart - 1 - off; yl = rd(bl, p); yr = rd(br, p); g = std::min({ 1.0, off / fade, (stepS - off) / fade }); }
            else if (act == 3) { const double sp = std::max(0.0, 1 - off / stepS); readPos += sp * sp; yl = rd(bl, readPos); yr = rd(br, readPos); g = std::min(1.0, (stepS - off) / fade); }
            else if (act == 4) { const double j = std::fmod(off, sliceS); g = j < sliceS * 0.5 ? std::min({ 1.0, j / fade, (sliceS * 0.5 - j) / fade }) : 0; }
            g = std::max(0.0, g);
            if (act) { yl = (float)(yl * g); yr = (float)(yr * g); }
            L[i] = (float)(L[i] * m.dry + yl * m.wet); R[i] = (float)(R[i] * m.dry + yr * m.wet);
            wp = (wp + 1) % W;
        }
        own = pos;
    }
};

// Vocoder (channel vocoder, filter bank): sits on the voice's bus. The voice (modulator) is split into `bands` log-spaced
// bands between `lo` and `hi` Hz (two cascaded band-passes each); an envelope follower per band sets the level of the
// same band of the carrier, so the carrier takes on the voice's changing spectrum. The carrier is another bus (side
// chain) or a built-in buzz (band-limited saw at `pitch` with a little noise). `shift` moves the carrier bands up or
// down (formant shift); `noise` adds hiss where the voice has sibilance above 5 kHz, so consonants stay clear.
class Vocoder : public Effect {
    static constexpr int MB = 32;
    Biquad mod1[MB], mod2[MB]; Biquad2 car1[MB], car2[MB]; double env[MB] = {};
    Biquad sibHp; Biquad2 hissHp; double sibEnv = 0, ph = 0;
    std::string src; int nb = -1; double lo = -1, hi = -1, qm = -1, shift = -99, att = 0.004, rel = 0.04, noise = 0.3, pitch = 48, out = 1; WetDry m;
    std::vector<float> kl, kr; bool haveSide = false; const WaveTable* saw = nullptr; Rng rng{ 77 };
    void design() {
        const double r = std::pow(hi / lo, 1.0 / std::max(1, nb - 1)), q = qm / (std::sqrt(r) - 1 / std::sqrt(r)), sh = std::pow(2.0, shift);
        for (int k = 0; k < nb; k++) {
            const double f = lo * std::pow(r, k), fc = std::min(f * sh, sr * 0.45);
            for (Biquad* b : { &mod1[k], &mod2[k] }) { b->type = FilterType::Bandpass; b->set(sr, f, q, 0); }
            for (Biquad2* b : { &car1[k], &car2[k] }) { b->setType(FilterType::Bandpass); b->set(sr, fc, q, 0); }
        }
    }
public:
    void prepare(double s, int b) override {
        Effect::prepare(s, b); kl.assign((size_t)std::max(b, 4096), 0.f); kr.assign(kl.size(), 0.f); saw = &WaveTable::get("sawtooth"); nb = -1;
        sibHp.type = FilterType::Highpass; sibHp.set(s, 5000, Q_BW2, 0); hissHp.setType(FilterType::Highpass); hissHp.set(s, 5000, Q_BW2, 0);
    }
    void set(const FxSlot& f) override {
        src = f.str("src", ""); att = f.num("att", 0.004); rel = f.num("rel", 0.04); noise = f.num("noise", 0.3); pitch = f.num("pitch", 48); out = dbToGain(f.num("out", 0)); m.mix(f.num("mix", 1));
        const int b = (int)clampd(std::round(f.num("bands", 20)), 4, MB); const double l = f.num("lo", 100), h = std::max(l * 2, f.num("hi", 8000)), q = f.num("q", 1), s = f.num("shift", 0);
        if (b != nb || l != lo || h != hi || q != qm || s != shift) { nb = b; lo = l; hi = h; qm = q; shift = s; design(); }
    }
    std::string sideSource() const override { return src; }
    void side(const float* L, const float* R, int n) override { std::copy(L, L + n, kl.begin()); std::copy(R, R + n, kr.begin()); haveSide = true; }
    void process(float* L, float* R, int n) override {
        juce::ScopedNoDenormals nd;
        const bool ext = haveSide && !src.empty(); haveSide = false;
        const double ca = std::exp(-1 / (std::max(1e-4, att) * sr)), cr = std::exp(-1 / (std::max(1e-3, rel) * sr)), f0 = mtof(pitch);
        const double g = out * 2.5 * std::sqrt((double)nb);   // band envelopes are each a fraction of the voice's level
        for (int i = 0; i < n; i++) {
            const double v = 0.5 * (L[i] + R[i]);
            float cl, cr2;
            if (ext) { cl = kl[(size_t)i]; cr2 = kr[(size_t)i]; }
            else { const float z = saw->read(ph, f0, sr) * 0.5f + (float)(rng.bi() * 0.04); ph += f0 / sr; ph -= std::floor(ph); cl = cr2 = z; }
            double ol = 0, orr = 0;
            for (int k = 0; k < nb; k++) {
                const double x = std::abs(mod2[k].tick(mod1[k].tick(v)));
                env[k] = x > env[k] ? ca * env[k] + (1 - ca) * x : cr * env[k] + (1 - cr) * x;
                float a = cl, b = cr2; car1[k].tick(a, b); car2[k].tick(a, b);
                ol += a * env[k]; orr += b * env[k];
            }
            const double sx = std::abs(sibHp.tick(v)); sibEnv = sx > sibEnv ? ca * sibEnv + (1 - ca) * sx : cr * sibEnv + (1 - cr) * sx;
            float hl = (float)rng.bi(), hr = (float)rng.bi(); hissHp.tick(hl, hr);
            ol = ol * g + hl * sibEnv * noise * 2; orr = orr * g + hr * sibEnv * noise * 2;
            L[i] = (float)(L[i] * m.dry + ol * m.wet); R[i] = (float)(R[i] * m.dry + orr * m.wet);
        }
    }
};

// Meters: passes audio through untouched and feeds the slot's MeterTap: the recent samples (the panel draws the spectrum,
// the scope and the tuner from them), loudness after ITU-R BS.1770 (K-weighting: a +4 dB shelf at 1682 Hz and a
// high-pass at 38 Hz; 400 ms momentary, 3 s short-term, integrated with the -70 LUFS absolute and -10 LU relative
// gates over 400 ms blocks every 100 ms), true peak (4x oversampled sample peak, held), and stereo correlation.
class Meters : public Effect {
    std::shared_ptr<MeterTap> tap; Biquad2 shelf, hp; std::unique_ptr<juce::dsp::Oversampling<float>> os; juce::AudioBuffer<float> buf;
    double sub[30] = {}; int subN = 0, subFill = 0, subLen = 4800; double acc = 0; uint32_t seenReset = 0;
    double hcnt[760] = {}, hpow[760] = {};   // integrated loudness: a histogram of block loudness in 0.1 LU steps from -70 LUFS
    double tp = 0, lr = 0, ll = 0, rr = 0;
    static float lufs(double ms) { return ms > 1e-20 ? (float)(-0.691 + 10 * std::log10(ms)) : -200.f; }
    void resetAll() { std::fill(std::begin(hcnt), std::end(hcnt), 0.0); std::fill(std::begin(hpow), std::end(hpow), 0.0); tp = 0; if (tap) { tap->integrated = -200; tap->truePeak = -200; } }
    void block() {   // every 100 ms
        sub[subN % 30] = acc / subLen; subN++; acc = 0;
        auto avg = [&](int k) { const int m = std::min(k, subN); double t = 0; for (int i = 0; i < m; i++) t += sub[(subN - 1 - i) % 30]; return m ? t / m : 0.0; };
        const double mom = avg(4); tap->momentary = lufs(mom); tap->shortTerm = lufs(avg(30));
        if (subN >= 4) { const double L = lufs(mom); if (L > -70) { const int b = std::min(759, (int)((L + 70) * 10)); hcnt[b] += 1; hpow[b] += mom; } }
        double n = 0, p = 0; for (int b = 0; b < 760; b++) { n += hcnt[b]; p += hpow[b]; }
        if (n > 0) {
            const double rel = lufs(p / n) - 10; double n2 = 0, p2 = 0;
            for (int b = 0; b < 760; b++) if (-70 + b * 0.1 + 0.05 > rel) { n2 += hcnt[b]; p2 += hpow[b]; }
            tap->integrated = n2 > 0 ? lufs(p2 / n2) : -200.f;
        }
    }
public:
    void prepare(double s, int b) override {
        Effect::prepare(s, b); subLen = std::max(1, (int)std::lround(0.1 * s)); subFill = 0; subN = 0; acc = 0;
        {   // the K-weighting filters at any rate, from BS.1770's analogue prototypes (the bilinear form libebur128 uses)
            double K = std::tan(PI * 1681.974450955533 / s), Q = 0.7071752369554196, Vh = std::pow(10.0, 3.999843853973347 / 20), Vb = std::pow(Vh, 0.4996667741545416);
            double a0 = 1 + K / Q + K * K;
            for (Biquad* q : { &shelf.l, &shelf.r }) { q->b0 = (Vh + Vb * K / Q + K * K) / a0; q->b1 = 2 * (K * K - Vh) / a0; q->b2 = (Vh - Vb * K / Q + K * K) / a0; q->a1 = 2 * (K * K - 1) / a0; q->a2 = (1 - K / Q + K * K) / a0; }
            K = std::tan(PI * 38.13547087602444 / s); Q = 0.5003270373238773; a0 = 1 + K / Q + K * K;
            for (Biquad* q : { &hp.l, &hp.r }) { q->b0 = 1; q->b1 = -2; q->b2 = 1; q->a1 = 2 * (K * K - 1) / a0; q->a2 = (1 - K / Q + K * K) / a0; }
        }
        os = std::make_unique<juce::dsp::Oversampling<float>>(2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, false);
        os->initProcessing((size_t)std::max(b, 4096)); buf.setSize(2, std::max(b, 4096));
    }
    void attach(const std::string& id) override { tap = MeterTap::forSlot(id); tap->rate = sr; seenReset = tap->resetReq; resetAll(); }
    void set(const FxSlot&) override {}
    void process(float* L, float* R, int n) override {
        if (!tap) return;
        juce::ScopedNoDenormals nd;
        if (tap->resetReq != seenReset) { seenReset = tap->resetReq; resetAll(); }
        uint32_t w = tap->w.load(std::memory_order_relaxed);
        const double c = std::exp(-1 / (0.3 * sr));
        for (int i = 0; i < n; i++) {
            tap->l[w % MeterTap::RING] = L[i]; tap->r[w % MeterTap::RING] = R[i]; w++;
            float a = L[i], b = R[i]; shelf.tick(a, b); hp.tick(a, b); acc += (double)a * a + (double)b * b;
            if (++subFill >= subLen) { subFill = 0; block(); }
            lr = c * lr + (1 - c) * L[i] * R[i]; ll = c * ll + (1 - c) * L[i] * L[i]; rr = c * rr + (1 - c) * R[i] * R[i];
        }
        tap->w.store(w, std::memory_order_release);
        tap->corr = ll * rr > 1e-14 ? (float)clampd(lr / std::sqrt(ll * rr), -1, 1) : 0.f;
        std::copy(L, L + n, buf.getWritePointer(0)); std::copy(R, R + n, buf.getWritePointer(1));
        juce::dsp::AudioBlock<float> blk(buf.getArrayOfWritePointers(), 2, (size_t)n); auto up = os->processSamplesUp(blk);
        for (size_t ch = 0; ch < 2; ch++) { const float* u = up.getChannelPointer(ch); for (size_t i = 0; i < up.getNumSamples(); i++) tp = std::max(tp, (double)std::abs(u[i])); }
        for (int i = 0; i < n; i++) tp = std::max(tp, (double)std::max(std::abs(L[i]), std::abs(R[i])));
        tap->truePeak = tp > 1e-10 ? (float)(20 * std::log10(tp)) : -200.f;
    }
};

// Pitch correction: real-time YIN on a 4x-decimated copy (every 128 samples) finds the voice's period; the correction
// toward the nearest note of the key (the song's or a chosen one) glides over `speed` ms (0 = instant, the hard robotic
// sound); TD-PSOLA moves the pitch while keeping the formants: two-period Hann grains centred on analysis marks one
// input period apart are laid down one output period apart. Unvoiced parts (breaths, consonants) keep their pitch.
// `formant` stretches each grain (moves the formants without the pitch). Latency: lowest period x (1 + formant
// stretch), about 20-30 ms; the dry path is delayed to match.
class PitchFix : public Effect {
    static constexpr int R = 1 << 16, MR = 1024, DR = 2048;
    std::vector<float> inM, inL, inR, acc; int64_t n = 0;
    Biquad aa1, aa2; std::vector<float> dh; int64_t dn = 0; int dec = 4, dcount = 0, W = 256, tMinD = 8, tMaxD = 160; std::vector<double> dbuf;
    double mPos[MR] = {}, mPer[MR] = {}, mRat[MR] = {}; int64_t mCount = 0, mIdx = 0; double lastMark = 0, nextSyn = 0;
    double period = 0, hist[3] = {}, corr = 0, maxP = 600, D = 1000, frMax = 1.42; bool voiced = false; int hopLeft = 0;
    std::string root = "song", scale = "song"; double speed = 20, amount = 1, formant = 0, low = 90; WetDry m; double lastLow = -1;
    inline float at(const std::vector<float>& b, double p) const { const int64_t i = (int64_t)std::floor(p); const double f = p - (double)i; return (float)(b[(size_t)(i & (R - 1))] * (1 - f) + b[(size_t)((i + 1) & (R - 1))] * f); }
    void reset() {
        inM.assign(R, 0.f); inL.assign(R, 0.f); inR.assign(R, 0.f); acc.assign(R, 0.f); dh.assign(DR, 0.f); n = 0; dn = 0; dcount = 0; mCount = 0; mIdx = 0; lastMark = 0; nextSyn = 0; corr = 0; voiced = false; period = 0; hopLeft = 0;
        maxP = sr / std::max(50.0, low); D = std::ceil(maxP * (1 + frMax)) + 4;
        dec = std::max(1, (int)std::lround(sr / 12000)); const double dr = sr / dec; tMinD = std::max(2, (int)(dr / 1200)); tMaxD = std::min(DR / 2 - W - 2, (int)std::ceil(dr / std::max(50.0, low)));
        dbuf.assign((size_t)tMaxD + 2, 0.0);
        for (Biquad* q : { &aa1, &aa2 }) { q->type = FilterType::Lowpass; q->set(sr, std::min(4500.0, dr * 0.4), Q_BW2, 0); }
    }
    double detect() {   // YIN on the newest W + tMax decimated samples: the period in input samples, or 0
        const int64_t end = dn; std::vector<double>& d = dbuf; double rms = 0;
        auto x = [&](int k) { return (double)dh[(size_t)((end - W - tMaxD + k) & (DR - 1))]; };
        for (int j = 0; j < W; j++) rms += x(j) * x(j);
        if (std::sqrt(rms / W) < 0.004) return 0;
        double run = 0; d[0] = 1;
        for (int tau = 1; tau <= tMaxD; tau++) { double s = 0; for (int j = 0; j < W; j++) { const double v = x(j) - x(j + tau); s += v * v; } run += s; d[(size_t)tau] = run > 0 ? s * tau / run : 1; }
        int best = -1; for (int tau = tMinD; tau < tMaxD; tau++) if (d[(size_t)tau] < 0.15) { while (tau + 1 < tMaxD && d[(size_t)tau + 1] < d[(size_t)tau]) tau++; best = tau; break; }
        if (best < 0) return 0;
        const double a = d[(size_t)best - 1], b = d[(size_t)best], c = d[(size_t)best + 1], den = a - 2 * b + c;
        const double t = best + (std::abs(den) > 1e-12 ? 0.5 * (a - c) / den : 0);
        return t * dec;
    }
    double nearestNote(double midi) const {
        const int r = root == "song" ? keyRoot : std::max(0, (int)(std::find(std::begin(NOTE_KEYS), std::end(NOTE_KEYS), root) - std::begin(NOTE_KEYS)) % 12);
        const std::string& sn = scale == "song" ? (keyScale ? *keyScale : scale) : scale;
        const std::vector<int>* sc = &scales()[0].second; for (auto& [k, v] : scales()) if (k == sn) sc = &v;
        double best = std::round(midi), bd = 1e9;
        for (int k = (int)std::floor(midi) - 3; k <= (int)std::ceil(midi) + 3; k++) { const int pc = (((k - r) % 12) + 12) % 12; if (std::find(sc->begin(), sc->end(), pc) != sc->end() && std::abs(k - midi) < bd) { bd = std::abs(k - midi); best = k; } }
        return best;
    }
public:
    static constexpr const char* NOTE_KEYS[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    double latencySamples() const { return D; }
    void prepare(double s, int b) override { Effect::prepare(s, b); lastLow = -1; }
    void set(const FxSlot& f) override {
        root = f.str("key", "song"); scale = f.str("scale", "song"); speed = f.num("speed", 20); amount = f.num("amount", 1); formant = clampd(f.num("formant", 0), -6, 6); m.mix(f.num("mix", 1));
        low = f.num("low", 90); if (low != lastLow) { lastLow = low; reset(); }
    }
    void process(float* L, float* Rr, int nn) override {
        juce::ScopedNoDenormals nd;
        const double fr = std::pow(2.0, formant / 12), hopSec = 128 / sr, k = speed <= 0.5 ? 1.0 : 1 - std::exp(-hopSec / (speed / 1000));
        for (int i = 0; i < nn; i++) {
            const size_t w = (size_t)(n & (R - 1)); const float mono = 0.5f * (L[i] + Rr[i]); inM[w] = mono; inL[w] = L[i]; inR[w] = Rr[i];
            const double fl = aa2.tick(aa1.tick(mono)); if (++dcount >= dec) { dcount = 0; dh[(size_t)(dn & (DR - 1))] = (float)fl; dn++; }
            if (--hopLeft <= 0) {
                hopLeft = 128;
                const double p = dn > W + tMaxD ? detect() : 0;
                hist[0] = hist[1]; hist[1] = hist[2]; hist[2] = p;
                const double med = std::max(std::min(hist[0], hist[1]), std::min(std::max(hist[0], hist[1]), hist[2]));
                voiced = med > 0 && p > 0; if (voiced) period = std::min(p, maxP);
                if (voiced) { const double mi = 69 + 12 * std::log2(sr / period / 440); const double tgt = clampd((nearestNote(mi) - mi) * amount, -6, 6); corr += (tgt - corr) * k; }
            }
            const double Tm = voiced ? period : std::round(0.005 * sr);
            while (lastMark + Tm <= (double)n) { lastMark += Tm; const int64_t s = mCount % MR; mPos[s] = lastMark; mPer[s] = Tm; mRat[s] = voiced ? std::pow(2.0, corr / 12) : 1.0; mCount++; }
            // synthesis: every grain that reaches the output read point p = n - D
            const double p = (double)n - D;
            while (mCount > 0) {
                if (mIdx < mCount - MR + 1) mIdx = mCount - MR + 1;
                if (mIdx < 0) mIdx = 0;
                while (mIdx + 1 < mCount && std::abs(mPos[(mIdx + 1) % MR] - nextSyn) <= std::abs(mPos[mIdx % MR] - nextSyn)) mIdx++;
                int64_t j = mIdx; while (j > 0 && mPos[j % MR] + mPer[j % MR] * fr > (double)n) j--;
                const double a = mPos[j % MR], h = std::min(mPer[j % MR], maxP), rat = mRat[j % MR];
                if (nextSyn > p + h) break;
                if (a + h * fr > (double)n) break;
                const double g = 1 / rat;
                for (int64_t q = (int64_t)std::ceil(nextSyn - h); q <= (int64_t)std::floor(nextSyn + h); q++) {
                    if ((double)q <= p - 1) continue;
                    const double t = (double)q - nextSyn; const double wv = 0.5 * (1 + std::cos(PI * t / h));
                    acc[(size_t)(q & (R - 1))] += (float)(wv * g * at(inM, a + t * fr));
                }
                nextSyn += h / rat;
            }
            float y = 0, dl = 0, dr = 0;
            if (p >= 0) { const size_t pi = (size_t)((int64_t)p & (R - 1)); y = acc[pi]; acc[pi] = 0; dl = inL[pi]; dr = inR[pi]; }
            L[i] = (float)(dl * m.dry + y * m.wet); Rr[i] = (float)(dr * m.dry + y * m.wet);
            n++;
        }
    }
};

// Multiband compressor: three bands split by 4th-order Linkwitz-Riley crossovers at `x1` and `x2` (the low band also
// goes through the second crossover's low + high pair, so all three sum back flat with matching phase); each band has
// its own threshold, ratio and gain, sharing attack / release and a 6 dB soft knee; stereo-linked peak detection, no
// look-ahead. Listen plays one band on its own. No dry / wet blend: the crossovers turn the phase, so a blend would comb.
class Multiband : public Effect {
    struct Lr { Biquad2 a, b; void set(FilterType t, double sr, double f) { for (Biquad2* q : { &a, &b }) { q->setType(t); q->set(sr, f, Q_BW2, 0); } } void tick(float& l, float& r) { a.tick(l, r); b.tick(l, r); } };
    Lr lp1, hp1, lp2, hp2, apL, apH; double x1 = -1, x2 = -1, thr[3] = { -20, -20, -20 }, ratio[3] = { 3, 3, 3 }, gain[3] = {}, gr[3] = {}, att = 0.01, rel = 0.15, out = 1; int listen = -1; WetDry m;
    double comp(int b, double lvl) {   // gain change in dB for a level in dB (soft knee 6 dB)
        const double T = thr[b], Rt = std::max(1.0, ratio[b]), Wk = 6, d = lvl - T;
        if (2 * d < -Wk) return 0; if (2 * std::abs(d) <= Wk) { const double e = d + Wk / 2; return (1 / Rt - 1) * e * e / (2 * Wk); }
        return (1 / Rt - 1) * d;
    }
public:
    double reductionDb(int b) const { return gr[b]; }
    void set(const FxSlot& f) override {
        static const char* K[3][3] = { { "lthr", "lratio", "lgain" }, { "mthr", "mratio", "mgain" }, { "hthr", "hratio", "hgain" } };
        for (int b = 0; b < 3; b++) { thr[b] = f.num(K[b][0], -20); ratio[b] = f.num(K[b][1], 3); gain[b] = f.num(K[b][2], 0); }
        att = f.num("att", 0.01); rel = f.num("rel", 0.15); out = dbToGain(f.num("out", 0)); m.mix(1);   // no dry mix: the bands are phase-turned, a blend would comb
        const std::string li = f.str("listen", "all"); listen = li == "low" ? 0 : li == "mid" ? 1 : li == "high" ? 2 : -1;
        const double a = f.num("x1", 200), b2 = std::max(a * 1.5, f.num("x2", 3000));
        if (a != x1 || b2 != x2) { x1 = a; x2 = b2; lp1.set(FilterType::Lowpass, sr, x1); hp1.set(FilterType::Highpass, sr, x1); lp2.set(FilterType::Lowpass, sr, x2); hp2.set(FilterType::Highpass, sr, x2); apL.set(FilterType::Lowpass, sr, x2); apH.set(FilterType::Highpass, sr, x2); }
    }
    void process(float* L, float* R, int n) override {
        juce::ScopedNoDenormals nd;
        const double ca = std::exp(-1 / (std::max(1e-4, att) * sr)), cr = std::exp(-1 / (std::max(1e-3, rel) * sr));
        const double mk[3] = { dbToGain(gain[0]), dbToGain(gain[1]), dbToGain(gain[2]) };
        for (int i = 0; i < n; i++) {
            float lo[2] = { L[i], R[i] }, rest[2] = { L[i], R[i] };
            lp1.tick(lo[0], lo[1]); hp1.tick(rest[0], rest[1]);
            float a1[2] = { lo[0], lo[1] }, a2[2] = { lo[0], lo[1] }; apL.tick(a1[0], a1[1]); apH.tick(a2[0], a2[1]);   // low band through the 2nd crossover's all-pass
            float band[3][2] = { { a1[0] + a2[0], a1[1] + a2[1] }, { rest[0], rest[1] }, { rest[0], rest[1] } };
            lp2.tick(band[1][0], band[1][1]); hp2.tick(band[2][0], band[2][1]);
            double ol = 0, orr = 0;
            for (int b = 0; b < 3; b++) {
                const double x = std::max(std::abs(band[b][0]), std::abs(band[b][1])), lv = x > 1e-9 ? 20 * std::log10(x) : -180, want = comp(b, lv);
                gr[b] = want < gr[b] ? ca * gr[b] + (1 - ca) * want : cr * gr[b] + (1 - cr) * want;
                if (listen >= 0 && b != listen) continue;
                const double g = dbToGain(gr[b]) * mk[b]; ol += band[b][0] * g; orr += band[b][1] * g;
            }
            L[i] = (float)(L[i] * m.dry + ol * out * m.wet); R[i] = (float)(R[i] * m.dry + orr * out * m.wet);
        }
    }
};

// LFO rate from a note-length choice at the project tempo, or free Hz.
inline double lfoRate(const std::string& sync, double hz, double bpm) { const double nb = noteBeats(sync); return nb > 0 ? std::max(20.0, bpm) / 60 / nb : hz; }

// Flanger: a short delay (`delay` ms) swept by a sine LFO (free or tempo-synced, the right side `phase` degrees later),
// with feedback (negative for the hollow sound) and an equal-power blend.
class Flanger : public Effect {
    DelayLine dl, dr; double base = 2, depth = 0.7, fb = 0.5, hz = 0.25, phase = 90, ph = 0, yl = 0, yr = 0; std::string sync = "free"; WetDry m;
public:
    void prepare(double s, int b) override { Effect::prepare(s, b); dl.init((size_t)(0.05 * s) + 8); dr.init((size_t)(0.05 * s) + 8); }
    void set(const FxSlot& f) override { base = f.num("delay", 2); depth = f.num("depth", 0.7); fb = f.num("fb", 0.5); hz = f.num("rate", 0.25); sync = f.str("sync", "free"); phase = f.num("phase", 90); m.mix(f.num("mix", 0.5)); }
    void process(float* L, float* R, int n) override {
        juce::ScopedNoDenormals nd;
        const double rate = lfoRate(sync, hz, bpm), po = phase / 360, b = base * 0.001 * sr;
        if (beat >= 0 && noteBeats(sync) > 0) { const double cyc = beat / noteBeats(sync); ph = cyc - std::floor(cyc); }   // locked to the song
        for (int i = 0; i < n; i++) {
            const double sl = 0.5 + 0.5 * std::sin(2 * PI * ph), sr2 = 0.5 + 0.5 * std::sin(2 * PI * (ph + po));
            ph += rate / sr; ph -= std::floor(ph);
            const double dL = std::max(1.0, b * (1 - depth * sl)), dR = std::max(1.0, b * (1 - depth * sr2));
            dl.push((float)(L[i] + fb * yl)); dr.push((float)(R[i] + fb * yr));
            yl = dl.read(dL); yr = dr.read(dR);
            L[i] = (float)(L[i] * m.dry + yl * m.wet); R[i] = (float)(R[i] * m.dry + yr * m.wet);
        }
    }
};

// Ring modulator: the signal times a band-limited sine / triangle / square carrier at `freq` Hz (the classic bell and
// robot sounds); `lfo` wobbles the carrier frequency by up to an octave at `lrate` Hz.
class RingMod : public Effect {
    double freq = 440, lfo = 0, lrate = 1, ph = 0, lph = 0; const WaveTable* wt = nullptr; std::string wave = "sine"; WetDry m;
public:
    void prepare(double s, int b) override { Effect::prepare(s, b); wt = &WaveTable::get("sine"); }
    void set(const FxSlot& f) override {
        freq = f.num("freq", 440); lfo = f.num("lfo", 0); lrate = f.num("lrate", 1); m.mix(f.num("mix", 1));
        const std::string w = f.str("wave", "sine"); if (w != wave || !wt) { wave = w; wt = &WaveTable::get(w == "square" ? "square" : w == "triangle" ? "triangle" : "sine"); }
    }
    void process(float* L, float* R, int n) override {
        for (int i = 0; i < n; i++) {
            const double f = freq * std::pow(2.0, lfo * std::sin(2 * PI * lph)); lph += lrate / sr; lph -= std::floor(lph);
            const float c = wt->read(ph, f, sr); ph += f / sr; ph -= std::floor(ph);
            L[i] = (float)(L[i] * m.dry + L[i] * c * m.wet); R[i] = (float)(R[i] * m.dry + R[i] * c * m.wet);
        }
    }
};

// Frequency shifter: an IIR Hilbert transformer (two chains of four first-order-in-z^2 all-passes whose outputs stay
// 90 degrees apart from about 20 Hz to 20 kHz; coefficients after Olli Niemitalo's published design) gives the analytic
// signal, which is turned by `shift` Hz (single sideband). Every partial moves by the same Hz, so harmonics stop being
// harmonic: the metallic, detuned sound. "Opposite" shifts the right side the other way, for width.
class FreqShift : public Effect {
    struct Chain { double c[4]; double x1[4] = {}, x2[4] = {}, y1[4] = {}, y2[4] = {};
        double tick(double x) { for (int k = 0; k < 4; k++) { const double y = c[k] * (x + y2[k]) - x2[k]; x2[k] = x1[k]; x1[k] = x; y2[k] = y1[k]; y1[k] = y; x = y; } return x; } };
    static Chain chainA() { Chain c{ { 0.6923878 * 0.6923878, 0.9360654322959 * 0.9360654322959, 0.9882295226860 * 0.9882295226860, 0.9987488452737 * 0.9987488452737 } }; return c; }
    static Chain chainB() { Chain c{ { 0.4021921162426 * 0.4021921162426, 0.8561710882420 * 0.8561710882420, 0.9722909545651 * 0.9722909545651, 0.9952884791278 * 0.9952884791278 } }; return c; }
    Chain aL = chainA(), bL = chainB(), aR = chainA(), bR = chainB(); double dL = 0, dR = 0, shift = 100, ph = 0; bool opp = false; WetDry m;
public:
    void set(const FxSlot& f) override { shift = f.num("shift", 100) + f.num("fine", 0); opp = f.str("stereo", "same") == "opposite"; m.mix(f.num("mix", 1)); }
    void process(float* L, float* R, int n) override {
        juce::ScopedNoDenormals nd;
        for (int i = 0; i < n; i++) {
            const double c = std::cos(2 * PI * ph), s = std::sin(2 * PI * ph); ph += shift / sr; ph -= std::floor(ph);
            const double iL = dL, qL = bL.tick(L[i]); dL = aL.tick(L[i]);   // the first chain is one sample later
            const double iR = dR, qR = bR.tick(R[i]); dR = aR.tick(R[i]);
            const double yl = iL * c + qL * s, yr = opp ? iR * c - qR * s : iR * c + qR * s;   // + : up by `shift`
            L[i] = (float)(L[i] * m.dry + yl * m.wet); R[i] = (float)(R[i] * m.dry + yr * m.wet);
        }
    }
};

// Rotary speaker: a crossover at 800 Hz splits a horn (highs) and a drum (lows), each spinning at its own slow / fast
// speed with inertia (the horn spins up in about half a second, the drum in two and a half). Each rotor gives
// amplitude and Doppler (pitch) modulation and swings between the two microphones; `drive` adds the amplifier's grit.
class Rotary : public Effect {
    Biquad2 lo1, lo2, hi1, hi2; DelayLine hd, dd; double hornHz = 0.8, drumHz = 0.7, hph = 0, dph = 0.25, drive = 0, width = 1; std::string speed = "slow"; WetDry m;
public:
    void prepare(double s, int b) override {
        Effect::prepare(s, b); hd.init((size_t)(0.01 * s) + 8); dd.init((size_t)(0.01 * s) + 8);
        for (Biquad2* q : { &lo1, &lo2 }) { q->setType(FilterType::Lowpass); q->set(s, 800, Q_BW2, 0); }
        for (Biquad2* q : { &hi1, &hi2 }) { q->setType(FilterType::Highpass); q->set(s, 800, Q_BW2, 0); }
    }
    void set(const FxSlot& f) override { speed = f.str("speed", "slow"); drive = f.num("drive", 0); width = f.num("width", 1); m.mix(f.num("mix", 1)); }
    void process(float* L, float* R, int n) override {
        juce::ScopedNoDenormals nd;
        const bool fast = speed == "fast", stop = speed == "stop";
        const double ht = stop ? 0 : fast ? 6.7 : 0.8, dt = stop ? 0 : fast ? 5.7 : 0.7;
        const double hk = 1 - std::exp(-1 / (0.5 * sr)), dk = 1 - std::exp(-1 / (2.5 * sr)), dg = 1 + drive * 6;
        for (int i = 0; i < n; i++) {
            hornHz += (ht - hornHz) * hk; drumHz += (dt - drumHz) * dk;
            hph += hornHz / sr; hph -= std::floor(hph); dph += drumHz / sr; dph -= std::floor(dph);
            double x = 0.5 * (L[i] + R[i]); if (drive > 0) x = std::tanh(x * dg) / std::tanh(dg);
            float h = (float)x, hb = (float)x, d = (float)x, db = (float)x; hi1.tick(h, hb); hi2.tick(h, hb); lo1.tick(d, db); lo2.tick(d, db);
            hd.push(h); dd.push(d);
            const double hs = std::sin(2 * PI * hph), hc = std::cos(2 * PI * hph), ds = std::sin(2 * PI * dph), dc = std::cos(2 * PI * dph);
            const double hv = hd.read(1 + 0.00035 * sr * (1 + hs)), dv = dd.read(1 + 0.00015 * sr * (1 + ds));   // Doppler
            const double ha = 0.75 + 0.25 * hc, da = 0.85 + 0.15 * dc;                                             // facing the listener or away
            const double pan = hc * width * 0.5, dpan = dc * width * 0.25;
            const double l = hv * ha * (1 - pan) * 0.7 + dv * da * (1 - dpan) * 0.7, r = hv * ha * (1 + pan) * 0.7 + dv * da * (1 + dpan) * 0.7;
            L[i] = (float)(L[i] * m.dry + l * m.wet); R[i] = (float)(R[i] * m.dry + r * m.wet);
        }
    }
};

// Auto-pan / tremolo: an LFO (sine, triangle or a softened square), free or locked to the song's beat, moves the sound
// between the sides (pan) or its level (tremolo) by `depth`.
class AutoPan : public Effect {
    std::string mode = "tremolo", shape = "sine", sync = "1/8"; double hz = 4, depth = 0.5, ph = 0;
    double lfo(double p) const {   // 1 at phase 0, -1 at phase 0.5
        if (shape == "triangle") return 1 - 4 * std::abs(p - std::round(p));
        if (shape == "square") return std::tanh(6 * std::cos(2 * PI * p)) / std::tanh(6.0);
        return std::cos(2 * PI * p);
    }
public:
    void set(const FxSlot& f) override { mode = f.str("mode", "tremolo"); shape = f.str("shape", "sine"); sync = f.str("sync", "1/8"); hz = f.num("rate", 4); depth = f.num("depth", 0.5); }
    void process(float* L, float* R, int n) override {
        const double rate = lfoRate(sync, hz, bpm);
        if (beat >= 0 && noteBeats(sync) > 0) { const double cyc = beat / noteBeats(sync); ph = cyc - std::floor(cyc); }
        for (int i = 0; i < n; i++) {
            const double v = lfo(ph); ph += rate / sr; ph -= std::floor(ph);
            if (mode == "pan") { const double p = clampd(-v * depth, -1, 1); const double gl = std::cos((p + 1) * PI / 4) * std::sqrt(2.0), gr = std::sin((p + 1) * PI / 4) * std::sqrt(2.0); L[i] = (float)(L[i] * gl); R[i] = (float)(R[i] * gr); }
            else { const double g = 1 - depth * (0.5 - 0.5 * v); L[i] = (float)(L[i] * g); R[i] = (float)(R[i] * g); }
        }
    }
};

// Lo-fi: sample-rate reduction (sample and hold, no anti-alias filter, so the images and aliasing stay), bit depth
// (optionally with TPDF dither), wow and flutter (a slowly swept delay), generated record-surface noise and crackle
// (random clicks through a resonant band-pass, the same on every run), band-limiting (24 dB/oct low and high cuts) and
// narrowing toward mono.
class LoFi : public Effect {
    DelayLine dl, dr; Biquad2 lc1, lc2, hc1, hc2; Biquad ck; Biquad2 hiss; Rng rng{ 4242 };
    double rate = 12000, bits = 12, crackle = 0, noise = 0, wow = 0, lo = -1, hi = -1, narrow = 0, ph = 1, hl = 0, hr = 0, w1 = 0, w2 = 0; bool dither = false; WetDry m;
public:
    void prepare(double s, int b) override {
        Effect::prepare(s, b); dl.init((size_t)(0.03 * s) + 8); dr.init((size_t)(0.03 * s) + 8); lo = hi = -1;
        ck.type = FilterType::Bandpass; ck.set(s, 2500, 1.5, 0); hiss.setType(FilterType::Lowpass); hiss.set(s, 5000, Q_BW2, 0);
    }
    void set(const FxSlot& f) override {
        rate = f.num("rate", 12000); bits = f.num("bits", 12); dither = f.str("dither", "off") == "on"; crackle = f.num("crackle", 0); noise = f.num("noise", 0); wow = f.num("wow", 0); narrow = f.num("narrow", 0); m.mix(f.num("mix", 1));
        const double l = f.num("lo", 60), h = f.num("hi", 9000);
        if (l != lo || h != hi) { lo = l; hi = h; for (Biquad2* q : { &lc1, &lc2 }) { q->setType(FilterType::Highpass); q->set(sr, lo, Q_BW2, 0); } for (Biquad2* q : { &hc1, &hc2 }) { q->setType(FilterType::Lowpass); q->set(sr, hi, Q_BW2, 0); } }
    }
    void process(float* L, float* R, int n) override {
        juce::ScopedNoDenormals nd;
        const double step = std::min(1.0, rate / sr), q = std::pow(2.0, bits - 1), clickP = crackle * crackle * 0.0008 * 48000 / sr;
        for (int i = 0; i < n; i++) {
            double l = L[i], r = R[i];
            if (wow > 0) {   // wow (0.55 Hz) and flutter (6.1 Hz) on a delay around 8 ms
                w1 += 0.55 / sr; w1 -= std::floor(w1); w2 += 6.1 / sr; w2 -= std::floor(w2);
                const double d = 0.008 * sr + wow * (0.0025 * sr * std::sin(2 * PI * w1) + 0.0002 * sr * std::sin(2 * PI * w2));
                dl.push((float)l); dr.push((float)r); l = dl.read(d); r = dr.read(d);
            }
            ph += step; if (ph >= 1) { ph -= 1; hl = l; hr = r; } l = hl; r = hr;                      // sample and hold
            if (bits < 16) { const double dl2 = dither ? (rng.next() + rng.next() - 1) : 0, dr2 = dither ? (rng.next() + rng.next() - 1) : 0; l = std::round(l * q + dl2) / q; r = std::round(r * q + dr2) / q; }
            double c = 0; if (crackle > 0 && rng.next() < clickP) c = (rng.next() * 0.25 + 0.05) * (rng.next() < 0.5 ? -1 : 1) * crackle * 8;
            c = ck.tick(c);
            float nl = 0, nr = 0; if (noise > 0) { nl = (float)(rng.bi() * noise * 0.03); nr = (float)(rng.bi() * noise * 0.03); hiss.tick(nl, nr); }
            l += c + nl; r += c * 0.9 + nr;
            float fl = (float)l, fr = (float)r; lc1.tick(fl, fr); lc2.tick(fl, fr); hc1.tick(fl, fr); hc2.tick(fl, fr);
            const double mid = 0.5 * (fl + fr), side = 0.5 * (fl - fr) * (1 - narrow);
            L[i] = (float)(L[i] * m.dry + (mid + side) * m.wet); R[i] = (float)(R[i] * m.dry + (mid - side) * m.wet);
        }
    }
};

class PassThrough : public Effect {
public:
    void set(const FxSlot&) override {}
    void process(float*, float*, int) override {}
};

}   // namespace

std::unique_ptr<Effect> makeEffect(const std::string& t) {
    if (t == "eq") return std::make_unique<Eq>();
    if (t == "filter") return std::make_unique<Filter>();
    if (t == "comp") return std::make_unique<Comp>();
    if (t == "limiter") return std::make_unique<Limiter>();
    if (t == "reverb") return std::make_unique<Reverb>();
    if (t == "delay") return std::make_unique<Delay>();
    if (t == "chorus") return std::make_unique<Chorus>();
    if (t == "phaser") return std::make_unique<Phaser>();
    if (t == "dist") return std::make_unique<Dist>();
    if (t == "crush") return std::make_unique<Crush>();
    if (t == "duck") return std::make_unique<Duck>();
    if (t == "peq") return std::make_unique<ParamEq>();
    if (t == "deess") return std::make_unique<DeEsser>();
    if (t == "sat") return std::make_unique<Saturator>();
    if (t == "fdn") return std::make_unique<FdnReverb>();
    if (t == "gate") return std::make_unique<Gate>();
    if (t == "trans") return std::make_unique<Transient>();
    if (t == "imager") return std::make_unique<Imager>();
    if (t == "echo") return std::make_unique<EchoLine>();
    if (t == "glitch") return std::make_unique<Glitch>();
    if (t == "vocoder") return std::make_unique<Vocoder>();
    if (t == "meters") return std::make_unique<Meters>();
    if (t == "pitchfix") return std::make_unique<PitchFix>();
    if (t == "multiband") return std::make_unique<Multiband>();
    if (t == "flanger") return std::make_unique<Flanger>();
    if (t == "ringmod") return std::make_unique<RingMod>();
    if (t == "fshift") return std::make_unique<FreqShift>();
    if (t == "rotary") return std::make_unique<Rotary>();
    if (t == "autopan") return std::make_unique<AutoPan>();
    if (t == "lofi") return std::make_unique<LoFi>();
    return std::make_unique<PassThrough>();   // plug-ins, until hosting is ported
}

}   // namespace us
