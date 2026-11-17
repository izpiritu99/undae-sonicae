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
