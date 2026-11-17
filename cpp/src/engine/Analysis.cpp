#include "Analysis.h"
#include "Dsp.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <juce_dsp/juce_dsp.h>

namespace us {

const char* const NOTE_NAMES[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

std::vector<float> toMono(const Sample& s) {
    std::vector<float> o(s.length(), 0.f); const float k = 1.f / (float)std::max<size_t>(1, s.ch.size());
    for (auto& c : s.ch) for (size_t i = 0; i < o.size(); i++) o[i] += c[i] * k;
    return o;
}
static std::vector<float> decimate(const std::vector<float>& x, double sr, double target, double& outSr) {
    const int f = std::max(1, (int)std::round(sr / target)); outSr = sr / f;
    if (f == 1) return x;
    std::vector<float> o(x.size() / (size_t)f);
    for (size_t i = 0, j = 0; i < o.size(); i++) { double s = 0; for (int k = 0; k < f; k++) s += x[j++]; o[i] = (float)(s / f); }
    return o;
}
// short-time magnitude spectra, Hann window; fn(mag, frame)
static void stft(std::vector<float> x, int order, int hop, const std::function<void(std::vector<float>&, int)>& fn, Progress prog) {
    const int N = 1 << order; if ((int)x.size() < N) x.resize((size_t)N, 0.f);
    juce::dsp::FFT fft(order); std::vector<float> win((size_t)N), buf((size_t)(2 * N)), mag((size_t)(N / 2));
    for (int i = 0; i < N; i++) win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * juce::MathConstants<double>::pi * i / N));
    const int total = ((int)x.size() - N) / hop + 1;
    for (int f = 0; f < total; f++) {
        std::fill(buf.begin(), buf.end(), 0.f);
        for (int i = 0; i < N; i++) buf[(size_t)i] = x[(size_t)(f * hop + i)] * win[(size_t)i];
        fft.performFrequencyOnlyForwardTransform(buf.data(), true);
        for (int i = 0; i < N / 2; i++) mag[(size_t)i] = buf[(size_t)i];
        fn(mag, f);
        if (prog && f % 40 == 39) prog((double)f / total);
    }
}

