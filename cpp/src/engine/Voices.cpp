#include "Voices.h"
#include <cstring>

namespace us {

namespace {

enum Dst { D_PITCH, D_CUT, D_CUT2, D_RES, D_AMP, D_PAN, D_FM, D_RING, D_MORPH, D_OSCA, D_OSCB, D_OSCC, D_SUB, D_NOISE, D_L1, D_L2, D_L3, D_COUNT };
int dstOf(const std::string& s) {
    static const char* names[] = { "pitch", "cutoff", "cutoff2", "res", "amp", "pan", "fm", "ring", "morph", "oscA", "oscB", "oscC", "sub", "noise", "lfo1rate", "lfo2rate", "lfo3rate" };
    for (int i = 0; i < D_COUNT; i++) if (s == names[i]) return i;
    return -1;
}
enum Src { S_L1, S_L2, S_L3, S_MENV, S_MENV2, S_FENV, S_VEL, S_KEY, S_WHEEL, S_RAND, S_M1, S_M2, S_M3, S_M4, S_COUNT };
int srcOf(const std::string& s) {
    static const char* names[] = { "lfo1", "lfo2", "lfo3", "menv", "menv2", "fenv", "vel", "key", "wheel", "rand", "macro1", "macro2", "macro3", "macro4" };
    for (int i = 0; i < S_COUNT; i++) if (s == names[i]) return i;
    return -1;
}

struct Osc { const WaveTable* w; double ratio, det; double ph = 0; int side = 0; double detF = 1; };   // detF: 2^(det/1200), fixed per note   // side: 0 centre, -1 left, +1 right
struct Bank { std::vector<Osc> osc; bool stereo = false; };
struct Layer { bool on = false; Bank a, b; bool morph = false; double level = 0, mo = 0, pan = 0; bool stereo = false; const WaveTable* fr[WT_FRAMES] = {}; bool wt = false; };
enum LfoW { W_SINE, W_TRI, W_SAW, W_SAWDOWN, W_SQUARE, W_SH, W_RAND };
struct Lfo { int wave = W_SINE; double rate = 1, fade = 0, ph = 0; double rnd[64]; bool used = false; };

class SynthVoice : public Voice {
public:
    SynthVoice(const SynthP& p, int n, double vel_, double t, double dur, const NoteOpts& o_, double sr) : P(p), vel(vel_), o(o_) {
        note = n; t0 = t; f = mtof(n);
        Rng& r = globalRng();
        for (auto& m : P.mod) { if (m.src == "none" || m.dst == "none" || m.amt == 0) continue; int d = dstOf(m.dst), s = srcOf(m.src); if (d < 0 || s < 0) continue; mods.push_back({ s, d, m.amt }); usesD[d] = true; usesS[s] = true; }
        uni = std::clamp(P.uni, 1, 9);
        if (o.glideFrom >= 0 && P.glide > 0.001 && o.glideFrom != n) glide = mtof(o.glideFrom) / f;
        auto mkOsc = [&](const std::string& wave, double ratio, double det, int side) { Osc x{ &WaveTable::get(isWaveSet(wave) ? frameName(wave, 0) : wave), ratio, det + (P.drift ? r.bi() * P.drift : 0.0) }; x.side = side; x.detF = std::exp2(x.det / 1200); return x; };
        auto ratioOf = [](const OscP& O) { return std::pow(2.0, O.oct + O.semi / 12.0); };
        for (int i = 0; i < 3; i++) {
            const OscP& O = P.osc[i]; Layer& L = layers[i];
            if (!O.on) continue;
            L.on = true; L.level = O.level / std::sqrt((double)uni); L.pan = O.pan;
            auto bank = [&](const std::string& wave, Bank& b) {
                b.stereo = uni > 1 && P.spread > 0;
                for (int u = 0; u < uni; u++) b.osc.push_back(mkOsc(wave, ratioOf(O), O.fine + (uni > 1 ? ((double)u / (uni - 1) * 2 - 1) * P.detune : 0), b.stereo ? (u % 2 ? 1 : -1) : 0));
            };
            L.mo = O.morph;
            if (isWaveSet(O.wave)) { L.wt = true; for (int k = 0; k < WT_FRAMES; k++) L.fr[k] = &WaveTable::get(frameName(O.wave, k)); bank(O.wave, L.a); }   // Morph scans the frames
            else if (L.mo > 0.001 || usesD[D_MORPH]) { L.morph = true; bank(O.wave, L.a); bank(O.wave2.empty() ? "sine" : O.wave2, L.b); }
            else bank(O.wave, L.a);
            L.stereo = L.a.stereo || L.pan != 0;
            if (L.stereo) stereoMix = true;
        }
        if (layers[0].on && (P.fm > 0.001 || usesD[D_FM])) { hasFm = true; fmOsc = mkOsc(P.osc[1].wave, ratioOf(P.osc[1]), P.osc[1].fine, 0); }
        if (P.ring > 0.001 || usesD[D_RING]) { hasRing = true; ringA = mkOsc(P.osc[0].wave, ratioOf(P.osc[0]), P.osc[0].fine, 0); ringB = mkOsc(P.osc[1].wave, ratioOf(P.osc[1]), P.osc[1].fine, 0); }
        if (P.sub > 0.001 || usesD[D_SUB]) { hasSub = true; subOsc = mkOsc(P.subWave.empty() ? "sine" : P.subWave, std::pow(2.0, P.subOct ? P.subOct : -1), 0, 0); }
        if (P.noise > 0.001 || usesD[D_NOISE]) { hasNoise = true; if (P.noiseTone < 0.98) { noiseLp = true; nlp.type = FilterType::Lowpass; nlp.set(sr, 80 * std::pow(250.0, P.noiseTone), 0.5, 0); } }
        if (P.pm.type == "pluck" || P.pm.type == "mallet") initModel(sr);
        // filters (a "24" type is two cascaded biquads), drive before them, shaper after
        auto mkF = [&](const FilterP& X, Flt& F) {
            if (!X.on) return;
            F.on = true; std::string ty = X.type; F.steep = ty.size() > 2 && ty.compare(ty.size() - 2, 2, "24") == 0; if (F.steep) ty = ty.substr(0, ty.size() - 2);
            for (auto& b : F.b) { b[0].type = b[1].type = filterTypeFrom(ty); }
            F.cut = X.cutoff; F.q = (F.steep ? X.q * 0.55 : X.q) * std::pow(4.0, o.res); F.det = X.key * (n - 60) * 100 + o.cut * 3600; F.env = std::abs(X.env) > 0.01 ? X.env * 100 : 0;
        };
        mkF(P.f1, f1); mkF(P.f2, f2);
        if (P.f1.drive > 0.01) { drive.curve = shapeCurve("soft", P.f1.drive); hasDrive = true; }
        if (P.shaperType != "off" && P.shaperAmt > 0.005) { shaper.curve = shapeCurve(P.shaperType, P.shaperAmt); hasShaper = true; }
        // envelopes (per-note release scales every release time)
        const double rk = o.rel ? std::pow(4.0, o.rel) : 1.0;
        peak = (1 - P.vel + P.vel * vel) * P.gain * 0.5;
        amp.start(P.aenv.a, P.aenv.d, P.aenv.s, P.aenv.r * rk, peak, 0, dur);
        fenv.start(P.fenv.a, P.fenv.d, P.fenv.s, P.fenv.r * rk, 1, 0, dur); menv.start(P.menv.a, P.menv.d, P.menv.s, P.menv.r * rk, 1, 0, dur); menv2.start(P.menv2.a, P.menv2.d, P.menv2.s, P.menv2.r * rk, 1, 0, dur);
        needFenv = usesS[S_FENV] || f1.env || f2.env;
        fltMoves = f1.env || f2.env || usesD[D_CUT] || usesD[D_CUT2] || usesD[D_RES];
        for (int i = 0; i < 3; i++) {
            Lfo& L = lfo[i]; const LfoP& lp = P.lfo[i]; L.used = usesS[S_L1 + i];
            L.wave = lp.wave == "triangle" ? W_TRI : lp.wave == "sawtooth" ? W_SAW : lp.wave == "sawdown" ? W_SAWDOWN : lp.wave == "square" ? W_SQUARE : lp.wave == "sh" ? W_SH : lp.wave == "rand" ? W_RAND : W_SINE; L.rate = lp.sync ? o.bpm / 60.0 / lp.sync : lp.rate; L.fade = lp.fade;
            for (double& v : L.rnd) v = r.bi();
            if (L.wave == W_SH || L.wave == W_RAND) L.ph = r.next() * 64;
        }
        randV = r.bi();
        bend.v = o.bend;
        end = dur >= 0 ? t0 + amp.endTime() : 1e30;
    }
    bool stereo() const override { return true; }   // the voice's own panner always outputs two channels

