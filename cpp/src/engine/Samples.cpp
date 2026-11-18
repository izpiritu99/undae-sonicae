#include "Samples.h"
#include "Dsp.h"
#include <functional>
#include <juce_audio_formats/juce_audio_formats.h>

namespace us {

std::shared_ptr<const Sample> Sample::reversed() const {
    if (!rev) { auto r = std::make_shared<Sample>(*this); for (auto& c : r->ch) std::reverse(c.begin(), c.end()); rev = r; }
    return rev;
}

// --- the kit, rendered like drums.js: one mono buffer at 44.1 kHz, peak-normalised to 0.9, 400-sample fade at the end ---
namespace {
constexpr double SR = 44100;

struct Ctx {
    std::vector<float> out;
    explicit Ctx(double dur) : out((size_t)std::ceil(SR * dur), 0.f) {}
    size_t n() const { return out.size(); }
};
Timeline env(double peak, double dec, double att = 0.001) { Timeline t(1); t.set(0.0001, 0).lin(peak, att).exp(0.0001, att + dec); return t; }

// an oscillator whose frequency follows a timeline (plus an optional per-sample modulation added in Hz)
std::vector<float> osc(size_t n, const std::string& type, Timeline freq, const std::vector<float>* fmod = nullptr) {
    std::vector<float> o(n); const WaveTable& w = WaveTable::get(type); double ph = 0;
    for (size_t i = 0; i < n; i++) {
        const double f = freq.at(i / SR) + (fmod ? (*fmod)[i] : 0.0);
        o[i] = w.read(ph, std::abs(f), SR); ph += f / SR; ph -= std::floor(ph);
    }
    return o;
}
std::vector<float> osc(size_t n, const std::string& type, double f) { Timeline t(f); return osc(n, type, t); }
std::vector<float> noise(size_t n, double dur, Rng& r) { std::vector<float> o(n, 0.f); size_t m = std::min(n, (size_t)std::ceil(SR * dur)); for (size_t i = 0; i < m; i++) o[i] = (float)r.bi(); return o; }
void filt(std::vector<float>& x, const char* type, double f, double q = 0.7) { Biquad b; b.type = filterTypeFrom(type); b.set(SR, f, q, 0); for (auto& v : x) v = (float)b.tick(v); }
void gain(std::vector<float>& x, Timeline g) { for (size_t i = 0; i < x.size(); i++) x[i] *= (float)g.at(i / SR); }
void addTo(Ctx& c, const std::vector<float>& x) { for (size_t i = 0; i < c.n() && i < x.size(); i++) c.out[i] += x[i]; }

std::vector<float> finish(Ctx& c) {
    float pk = 0; for (float v : c.out) pk = std::max(pk, std::abs(v));
    if (pk > 0) { const float k = 0.9f / pk; for (auto& v : c.out) v *= k; }
    const size_t f = std::min<size_t>(c.n(), 400);
    for (size_t i = 0; i < f; i++) c.out[c.n() - 1 - i] *= (float)i / f;
    return c.out;
}

using Gen = std::function<std::vector<float>(Rng&)>;
Gen sweep(double f0, double f1, double t, double dec, double dur) {
    return [=](Rng& r) {
        Ctx c(dur); Timeline fr(f0); fr.set(f0, 0).exp(f1, t);
        auto o = osc(c.n(), "sine", fr); gain(o, env(1, dec)); addTo(c, o);
        auto nz = noise(c.n(), 0.02, r); filt(nz, "lowpass", 3000); gain(nz, env(0.25, 0.008)); addTo(c, nz);
        return finish(c);
    };
}
const double METAL[] = { 205.3, 304.4, 369.6, 522.7, 540, 800 };
Gen metal(double dec, double bp, double hp, double dur) {
    return [=](Rng&) {
        Ctx c(dur); std::vector<float> sum(c.n(), 0.f);
        for (double f : METAL) { auto o = osc(c.n(), "square", f); for (size_t i = 0; i < c.n(); i++) sum[i] += o[i] * 0.2f; }
        filt(sum, "bandpass", bp, 0.8); filt(sum, "highpass", hp); gain(sum, env(0.8, dec)); addTo(c, sum);
        return finish(c);
    };
}
struct KitDef { const char* id; const char* name; Gen gen; const char* group; };
std::vector<KitDef> kitDefs() {
    std::vector<KitDef> k = {
        { "808:kick", "Kick", sweep(165, 46, 0.075, 0.55, 0.7), "808 Kit" },
        { "808:kicklong", "Kick Long", sweep(150, 41, 0.09, 1.3, 1.5), "808 Kit" },
        { "808:snare", "Snare", [](Rng& r) { Ctx c(0.4);
            auto a = osc(c.n(), "sine", 185); gain(a, env(0.55, 0.12)); addTo(c, a);
            auto b = osc(c.n(), "sine", 331); gain(b, env(0.3, 0.07)); addTo(c, b);
            auto nz = noise(c.n(), 0.4, r); filt(nz, "highpass", 1600); gain(nz, env(0.7, 0.19)); addTo(c, nz); return finish(c); }, "808 Kit" },
        { "808:clap", "Clap", [](Rng& r) { Ctx c(0.5); Timeline g(1); g.set(0, 0);
            for (double t : { 0.0, 0.011, 0.022 }) { g.set(1, t).exp(0.15, t + 0.009); }
            g.set(0.9, 0.033).exp(0.0001, 0.36);
            auto nz = noise(c.n(), 0.5, r); filt(nz, "bandpass", 1150, 1.3); gain(nz, g); addTo(c, nz); return finish(c); }, "808 Kit" },
        { "808:hat", "Closed Hat", metal(0.05, 10000, 7000, 0.15), "808 Kit" },
        { "808:ohat", "Open Hat", metal(0.42, 10000, 7000, 0.6), "808 Kit" },
        { "808:cymbal", "Cymbal", metal(1.5, 6500, 3800, 1.8), "808 Kit" },
        { "808:cowbell", "Cowbell", [](Rng&) { Ctx c(0.5); Timeline g(1); g.set(0.7, 0).exp(0.25, 0.03).exp(0.0001, 0.42);
            auto a = osc(c.n(), "square", 540), b = osc(c.n(), "square", 800); for (size_t i = 0; i < c.n(); i++) a[i] += b[i];
            filt(a, "bandpass", 2640, 1); gain(a, g); addTo(c, a); return finish(c); }, "808 Kit" },
        { "808:rim", "Rimshot", [](Rng&) { Ctx c(0.1);
            auto a = osc(c.n(), "sine", 1667); gain(a, env(0.6, 0.012)); auto b = osc(c.n(), "triangle", 455); gain(b, env(0.5, 0.018));
            for (size_t i = 0; i < c.n(); i++) a[i] += b[i]; filt(a, "highpass", 500); addTo(c, a); return finish(c); }, "808 Kit" },
        { "808:clave", "Clave", [](Rng&) { Ctx c(0.12); auto a = osc(c.n(), "sine", 2500); gain(a, env(0.7, 0.035)); addTo(c, a); return finish(c); }, "808 Kit" },
        { "808:maracas", "Maracas", [](Rng& r) { Ctx c(0.12); auto nz = noise(c.n(), 0.12, r); filt(nz, "highpass", 6000); gain(nz, env(0.6, 0.045, 0.018)); addTo(c, nz); return finish(c); }, "808 Kit" },
        { "808:tomlo", "Low Tom", sweep(140, 88, 0.1, 0.45, 0.6), "808 Kit" },
        { "808:tommid", "Mid Tom", sweep(200, 128, 0.09, 0.36, 0.5), "808 Kit" },
        { "808:tomhi", "High Tom", sweep(280, 185, 0.08, 0.3, 0.45), "808 Kit" },
        { "808:congalo", "Low Conga", sweep(230, 200, 0.03, 0.2, 0.3), "808 Kit" },
        { "808:congahi", "High Conga", sweep(420, 380, 0.03, 0.16, 0.25), "808 Kit" },
        // Electric-piano style tine at C4 (root 60): FM pair with a fast-decaying modulator.
        { "inst:keys", "Keys (C4)", [](Rng&) { Ctx c(2.6); const double f = 261.63;
            Timeline mg(1); mg.set(f * 1.4, 0).exp(1, 0.35);
            auto mod = osc(c.n(), "sine", f * 14); for (size_t i = 0; i < c.n(); i++) mod[i] *= (float)mg.at(i / SR);
            Timeline base(f); auto car = osc(c.n(), "sine", base, &mod); gain(car, env(0.8, 2.4, 0.003)); addTo(c, car);
            auto h2 = osc(c.n(), "sine", f * 2); gain(h2, env(0.18, 1.1, 0.003)); addTo(c, h2); return finish(c); }, "Instruments" },
        // Long saturated sub at C2 (root 36).
        { "inst:sub", "Sub 808 (C2)", [](Rng&) { Ctx c(1.8); Timeline fr(65.41); fr.set(130, 0).exp(65.41, 0.04);
            auto o = osc(c.n(), "sine", fr); gain(o, env(1, 1.6, 0.002));
            Shaper ws; ws.curve.resize(512); for (int i = 0; i < 512; i++) ws.curve[i] = (float)std::tanh((i / 255.5 - 1) * 2.2);
            for (auto& v : o) v = ws.apply(v); addTo(c, o); return finish(c); }, "Instruments" },
        // Studio Kit: an acoustic-flavoured kit, synthesised (appended after the others, so their random draws stay the same)
        { "kit2:kick", "Studio Kick", [](Rng& r) { Ctx c(0.6); Timeline fr(110); fr.set(110, 0).exp(52, 0.05);
            auto o = osc(c.n(), "sine", fr); gain(o, env(1, 0.42)); addTo(c, o);
            auto b = osc(c.n(), "triangle", 1600); gain(b, env(0.35, 0.006)); addTo(c, b);
            auto nz = noise(c.n(), 0.01, r); filt(nz, "highpass", 2000); gain(nz, env(0.3, 0.004)); addTo(c, nz); return finish(c); }, "Studio Kit" },
        { "kit2:snare", "Studio Snare", [](Rng& r) { Ctx c(0.45);
            Timeline f1(220); f1.set(220, 0).exp(185, 0.02); auto m1 = osc(c.n(), "sine", f1); gain(m1, env(0.7, 0.11)); addTo(c, m1);
            Timeline f2(360); f2.set(360, 0).exp(330, 0.02); auto m2 = osc(c.n(), "sine", f2); gain(m2, env(0.35, 0.07)); addTo(c, m2);
            auto nz = noise(c.n(), 0.4, r); filt(nz, "highpass", 900); filt(nz, "bandpass", 4500, 0.6); gain(nz, env(0.9, 0.2)); addTo(c, nz); return finish(c); }, "Studio Kit" },
        { "kit2:stick", "Side Stick", [](Rng& r) { Ctx c(0.12); Timeline f(1900); f.set(1900, 0).exp(1500, 0.02);
            auto o = osc(c.n(), "sine", f); gain(o, env(0.8, 0.03)); addTo(c, o);
            auto nz = noise(c.n(), 0.02, r); filt(nz, "bandpass", 2500, 1.5); gain(nz, env(0.6, 0.012)); addTo(c, nz); return finish(c); }, "Studio Kit" },
        { "kit2:clap", "Hand Clap", [](Rng& r) { Ctx c(0.5); Timeline g(1); g.set(0, 0);
            for (int k = 0; k < 4; k++) { const double t0 = k * 0.011; g.set(0, t0).lin(k == 3 ? 0.9 : 0.7, t0 + 0.001).exp(0.05, t0 + 0.009); }
            g.exp(0.0001, 0.42); auto nz = noise(c.n(), 0.5, r); filt(nz, "bandpass", 1300, 1.1); filt(nz, "highpass", 600); gain(nz, g); addTo(c, nz); return finish(c); }, "Studio Kit" },
        { "kit2:hat", "Studio Closed Hat", [](Rng& r) { Ctx c(0.12); std::vector<float> m(c.n(), 0.f);
            for (double f : METAL) { auto o = osc(c.n(), "square", f * 1.47); for (size_t i = 0; i < c.n(); i++) m[i] += o[i] * 0.12f; }
            auto nz = noise(c.n(), 0.12, r); for (size_t i = 0; i < c.n(); i++) m[i] += nz[i] * 0.5f; filt(m, "highpass", 7000); gain(m, env(0.8, 0.055)); addTo(c, m); return finish(c); }, "Studio Kit" },
        { "kit2:pedal", "Pedal Hat", [](Rng& r) { Ctx c(0.1); std::vector<float> m(c.n(), 0.f);
            for (double f : METAL) { auto o = osc(c.n(), "square", f * 1.47); for (size_t i = 0; i < c.n(); i++) m[i] += o[i] * 0.12f; }
            auto nz = noise(c.n(), 0.1, r); for (size_t i = 0; i < c.n(); i++) m[i] += nz[i] * 0.3f; filt(m, "bandpass", 6000, 1.2); gain(m, env(0.6, 0.032, 0.004)); addTo(c, m); return finish(c); }, "Studio Kit" },
        { "kit2:ohat", "Studio Open Hat", [](Rng& r) { Ctx c(0.8); std::vector<float> m(c.n(), 0.f);
            for (double f : METAL) { auto o = osc(c.n(), "square", f * 1.47); for (size_t i = 0; i < c.n(); i++) m[i] += o[i] * 0.12f; }
            auto nz = noise(c.n(), 0.8, r); for (size_t i = 0; i < c.n(); i++) m[i] += nz[i] * 0.5f; filt(m, "highpass", 6500); gain(m, env(0.8, 0.55)); addTo(c, m); return finish(c); }, "Studio Kit" },
        { "kit2:ride", "Ride", [](Rng& r) { Ctx c(2.8); std::vector<float> m(c.n(), 0.f);
            static const double P[] = { 341, 527, 853, 1113, 1490, 2347, 3127, 4519 };
            for (double f : P) { auto o = osc(c.n(), "sine", f); for (size_t i = 0; i < c.n(); i++) m[i] += o[i] * (float)(0.25 * std::exp(-(double)i / SR * (0.8 + f / 2500))); }
            auto ping = osc(c.n(), "sine", 2347); gain(ping, env(0.4, 0.35)); for (size_t i = 0; i < c.n(); i++) m[i] += ping[i];
            auto nz = noise(c.n(), 2.8, r); filt(nz, "highpass", 5000); gain(nz, env(0.25, 1.8)); for (size_t i = 0; i < c.n(); i++) m[i] += nz[i];
            filt(m, "highpass", 300); addTo(c, m); return finish(c); }, "Studio Kit" },
        { "kit2:crash", "Crash", [](Rng& r) { Ctx c(2.4); std::vector<float> m(c.n(), 0.f);
            for (double f : METAL) { auto o = osc(c.n(), "square", f * 2.1); for (size_t i = 0; i < c.n(); i++) m[i] += o[i] * 0.1f; }
            auto nz = noise(c.n(), 2.4, r); for (size_t i = 0; i < c.n(); i++) m[i] += nz[i] * 0.8f; filt(m, "highpass", 3000);
            Timeline g(1); g.set(0.0001, 0).lin(0.9, 0.004).exp(0.5, 0.06).exp(0.0001, 2.2); gain(m, g); addTo(c, m); return finish(c); }, "Studio Kit" },
        { "kit2:tomlo", "Floor Tom", [](Rng& r) { Ctx c(0.8); Timeline f(115); f.set(115, 0).exp(92, 0.12); auto o = osc(c.n(), "sine", f); gain(o, env(1, 0.55)); addTo(c, o);
            Timeline f2(170); f2.set(170, 0).exp(140, 0.1); auto o2 = osc(c.n(), "sine", f2); gain(o2, env(0.3, 0.25)); addTo(c, o2);
            auto nz = noise(c.n(), 0.03, r); filt(nz, "lowpass", 2500); gain(nz, env(0.3, 0.015)); addTo(c, nz); return finish(c); }, "Studio Kit" },
        { "kit2:tommid", "Rack Tom", [](Rng& r) { Ctx c(0.6); Timeline f(165); f.set(165, 0).exp(138, 0.1); auto o = osc(c.n(), "sine", f); gain(o, env(1, 0.42)); addTo(c, o);
            Timeline f2(250); f2.set(250, 0).exp(210, 0.08); auto o2 = osc(c.n(), "sine", f2); gain(o2, env(0.3, 0.2)); addTo(c, o2);
            auto nz = noise(c.n(), 0.03, r); filt(nz, "lowpass", 3000); gain(nz, env(0.3, 0.012)); addTo(c, nz); return finish(c); }, "Studio Kit" },
        { "kit2:tomhi", "Studio High Tom", [](Rng& r) { Ctx c(0.5); Timeline f(230); f.set(230, 0).exp(196, 0.08); auto o = osc(c.n(), "sine", f); gain(o, env(1, 0.34)); addTo(c, o);
            Timeline f2(345); f2.set(345, 0).exp(300, 0.07); auto o2 = osc(c.n(), "sine", f2); gain(o2, env(0.3, 0.16)); addTo(c, o2);
            auto nz = noise(c.n(), 0.03, r); filt(nz, "lowpass", 3500); gain(nz, env(0.3, 0.01)); addTo(c, nz); return finish(c); }, "Studio Kit" },
        { "kit2:shaker", "Shaker", [](Rng& r) { Ctx c(0.2); Timeline g(1); g.set(0.0001, 0).lin(0.5, 0.03).exp(0.2, 0.07).lin(0.8, 0.1).exp(0.0001, 0.19);
            auto nz = noise(c.n(), 0.2, r); filt(nz, "bandpass", 7000, 1.4); filt(nz, "highpass", 4000); gain(nz, g); addTo(c, nz); return finish(c); }, "Studio Kit" },
        { "kit2:tamb", "Tambourine", [](Rng& r) { Ctx c(0.5); std::vector<float> m(c.n(), 0.f);
            static const double J[] = { 5650, 6930, 7730, 8540, 9870 };
            for (double f : J) { auto o = osc(c.n(), "sine", f); for (size_t i = 0; i < c.n(); i++) m[i] += o[i] * 0.15f; }
            auto nz = noise(c.n(), 0.5, r); for (size_t i = 0; i < c.n(); i++) m[i] += nz[i] * 0.4f; filt(m, "highpass", 5000);
            Timeline g(1); g.set(0.0001, 0).lin(0.9, 0.002).exp(0.3, 0.03).lin(0.6, 0.045).exp(0.0001, 0.45); gain(m, g); addTo(c, m); return finish(c); }, "Studio Kit" },
    };
    return k;
}
}   // namespace

SampleBank::SampleBank() {
    WaveTable::warmUp();
    Rng r(808);
    for (auto& d : kitDefs()) {
        auto s = std::make_shared<Sample>(); s->id = d.id; s->name = d.name; s->group = d.group; s->sr = SR; s->ch.push_back(d.gen(r));
        put(s); order.push_back(d.id);
    }
}

void SampleBank::put(SampleRef s) { auto m = std::make_shared<Map>(*map.load()); (*m)[s->id] = std::move(s); map.store(std::move(m)); }
SampleRef SampleBank::get(const std::string& id) const { auto m = map.load(); auto it = m->find(id); return it == m->end() ? nullptr : it->second; }
std::vector<SampleRef> SampleBank::kit() const { auto m = map.load(); std::vector<SampleRef> v; for (auto& id : order) v.push_back(m->at(id)); return v; }
std::vector<SampleRef> SampleBank::imported() const { auto m = map.load(); std::vector<SampleRef> v; for (auto& [id, s] : *m) if (s->group == "Project") v.push_back(s); return v; }
SampleRef SampleBank::add(const std::string& id, const std::string& name, const juce::MemoryBlock& bytes) {
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd(fm.createReaderFor(std::make_unique<juce::MemoryInputStream>(bytes, false)));
    if (!rd || rd->lengthInSamples <= 0) return nullptr;
    juce::AudioBuffer<float> buf((int)rd->numChannels, (int)rd->lengthInSamples);
    rd->read(&buf, 0, (int)rd->lengthInSamples, 0, true, true);
    auto s = std::make_shared<Sample>(); s->id = id; s->name = name; s->group = "Project"; s->sr = rd->sampleRate;
    for (int c = 0; c < std::min(2, buf.getNumChannels()); c++) s->ch.emplace_back(buf.getReadPointer(c), buf.getReadPointer(c) + buf.getNumSamples());
    put(s); return s;
}

void SampleBank::addFromProject(const Project& p) {
    for (auto& [id, bytes] : p.embedded->data) add(id, p.embedded->names.count(id) ? p.embedded->names.at(id) : id, bytes);
}

}   // namespace us
