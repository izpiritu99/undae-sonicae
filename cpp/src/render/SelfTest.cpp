// undae-selftest <project.undae.json>: the editing path without a window. Live edits while playing, structural edits
// (graph swap), undo / redo, save and reload (unknown fields kept, identical render). Prints PASS / FAIL per check.
#include <iostream>
#include <set>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include "app/Doc.h"
#include "engine/Analysis.h"
#include "engine/SoundDesign.h"
#include "engine/Effects.h"
#include "engine/Voices.h"
#include "engine/Writer.h"
#include "app/Presets.h"

static int fails = 0;
static void check(bool ok, const std::string& what) { std::cout << (ok ? "PASS  " : "FAIL  ") << what << std::endl; if (!ok) fails++; }   // flushed: a crash still shows how far the run got

// run the engine like an audio device would, n blocks; returns the peak
static float run(us::Engine& e, int blocks, std::function<void()> collect = {}) {
    std::vector<float> L(512), R(512); float pk = 0;
    for (int b = 0; b < blocks; b++) { e.process(L.data(), R.data(), 512); for (int i = 0; i < 512; i++) pk = std::max({ pk, std::abs(L[(size_t)i]), std::abs(R[(size_t)i]) }); if (collect) collect(); }
    return pk;
}
static double rms(const juce::AudioBuffer<float>& b) { double s = 0; for (int c = 0; c < 2; c++) for (int i = 0; i < b.getNumSamples(); i++) s += b.getSample(c, i) * b.getSample(c, i); return std::sqrt(s / (2.0 * b.getNumSamples())); }

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 2) { std::cerr << "usage: undae-selftest <project.undae.json>\n"; return 2; }
    const juce::File src = juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]);
    auto bank = std::make_shared<us::SampleBank>(); us::Engine eng(bank); eng.prepare(48000, 512);
    ui::Doc doc(eng, *bank);
    try { doc.load(src); } catch (const std::exception& e) { std::cout << "FAIL  load: " << e.what() << "\n"; return 1; }
    check(doc.p.channels.size() == 8 && doc.p.buses.size() == 7, "demo loaded (8 channels, 7 buses)");

    eng.play(us::Mode::Pattern, doc.pat, 0);
    check(run(eng, 100) > 0.05f, "pattern plays");

    // live edit with the same structure: the Drums bus to silence while playing
    auto* drums = doc.bus(doc.p.channels[0].bus); const double before = drums->vol;
    for (auto& b : doc.p.buses) if (b.kind != "master") b.vol = 0;
    doc.live();
    float pk = run(eng, 60);
    check(pk < 1e-3f, "live edit reaches the audio thread (all buses at zero -> silence), peak " + std::to_string(pk));
    doc.undo(); doc.redo(); doc.undo();   // live() is not an undo step: undo goes back to the loaded state
    check(std::abs(doc.bus(drums->id)->vol - before) < 1e-9, "undo restores the bus level");
    check(run(eng, 60) > 0.05f, "and the sound comes back");

    // structural edit while playing: a new synth channel with a note, and a delay on its bus
    auto c = ui::newChannel(doc.p, "synth", "Test synth"); c.bus = "master";
    us::Note n; n.id = us::newId(); n.t = 0; n.d = 96; n.n = 64; n.v = 1; doc.p.patterns[0].notes[c.id].push_back(n);
    doc.p.channels.push_back(c); doc.p.buses[0].fx.insert(doc.p.buses[0].fx.begin(), ui::newFx("delay"));
    for (auto& ch : doc.p.channels) if (ch.id != c.id) ch.mute = true;
    doc.commit();
    eng.play(us::Mode::Pattern, doc.pat, 0);
    int voices = 0; pk = run(eng, 120, [&] { voices = std::max(voices, eng.voiceCount()); });
    check(pk > 0.01f && voices >= 1, "structural edit swaps the graph and the new channel plays (peak " + std::to_string(pk) + ", voices " + std::to_string(voices) + ")");
    eng.collectGarbage();
    doc.undo();
    check(doc.p.channels.size() == 8 && doc.p.buses[0].fx.size() == 1, "undo removes the channel and the effect");
    doc.redo();
    check(doc.p.channels.size() == 9 && doc.p.buses[0].fx[0].type == "delay", "redo brings them back");
    eng.stop(); run(eng, 4);

    // save + reload: everything kept, unknown fields too
    const juce::File out = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("undae-selftest.undae.json");
    doc.save(out);
    us::Project back = us::loadProject(out);
    check(back.channels.size() == 9 && back.patterns[0].notes[c.id].size() == 1 && back.buses[0].fx[0].type == "delay", "saved file reloads with the edits");
    check(back.channels.back().synth.has_value() && back.channels.back().synth->osc[0].wave == "sawtooth", "synth settings saved");
    const juce::var key = back.raw.getProperty("key", juce::var());
    check((int)key.getProperty("root", -1) == 9 && key.getProperty("scale", "").toString() == "minor", "fields the native edition does not use are kept (key: A minor)");

    // the unedited demo, saved by the native edition, renders exactly like the original file
    us::Project orig = us::loadProject(src); const juce::File out2 = out.getSiblingFile("undae-selftest2.undae.json"); us::saveProject(orig, out2);
    us::Project again = us::loadProject(out2);
    us::Engine e1(bank), e2(bank); e1.prepare(48000, 512); e2.prepare(48000, 512);
    e1.setProject(std::make_shared<us::Project>(orig)); e2.setProject(std::make_shared<us::Project>(again));
    auto b1 = e1.render(us::Mode::Song, "", 1, 48000), b2 = e2.render(us::Mode::Song, "", 1, 48000);
    double diff = 0; for (int ch = 0; ch < 2; ch++) for (int i = 0; i < std::min(b1.getNumSamples(), b2.getNumSamples()); i++) diff = std::max(diff, (double)std::abs(b1.getSample(ch, i) - b2.getSample(ch, i)));
    check(b1.getNumSamples() == b2.getNumSamples() && diff < 1e-6, "saved-and-reloaded song renders identically (max difference " + std::to_string(diff) + ", RMS " + std::to_string(rms(b1)) + ")");
    if (!getenv("KEEP")) { out.deleteFile(); out2.deleteFile(); }
    // MIDI recording: a note played while playing lands in the pattern at the right place
    {
        ui::Doc d2(eng, *bank); d2.load(src); const std::string chId = d2.p.channels[6].id;   // Bass (synth)
        eng.setRecording(true); eng.play(us::Mode::Pattern, d2.pat, 0);
        run(eng, 1);
        const int startBlock = 23;   // 23 x 512 samples at 48 kHz ~ 0.245 s ~ 72 ticks at 92 BPM
        run(eng, startBlock - 1); eng.noteOn(chId, 50, 0.9); run(eng, 20); eng.noteOff(chId, 50); run(eng, 2);
        auto rec = eng.takeRecorded(); eng.setRecording(false); eng.stop(); run(eng, 2);
        const double spt = d2.p.secondsPerTick(), expect = startBlock * 512 / 48000.0 / spt, dur = 20 * 512 / 48000.0 / spt;
        check(rec.size() == 1 && rec[0].ch == chId && rec[0].note == 50 && std::abs(rec[0].tick - expect) < 4 && std::abs(rec[0].dur - dur) < 4,
              "recorded note: " + (rec.empty() ? std::string("none") : "tick " + std::to_string(rec[0].tick) + " (expected ~" + std::to_string(expect) + "), length " + std::to_string(rec[0].dur)));
        // pitch bend and sustain do not leave notes hanging
        eng.play(us::Mode::Pattern, d2.pat, 0); eng.sustain(true); eng.noteOn(chId, 55, 0.8); run(eng, 4); eng.noteOff(chId, 55); eng.pitchBend(0.5); run(eng, 20);
        int during = eng.voiceCount(); eng.sustain(false); eng.stop(); run(eng, 60);
        check(during >= 1 && eng.voiceCount() == 0, "sustain holds the note, pedal up and stop release it");
    }
    // importing audio: it is stored in the project, plays, and survives save / reload
    {
        const juce::File wav = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("undae-import-test.wav");
        { juce::AudioBuffer<float> b(1, 24000); for (int i = 0; i < 24000; i++) b.setSample(0, i, 0.5f * (float)std::sin(i * 2 * 3.14159265 * 440 / 48000)); wav.deleteFile();
          juce::WavAudioFormat f; auto w = us::makeWriter(f, std::make_unique<juce::FileOutputStream>(wav), 48000, 1, 16); w->writeFromAudioSampleBuffer(b, 0, 24000); }
        ui::Doc d3(eng, *bank); d3.setProject(ui::blankProject());
        const std::string id = d3.importAudio(wav);
        check(!id.empty() && bank->get(id) && std::abs(bank->get(id)->seconds() - 0.5) < 0.01, "audio file imported (0.5 s)");
        d3.addSampleChannel(id); d3.placeAudio(id, 0);
        const juce::File out3 = out.getSiblingFile("undae-selftest3.undae.json"); d3.save(out3);
        us::Project p3 = us::loadProject(out3); auto bank2 = std::make_shared<us::SampleBank>(); bank2->addFromProject(p3);
        us::Engine e3(bank2); e3.prepare(48000, 512); e3.setProject(std::make_shared<us::Project>(p3));
        auto b3 = e3.render(us::Mode::Song, "", 1, 48000);
        check(p3.embedded->data.count(id) && p3.audio.size() == 1 && rms(b3) > 0.05, "imported audio saved inside the project and renders from the reloaded file (RMS " + std::to_string(rms(b3)) + ")");
        wav.deleteFile(); out3.deleteFile();
    }
    // analysis: transcribe the demo's lead line, find the song's key and tempo
    {
        us::Project P = us::loadProject(src); const us::Pattern* lead = nullptr; for (auto& pt : P.patterns) if (pt.name == "Lead") lead = &pt;
        for (auto& c : P.channels) c.solo = c.name == "Lead";
        for (auto& b : P.buses) for (auto& f : b.fx) if (f.type == "reverb" || f.type == "delay") f.on = false;   // a dry line
        us::Engine e(bank); e.prepare(48000, 512); e.setProject(std::make_shared<us::Project>(P));
        auto buf = e.render(us::Mode::Pattern, lead->id, 1, 48000);
        std::vector<float> mono((size_t)buf.getNumSamples()); for (int i = 0; i < buf.getNumSamples(); i++) mono[(size_t)i] = 0.5f * (buf.getSample(0, i) + buf.getSample(1, i));
        auto found = us::melodyNotes(mono, 48000);
        std::vector<int> want; for (auto& [id, ns] : lead->notes) for (auto& n : ns) want.push_back(n.n); std::sort(want.begin(), want.end());
        std::vector<int> got; for (auto& f : found) got.push_back(f.n); auto gs = got; std::sort(gs.begin(), gs.end());
        juce::String gl; for (int g : got) gl << g << " ";
        check(gs == want, "melody transcription of the lead line: " + gl.toStdString() + "(" + std::to_string(want.size()) + " notes expected)");
        auto ticks = us::toTicks(found, P.bpm, us::STEP, false); bool timing = ticks.size() == lead->notes.begin()->second.size();
        if (timing) { auto ref = lead->notes.begin()->second; std::sort(ref.begin(), ref.end(), [](auto& a, auto& b) { return a.t < b.t; }); for (size_t i = 0; i < ticks.size(); i++) timing &= std::abs(ticks[i].t - ref[i].t) <= us::STEP; }
        check(timing, "transcribed notes start within a 16th of the originals");
        us::Project P2 = us::loadProject(src); us::Engine e2(bank); e2.prepare(48000, 512); e2.setProject(std::make_shared<us::Project>(P2));
        auto song = e2.render(us::Mode::Song, "", 1, 48000); std::vector<float> m2((size_t)song.getNumSamples()); for (int i = 0; i < song.getNumSamples(); i++) m2[(size_t)i] = 0.5f * (song.getSample(0, i) + song.getSample(1, i));
        auto key = us::detectKey(m2, 48000); const double bpm = us::detectBpm(m2, 48000);
        check((key.root == 9 && key.minor) || (key.root == 0 && !key.minor), "key of the demo song: " + key.name + " (it is A minor; C major is its relative)");
        check(std::abs(bpm - 92) < 2 || std::abs(bpm - 184) < 3 || std::abs(bpm - 46) < 1, "tempo of the demo song: " + std::to_string(bpm) + " BPM (92)");
        auto kick = us::renderDesign(us::designPresets()[0].second, *bank); float pk = 0; for (float v : kick->ch[0]) pk = std::max(pk, std::abs(v));
        check(kick->ch.size() == 1 && std::abs(kick->seconds() - 0.5) < 0.01 && std::abs(pk - 0.95f) < 0.01f, "sound design: the Kick preset renders 0.5 s, normalised");
    }
    // built-in effects that stand in for third-party plug-ins: each made from its UI spec, so engine and app agree on names
    {
        const double sr = 48000; const int N = 48000;
        auto runFx = [&](const us::FxSlot& f, std::vector<float> x) {   // mono test signal on both sides, processed in blocks
            auto e = us::makeEffect(f.type); e->prepare(sr, 512); e->set(f); std::vector<float> L = x, R = x;
            for (int o = 0; o < N; o += 512) e->process(L.data() + o, R.data() + o, std::min(512, N - o));
            return L;
        };
        auto sine = [&](double hz, double amp) { std::vector<float> v((size_t)N); for (int i = 0; i < N; i++) v[(size_t)i] = (float)(amp * std::sin(2 * 3.14159265358979 * hz * i / sr)); return v; };
        auto lvl = [&](const std::vector<float>& v) { double s = 0; for (int i = N / 2; i < N; i++) s += (double)v[(size_t)i] * v[(size_t)i]; return 10 * std::log10(s / (N / 2) + 1e-30); };   // dB, second half
        auto tone = [&](const std::vector<float>& v, double hz) {   // amplitude of one frequency over the second half (correlation)
            double c = 0, s = 0; for (int i = N / 2; i < N; i++) { const double w = 2 * 3.14159265358979 * hz * i / sr; c += v[(size_t)i] * std::cos(w); s += v[(size_t)i] * std::sin(w); }
            return 2 * std::sqrt(c * c + s * s) / (N / 2);
        };
        bool specs = true; for (const char* t : { "peq", "deess", "sat", "fdn" }) specs &= ui::fxSpec(t) != nullptr;
        check(specs, "new built-in effects listed for the mixer (Parametric EQ, De-esser, Saturator, Vault reverb)");

        auto eq = ui::newFx("peq"); eq.p.set("p2f", 1000.0); eq.p.set("p2g", 12.0); eq.p.set("hps", juce::String("24 dB/oct")); eq.p.set("hpf", 200.0);
        const double at1k = lvl(runFx(eq, sine(1000, 0.1))) - lvl(sine(1000, 0.1)), at50 = lvl(runFx(eq, sine(50, 0.1))) - lvl(sine(50, 0.1)), at4k = lvl(runFx(eq, sine(4000, 0.1))) - lvl(sine(4000, 0.1));
        check(std::abs(at1k - 12) < 0.5 && at50 < -20 && std::abs(at4k) < 2.5, "parametric EQ: +12 dB band at 1 kHz gives " + std::to_string(at1k) + " dB, 24 dB/oct low cut at 200 Hz leaves 50 Hz at " + std::to_string(at50) + " dB, 4 kHz at " + std::to_string(at4k) + " dB");

        auto ds = ui::newFx("deess"); ds.p.set("thr", -40.0); ds.p.set("range", 12.0);
        const double ess = lvl(runFx(ds, sine(10000, 0.5))) - lvl(sine(10000, 0.5)), low = lvl(runFx(ds, sine(200, 0.5))) - lvl(sine(200, 0.5));
        check(ess < -7 && std::abs(low) < 0.5, "de-esser: a loud 10 kHz tone is turned down " + std::to_string(-ess) + " dB, 200 Hz changes " + std::to_string(low) + " dB");

        auto sat = ui::newFx("sat"); sat.p.set("drive", 18.0); sat.p.set("bias", 0.6);
        auto sy = runFx(sat, sine(200, 0.3)); const double h2 = tone(sy, 400) / tone(sy, 200), gain = lvl(sy) - lvl(sine(200, 0.3));
        bool finite = true; for (float v : sy) finite &= std::isfinite(v);
        check(finite && h2 > 0.02 && std::abs(gain) < 4, "saturator (tube): 2nd harmonic at " + std::to_string(20 * std::log10(h2)) + " dB of the fundamental, level change " + std::to_string(gain) + " dB");

        auto rv = ui::newFx("fdn"); rv.p.set("decay", 2.0); rv.p.set("mix", 1.0); rv.p.set("pre", 0.0); rv.p.set("mod", 0.0);
        { auto e = us::makeEffect("fdn"); e->prepare(sr, 512); e->set(rv); std::vector<float> L(3 * 48000, 0.f), R(L.size(), 0.f); L[0] = R[0] = 1;
          for (size_t o = 0; o < L.size(); o += 512) e->process(L.data() + o, R.data() + o, (int)std::min<size_t>(512, L.size() - o));
          auto win = [&](double a, double b) { double s = 0; for (size_t i = (size_t)(a * sr); i < (size_t)(b * sr); i++) s += (double)L[i] * L[i] + (double)R[i] * R[i]; return 10 * std::log10(s + 1e-30); };
          const double drop = win(0.3, 0.4) - win(1.3, 1.4);   // 2 s RT60 -> 30 dB per second
          bool fin = true; for (float v : L) fin &= std::isfinite(v);
          check(fin && drop > 22 && drop < 38, "Vault reverb: 2 s decay falls " + std::to_string(drop) + " dB in one second (30 expected)"); }
        // every mode at its extremes stays finite and bounded on a loud chord
        std::vector<float> loud((size_t)N); for (int i = 0; i < N; i++) loud[(size_t)i] = (float)(0.9 * (std::sin(2 * 3.14159265358979 * 110 * i / sr) * 0.5 + std::sin(2 * 3.14159265358979 * 3300 * i / sr) * 0.5));
        float worst = 0; bool ok = true; std::string worstMode, peaks;
        auto measure = [&](const us::FxSlot& f, const char* md) { float pk = 0; for (float v : runFx(f, loud)) { if (!std::isfinite(v)) ok = false; pk = std::max(pk, std::abs(v)); } if (pk > worst) { worst = pk; worstMode = md; } peaks += std::string(md) + " " + juce::String(pk, 2).toStdString() + ", "; };
        for (const char* md : { "tube", "tape", "transformer", "clip", "exciter" }) { auto f = ui::newFx("sat"); f.p.set("mode", juce::String(md)); f.p.set("drive", 30.0); f.p.set("bias", 1.0); measure(f, md); }
        for (const char* md : { "hall", "room", "plate", "gated" }) { auto f = ui::newFx("fdn"); f.p.set("mode", juce::String(md)); f.p.set("decay", 20.0); f.p.set("size", 2.0); f.p.set("damp", 20000.0); f.p.set("mod", 1.0); f.p.set("mix", 1.0); measure(f, md); }
        check(ok && worst < 4, "saturator and Vault reverb modes at full settings stay finite and bounded (peaks: " + peaks + "input peak 0.9)");

        // second batch: gate, transient shaper, stereo width
        auto runSt = [&](const us::FxSlot& f, std::vector<float> L, std::vector<float> R, const std::vector<float>* key = nullptr) {
            auto e = us::makeEffect(f.type); e->prepare(sr, 512); e->set(f);
            for (int o = 0; o < N; o += 512) { const int b = std::min(512, N - o); if (key) e->side(key->data() + o, key->data() + o, b); e->process(L.data() + o, R.data() + o, b); }
            return std::make_pair(L, R);
        };
        auto win = [&](const std::vector<float>& v, double a, double b) { double s = 0; size_t c = 0; for (size_t i = (size_t)(a * sr); i < (size_t)(b * sr); i++, c++) s += (double)v[i] * v[i]; return 10 * std::log10(s / std::max<size_t>(1, c) + 1e-30); };
        std::vector<float> sig((size_t)N); for (int i = 0; i < N; i++) sig[(size_t)i] = (float)((i < N / 2 ? 0.5 : 0.001) * std::sin(2 * 3.14159265358979 * 440 * i / sr));
        auto gt = ui::newFx("gate");
        auto go = runSt(gt, sig, sig).first;
        const double openD = win(go, 0.1, 0.5) - win(sig, 0.1, 0.5), shutD = win(go, 0.8, 1.0) - win(sig, 0.8, 1.0);
        auto gk = gt; gk.p.set("src", juce::String("kick")); std::vector<float> quiet = sine(440, 0.001), keyLoud = sine(60, 0.5);
        const double keyD = win(runSt(gk, quiet, quiet, &keyLoud).first, 0.2, 1.0) - win(quiet, 0.2, 1.0);
        check(std::abs(openD) < 0.5 && shutD < -40 && std::abs(keyD) < 1, "gate: open part changes " + std::to_string(openD) + " dB, quiet part shut " + std::to_string(shutD) + " dB, a loud key bus holds a quiet signal open (" + std::to_string(keyD) + " dB)");

        std::vector<float> hits((size_t)N, 0.f); for (int i = 0; i < N; i++) { const double t = std::fmod((double)i / sr, 0.25); hits[(size_t)i] = (float)(0.5 * std::exp(-t / 0.08) * std::sin(2 * 3.14159265358979 * 200 * t)); }
        auto ta = ui::newFx("trans"); ta.p.set("attack", 100.0); auto ts = ui::newFx("trans"); ts.p.set("sustain", -100.0);
        auto at = runSt(ta, hits, hits).first, su = runSt(ts, hits, hits).first;
        const double hitUp = win(at, 0.5, 0.505) - win(hits, 0.5, 0.505), tailDown = win(su, 0.58, 0.7) - win(hits, 0.58, 0.7);
        check(hitUp > 3 && tailDown < -3, "transient shaper: attack +100 % lifts the first 5 ms of a hit " + std::to_string(hitUp) + " dB, sustain -100 % lowers its tail " + std::to_string(tailDown) + " dB");

        std::vector<float> sL((size_t)N), sR((size_t)N); for (int i = 0; i < N; i++) { const double w = 0.5 * std::sin(2 * 3.14159265358979 * 60 * i / sr) + 0.5 * std::sin(2 * 3.14159265358979 * 2000 * i / sr); sL[(size_t)i] = (float)w; sR[(size_t)i] = (float)-w; }   // all side
        auto im = ui::newFx("imager"); im.p.set("bass", juce::String("mono")); im.p.set("mono", 150.0);
        auto bm = runSt(im, sL, sR); std::vector<float> side((size_t)N); for (int i = 0; i < N; i++) side[(size_t)i] = 0.5f * (bm.first[(size_t)i] - bm.second[(size_t)i]);
        const double lowD = 20 * std::log10(tone(side, 60) / 0.5 + 1e-12), highD = 20 * std::log10(tone(side, 2000) / 0.5 + 1e-12);
        auto im0 = ui::newFx("imager"); im0.p.set("width", 0.0); auto mo = runSt(im0, sL, sR); float diff0 = 0; for (int i = 0; i < N; i++) diff0 = std::max(diff0, std::abs(mo.first[(size_t)i] - mo.second[(size_t)i]));
        check(lowD < -20 && std::abs(highD) < 1 && diff0 < 1e-6f, "stereo width: bass mono below 150 Hz takes the 60 Hz side down " + std::to_string(-lowD) + " dB, 2 kHz side changes " + std::to_string(highD) + " dB; width 0 % is mono");

        // third batch: tempo-aware effects, fed tempo and song position as the engine does (120 BPM from beat 0)
        auto runTempo = [&](const us::FxSlot& f, std::vector<float> L, std::vector<float> R) {
            auto e = us::makeEffect(f.type); e->prepare(sr, 512); e->set(f);
            for (int o = 0; o < N; o += 512) { e->bpm = 120; e->beat = o / sr * 2; e->process(L.data() + o, R.data() + o, std::min(512, N - o)); }
            return std::make_pair(L, R);
        };
        auto peakIn = [&](const std::vector<float>& v, int a, int b) { int at = a; for (int i = a; i < b; i++) if (std::abs(v[(size_t)i]) > std::abs(v[(size_t)at])) at = i; return at; };
        std::vector<float> click((size_t)N, 0.f); click[0] = 1;
        auto ec = ui::newFx("echo"); ec.p.set("fb", 0.5); ec.p.set("lo", 20.0); ec.p.set("hi", 20000.0); ec.p.set("mix", 1.0);
        auto eo = runTempo(ec, click, click).first; const int p1 = peakIn(eo, 1000, 18000), p2 = peakIn(eo, 18000, 30000); auto en = [&](int c) { double e = 0; for (int i = c - 200; i < c + 200; i++) e += (double)eo[(size_t)i] * eo[(size_t)i]; return std::sqrt(e); }; const double ratio = en(p2) / en(p1);   // energy: the loop filter spreads the click
        auto epp = ec; epp.p.set("mode", juce::String("ping-pong")); auto po = runTempo(epp, click, click);
        const double rFirst = std::abs(po.second[(size_t)peakIn(po.second, 11000, 13000)]), rSecond = std::abs(po.second[(size_t)peakIn(po.second, 23000, 25000)]);
        check(std::abs(p1 - 12000) <= 3 && std::abs(p2 - 24000) <= 6 && ratio > 0.4 && ratio < 0.6 && rFirst < 1e-3 && rSecond > 0.3,
              "tempo delay: 1/8 at 120 BPM echoes at sample " + std::to_string(p1) + " and " + std::to_string(p2) + " (12000, 24000), feedback 0.5 gives " + std::to_string(ratio) + "; ping-pong answers on the right one echo later");
        {   // the engine hands its tempo to effects in an export: a 1/4 echo of a kick at 120 BPM lands 0.5 s later
            us::Project tp = ui::blankProject(); tp.bpm = 120; auto kc = ui::newChannel(tp, "sample", "Kick", "808:kick"); kc.bus = "master"; tp.channels.push_back(kc);
            us::Note kn; kn.id = us::newId(); kn.t = 0; kn.d = 24; kn.n = 60; kn.v = 1; tp.patterns[0].notes[kc.id].push_back(kn);
            auto q = ui::newFx("echo"); q.p.set("sync", juce::String("1/4")); q.p.set("fb", 0.0); q.p.set("mix", 0.5); q.p.set("lo", 20.0); q.p.set("hi", 20000.0); tp.buses[0].fx.insert(tp.buses[0].fx.begin(), q);
            us::Engine te(bank); te.prepare(48000, 512); te.setProject(std::make_shared<us::Project>(tp));
            auto tb = te.render(us::Mode::Pattern, tp.patterns[0].id, 1, 48000); std::vector<float> m0((size_t)tb.getNumSamples()); for (int i = 0; i < tb.getNumSamples(); i++) m0[(size_t)i] = tb.getSample(0, i);
            const int k0 = peakIn(m0, 0, 12000), k1 = peakIn(m0, 18000, 36000);
            check(std::abs((k1 - k0) - 24000) <= 8, "tempo delay in an export follows the project tempo: the echo comes " + std::to_string(k1 - k0) + " samples after the kick (24000)");
        }
        std::vector<float> mus((size_t)N); for (int i = 0; i < N; i++) mus[(size_t)i] = (float)(0.3 * std::sin(i * 0.0371) + 0.2 * std::sin(i * 0.1173 + 1) + 0.1 * std::sin(i * 0.0059));
        auto gr = ui::newFx("glitch"); gr.p.set("repeat", 1.0); gr.p.set("reverse", 0.0); gr.p.set("stop", 0.0); gr.p.set("gate", 0.0);
        auto ro = runTempo(gr, mus, mus).first; float repD = 0; for (int j = 200; j < 2800; j++) repD = std::max(repD, std::abs(ro[(size_t)(24000 + j)] - ro[(size_t)(24000 + j + 3000)]));
        auto gv = gr; gv.p.set("repeat", 0.0); gv.p.set("reverse", 1.0); auto vo = runTempo(gv, mus, mus).first; float revD = 0; for (int o = 200; o < 11800; o++) revD = std::max(revD, std::abs(vo[(size_t)(24000 + o)] - mus[(size_t)(24000 - 1 - o)]));
        auto gd = ui::newFx("glitch"); gd.p.set("stop", 0.3); auto d1 = runTempo(gd, mus, mus).first, d2 = runTempo(gd, mus, mus).first; bool same = d1 == d2, fin = true; for (float v : d1) fin &= std::isfinite(v);
        check(repD < 1e-4f && revD < 1e-4f && same && fin, "glitch: a repeat step loops a 1/32 slice (difference " + std::to_string(repD) + "), a reverse step plays the step before backwards (" + std::to_string(revD) + "), and the same song gives the same glitches every time");

        // vocoder: the carrier takes on the voice's spectrum, and is silent when the voice is
        {
            std::vector<float> voice((size_t)N); for (int i = 0; i < N; i++) voice[(size_t)i] = (float)(i < N / 2 ? 0.3 * std::sin(2 * 3.14159265358979 * 1000 * i / sr) : 0.0);
            auto vc = ui::newFx("vocoder"); vc.p.set("noise", 0.0);   // built-in buzz at note 48 (130.8 Hz): harmonic 8 is 1046 Hz, harmonic 3 is 392 Hz
            auto vo2 = runSt(vc, voice, voice).first;
            std::vector<float> fh2((size_t)N, 0.f); std::copy(vo2.begin() + N / 4, vo2.begin() + N / 2, fh2.begin() + N / 2);   // put 0.25-0.5 s where tone() looks
            const double near = tone(fh2, 1046.5), far = tone(fh2, 392.4);
            const double loud = win(vo2, 0.25, 0.5), silent = win(vo2, 0.75, 1.0);
            // carrier from another bus (side chain): 440 Hz + 3 kHz; a 3 kHz voice keeps 3 kHz and drops 440 Hz
            std::vector<float> car((size_t)N), v3 = sine(3000, 0.3); for (int i = 0; i < N; i++) car[(size_t)i] = (float)(0.3 * std::sin(2 * 3.14159265358979 * 440 * i / sr) + 0.3 * std::sin(2 * 3.14159265358979 * 3000 * i / sr));
            auto vs = vc; vs.p.set("src", juce::String("synth")); auto so = runSt(vs, v3, v3, &car).first;
            const double keep = tone(so, 3000), drop = tone(so, 440);
            check(near > 10 * far && silent < loud - 60 && keep > 30 * drop && loud > -40,
                  "vocoder: a 1 kHz voice brings out the buzz's 1046 Hz harmonic " + std::to_string(20 * std::log10(near / std::max(1e-12, far))) + " dB over 392 Hz, silence in gives "
                  + std::to_string(silent - loud) + " dB, a side-chained carrier keeps the voice's 3 kHz " + std::to_string(20 * std::log10(keep / std::max(1e-12, drop))) + " dB over 440 Hz (level " + std::to_string(loud) + " dB)");
        }
        // meters: BS.1770 reference (a 1 kHz sine at -20 dBFS peak on both channels reads -20 LUFS), true peak, correlation, tuner
        {
            auto e = us::makeEffect("meters"); e->prepare(sr, 512); e->attach("selftest-meters"); e->set(ui::newFx("meters"));
            auto tap = us::MeterTap::forSlot("selftest-meters");
            auto feed = [&](double seconds, bool anti) { std::vector<float> l(512), r(512); const int blocks = (int)(seconds * sr / 512); static int64_t t = 0;
                for (int b = 0; b < blocks; b++) { for (int i = 0; i < 512; i++, t++) { const float v = (float)(0.1 * std::sin(2 * 3.14159265358979 * 1000 * t / sr)); l[(size_t)i] = v; r[(size_t)i] = anti ? -v : v; } e->process(l.data(), r.data(), 512); } };
            feed(4, false);
            const float mom = tap->momentary, sh = tap->shortTerm, in = tap->integrated, tpk = tap->truePeak, c1 = tap->corr;
            feed(3, true); const float c2 = tap->corr;
            std::vector<float> lastL, lastR; tap->last(2048, lastL, lastR); bool ringOk = lastL.size() == 2048 && std::abs(lastL.back() + lastR.back()) < 1e-6f && std::abs(lastL.back()) > 0;
            const double midi = us::pitchOf(sine(440, 0.3), sr);
            check(std::abs(mom + 20) < 0.2 && std::abs(sh + 20) < 0.2 && std::abs(in + 20) < 0.2 && std::abs(tpk + 20) < 0.2 && c1 > 0.99f && c2 < -0.99f && ringOk && std::abs(midi - 69) < 0.03,
                  "meters: -20 dBFS 1 kHz reads momentary " + std::to_string(mom) + ", short " + std::to_string(sh) + ", integrated " + std::to_string(in) + " LUFS (-20), true peak "
                  + std::to_string(tpk) + " dBTP, correlation " + std::to_string(c1) + " / " + std::to_string(c2) + " (opposite phase), tuner hears 440 Hz as note " + std::to_string(midi));
        }
        // pitch correction: an off-key voice-like tone (8 harmonics) comes out on the nearest note of the key
        {
            auto voiceAt = [&](double hz) { std::vector<float> v((size_t)N); double ph = 0; for (int i = 0; i < N; i++) { double s = 0; for (int h = 1; h <= 8; h++) s += std::sin(2 * 3.14159265358979 * h * ph) / h; v[(size_t)i] = (float)(0.25 * s); ph += hz / sr; ph -= std::floor(ph); } return v; };
            auto runPf = [&](const us::FxSlot& f, const std::vector<float>& x, int root, const std::string& scale) {
                auto e = us::makeEffect("pitchfix"); e->prepare(sr, 512); e->set(f); e->keyRoot = root; e->keyScale = &scale; std::vector<float> L = x, R = x;
                for (int o = 0; o < N; o += 512) e->process(L.data() + o, R.data() + o, std::min(512, N - o));
                return L;
            };
            auto pitchAt = [&](const std::vector<float>& v) { std::vector<float> seg(v.begin() + N / 2, v.begin() + N / 2 + 4096); return us::pitchOf(seg, sr); };
            const std::string chrom = "chromatic", cmaj = "major";
            auto pf = ui::newFx("pitchfix"); pf.p.set("speed", 0.0);
            const auto sharpA = voiceAt(450.0);                       // A4 + 39 cents
            const double fixedA = pitchAt(runPf(pf, sharpA, 0, chrom)), rawA = pitchAt(sharpA);
            const auto cs = voiceAt(440.0 * std::pow(2.0, (61.3 - 69) / 12));   // C#4 + 30 cents: in C major the nearest note is D4
            const double fixedD = pitchAt(runPf(pf, cs, 0, cmaj));
            auto pf0 = pf; pf0.p.set("amount", 0.0); auto same = runPf(pf0, sharpA, 0, chrom); const double untouched = pitchAt(same);
            const double lvl0 = win(same, 0.5, 0.9) - win(sharpA, 0.5, 0.9);
            // steadiness (no warble): 10 ms RMS windows of the corrected tone stay within 1.5 dB; speed for live use
            const auto fx = runPf(pf, sharpA, 0, chrom); double lo = 1e9, hi = -1e9; for (double t0 = 0.3; t0 < 0.95; t0 += 0.01) { const double v = win(fx, t0, t0 + 0.01); lo = std::min(lo, v); hi = std::max(hi, v); }
            const auto t1 = juce::Time::getMillisecondCounterHiRes(); for (int r = 0; r < 4; r++) runPf(pf, sharpA, 0, chrom); const double rt = 4000.0 / (juce::Time::getMillisecondCounterHiRes() - t1);
            check(std::abs(fixedA - 69) < 0.06 && std::abs(rawA - 69.39) < 0.06 && std::abs(fixedD - 62) < 0.06 && std::abs(untouched - 69.39) < 0.06 && std::abs(lvl0) < 1.5 && hi - lo < 1.5 && rt > 10,
                  "pitch correction: A4 +39 cents comes out as note " + std::to_string(fixedA) + " (69), C#4 +30 cents in C major as " + std::to_string(fixedD) + " (62), amount 0 leaves "
                  + std::to_string(untouched) + " at " + std::to_string(lvl0) + " dB; level ripple " + std::to_string(hi - lo) + " dB, " + std::to_string((int)rt) + "x real time");
        }
        // multiband: flat when nothing is compressed, the low band alone squeezed, Listen isolates a band
        {
            auto gainAt = [&](const us::FxSlot& f, double hz) { const auto x = sine(hz, 0.5); return lvl(runFx(f, x)) - lvl(x); };
            auto flat = ui::newFx("multiband"); for (const char* k : { "lratio", "mratio", "hratio" }) flat.p.set(k, 1.0);
            double worstFlat = 0; for (double hz : { 60.0, 200.0, 800.0, 3000.0, 9000.0 }) worstFlat = std::max(worstFlat, std::abs(gainAt(flat, hz)));
            auto lowSq = flat; lowSq.p.set("lthr", -30.0); lowSq.p.set("lratio", 10.0);
            const double low80 = gainAt(lowSq, 80), mid1k = gainAt(lowSq, 1000);
            auto lis = flat; lis.p.set("listen", juce::String("mid")); const double lis80 = gainAt(lis, 60), lis1k = gainAt(lis, 1000);
            check(worstFlat < 0.1 && low80 < -15 && std::abs(mid1k) < 0.5 && lis80 < -30 && std::abs(lis1k) < 0.5,
                  "multiband: flat within " + std::to_string(worstFlat) + " dB with ratio 1, the low band at 10:1 pulls 80 Hz down " + std::to_string(-low80) + " dB and leaves 1 kHz at "
                  + std::to_string(mid1k) + " dB; Listen mid drops 60 Hz " + std::to_string(-lis80) + " dB");
        }
        // modulation: flanger comb, ring modulator sidebands, frequency shifter single sideband, rotary movement, auto-pan
        {
            auto fl = ui::newFx("flanger"); fl.p.set("depth", 0.0); fl.p.set("fb", 0.0); fl.p.set("delay", 5.0); fl.p.set("mix", 0.5);   // a fixed 5 ms comb
            const double notch = lvl(runFx(fl, sine(100, 0.3))) - lvl(sine(100, 0.3)), peak = lvl(runFx(fl, sine(200, 0.3))) - lvl(sine(200, 0.3));
            auto rm = ui::newFx("ringmod"); rm.p.set("freq", 1000.0); auto ro2 = runFx(rm, sine(300, 0.5));
            const double side = std::min(tone(ro2, 700), tone(ro2, 1300)), carrierLeak = tone(ro2, 300);
            auto fs = ui::newFx("fshift"); fs.p.set("shift", 100.0); auto fo = runFx(fs, sine(1000, 0.5));
            const double up = tone(fo, 1100), mirror = tone(fo, 900), orig = tone(fo, 1000);
            auto rt = ui::newFx("rotary"); rt.p.set("speed", juce::String("fast")); auto rs = runSt(rt, sine(1500, 0.3), sine(1500, 0.3));
            double rlo = 1e9, rhi = -1e9, lrDiff = 0; for (double t0 = 0.5; t0 < 0.95; t0 += 0.01) { const double v = win(rs.first, t0, t0 + 0.01); rlo = std::min(rlo, v); rhi = std::max(rhi, v); }
            for (int i = N / 2; i < N; i++) lrDiff = std::max(lrDiff, (double)std::abs(rs.first[(size_t)i] - rs.second[(size_t)i]));
            auto ap = ui::newFx("autopan"); ap.p.set("sync", juce::String("1/4")); ap.p.set("depth", 1.0); auto ao = runTempo(ap, sine(1000, 0.5), sine(1000, 0.5)).first;   // 1/4 at 120 BPM: 0.5 s
            const double onBeat = win(ao, 0.495, 0.505), offBeat = win(ao, 0.245, 0.255);
            auto pp = ap; pp.p.set("mode", juce::String("pan")); auto po2 = runTempo(pp, sine(1000, 0.5), sine(1000, 0.5));
            const double panL = win(po2.first, 0.245, 0.255) - win(po2.second, 0.245, 0.255);
            check(notch < -20 && std::abs(peak - 3) < 0.5 && side > 0.2 && carrierLeak < 0.005 && up > 0.45 && mirror < 0.02 && orig < 0.02 && rhi - rlo > 3 && lrDiff > 0.05 && offBeat < onBeat - 30 && std::abs(panL) > 20,
                  "modulation: flanger comb " + std::to_string(notch) + " / +" + std::to_string(peak) + " dB, ring mod sidebands " + std::to_string(side) + " (300 Hz left " + std::to_string(carrierLeak)
                  + "), shifter 1000 -> 1100 Hz at " + std::to_string(up) + " (900 Hz " + std::to_string(mirror) + "), rotary moves " + std::to_string(rhi - rlo) + " dB, tremolo dips "
                  + std::to_string(onBeat - offBeat) + " dB off the beat, auto-pan " + std::to_string(panL) + " dB side to side");
        }
        // lo-fi: sample-and-hold images, bit levels, crackle on silence (the same every run), band-limiting
        {
            auto clean = [&](us::FxSlot f) { f.p.set("lo", 20.0); f.p.set("hi", 20000.0); return f; };
            auto sh = clean(ui::newFx("lofi")); sh.p.set("rate", 8000.0); sh.p.set("bits", 16.0);
            const double image = tone(runFx(sh, sine(1000, 0.5)), 7000);
            auto bt = clean(ui::newFx("lofi")); bt.p.set("rate", 48000.0); bt.p.set("bits", 4.0); auto bo = runFx(bt, sine(440, 0.9));
            auto b16 = bt; b16.p.set("bits", 16.0); auto ref = runFx(b16, sine(440, 0.9)); std::vector<float> err((size_t)N); for (int i = 0; i < N; i++) err[(size_t)i] = bo[(size_t)i] - ref[(size_t)i];
            const double snr = lvl(ref) - lvl(err);   // 4 bits on a near-full sine: about 6.02 x 4 + 1.76 = 26 dB
            auto ck = clean(ui::newFx("lofi")); ck.p.set("rate", 48000.0); ck.p.set("bits", 16.0); ck.p.set("crackle", 1.0); std::vector<float> zero((size_t)N, 0.f);
            auto c1 = runFx(ck, zero), c2 = runFx(ck, zero); float cpk = 0; for (float v : c1) cpk = std::max(cpk, std::abs(v));
            auto bl = ui::newFx("lofi"); bl.p.set("rate", 48000.0); bl.p.set("bits", 16.0); bl.p.set("hi", 3000.0);
            const double cut10k = lvl(runFx(bl, sine(10000, 0.5))) - lvl(sine(10000, 0.5)), pass1k = lvl(runFx(bl, sine(1000, 0.5))) - lvl(sine(1000, 0.5));
            check(image > 0.05 && snr > 20 && snr < 32 && cpk > 0.01f && c1 == c2 && cut10k < -30 && std::abs(pass1k) < 1,
                  "lo-fi: 8 kHz sample and hold leaves a 7 kHz image (" + std::to_string(image) + "), 4 bits leaves the signal " + std::to_string(snr) + " dB above its quantising noise (about 26), crackle peaks "
                  + std::to_string(cpk) + " on silence the same every run, the 3 kHz high cut takes 10 kHz down " + std::to_string(-cut10k) + " dB");
        }
        // instruments: the synth's wavetable mode (Morph scans the frames)
        {
            auto renderNote = [&](const us::SynthP& P, int note) { us::NoteOpts o; auto v = us::makeSynthVoice(P, note, 1.0, 0, 0.9, o, sr); std::vector<float> L((size_t)N, 0.f), R((size_t)N, 0.f);
                for (int off = 0; off < N; off += 512) v->render(L.data() + off, R.data() + off, nullptr, std::min(512, N - off), off / sr, sr); return L; };
            us::SynthP P = ui::defSynth(); P.f1.on = false; P.osc[0].wave = "wt:pulse";
            P.osc[0].morph = 0; auto sq = renderNote(P, 57); P.osc[0].morph = 1; auto nar = renderNote(P, 57);   // 220 Hz: square, then a 5 % pulse
            const double evenSq = tone(sq, 440) / tone(sq, 220), evenNar = tone(nar, 440) / tone(nar, 220);
            P.osc[0].wave = "wt:bright"; P.osc[0].morph = 0; auto dark = renderNote(P, 45); P.osc[0].morph = 1; auto open = renderNote(P, 45);   // 110 Hz
            const double darkHi = tone(dark, 1100) / tone(dark, 110), openHi = tone(open, 1100) / tone(open, 110);
            bool presetsOk = true; std::string badP;
            for (auto& [name, fn] : ui::synthPresets()) { us::SynthP x = ui::defSynth(); fn(x); if (!us::isWaveSet(x.osc[0].wave)) continue; auto y = renderNote(x, 60); float pk = 0; bool fin = true; for (float v : y) { fin &= std::isfinite(v); pk = std::max(pk, std::abs(v)); } if (!fin || pk < 0.01f || pk > 2) { presetsOk = false; badP = name; } }
            check(evenSq < 0.02 && evenNar > 0.5 && darkHi < 0.01 && openHi > 0.05 && presetsOk,
                  "synth wavetables: the pulse table at position 0 has no 2nd harmonic (" + std::to_string(evenSq) + "), at 1 a strong one (" + std::to_string(evenNar) + "); the opening table's 10th harmonic goes "
                  + std::to_string(darkHi) + " -> " + std::to_string(openHi) + "; the wavetable presets play" + (badP.empty() ? "" : " (not " + badP + ")"));
        }
        // granular sampler: grains of a 440 Hz sample keep its pitch, follow the keyboard, sound for as long as the note
        {
            auto smp = std::make_shared<us::Sample>(); smp->sr = 48000; smp->ch.assign(1, std::vector<float>(48000)); for (int i = 0; i < 48000; i++) smp->ch[0][(size_t)i] = (float)(0.5 * std::sin(2 * 3.14159265358979 * 440 * i / 48000.0));
            us::Channel C; C.root = 69; C.gran.on = true; C.gran.size = 0.06; C.gran.density = 40; C.gran.jitter = 0.2; C.gran.spread = 0.5;
            auto play = [&](int note, double dur) { us::NoteOpts o; auto v = us::makeSampleVoice(smp, C, note, 1.0, 0, dur, o, sr); std::vector<float> L((size_t)N, 0.f), R((size_t)N, 0.f);
                for (int off = 0; off < N; off += 512) v->render(L.data() + off, R.data() + off, nullptr, std::min(512, N - off), off / sr, sr); return std::make_pair(L, R); };
            auto a = play(69, 0.8), b = play(81, 0.8);
            std::vector<float> ma((size_t)N), mb((size_t)N); for (int i = 0; i < N; i++) { ma[(size_t)i] = a.first[(size_t)i] + a.second[(size_t)i]; mb[(size_t)i] = b.first[(size_t)i] + b.second[(size_t)i]; }
            auto focus = [&](const std::vector<float>& v, double hz) { return tone(v, hz) / std::max(tone(v, hz * 0.9439), tone(v, hz * 1.0595)); };   // against the semitones either side (overlapping grains with random phases spread the pitch into a band of a few tens of Hz, so the unison margin is smaller)
            const double pa = focus(ma, 440), pb = focus(mb, 880), held = win(ma, 0.3, 0.7), after = win(ma, 0.95, 1.0);
            bool fin = true; for (float v : ma) fin &= std::isfinite(v);
            us::Project gp = ui::blankProject(); auto gc = ui::newChannel(gp, "sample", "Grains", "808:kick"); gc.gran.on = true; gc.gran.size = 0.2; gc.gran.spray = 3; gp.channels.push_back(gc);
            const juce::File gf = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("undae-selftest-gran.undae.json"); us::saveProject(gp, gf); auto gb = us::loadProject(gf); gf.deleteFile();
            const bool saved = gb.channels.size() == 1 && gb.channels[0].gran.on && std::abs(gb.channels[0].gran.size - 0.2) < 1e-9 && std::abs(gb.channels[0].gran.spray - 3) < 1e-9;
            check(fin && pa > 2 && pb > 10 && held > -20 && after < held - 40 && saved,
                  "granular: grains of a 440 Hz sample keep 440 Hz (" + std::to_string(20 * std::log10(pa)) + " dB over the neighbouring semitones), an octave up 880 Hz (" + std::to_string(20 * std::log10(pb)) + " dB), " + std::to_string(held) + " dB while held, "
                  + std::to_string(after - held) + " dB after the release; settings saved and reloaded");
        }
        // multi-sample zones, through the engine: each note plays its zone's sample from that zone's root; overlapping zones take turns
        {
            auto wavOf = [&](double hz, const char* name) { const juce::File w = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(name); w.deleteFile(); juce::AudioBuffer<float> b(1, 48000);
                for (int i = 0; i < 48000; i++) b.setSample(0, i, 0.4f * (float)std::sin(i * 2 * 3.14159265358979 * hz / 48000)); juce::WavAudioFormat f;
                auto wr = us::makeWriter(f, std::make_unique<juce::FileOutputStream>(w), 48000, 1, 16); wr->writeFromAudioSampleBuffer(b, 0, 48000); return w; };
            const juce::File w1 = wavOf(220, "undae-zone-a3.wav"), w2 = wavOf(880, "undae-zone-a5.wav");
            ui::Doc dz(eng, *bank); dz.setProject(ui::blankProject()); const std::string id1 = dz.importAudio(w1), id2 = dz.importAudio(w2); w1.deleteFile(); w2.deleteFile();
            auto renderZones = [&](std::vector<us::Zone> zones, std::vector<std::pair<int, double>> notes) {
                us::Project zp = dz.p; zp.bpm = 120; auto zc = ui::newChannel(zp, "sample", "Multi", id1); zc.bus = "master"; zc.gate = true; zc.zones = zones; zp.channels.push_back(zc);
                for (auto& [nt, at] : notes) { us::Note q; q.id = us::newId(); q.t = at * us::PPQ; q.d = us::PPQ; q.n = nt; q.v = 1; zp.patterns[0].notes[zc.id].push_back(q); }
                us::Engine ez(bank); ez.prepare(48000, 512); ez.setProject(std::make_shared<us::Project>(zp));
                auto b = ez.render(us::Mode::Pattern, zp.patterns[0].id, 1, 48000); std::vector<float> m((size_t)b.getNumSamples()); for (int i = 0; i < b.getNumSamples(); i++) m[(size_t)i] = b.getSample(0, i); return m; };
            auto toneWin = [&](const std::vector<float>& v, double hz, double a, double b) { double c = 0, s2 = 0; const int i0 = (int)(a * sr), i1 = (int)(b * sr); for (int i = i0; i < i1; i++) { const double w = 2 * 3.14159265358979 * hz * i / sr; c += v[(size_t)i] * std::cos(w); s2 += v[(size_t)i] * std::sin(w); } return 2 * std::sqrt(c * c + s2 * s2) / (i1 - i0); };
            us::Zone za; za.sample = id1; za.lo = 0; za.hi = 68; za.root = 57; us::Zone zb; zb.sample = id2; zb.lo = 69; zb.hi = 127; zb.root = 81;
            auto split = renderZones({ za, zb }, { { 60, 0 }, { 76, 2 } });   // C4 from the A3 sample, E5 from the A5 sample (beat 2 = 1 s at 120 BPM)
            const double c4 = toneWin(split, 261.63, 0.1, 0.4), e5 = toneWin(split, 659.26, 1.1, 1.4), wrong = toneWin(split, 659.26, 0.1, 0.4);
            us::Zone r1 = za, r2 = za; r1.hi = r2.hi = 127; r2.gain = -12;
            auto rr = renderZones({ r1, r2 }, { { 57, 0 }, { 57, 2 } });
            auto rmsWin = [&](const std::vector<float>& v, double a, double b) { double s2 = 0; for (int i = (int)(a * sr); i < (int)(b * sr); i++) s2 += (double)v[(size_t)i] * v[(size_t)i]; return 10 * std::log10(s2 / ((b - a) * sr) + 1e-30); };
            const double rrDiff = rmsWin(rr, 0.1, 0.4) - rmsWin(rr, 1.1, 1.4);
            check(!id1.empty() && !id2.empty() && c4 > 0.05 && e5 > 0.05 && wrong < 0.005 && std::abs(rrDiff - 12) < 1,
                  "zones: C4 plays from the low zone (" + std::to_string(c4) + "), E5 from the high zone (" + std::to_string(e5) + "), nothing crosses (" + std::to_string(wrong) + "); two overlapping zones take turns ("
                  + std::to_string(rrDiff) + " dB apart, 12 set)");
        }
        // physical models: the plucked string is in tune and dies away at the set rate; the struck bar rings at its mode ratios
        {
            auto renderNote = [&](const us::SynthP& P, int note) { us::NoteOpts o; auto v = us::makeSynthVoice(P, note, 1.0, 0, 0.95, o, sr); std::vector<float> L((size_t)N, 0.f), R((size_t)N, 0.f);
                for (int off = 0; off < N; off += 512) v->render(L.data() + off, R.data() + off, nullptr, std::min(512, N - off), off / sr, sr); return L; };
            auto rmsWin = [&](const std::vector<float>& v, double a, double b) { double s2 = 0; for (int i = (int)(a * sr); i < (int)(b * sr); i++) s2 += (double)v[(size_t)i] * v[(size_t)i]; return 10 * std::log10(s2 / ((b - a) * sr) + 1e-30); };
            us::SynthP P = ui::defSynth(); P.osc[0].on = false; P.f1.on = false; P.aenv = { 0.001, 3, 1, 0.3 };
            P.pm = { "pluck", "bar", 2, 0.5, 0.2, 0.5, 0.9 }; auto pl = renderNote(P, 57);
            const double pp = us::pitchOf(std::vector<float>(pl.begin() + 4800, pl.begin() + 4800 + 4096), sr), drop = rmsWin(pl, 0.1, 0.2) - rmsWin(pl, 0.6, 0.7);
            P.pm.type = "mallet"; P.pm.mat = "bar"; auto ml = renderNote(P, 69);
            const double m1 = tone(ml, 440), m2 = tone(ml, 440 * 2.756), off = tone(ml, 1000);
            bool presetsOk = true; std::string badP;
            for (auto& [name, fn] : ui::synthPresets()) { us::SynthP x = ui::defSynth(); fn(x); if (x.pm.type == "off") continue; auto y = renderNote(x, 60); float pk = 0; bool fin = true; for (float v : y) { fin &= std::isfinite(v); pk = std::max(pk, std::abs(v)); } if (!fin || pk < 0.005f || pk > 2) { presetsOk = false; badP = name; } }
            check(std::abs(pp - 57) < 0.1 && drop > 8 && drop < 25 && m1 > 0.001 && m2 > 5 * off && presetsOk,
                  "physical models: the plucked string plays note " + std::to_string(pp) + " (57) and falls " + std::to_string(drop) + " dB in 0.5 s (2 s decay: 15 in theory); the struck bar rings at 1 and 2.756 x ("
                  + std::to_string(20 * std::log10(m2 / std::max(1e-9, off))) + " dB over the gap); the model presets play" + (badP.empty() ? "" : " (not " + badP + ")"));
        }
        // arpeggiator, through the engine: a held C major chord at 120 BPM plays C, E, G, C on the 1/16 grid; switched-off steps rest
        {
            auto renderArp = [&](us::ArpP arp) {
                us::Project ap = ui::blankProject(); ap.bpm = 120; auto ac = ui::newChannel(ap, "synth", "Arp"); ac.bus = "master"; ac.synth->osc[0].wave = "sine"; ac.synth->f1.on = false; ac.synth->aenv = { 0.002, 0.05, 1, 0.01 }; ac.arp = arp; ap.channels.push_back(ac);
                for (int nt : { 60, 64, 67 }) { us::Note q; q.id = us::newId(); q.t = 0; q.d = us::PPQ; q.n = nt; q.v = 1; ap.patterns[0].notes[ac.id].push_back(q); }
                us::Engine ea(bank); ea.prepare(48000, 512); ea.setProject(std::make_shared<us::Project>(ap));
                auto b = ea.render(us::Mode::Pattern, ap.patterns[0].id, 1, 48000); std::vector<float> m((size_t)b.getNumSamples()); for (int i = 0; i < b.getNumSamples(); i++) m[(size_t)i] = b.getSample(0, i); return m; };
            auto toneW = [&](const std::vector<float>& v, double hz, double a, double b) { double c = 0, s2 = 0; const int i0 = (int)(a * sr), i1 = (int)(b * sr); for (int i = i0; i < i1; i++) { const double w = 2 * 3.14159265358979 * hz * i / sr; c += v[(size_t)i] * std::cos(w); s2 += v[(size_t)i] * std::sin(w); } return 2 * std::sqrt(c * c + s2 * s2) / (i1 - i0); };
            us::ArpP A; A.on = true; A.mode = "up"; A.rate = "1/16"; A.gate = 0.5;
            auto up = renderArp(A); const double hz[] = { 261.63, 329.63, 392.0, 261.63 };
            bool order = true; std::string got;
            for (int k = 0; k < 4; k++) { const double a = k * 0.125 + 0.01, b = a + 0.045; int best = 0; double bv = 0; for (int j = 0; j < 3; j++) { const double v = toneW(up, hz[j], a, b); if (v > bv) { bv = v; best = j; } } got += "CEG"[best]; order &= std::abs(hz[best] - hz[k]) < 1 && bv > 0.02; }
            const double between = toneW(up, 261.63, 0.08, 0.12) + toneW(up, 329.63, 0.08, 0.12);   // gate 0.5: silent in the second half of a step
            A.steps = 0x5; A.len = 4; auto rest = renderArp(A);   // steps 1 and 3 on, 2 and 4 off
            double on1 = 0, off2 = 0; for (double h : { 261.63, 329.63, 392.0 }) { on1 += toneW(rest, h, 0.01, 0.055); off2 += toneW(rest, h, 0.135, 0.18); }
            check(order && between < 0.01 && on1 > 0.02 && off2 < on1 * 0.02, "arpeggiator: a held C major chord plays " + got + " on the 1/16 grid at 120 BPM, silent between notes at gate 50 %; a switched-off step rests ("
                  + std::to_string(20 * std::log10(std::max(1e-9, off2) / on1)) + " dB)");
        }
        // the whole synth preset bank: unique names, and every patch sounds (audible, finite, not clipping hard)
        {
            std::set<std::string> names; bool unique = true, allOk = true; std::string bad; int count = 0;
            for (auto& [name, fn] : ui::synthPresets()) {
                unique &= names.insert(name).second; count++;
                us::SynthP x = ui::defSynth(); fn(x); us::NoteOpts o; auto v = us::makeSynthVoice(x, 60, 0.9, 0, 0.6, o, sr); std::vector<float> L((size_t)N, 0.f), R((size_t)N, 0.f);
                for (int off = 0; off < N; off += 512) v->render(L.data() + off, R.data() + off, nullptr, std::min(512, N - off), off / sr, sr);
                float pk = 0; bool fin = true; for (int i = 0; i < N; i++) { fin &= std::isfinite(L[(size_t)i]) && std::isfinite(R[(size_t)i]); pk = std::max({ pk, std::abs(L[(size_t)i]), std::abs(R[(size_t)i]) }); }
                if (!fin || pk < 0.003f || pk > 2.5f) { allOk = false; bad += std::string(name) + " (" + std::to_string(pk) + ") "; }
            }
            check(unique && allOk && count >= 40, "synth presets: " + std::to_string(count) + " patches, names unique, every one sounds" + (bad.empty() ? "" : ": not " + bad));
        }
        // the Studio Kit: every sound present and normalised; the kick sits low, the hats high, and kit names do not repeat
        {
            int n2 = 0; bool okAll = true; std::set<std::string> nm; bool uniq = true;
            for (auto& sm : bank->kit()) { uniq &= nm.insert(sm->name).second; if (sm->group != "Studio Kit") continue; n2++; float pk = 0; for (float v : sm->ch[0]) pk = std::max(pk, std::abs(v)); okAll &= std::abs(pk - 0.9f) < 0.01f && sm->seconds() > 0.05; }
            auto band = [&](const std::string& id, double lo, double hi) {   // share of the energy between lo and hi Hz (FFT of the first 8192 samples)
                auto sm = bank->get(id); juce::dsp::FFT f(13); std::vector<float> x(16384, 0.f); for (size_t i = 0; i < 8192 && i < sm->ch[0].size(); i++) x[i] = sm->ch[0][i];
                f.performFrequencyOnlyForwardTransform(x.data()); double in = 0, all = 0; for (int b = 1; b < 4096; b++) { const double hz = b * sm->sr / 8192, e = (double)x[(size_t)b] * x[(size_t)b]; all += e; if (hz >= lo && hz < hi) in += e; } return in / std::max(1e-12, all); };
            const double kickLow = band("kit2:kick", 30, 200), hatHigh = band("kit2:hat", 5000, 20000), snareMid = band("kit2:snare", 150, 8000);
            check(n2 == 14 && okAll && uniq && kickLow > 0.7 && hatHigh > 0.7 && snareMid > 0.7, "Studio Kit: " + std::to_string(n2) + " sounds, normalised, names unique across kits; kick energy below 200 Hz "
                  + std::to_string((int)(kickLow * 100)) + " %, closed hat above 5 kHz " + std::to_string((int)(hatHigh * 100)) + " %");
        }
        // second round of effects
        {
            auto ps = ui::newFx("pitchshift"); ps.p.set("semi", 12.0); auto po = runFx(ps, sine(440, 0.4));
            const double up880 = tone(po, 880), left440 = tone(po, 440);
            auto db = ui::newFx("doubler"); auto dbo = runSt(db, sine(440, 0.4), sine(440, 0.4)); double lr = 0, ll = 0, rr = 0; for (int i = N / 2; i < N; i++) { lr += (double)dbo.first[(size_t)i] * dbo.second[(size_t)i]; ll += (double)dbo.first[(size_t)i] * dbo.first[(size_t)i]; rr += (double)dbo.second[(size_t)i] * dbo.second[(size_t)i]; }
            const double dcorr = lr / std::sqrt(ll * rr + 1e-30), dlev = lvl(dbo.first) - lvl(sine(440, 0.4));
            auto af = ui::newFx("autofilter"); af.p.set("cut", 300.0); af.p.set("env", 4.0); af.p.set("q", 0.7);
            const double afLoud = lvl(runFx(af, sine(3000, 0.5))) - lvl(sine(3000, 0.5)), afQuiet = lvl(runFx(af, sine(3000, 0.005))) - lvl(sine(3000, 0.005));
            auto am = ui::newFx("amp"); am.p.set("drive", 0.8); auto ao2 = runFx(am, sine(200, 0.3)); bool afin = true; for (float v : ao2) afin &= std::isfinite(v);
            const double h3 = tone(ao2, 600) / std::max(1e-9, tone(ao2, 200)), amp10k = lvl(runFx(am, sine(10000, 0.05)));
            auto sg = ui::newFx("subgen"); sg.p.set("amount", 1.0); auto so2 = runFx(sg, sine(100, 0.5)); const double sub50 = tone(so2, 50);
            auto de = ui::newFx("dyneq"); de.p.set("f2", 1000.0); de.p.set("t2", -30.0); de.p.set("r2", -12.0);
            const double deLoud = lvl(runFx(de, sine(1000, 0.5))) - lvl(sine(1000, 0.5)), deQuiet = lvl(runFx(de, sine(1000, 0.003))) - lvl(sine(1000, 0.003)), deOther = lvl(runFx(de, sine(100, 0.5))) - lvl(sine(100, 0.5));
            check(up880 > 10 * left440 && dcorr < 0.95 && std::abs(dlev) < 6 && afLoud - afQuiet > 10 && afin && h3 > 0.05 && sub50 > 0.05 && deLoud < -9 && std::abs(deQuiet) < 0.5 && std::abs(deOther) < 0.5,
                  "second round: pitch shifter +12 puts 440 Hz at 880 (" + std::to_string(20 * std::log10(up880 / std::max(1e-9, left440))) + " dB over the original); doubler correlation " + std::to_string(dcorr)
                  + "; auto-filter opens " + std::to_string(afLoud - afQuiet) + " dB more for a loud tone; amp 3rd harmonic " + std::to_string(h3) + " (10 kHz through the cab " + std::to_string(amp10k)
                  + " dB); sub enhancer adds 50 Hz under 100 Hz (" + std::to_string(sub50) + "); dynamic EQ takes a loud 1 kHz down " + std::to_string(-deLoud) + " dB, leaves it quiet (" + std::to_string(deQuiet) + ") and 100 Hz alone");
        }
        // effect chains and the Mixer menu: every chain slot and setting exists, every built-in effect sits in a category, chains process a mix cleanly
        {
            bool known = true; std::string bad;
            for (auto& ch : ui::fxChains()) for (auto& [type, ps] : ch.slots) { auto* sp = ui::fxSpec(type); if (!sp) { known = false; bad += type + " "; continue; }
                for (auto& [k, v] : ps) { bool has = false; for (auto& q : sp->params) has |= std::string(q.k) == k; if (!has) { known = false; bad += type + "." + k + " "; } } }
            std::set<std::string> inCat; for (auto& [c, ts] : ui::fxCategories()) for (auto& t : ts) inCat.insert(t);
            bool allCat = true; for (auto& sp : ui::fxSpecs()) if (!inCat.count(sp.type)) { allCat = false; bad += std::string("uncategorised:") + sp.type + " "; }
            bool clean = true; std::vector<float> mixL((size_t)N), mixR((size_t)N); for (int i = 0; i < N; i++) { mixL[(size_t)i] = (float)(0.3 * std::sin(i * 0.0571) + 0.2 * std::sin(i * 0.31)); mixR[(size_t)i] = (float)(0.3 * std::sin(i * 0.0573) + 0.15 * std::sin(i * 0.47)); }
            for (auto& ch : ui::fxChains()) { auto L = mixL, R = mixR; for (auto& f : ui::chainSlots(ch)) { auto e = us::makeEffect(f.type); e->prepare(sr, 512); e->set(f); for (int o = 0; o < N; o += 512) e->process(L.data() + o, R.data() + o, std::min(512, N - o)); }
                float pk = 0; bool fin = true; for (int i = 0; i < N; i++) { fin &= std::isfinite(L[(size_t)i]) && std::isfinite(R[(size_t)i]); pk = std::max({ pk, std::abs(L[(size_t)i]), std::abs(R[(size_t)i]) }); }
                if (!fin || pk < 0.01f || pk > 4) { clean = false; bad += std::string(ch.name) + "(" + std::to_string(pk) + ") "; } }
            check(known && allCat && clean, std::to_string(ui::fxChains().size()) + " effect chains: every slot and setting exists, every effect is in a Mixer category, each chain processes a mix cleanly" + (bad.empty() ? "" : ": " + bad));
        }
        // IR reverb: an imported impulse (a click, then an echo at half level 0.1 s later) is what a click comes out as
        {
            const juce::File iw = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("undae-ir-test.wav"); iw.deleteFile();
            { juce::AudioBuffer<float> b(1, 24000); b.clear(); b.setSample(0, 0, 0.8f); b.setSample(0, 4410, 0.4f); juce::WavAudioFormat f;
              auto wr = us::makeWriter(f, std::make_unique<juce::FileOutputStream>(iw), 44100, 1, 24); wr->writeFromAudioSampleBuffer(b, 0, 24000); }
            ui::Doc di(eng, *bank); di.setProject(ui::blankProject()); const std::string irId = di.importAudio(iw); iw.deleteFile();
            auto ir = ui::newFx("irverb"); ir.p.set("ir", juce::String(irId)); ir.p.set("mix", 1.0);
            auto e = us::makeEffect("irverb"); e->samples = bank.get(); e->prepare(sr, 512); e->set(ir);
            std::vector<float> L((size_t)N, 0.f), R((size_t)N, 0.f); L[0] = R[0] = 1;
            for (int o = 0; o < N; o += 512) e->process(L.data() + o, R.data() + o, std::min(512, N - o));
            int p1 = 0, p2 = 2000; for (int i = 0; i < 2000; i++) if (std::abs(L[(size_t)i]) > std::abs(L[(size_t)p1])) p1 = i; for (int i = 2000; i < 12000; i++) if (std::abs(L[(size_t)i]) > std::abs(L[(size_t)p2])) p2 = i;
            const double ratio = std::abs(L[(size_t)p2]) / std::max(1e-9f, std::abs(L[(size_t)p1]));
            check(!irId.empty() && p1 == 512 && std::abs((p2 - p1) - 4800) <= 2 && std::abs(ratio - 0.5) < 0.05, "IR reverb: a click through an imported impulse comes out at " + std::to_string(p1)
                  + " samples (the convolver latency, 512) with its echo " + std::to_string(p2 - p1) + " samples later (0.1 s at 48 kHz: 4800) at " + std::to_string(ratio) + " of the level (0.5)");
        }
        // beat slicer: four hits (200, 400, 800, 1600 Hz bursts every 0.25 s) are found, and key root + 2 plays only the third
        {
            auto loop = std::make_shared<us::Sample>(); loop->sr = 48000; loop->ch.assign(1, std::vector<float>(48000, 0.f));
            const double fq[] = { 200, 400, 800, 1600 };
            for (int h = 0; h < 4; h++) for (int i = 0; i < 9000; i++) loop->ch[0][(size_t)(h * 12000 + i)] = (float)(0.7 * std::exp(-i / 2500.0) * std::sin(2 * 3.14159265358979 * fq[h] * i / 48000));
            auto on = us::detectOnsets(loop->ch[0], 48000, 0.5);
            bool found = on.size() == 4; for (size_t k = 1; k < on.size() && k < 4; k++) found &= std::abs(on[k] - k * 0.25) < 0.003;
            us::Channel C; C.root = 60; C.slice = true; C.slices = on.size() == 4 ? on : std::vector<double>{ 0, 0.25, 0.5, 0.75 };
            us::NoteOpts o; auto v = us::makeSampleVoice(loop, C, 62, 1.0, 0, 1.0, o, sr); std::vector<float> L((size_t)N, 0.f), R((size_t)N, 0.f), M((size_t)N, 0.f);   // a mono sample voice writes M
            for (int off = 0; off < N; off += 512) v->render(L.data() + off, R.data() + off, M.data() + off, std::min(512, N - off), off / sr, sr);
            std::vector<float> m((size_t)N); for (int i = 0; i < N; i++) m[(size_t)i] = L[(size_t)i] + R[(size_t)i] + M[(size_t)i];
            auto toneW = [&](double hz, double a, double b) { double c = 0, s2 = 0; for (int i = (int)(a * sr); i < (int)(b * sr); i++) { const double w = 2 * 3.14159265358979 * hz * i / sr; c += m[(size_t)i] * std::cos(w); s2 += m[(size_t)i] * std::sin(w); } return 2 * std::sqrt(c * c + s2 * s2) / ((b - a) * sr); };
            const double third = toneW(800, 0.0, 0.1), first = toneW(200, 0.0, 0.1), after = toneW(1600, 0.26, 0.4);
            const bool outOfRange = !us::makeSampleVoice(loop, C, 64, 1.0, 0, 1.0, o, sr) && !us::makeSampleVoice(loop, C, 59, 1.0, 0, 1.0, o, sr);
            juce::String ons; for (double t : on) ons << juce::String(t, 3) << " ";
            check(found && third > 0.05 && first < third * 0.02 && after < third * 0.02 && outOfRange, "beat slicer: hits found at " + ons.toStdString() + "s (0, 0.25, 0.5, 0.75); key root+2 plays only the third slice (800 Hz "
                  + std::to_string(third) + ", 200 Hz " + std::to_string(first) + ") and stops at its end; keys outside the slices stay silent");
        }
        // third round: trance gate, multi-tap delay, utility, test tone
        {
            auto tg = ui::newFx("tgate"); tg.p.set("steps", (double)0x5555); tg.p.set("depth", 1.0); tg.p.set("rel", 0.002); tg.p.set("att", 0.001); auto to = runTempo(tg, sine(1000, 0.5), sine(1000, 0.5)).first;   // 1/16 at 120 BPM: 0.125 s steps, every other one on
            const double gOn = win(to, 0.51, 0.61), gOff = win(to, 0.635, 0.735);
            auto mt = ui::newFx("multitap"); mt.p.set("fb", 0.0); mt.p.set("mix", 1.0); mt.p.set("tone", 18000.0); std::vector<float> clk((size_t)N, 0.f); clk[0] = 1;
            auto mo = runTempo(mt, clk, clk); int taps = 0; for (double tt : { 0.25, 0.5, 0.75, 1.0 }) { const int c0 = (int)(tt * sr); if (c0 + 4 < N) { float pk = 0; for (int i = c0 - 4; i <= c0 + 4; i++) pk = std::max(pk, std::abs(mo.first[(size_t)i]) + std::abs(mo.second[(size_t)i])); taps += pk > 0.05f; } }
            const double tap1L = std::abs(mo.first[12000]), tap1R = std::abs(mo.second[12000]);
            auto ut = ui::newFx("utility"); ut.p.set("phase", juce::String("flip left")); ut.p.set("gain", -6.0); auto uo = runSt(ut, sine(300, 0.5), sine(300, 0.5));
            const double sumUt = lvl(std::vector<float>(uo.first.size())) , flipOk = std::abs(uo.first[1000] + uo.second[1000]), gainUt = lvl(uo.second) - lvl(sine(300, 0.5)); (void)sumUt;
            auto tt = ui::newFx("testtone"); tt.p.set("freq", 440.0); tt.p.set("level", -20.0); std::vector<float> z((size_t)N, 0.f); auto tto = runFx(tt, z); const double ttLv = lvl(tto);
            check(gOn - gOff > 40 && taps == 3 && tap1L > 2 * tap1R && flipOk < 1e-6 && std::abs(gainUt + 6) < 0.05 && std::abs(ttLv + 23.01) < 0.1 && tone(tto, 440) > 0.05,
                  "trance gate opens and shuts " + std::to_string(gOn - gOff) + " dB on alternate 1/16 steps; multi-tap: " + std::to_string(taps) + " taps inside 1 s at 1/8, 1/4, 3/8 (the 1/2 tap falls at 1 s), the first on the left; "
                  "utility flips one side exactly and takes 6.0 dB; test tone at -20 dBFS peak reads " + std::to_string(ttLv) + " dB RMS (-23.0)");
        }
        // fourth round: spectral gate takes hiss down under a tone that keeps its level; ensemble spreads; scratch swoops
        {
            std::vector<float> hiss((size_t)(2 * N)); us::Rng hr(5); for (auto& v : hiss) v = (float)(hr.bi() * 0.01);
            std::vector<float> sigH = hiss; for (int i = N; i < 2 * N; i++) sigH[(size_t)i] += (float)(0.3 * std::sin(2 * 3.14159265358979 * 1000 * i / sr));   // 1 s hiss alone, then hiss + tone
            auto sgf = ui::newFx("specgate"); auto e = us::makeEffect("specgate"); e->prepare(sr, 512); e->set(sgf); auto Lg = sigH, Rg = sigH;
            for (int o = 0; o < 2 * N; o += 512) e->process(Lg.data() + o, Rg.data() + o, std::min(512, 2 * N - o));
            auto wv = [&](const std::vector<float>& v, double a, double b) { double s2 = 0; for (int i = (int)(a * sr); i < (int)(b * sr); i++) s2 += (double)v[(size_t)i] * v[(size_t)i]; return 10 * std::log10(s2 / ((b - a) * sr) + 1e-30); };
            const double hissDown = wv(Lg, 0.6, 0.95) - wv(hiss, 0.6 - 2048 / sr, 0.95 - 2048 / sr);
            std::vector<float> lagged((size_t)(2 * N), 0.f); for (int i = 1536; i < 2 * N; i++) lagged[(size_t)i] = sigH[(size_t)(i - 1536)];
            const double toneKept = wv(Lg, 1.5, 1.9) - wv(lagged, 1.5, 1.9);
            auto en = ui::newFx("ensemble"); auto eo = runSt(en, sine(440, 0.4), sine(440, 0.4)); double lr = 0, ll = 0, rr = 0; for (int i = N / 2; i < N; i++) { lr += (double)eo.first[(size_t)i] * eo.second[(size_t)i]; ll += (double)eo.first[(size_t)i] * eo.first[(size_t)i]; rr += (double)eo.second[(size_t)i] * eo.second[(size_t)i]; }
            const double ecorr = lr / std::sqrt(ll * rr + 1e-30);
            auto sc = ui::newFx("scratch"); auto so3 = runTempo(sc, sine(1000, 0.4), sine(1000, 0.4)).first; bool sfin = true; for (float v : so3) sfin &= std::isfinite(v);
            const double moved = 1 - tone(so3, 1000) / 0.4;   // the swinging read speed smears the steady tone across other pitches
            check(hissDown < -12 && std::abs(toneKept) < 1.5 && ecorr < 0.9 && sfin && moved > 0.3, "spectral gate takes steady hiss down " + std::to_string(-hissDown) + " dB and keeps a tone within " + std::to_string(toneKept)
                  + " dB; ensemble decorrelates the sides (" + std::to_string(ecorr) + "); scratch swoops the pitch (" + std::to_string((int)(moved * 100)) + " % of a steady tone moved off its pitch)");
        }
        // speed: every built-in effect, on 2 s of a stereo mix in 512-sample blocks, runs at least 20 times faster than real time
        {
            std::vector<float> mL((size_t)(2 * N)), mR((size_t)(2 * N)); for (int i = 0; i < 2 * N; i++) { mL[(size_t)i] = (float)(0.3 * std::sin(i * 0.0571) + 0.2 * std::sin(i * 0.31)); mR[(size_t)i] = (float)(0.3 * std::sin(i * 0.0573) + 0.15 * std::sin(i * 0.47)); }
            std::string slow, fastest, slowest; double worst = 1e9, best = 0;
            for (auto& sp : ui::fxSpecs()) {
                auto f = ui::newFx(sp.type); auto e = us::makeEffect(sp.type); e->samples = bank.get(); e->prepare(sr, 512); e->set(f); auto L = mL, R = mR;
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                for (int o = 0; o < 2 * N; o += 512) { e->bpm = 120; e->beat = o / sr * 2; e->process(L.data() + o, R.data() + o, std::min(512, 2 * N - o)); }
                const double x = 2000.0 / std::max(0.01, juce::Time::getMillisecondCounterHiRes() - t0);
                if (x < worst) { worst = x; slowest = sp.type; } if (x > best) { best = x; fastest = sp.type; } if (x < 20) slow += std::string(sp.type) + "(" + std::to_string((int)x) + "x) ";
            }
            check(slow.empty(), std::to_string(ui::fxSpecs().size()) + " built-in effects all run at least 20x real time (slowest " + slowest + " at " + std::to_string((int)worst) + "x)" + (slow.empty() ? "" : "; too slow: " + slow));
        }
    }
    std::cout << (fails ? "FAILED: " + std::to_string(fails) : std::string("all passed")) << "\n";
    return fails ? 1 : 0;
}