    void release(double t) override { if (released) return; released = true; const double tl = t - t0; amp.release(tl); fenv.release(tl); menv.release(tl); menv2.release(tl); end = std::min(end, t0 + amp.endTime()); }
    void kill(double t) override { released = true; const double tl = std::max(0.0, t - t0); if (killAt < 0 || tl < killAt) { killFrom = ampAt(tl); killAt = tl; } end = std::min(end, t0 + killAt + 0.06); }
    void slide(int to, double t, double d) override { bend.glide(o.bend + (to - note) * 100.0, t, d); }
    void setBend(double cents) override { bend.v = cents; bend.ts = -1; }
    void setWheel(double w) override { o.wheel = w; }

    void render(float* L, float* R, float*, int n, double t, double sr) override {
        const double dt = 1.0 / sr;
        for (int i = 0; i < n; i++, t += dt) {
            const double tl = t - t0;
            if (tl < 0) continue;
            if (t >= end) break;
            // modulation sources
            double src[S_COUNT] = {};
            for (int k = 0; k < 3; k++) if (lfo[k].used) src[S_L1 + k] = lfoValue(k, tl);
            if (usesS[S_MENV]) src[S_MENV] = menv.at(tl);
            if (usesS[S_MENV2]) src[S_MENV2] = menv2.at(tl);
            const double fe = needFenv ? fenv.at(tl) : 0; src[S_FENV] = fe;
            src[S_VEL] = vel; src[S_KEY] = (note - 60) / 48.0; src[S_WHEEL] = o.wheel; src[S_RAND] = randV;
            for (int k = 0; k < 4; k++) src[S_M1 + k] = P.macro[k];
            double md[D_COUNT] = {};
            for (auto& m : mods) md[m.d] += src[m.s] * m.amt;
            for (int k = 0; k < 3; k++) if (lfo[k].used) advanceLfo(k, md[D_L1 + k] * 20, sr);
            // pitch: cents shared by every oscillator, glide as an exponential slide of the frequency
            const double pf = std::exp2((bend.at(t) + md[D_PITCH] * 2400) / 1200.0);
            const double gl = glide ? std::pow(glide, 1 - std::min(1.0, tl / P.glide)) : 1.0;
            const double base = f * gl;
            double fmHz = 0;
            if (hasFm) { const double fr = base * fmOsc.ratio * fmOsc.detF * pf; fmHz = fmOsc.w->read(fmOsc.ph, fr, sr) * (P.fm * f * 6 + md[D_FM] * f * 6); fmOsc.ph = wrap(fmOsc.ph + fr / sr); }
            double mM = 0, mL = 0, mR = 0;
            for (int k = 0; k < 3; k++) {
                Layer& Ly = layers[k]; if (!Ly.on) continue;
                const double lv = Ly.level + md[D_OSCA + k];
                double aM = 0, aL = 0, aR = 0, bM = 0, bL = 0, bR = 0;
                if (Ly.wt) runTable(Ly, clampd(Ly.mo + md[D_MORPH], 0, 1), base, pf, k == 0 ? fmHz : 0, sr, aM, aL, aR);
                else runBank(Ly.a, base, pf, k == 0 ? fmHz : 0, sr, aM, aL, aR);
                double ga = 1, gb = 0;
                if (Ly.morph) { runBank(Ly.b, base, pf, k == 0 ? fmHz : 0, sr, bM, bL, bR); ga = 1 - Ly.mo - md[D_MORPH]; gb = Ly.mo + md[D_MORPH]; }
                double sM = (aM * ga + bM * gb) * lv, sL = (aL * ga + bL * gb) * lv, sR = (aR * ga + bR * gb) * lv;
                if (Ly.pan != 0) {
                    float l, r;
                    if (Ly.a.stereo) { l = (float)sL; r = (float)sR; panStereo(l, r, Ly.pan); } else panMono((float)sM, Ly.pan, l, r);
                    mL += l; mR += r;
                } else if (Ly.a.stereo) { mL += sL; mR += sR; } else mM += sM;
            }
            if (hasRing) { const double fa = base * ringA.ratio * ringA.detF * pf, fb = base * ringB.ratio * ringB.detF * pf;
                mM += ringA.w->read(ringA.ph, fa, sr) * ringB.w->read(ringB.ph, fb, sr) * (P.ring + md[D_RING]); ringA.ph = wrap(ringA.ph + fa / sr); ringB.ph = wrap(ringB.ph + fb / sr); }
            if (hasSub) { const double fs = base * subOsc.ratio * pf; mM += subOsc.w->read(subOsc.ph, fs, sr) * (P.sub + md[D_SUB]); subOsc.ph = wrap(subOsc.ph + fs / sr); }
            if (hasNoise) { double nz = globalRng().bi(); if (noiseLp) nz = nlp.tick(nz); mM += nz * (P.noise + md[D_NOISE]); }
            if (mtype) mM += runModel(base * pf, sr) * P.pm.level;
            float l, r;
            if (stereoMix) { l = (float)(mL + mM); r = (float)(mR + mM); } else { l = r = (float)mM; }
            if (hasDrive) { l = drive.apply(l); r = drive.apply(r); }
            if (!fltReady || (fltMoves && (i & 7) == 0)) { updFilter(f1, fe, md[D_CUT], md[D_RES] * 20, sr); updFilter(f2, fe, md[D_CUT2], 0, sr); fltReady = true; }
            if (f1.on && f2.on && P.froute == "parallel") { float l1 = l, r1 = r, l2 = l, r2 = r; runF(f1, l1, r1); runF(f2, l2, r2); l = (l1 + l2) * 0.7f; r = (r1 + r2) * 0.7f; }
            else { if (f1.on) runF(f1, l, r); if (f2.on) runF(f2, l, r); }
            if (hasShaper) { l = shaper.apply(l); r = shaper.apply(r); }
            const double g = ampAt(tl) * (1 + md[D_AMP]);
            l *= (float)g; r *= (float)g;
            const double pan = clampd(md[D_PAN], -1, 1);
            if (stereoMix) panStereo(l, r, pan); else { float m = l; panMono(m, pan, l, r); }
            if (o.pan != 0) panStereo(l, r, o.pan);
            L[i] += l; R[i] += r;
        }
    }

private:
    struct Flt { bool on = false, steep = false; Biquad b[2][2]; double cut = 1000, q = 1, det = 0, env = 0; };
    struct M { int s, d; double amt; };
    SynthP P; double vel, f = 440, peak = 0.3, glide = 0, randV = 0; NoteOpts o; int uni = 1;
    std::vector<M> mods; bool usesD[D_COUNT] = {}, usesS[S_COUNT] = {};
    Layer layers[3]; bool stereoMix = false;
    bool fltMoves = false, hasFm = false, hasRing = false, hasSub = false, hasNoise = false, noiseLp = false, hasDrive = false, hasShaper = false, needFenv = false, fltReady = false;
    Osc fmOsc{}, ringA{}, ringB{}, subOsc{}; Biquad nlp; Shaper drive, shaper; Flt f1, f2;
    Adsr amp, fenv, menv, menv2; Lfo lfo[3]; Glider bend;
    double killAt = -1, killFrom = 0;
    // physical model (ModelP): 1 = plucked string, 2 = struck modes
    int mtype = 0; std::vector<float> sbuf, burst; size_t sw = 0, bk = 0; double sLp = 0, sC = 0.5, sDec = 0.99, mDecay = 2;
    int nm = 0; double mw[8] = {}, ma1[8] = {}, ma2[8] = {}, mg[8] = {}, my1[8] = {}, my2[8] = {};
    void initModel(double sr) {
        Rng& r = globalRng(); const ModelP& M = P.pm; mDecay = std::max(0.05, M.decay);
        if (M.type == "pluck") {
            mtype = 1; const double P0 = sr / std::max(20.0, f);
            sbuf.assign((size_t)(sr / 20) + 8, 0.f); sw = 0;
            sC = 0.25 + 0.7 * clampd(M.bright, 0, 1);                       // loop low-pass: brighter = less damping per pass
            Biquad lp; lp.type = FilterType::Lowpass; lp.set(sr, 400 + M.bright * M.bright * 14000 + M.hard * 3000, 0.7, 0);
            const size_t L = (size_t)std::max(2.0, std::round(P0)); burst.assign(L, 0.f);
            for (auto& v : burst) v = (float)lp.tick(r.bi());
            const size_t d = (size_t)std::round(clampd(M.pos, 0.02, 0.5) * L);   // pluck position: a comb on the burst
            for (size_t i = L; i-- > d;) burst[i] -= burst[i - d];
            double pk = 0; for (float v : burst) pk = std::max(pk, (double)std::abs(v)); if (pk > 0) for (auto& v : burst) v = (float)(v / pk * 0.8);
            bk = 0;
        } else {
            mtype = 2;
            static const double bar[] = { 1, 2.756, 5.404, 8.933 }, glass[] = { 1, 2.32, 4.25, 6.63, 9.38 }, wood[] = { 1, 3.99, 8.9 }, bell[] = { 1, 2, 2.4, 3, 4.1, 5.4 };
            static const double barA[] = { 1, 0.5, 0.3, 0.2 }, glassA[] = { 1, 0.6, 0.4, 0.3, 0.2 }, woodA[] = { 1, 0.3, 0.1 }, bellA[] = { 1, 0.8, 0.6, 0.5, 0.3, 0.2 };
            const double* rat = bar; const double* amp = barA; int n = 4; double dk = 1;
            if (M.mat == "glass") { rat = glass; amp = glassA; n = 5; } else if (M.mat == "wood") { rat = wood; amp = woodA; n = 3; dk = 0.2; } else if (M.mat == "bell") { rat = bell; amp = bellA; n = 6; dk = 1.6; }
            nm = 0;
            for (int k = 0; k < n; k++) {
                const double fr = f * rat[k]; if (fr >= sr * 0.45) break;
                const double T = mDecay * dk / (1 + k * 0.8 * (1 - clampd(M.bright, 0, 1)));
                const double rr = std::exp(-6.9078 / (std::max(0.01, T) * sr)), w = 2 * PI * fr / sr;
                mw[nm] = w; ma1[nm] = 2 * rr * std::cos(w); ma2[nm] = rr * rr;
                mg[nm] = amp[k] * std::pow(clampd(M.hard, 0.05, 1), k * 0.35) * std::sin(w); nm++;
            }
            const size_t L = (size_t)std::max(1.0, (1 - clampd(M.hard, 0, 1)) * 0.004 * sr) + 1; burst.assign(L, 0.f);   // a raised-cosine strike, shorter when harder
            for (size_t i = 0; i < L; i++) burst[i] = (float)((0.5 - 0.5 * std::cos(2 * PI * (i + 0.5) / L)) * 2.0 / L);
            bk = 0;
        }
    }
    double runModel(double freq, double sr) {
        const double x = bk < burst.size() ? burst[bk++] : 0.0;
        if (mtype == 1) {
            const double Pd = sr / std::max(20.0, freq), lpd = (1 - sC) / sC;    // the loop filter delays by (1-c)/c samples
            const double d = clampd(Pd - lpd, 2, (double)sbuf.size() - 3);
            double p = (double)sw - d; while (p < 0) p += (double)sbuf.size();
            const size_t i0 = (size_t)p % sbuf.size(), i1 = (i0 + 1) % sbuf.size(); const double fr = p - std::floor(p);
            const double v = sbuf[i0] * (1 - fr) + sbuf[i1] * fr;
            sLp += sC * (v - sLp);
            const double g = std::pow(10.0, -3.0 / (mDecay * std::max(20.0, freq)));   // 60 dB down after `decay` seconds
            const double y = sLp * g + x;
            sbuf[sw] = (float)y; sw = (sw + 1) % sbuf.size();
            return y;
        }
        double out = 0;
        for (int k = 0; k < nm; k++) { const double y = ma1[k] * my1[k] - ma2[k] * my2[k] + mg[k] * x; my2[k] = my1[k]; my1[k] = y; out += y; }   // a unit-area strike rings each mode at about its amp
        return out * 0.35;
    }