static const double MAJ[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 }, MIN_[12] = { 6.33, 2.68, 3.52, 5.38, 2.6, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
static KeyResult keyFromChroma(const std::array<double, 12>& ch) {
    std::vector<KeyResult> res; double cm = 0; for (double v : ch) cm += v / 12;
    for (int root = 0; root < 12; root++) for (int mi = 0; mi < 2; mi++) {
        const double* prof = mi ? MIN_ : MAJ; double pm = 0; for (int i = 0; i < 12; i++) pm += prof[i] / 12;
        double num = 0, da = 0, db = 0;
        for (int i = 0; i < 12; i++) { const double a = ch[(size_t)((i + root) % 12)] - cm, b = prof[i] - pm; num += a * b; da += a * a; db += b * b; }
        KeyResult k; k.root = root; k.minor = mi; k.r = da > 0 ? num / std::sqrt(da * db) : 0; k.name = std::string(NOTE_NAMES[root]) + (mi ? " minor" : " major");
        const int n12 = ((((mi ? root + 3 : root) * 7) % 12 + 7) % 12) + 1; k.camelot = std::to_string(n12) + (mi ? "A" : "B");
        res.push_back(k);
    }
    std::sort(res.begin(), res.end(), [](auto& a, auto& b) { return a.r > b.r; });
    KeyResult best = res[0]; double mx = 0; for (double v : ch) mx = std::max(mx, v); if (mx <= 0) mx = 1;
    for (int i = 0; i < 12; i++) best.chroma[(size_t)i] = ch[(size_t)i] / mx;
    best.alts.assign(res.begin() + 1, res.begin() + 4);
    best.conf = std::clamp((res[0].r - res[1].r) * 4 + res[0].r * 0.5, 0.0, 1.0);
    return best;
}
KeyResult detectKey(const std::vector<float>& xin, double srIn, Progress prog) {
    double sr; auto x = decimate(xin, srIn, 11025, sr); const int order = 12, N = 1 << order;
    std::array<double, 12> chroma{}; std::vector<int> pc((size_t)(N / 2), -1);
    for (int i = 1; i < N / 2; i++) { const double f = i * sr / N; if (f >= 60 && f <= 2100) pc[(size_t)i] = (((int)std::lround(12 * std::log2(f / 440) + 69) % 12) + 12) % 12; }
    stft(x, order, 2048, [&](std::vector<float>& m, int) { for (int i = 1; i < N / 2; i++) if (pc[(size_t)i] >= 0) chroma[(size_t)pc[(size_t)i]] += m[(size_t)i]; }, prog);
    return keyFromChroma(chroma);
}
KeyResult keyFromNotes(const std::vector<Note>& notes) { std::array<double, 12> ch{}; for (auto& n : notes) ch[(size_t)(((n.n % 12) + 12) % 12)] += n.d; return keyFromChroma(ch); }

double detectBpm(const std::vector<float>& xin, double srIn) {
    double sr; auto x = decimate(xin, srIn, 11025, sr); const int hop = 128; const int n = (int)x.size() / hop; const double fps = sr / hop;
    if (n < fps * 3) return 0;
    std::vector<double> o((size_t)n); double prev = 0, mean = 0;
    for (int k = 0; k < n; k++) { double s = 0; for (int j = 0; j < hop; j++) { const double v = x[(size_t)(k * hop + j)]; s += v * v; } const double e = std::log(1 + 100 * std::sqrt(s / hop)); o[(size_t)k] = std::max(0.0, e - prev); prev = e; mean += o[(size_t)k]; }
    mean /= n; for (auto& v : o) v -= mean;
    const int lo = (int)std::floor(fps * 60 / 200), hi = (int)std::ceil(fps * 60 / 60); std::vector<double> ac((size_t)hi + 2, 0); double best = -1; int bl = 0;
    for (int lag = lo - 1; lag <= hi + 1; lag++) {
        if (lag <= 0 || lag >= n) continue;
        double s = 0; for (int k = lag; k < n; k++) s += o[(size_t)k] * o[(size_t)(k - lag)];
        const double w = std::log2(60 * fps / lag / 120) / 0.8; ac[(size_t)lag] = (s / (n - lag)) * std::exp(-0.5 * w * w);
        if (lag >= lo && lag <= hi && ac[(size_t)lag] > best) { best = ac[(size_t)lag]; bl = lag; }
    }
    if (!bl || best <= 0) return 0;
    const double a = ac[(size_t)bl - 1], b = ac[(size_t)bl], c = ac[(size_t)bl + 1], den = a - 2 * b + c;
    return std::round(600 * fps / (bl + (den != 0 ? (a - c) / (2 * den) : 0))) / 10;
}

// YIN on one frame: the period in samples, or 0 when unvoiced
static double yin(const std::vector<float>& x, size_t off, int W, int tMin, int tMax, std::vector<double>& d) {
    double run = 0, bv = 1; int best = 0; d[0] = 1;
    for (int tau = 1; tau <= tMax; tau++) { double s = 0; for (int j = 0; j < W; j++) { const double q = x[off + (size_t)j] - x[off + (size_t)(j + tau)]; s += q * q; } run += s; d[(size_t)tau] = run > 0 ? s * tau / run : 1; }
    for (int tau = tMin; tau < tMax; tau++) {
        if (d[(size_t)tau] < 0.12) { while (tau + 1 < tMax && d[(size_t)tau + 1] < d[(size_t)tau]) tau++; best = tau; break; }
        if (d[(size_t)tau] < bv) { bv = d[(size_t)tau]; best = tau; }
    }
    if (!best || d[(size_t)best] > 0.2) return 0;
    const double a = d[(size_t)best - 1], b = d[(size_t)best], c = d[(size_t)best + 1], den = a - 2 * b + c;
    return best + (den != 0 ? (a - c) / (2 * den) : 0);
}

std::vector<FoundNote> melodyNotes(const std::vector<float>& xin, double srIn, const MelodyOpts& o, Progress prog) {
    double sr; auto x = decimate(xin, srIn, 11025, sr); const int W = 512, hop = 128;
    const int tMin = std::max(2, (int)std::floor(sr / 1600)), tMax = (int)std::ceil(sr / o.minHz); std::vector<double> d((size_t)tMax + 2);
    const int n = std::max(0, ((int)x.size() - W - tMax - 1) / hop); std::vector<double> raw((size_t)n), amp((size_t)n); double peak = 0;
    for (int k = 0; k < n; k++) { double s = 0; const size_t off = (size_t)(k * hop); for (int j = 0; j < W; j++) s += x[off + (size_t)j] * x[off + (size_t)j]; amp[(size_t)k] = std::sqrt(s / W); peak = std::max(peak, amp[(size_t)k]); }
    const double gate = peak * o.gate;
    for (int k = 0; k < n; k++) {
        const double tau = amp[(size_t)k] < gate ? 0 : yin(x, (size_t)(k * hop), W, tMin, tMax, d);
        raw[(size_t)k] = tau > 0 ? 69 + 12 * std::log2(sr / tau / 440) : NAN;
        if (prog && k % 300 == 299) prog((double)k / n);
    }
    std::vector<double> pit((size_t)n); std::vector<double> w;
    for (int k = 0; k < n; k++) { w.clear(); for (int j = std::max(0, k - 2); j <= std::min(n - 1, k + 2); j++) if (!std::isnan(raw[(size_t)j])) w.push_back(raw[(size_t)j]); if (w.size() >= 3) { std::sort(w.begin(), w.end()); pit[(size_t)k] = w[w.size() / 2]; } else pit[(size_t)k] = NAN; }
    const double hs = hop / sr; const int minF = std::max(2, (int)std::lround(o.minDur / hs));
    std::vector<FoundNote> notes; struct Cur { int start, end; double sum; int cnt; double pk; }; std::optional<Cur> cur; int gap = 0;
    auto close = [&] { if (cur && cur->end - cur->start + 1 >= minF) notes.push_back({ cur->start * hs, (cur->end + 1) * hs, (int)std::lround(cur->sum / cur->cnt), std::min(1.0, 0.3 + 0.7 * std::sqrt(cur->pk / peak)) }); cur.reset(); };
    for (int k = 0; k < n; k++) {
        const double m = pit[(size_t)k];
        if (std::isnan(m)) { if (cur && ++gap > 3) close(); continue; }
        const bool reattack = cur && k - cur->start > 6 && k >= 3 && amp[(size_t)k] > amp[(size_t)k - 3] * 1.9 && amp[(size_t)k - 3] < cur->pk * 0.5;
        if (cur && !reattack && std::abs(m - cur->sum / cur->cnt) < 0.8) { cur->end = k; cur->sum += m; cur->cnt++; cur->pk = std::max(cur->pk, amp[(size_t)k]); gap = 0; }
        else { close(); cur = Cur{ k, k, m, 1, amp[(size_t)k] }; gap = 0; }
    }
    close();
    // drop brief octave-jump blips, then join split notes
    auto far = [](const FoundNote& a, const FoundNote* b) { return !b || std::abs(a.n - b->n) >= 11; };
    std::vector<FoundNote> kept, out;
    for (size_t i = 0; i < notes.size(); i++) { const auto& nn = notes[i]; const FoundNote* pv = i ? &notes[i - 1] : nullptr; const FoundNote* nx = i + 1 < notes.size() ? &notes[i + 1] : nullptr; if (!(nn.e - nn.s < 0.16 && far(nn, pv) && far(nn, nx) && notes.size() > 2)) kept.push_back(nn); }
    for (auto& nn : kept) { if (!out.empty() && out.back().n == nn.n && nn.s - out.back().e < 0.05) { out.back().e = nn.e; out.back().v = std::max(out.back().v, nn.v); } else out.push_back(nn); }
    return out;
}

std::vector<FoundNote> chordNotes(const std::vector<float>& xin, double srIn, const ChordOpts& o, Progress prog) {
    double sr; auto x = decimate(xin, srIn, 22050, sr); const int order = 12, N = 1 << order, hop = 1024, lo = 40, hi = 96, H = 6;
    std::vector<std::vector<int>> bins;
    for (int p = lo; p <= hi; p++) { const double f = mtof(p); std::vector<int> b; for (int h = 1; h <= H; h++) { const int k = (int)std::lround(f * h * N / sr); if (k < N / 2 - 3) b.push_back(k); } bins.push_back(b); }
    std::vector<std::vector<std::pair<int, double>>> frames; std::vector<double> firsts;
    stft(x, order, hop, [&](std::vector<float>& m, int) {
        std::vector<std::pair<int, double>> picks;
        for (int it = 0; it < o.maxPoly; it++) {
            int bp = -1; double bs = 0;
            for (size_t i = 0; i < bins.size(); i++) {
                const auto& b = bins[i]; if (b.empty() || b[0] < 1) continue;
                auto at = [&](int k) { return std::max({ m[(size_t)k - 1], m[(size_t)k], m[(size_t)k + 1] }); };
                const double f0 = at(b[0]); double s = 0; for (size_t h = 0; h < b.size(); h++) s += at(b[h]) / (double)(h + 1);
                if (f0 > s * 0.22 && s > bs) { bs = s; bp = (int)i; }
            }
            if (bp < 0 || (!picks.empty() && bs < picks[0].second * 0.3)) break;
            picks.push_back({ bp + lo, bs });
            for (int k : bins[(size_t)bp]) for (int j = -2; j <= 2; j++) if (k + j >= 0 && k + j < N / 2) m[(size_t)(k + j)] = 0;
        }
        if (!picks.empty()) firsts.push_back(picks[0].second);
        frames.push_back(picks);
    }, prog);
    if (firsts.empty()) return {};
    std::sort(firsts.begin(), firsts.end());
    const double ref = firsts[(size_t)(firsts.size() * 0.9)], thr = ref * o.gate * 2.5, hs = hop / sr;
    struct Run { int start, last; double sum; int cnt; }; std::map<int, Run> on; std::vector<FoundNote> notes;
    auto close = [&](int p, const Run& r) { if (r.last - r.start >= 1) notes.push_back({ r.start * hs, (r.last + 1) * hs, p, std::min(1.0, 0.35 + 0.65 * std::sqrt(r.sum / r.cnt / ref)) }); };
    for (int f = 0; f < (int)frames.size(); f++) {
        for (auto& [p, s] : frames[(size_t)f]) {
            if (s < thr) continue;
            auto it = on.find(p);
            if (it != on.end() && f - it->second.last <= 2) { it->second.last = f; it->second.sum += s; it->second.cnt++; }
            else { if (it != on.end()) close(p, it->second); on[p] = { f, f, s, 1 }; }
        }
        for (auto it = on.begin(); it != on.end();) { if (f - it->second.last > 2) { close(it->first, it->second); it = on.erase(it); } else ++it; }
    }
    for (auto& [p, r] : on) close(p, r);
    std::sort(notes.begin(), notes.end(), [](auto& a, auto& b) { return a.s < b.s; });
    return notes;
}

std::vector<Note> toTicks(const std::vector<FoundNote>& notes, double bpm, int q, bool align) {
    const double tps = bpm / 60 * PPQ; double t0 = 0; if (align && !notes.empty()) { t0 = 1e30; for (auto& n : notes) t0 = std::min(t0, n.s); }
    std::vector<Note> out;
    for (auto& n : notes) {
        double t = (n.s - t0) * tps, d = (n.e - n.s) * tps;
        if (q > 0) { t = std::round(t / q) * q; d = std::max((double)q, std::round(d / q) * q); }
        Note x; x.id = newId(); x.t = std::max(0.0, std::round(t)); x.d = std::max(6.0, std::round(d)); x.n = n.n; x.v = n.v; out.push_back(x);
    }
    return out;
}
const std::vector<std::pair<std::string, std::vector<int>>>& scales() {
    static const std::vector<std::pair<std::string, std::vector<int>>> s = {
        { "chromatic", { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } }, { "major", { 0, 2, 4, 5, 7, 9, 11 } }, { "minor", { 0, 2, 3, 5, 7, 8, 10 } },
        { "dorian", { 0, 2, 3, 5, 7, 9, 10 } }, { "phrygian", { 0, 1, 3, 5, 7, 8, 10 } }, { "lydian", { 0, 2, 4, 6, 7, 9, 11 } }, { "mixolydian", { 0, 2, 4, 5, 7, 9, 10 } },
        { "harmonic minor", { 0, 2, 3, 5, 7, 8, 11 } }, { "melodic minor", { 0, 2, 3, 5, 7, 9, 11 } }, { "major pentatonic", { 0, 2, 4, 7, 9 } }, { "minor pentatonic", { 0, 3, 5, 7, 10 } }, { "blues", { 0, 3, 5, 6, 7, 10 } } };
    return s;
}
int snapToScale(int n, int root, const std::string& scale) {
    const std::vector<int>* sc = &scales()[0].second; for (auto& [k, v] : scales()) if (k == scale) sc = &v;
    for (int d : { 0, 1, -1, 2, -2 }) { const int pc = (((n + d - root) % 12) + 12) % 12; if (std::find(sc->begin(), sc->end(), pc) != sc->end()) return n + d; }
    return n;
}

// the pitch of a short buffer (the live tuner): a MIDI note with cents as the fraction, or -1 when quiet / unclear
double pitchOf(const std::vector<float>& in, double sr) {
    double rate; auto x = decimate(in, sr, 11025, rate); const int W = 256; const int tMax = std::min((int)std::ceil(rate / 70), (int)x.size() - W - 2); if (tMax < 8) return -1;
    double rms = 0; for (float v : x) rms += v * v; rms = std::sqrt(rms / std::max<size_t>(1, x.size())); if (rms < 0.012) return -1;
    std::vector<double> d((size_t)tMax + 2); const double tau = yin(x, 0, W, std::max(2, (int)std::floor(rate / 1500)), tMax, d);
    return tau > 0 ? 69 + 12 * std::log2(rate / tau / 440) : -1;
}
}   // namespace us
