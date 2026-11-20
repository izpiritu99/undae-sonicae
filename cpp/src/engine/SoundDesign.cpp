#include "SoundDesign.h"
#include "PluginHost.h"
#include "Dsp.h"
#include "Effects.h"
#include "Writer.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace us {

const std::vector<std::pair<const char*, DesignP>>& designPresets() {
    static const std::vector<std::pair<const char*, DesignP>> list = [] {
        std::vector<std::pair<const char*, DesignP>> v;
        auto add = [&](const char* n, auto f) { DesignP d; f(d); v.push_back({ n, d }); };
        add("Kick", [](DesignP& d) { d.len = 0.5; d.tone.f0 = 180; d.tone.f1 = 45; d.tone.sweep = 0.07; d.tone.dec = 0.35; d.drive = 0.25; });
        add("808 Sub", [](DesignP& d) { d.len = 1.6; d.tone.f0 = 120; d.tone.f1 = 43; d.tone.sweep = 0.05; d.tone.dec = 1.3; d.drive = 0.4; });
        add("Snare", [](DesignP& d) { d.len = 0.3; d.tone.wave = "triangle"; d.tone.f0 = 260; d.tone.f1 = 170; d.tone.sweep = 0.04; d.tone.dec = 0.12; d.tone.level = 0.6; d.noise.on = true; d.noise.cut = 1800; d.noise.level = 0.8; d.noise.dec = 0.16; });
        add("Closed Hat", [](DesignP& d) { d.len = 0.12; d.tone.on = false; d.noise.on = true; d.noise.cut = 7500; d.noise.level = 0.9; d.noise.dec = 0.04; });
        add("Open Hat", [](DesignP& d) { d.len = 0.6; d.tone.on = false; d.noise.on = true; d.noise.cut = 6500; d.noise.level = 0.9; d.noise.dec = 0.35; });
        add("Tom", [](DesignP& d) { d.len = 0.5; d.tone.f0 = 220; d.tone.f1 = 110; d.tone.sweep = 0.15; d.tone.dec = 0.3; });
        add("Zap", [](DesignP& d) { d.len = 0.35; d.tone.wave = "sawtooth"; d.tone.f0 = 4000; d.tone.f1 = 80; d.tone.sweep = 0.12; d.tone.dec = 0.25; d.filt.on = true; d.filt.cut = 9000; d.filt.cut2 = 600; d.filt.q = 4; });
        add("Riser", [](DesignP& d) { d.len = 3; d.tone.wave = "sawtooth"; d.tone.f0 = 80; d.tone.f1 = 1600; d.tone.sweep = 3; d.tone.dec = 6; d.tone.level = 0.5; d.noise.on = true; d.noise.type = "bandpass"; d.noise.cut = 3000; d.noise.q = 2; d.noise.level = 0.6; d.noise.dec = 6; d.filt.on = true; d.filt.cut = 300; d.filt.cut2 = 12000; d.filt.q = 3; d.att = 2.2; d.fade = 0.3; });
        add("FM Blip", [](DesignP& d) { d.len = 0.25; d.tone.f0 = 660; d.tone.f1 = 660; d.tone.sweep = 0.05; d.tone.dec = 0.12; d.tone.fm = 0.6; d.tone.ratio = 3.5; });
        add("Clap", [](DesignP& d) { d.len = 0.35; d.tone.on = false; d.noise.on = true; d.noise.type = "bandpass"; d.noise.cut = 1400; d.noise.q = 1.6; d.noise.level = 1; d.noise.dec = 0.18; d.drive = 0.3; });
        return v;
    }();
    return list;
}

