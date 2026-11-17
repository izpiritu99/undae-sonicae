#include "Effects.h"
#include <complex>
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
    return std::make_unique<PassThrough>();   // plug-ins, until hosting is ported
}

}   // namespace us