    static double wrap(double p) { return p - std::floor(p); }
    double ampAt(double tl) const { if (killAt >= 0 && tl >= killAt) return killFrom * std::exp(-(tl - killAt) / 0.006); return amp.at(tl); }
    void runTable(Layer& Ly, double pos, double base, double pf, double fmHz, double sr, double& M, double& Lo, double& Ro) {   // two neighbouring frames, crossfaded
        const double x = pos * (WT_FRAMES - 1); const int k0 = std::min(WT_FRAMES - 2, (int)x); const double fx = x - k0;
        Bank& b = Ly.a;
        for (auto& o : b.osc) {
            const double fr = (base * o.ratio + fmHz) * o.detF * pf, af = std::abs(fr);
            const double v = Ly.fr[k0]->read(o.ph, af, sr) * (1 - fx) + Ly.fr[k0 + 1]->read(o.ph, af, sr) * fx; o.ph = wrap(o.ph + fr / sr);
            if (!b.stereo) M += v;
            else { float l, r; panMono((float)v, o.side * P.spread, l, r); Lo += l; Ro += r; }
        }
    }
    void runBank(Bank& b, double base, double pf, double fmHz, double sr, double& M, double& Lo, double& Ro) {
        for (auto& x : b.osc) {
            const double fr = (base * x.ratio + fmHz) * x.detF * pf;
            const double v = x.w->read(x.ph, std::abs(fr), sr); x.ph = wrap(x.ph + fr / sr);
            if (!b.stereo) M += v;
            else { float l, r; panMono((float)v, x.side * P.spread, l, r); Lo += l; Ro += r; }
        }
    }
    void updFilter(Flt& F, double fe, double modCut, double modRes, double sr) {
        if (!F.on) return;
        const double fr = F.cut * std::exp2((F.det + fe * F.env + modCut * 4800) / 1200.0), q = F.q + modRes;
        for (auto& ch : F.b) for (int k = 0; k < (F.steep ? 2 : 1); k++) ch[k].set(sr, fr, q, 0);
    }
    static void runF(Flt& F, float& l, float& r) {
        const int st = F.steep ? 2 : 1;
        double a = l, b = r;
        for (int k = 0; k < st; k++) { a = F.b[0][k].tick(a); b = F.b[1][k].tick(b); }
        l = (float)a; r = (float)b;
    }
    double lfoValue(int k, double tl) const {
        const Lfo& L = lfo[k]; const double p = L.ph - std::floor(L.ph); double v;
        switch (L.wave) {
        case W_TRI: v = p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4; break;
        case W_SAW: v = p < 0.5 ? 2 * p : 2 * p - 2; break;
        case W_SAWDOWN: v = -(p < 0.5 ? 2 * p : 2 * p - 2); break;
        case W_SQUARE: v = p < 0.5 ? 1 : -1; break;
        case W_SH: v = L.rnd[(int)std::floor(L.ph) & 63]; break;
        case W_RAND: { const int a = (int)std::floor(L.ph) & 63; v = L.rnd[a] + (L.rnd[(a + 1) & 63] - L.rnd[a]) * p; break; }
        default: v = std::sin(2 * PI * p);
        }
        if (L.fade > 0.005 && tl < L.fade) v *= tl / L.fade;
        return v;
    }
    void advanceLfo(int k, double extraHz, double sr) { Lfo& L = lfo[k]; L.ph += (L.rate + extraHz) / sr; if (L.ph > 1e6) L.ph -= 1e6 - std::fmod(1e6, 64.0); }
};

class SampleVoice : public Voice {
public:
    SampleVoice(SampleRef s, const Channel& C, int n, double vel, double t, double dur, const NoteOpts& o, double sr) : smp(C.rev ? s->reversed() : s) {
        note = n; t0 = t;
        rate = std::pow(2.0, (n - (o.root >= 0 ? o.root : C.root) + C.tune) / 12.0); base = C.fine; bend.v = base + o.bend; bendBase = base + o.bend;
        off = clampd(C.start, 0, 0.98) * smp->seconds(); pos = off * smp->sr;
        A = std::max(C.att, 0.001); R = std::max(C.rel, 0.005) * std::pow(4.0, o.rel); g = std::pow(vel, 1.5) * o.zoneGain;
        end = t0 + (smp->seconds() - off) / (rate * std::exp2(bend.v / 1200));
        if (o.cut < 0 || o.res != 0) { lp = true; for (auto& b : lpf) { b.type = FilterType::Lowpass; b.set(sr, 20000 * std::pow(2.0, std::min(0.0, o.cut) * 7), 0.707 * std::pow(4.0, o.res), 0); } }
        if (C.gate && dur >= 0) fade(t0 + dur, R / 4);
        gate = C.gate; notePan = o.pan;
    }
    bool stereo() const override { return smp->ch.size() > 1 || notePan != 0; }   // a per-note pan makes a stereo panner
    void release(double t) override { if (gate) fade(t, R / 4); released = true; }
    void kill(double t) override { fade(std::max(t, t0), 0.006); released = true; }
    void slide(int to, double t, double d) override { bend.glide(bendBase + (to - note) * 100.0, t, d); }
    void setBend(double cents) override { bend.v = base + cents; bend.ts = -1; }
    void render(float* L, float* Rr, float* M, int n, double t, double sr) override {
        const double dt = 1.0 / sr; const size_t len = smp->length(); const bool st = stereo();
        const bool two = smp->ch.size() > 1; const float* c0 = smp->ch[0].data(); const float* c1 = two ? smp->ch[1].data() : nullptr;
        for (int i = 0; i < n; i++, t += dt) {
            const double tl = t - t0;
            if (tl < 0) continue;
            if (t >= end) break;
            double gain = tl < A ? g * tl / A : g;
            if (fadeAt >= 0 && t >= fadeAt) gain = fadeFrom * std::exp(-(t - fadeAt) / fadeTau);
            const size_t k = (size_t)pos; if (k + 1 >= len) { end = t; break; }
            const double fr = pos - k;
            double a = c0[k] + (c0[k + 1] - c0[k]) * fr, b = two ? c1[k] + (c1[k + 1] - c1[k]) * fr : a;
            if (lp) { a = lpf[0].tick(a); if (two) b = lpf[1].tick(b); }
            if (!st) M[i] += (float)(a * gain);
            else { float l, r; if (two) { l = (float)(a * gain); r = (float)(b * gain); if (notePan != 0) panStereo(l, r, notePan); } else panMono((float)(a * gain), notePan, l, r); L[i] += l; Rr[i] += r; }
            pos += rate * std::exp2(bend.at(t) / 1200) * smp->sr / sr;
        }
    }
private:
    SampleRef smp; double rate = 1, base = 0, bendBase = 0, off = 0, pos = 0, A = 0.001, R = 0.08, g = 1; bool gate = false, lp = false; Biquad lpf[2]; Glider bend;
    double fadeAt = -1, fadeTau = 0.01, fadeFrom = 0, notePan = 0;
    double gainAt(double t) const { const double tl = t - t0; double gain = tl < A ? g * std::max(0.0, tl) / A : g; if (fadeAt >= 0 && t >= fadeAt) gain = fadeFrom * std::exp(-(t - fadeAt) / fadeTau); return gain; }
    void fade(double tr, double tau) {
        if (tr >= end) return;
        fadeFrom = gainAt(tr); fadeAt = tr; fadeTau = tau; end = tr + tau * 6;
    }
};

}   // namespace

namespace {
// Granular voice: Hann-windowed grains read from the sample (see GranP), scheduled at `density` per second with 30 %
// random spacing, for as long as the note is held, then faded over the channel's release.
class GrainVoice : public Voice {
public:
    GrainVoice(SampleRef s, const Channel& C, int n, double vel, double t, double dur, const NoteOpts& o, double sr_) : smp(s), G(C.gran), sr(sr_) {
        note = n; t0 = t; rate = std::pow(2.0, (n - (o.root >= 0 ? o.root : C.root) + C.tune + C.fine / 100) / 12.0) * smp->sr / sr; bendC = o.bend;
        A = std::max(C.att, 0.003); R = std::max(C.rel, 0.02) * std::pow(4.0, o.rel); g = std::pow(vel, 1.5) * o.zoneGain / std::sqrt(std::max(1.0, G.density * G.size * 0.6));
        relAt = dur >= 0 ? t0 + dur : -1; end = relAt >= 0 ? relAt + R * 5 : 1e30; notePan = o.pan;
    }
    bool stereo() const override { return true; }
    void release(double t) override { if (relAt < 0 || t < relAt) { relAt = t; end = t + R * 5; } released = true; }
    void kill(double t) override { relAt = std::max(t, t0); R = 0.005; end = relAt + 0.03; released = true; }
    void slide(int to, double, double) override { (void)to; }
    void setBend(double cents) override { bendC = cents; }
    void render(float* L, float* Rr, float*, int n, double t, double) override {
        const double len = (double)smp->length(), dt = 1.0 / sr; const bool two = smp->ch.size() > 1; const float* c0 = smp->ch[0].data(); const float* c1 = two ? smp->ch[1].data() : c0;
        Rng& r = globalRng(); const double bendF = std::exp2(bendC / 1200);
        for (int i = 0; i < n; i++, t += dt) {
            const double tl = t - t0; if (tl < 0) continue; if (t >= end) break;
            if ((relAt < 0 || t < relAt) && tl >= nextGrain && active < MAXG) {   // new grains only while held
                Grain& q = grains[active++];
                double p = G.pos + G.scan * tl + r.bi() * G.jitter; p -= std::floor(p);
                q.len = std::max(16.0, G.size * sr); q.age = 0;
                q.inc = rate * bendF * std::exp2(r.bi() * G.spray / 12) * (r.next() < G.rev ? -1 : 1);
                q.pos = p * (len - 2); const double pan = clampd(r.bi() * G.spread + notePan, -1, 1); q.gl = std::cos((pan + 1) * PI / 4); q.gr = std::sin((pan + 1) * PI / 4);
                nextGrain = tl + (1 / std::max(0.5, G.density)) * (0.7 + 0.6 * r.next());
            }
            double env = tl < A ? tl / A : 1; if (relAt >= 0 && t >= relAt) env *= std::exp(-(t - relAt) / (R / 4));
            double ol = 0, orr = 0;
            for (int k = 0; k < active;) {
                Grain& q = grains[k];
                const double w = 0.5 - 0.5 * std::cos(2 * PI * q.age / q.len);
                double p = q.pos; while (p < 0) p += len - 2; while (p >= len - 2) p -= len - 2;
                const size_t j = (size_t)p; const double f = p - (double)j;
                const double a = c0[j] + (c0[j + 1] - c0[j]) * f, b = c1[j] + (c1[j + 1] - c1[j]) * f;
                ol += a * w * q.gl; orr += b * w * q.gr;
                q.pos += q.inc; q.age += 1;
                if (q.age >= q.len) { grains[k] = grains[--active]; continue; }
                k++;
            }
            const double gg = g * env * 1.4;
            L[i] += (float)(ol * gg); Rr[i] += (float)(orr * gg);
        }
    }
private:
    struct Grain { double pos = 0, inc = 1, age = 0, len = 1, gl = 0.7, gr = 0.7; };
    static constexpr int MAXG = 96;
    SampleRef smp; GranP G; double sr, rate = 1, bendC = 0, A = 0.003, R = 0.1, g = 1, relAt = -1, nextGrain = 0, notePan = 0; Grain grains[MAXG]; int active = 0;
};
}   // namespace

std::unique_ptr<Voice> makeSynthVoice(const SynthP& P, int note, double vel, double t, double dur, const NoteOpts& o, double sr) { return std::make_unique<SynthVoice>(P, note, vel, t, dur, o, sr); }
std::unique_ptr<Voice> makeSampleVoice(SampleRef smp, const Channel& C, int note, double vel, double t, double dur, const NoteOpts& o, double sr) {
    if (!smp || smp->length() < 2) return nullptr;
    if (C.gran.on && smp->length() > 64) return std::make_unique<GrainVoice>(smp, C, note, vel, t, dur, o, sr);
    return std::make_unique<SampleVoice>(smp, C, note, vel, t, dur, o, sr);
}

}   // namespace us