std::shared_ptr<Sample> renderDesign(const DesignP& D, const SampleBank& bank) {
    const double sr = 44100; const size_t n = (size_t)std::max(64.0, std::floor(D.len * sr));
    std::vector<float> sum(n, 0.f);
    auto env = [](double t, double level, double dec) { return t < 0.002 ? level : level * std::exp(-(t - 0.002) / (std::max(0.005, dec) / 3)); };
    if (D.tone.on) {
        const auto& T = D.tone; const WaveTable& w = WaveTable::get(T.wave); Timeline fr(T.f0); fr.set(std::max(10.0, T.f0), 0).exp(std::max(10.0, T.f1), std::max(0.003, T.sweep));
        double ph = 0, mph = 0;
        for (size_t i = 0; i < n; i++) {
            const double t = i / sr; double f = fr.at(t);
            if (T.fm > 0.005) { const double mg = T.fm * T.f0 * 4 * (t < 0.002 ? 1 : std::exp(-(t - 0.002) / (T.dec / 3))); f += std::sin(2 * PI * mph) * mg; mph += T.f0 * T.ratio / sr; }
            sum[i] += (float)(w.read(ph, std::abs(f), sr) * env(t, T.level, T.dec)); ph += f / sr; ph -= std::floor(ph);
        }
    }
    if (D.noise.on) {
        const auto& N = D.noise; Biquad b; b.type = filterTypeFrom(N.type); b.set(sr, N.cut, N.q, 0); Rng r(4242);
        for (size_t i = 0; i < n; i++) sum[i] += (float)(b.tick(r.bi()) * env(i / sr, N.level, N.dec));
    }
    if (D.smp.on) if (auto s = bank.get(D.smp.id); s && s->length() > 1) {
        auto src = D.smp.rev ? s->reversed() : s; const double rate = std::pow(2.0, D.smp.semi / 12) * src->sr / sr; double pos = D.smp.start * src->length();
        const auto& c0 = src->ch[0];
        for (size_t i = 0; i < n; i++) { const size_t k = (size_t)pos; if (k + 1 >= c0.size()) break; const double f = pos - k; sum[i] += (float)((c0[k] + (c0[k + 1] - c0[k]) * f) * D.smp.level); pos += rate; }
    }
    if (D.drive > 0.01) { const double k = 1 + D.drive * 24; for (auto& v : sum) v = (float)(std::tanh(std::clamp((double)v, -1.0, 1.0) * k) / std::tanh(k)); }
    if (D.filt.on) { Biquad b; b.type = FilterType::Lowpass; Timeline fr(D.filt.cut); fr.set(D.filt.cut, 0).exp(std::max(20.0, D.filt.cut2), D.len);
        for (size_t i = 0; i < n; i++) { if ((i & 15) == 0) b.set(sr, fr.at(i / sr), D.filt.q, 0); sum[i] = (float)b.tick(sum[i]); } }
    { Timeline g(0); g.set(0, 0).lin(1, std::min(D.len * 0.9, std::max(0.0005, D.att))).set(1, std::max(0.0, D.len - D.fade)).lin(0, D.len); for (size_t i = 0; i < n; i++) sum[i] *= (float)g.at(i / sr); }
    auto out = std::make_shared<Sample>(); out->sr = sr; out->group = "Project";
    bool anyFx = false; for (auto& f : D.fx) anyFx |= f.on;
    if (anyFx) {
        const size_t m = n + (size_t)(std::max(0.0, D.tail) * sr); std::vector<float> L(m, 0.f), R(m, 0.f); std::copy(sum.begin(), sum.end(), L.begin()); std::copy(sum.begin(), sum.end(), R.begin());
        for (auto& f : D.fx) { if (!f.on) continue;
            if (f.type == "plugin") {   // a CLAP / VST effect: its own instance, run offline
                juce::var slot(new juce::DynamicObject()); for (auto& kv : f.p) slot.getDynamicObject()->setProperty(kv.name, kv.value);
                auto pi = std::make_shared<PluginInstance>(); std::string err; if (!pi->load(slot, sr, err)) continue;
                uint32_t fr = 0; for (size_t i = 0; i < m; i += 512) { const int k = (int)std::min<size_t>(512, m - i); pi->process(&L[i], &R[i], k, fr, 10000); fr += (uint32_t)k; }
                continue;
            }
            auto e = makeEffect(f.type); e->prepare(sr, 512); e->set(f); e->offlineReady(); for (size_t i = 0; i < m; i += 512) e->process(&L[i], &R[i], (int)std::min<size_t>(512, m - i)); }
        out->ch = { L, R };
    } else out->ch = { sum };
    if (D.norm) { float mx = 0; for (auto& c : out->ch) for (float v : c) mx = std::max(mx, std::abs(v)); if (mx > 1e-4f) { const float k = 0.95f / mx; for (auto& c : out->ch) for (auto& v : c) v *= k; } }
    return out;
}

juce::MemoryBlock wavBytes(const Sample& s, int bits) {
    juce::MemoryBlock mb; juce::AudioBuffer<float> b((int)s.ch.size(), (int)s.length());
    for (size_t c = 0; c < s.ch.size(); c++) b.copyFrom((int)c, 0, s.ch[c].data(), (int)s.length());
    juce::WavAudioFormat wav;
    if (auto w = makeWriter(wav, std::make_unique<juce::MemoryOutputStream>(mb, false), s.sr, (int)s.ch.size(), bits)) w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
    return mb;
}

}   // namespace us
