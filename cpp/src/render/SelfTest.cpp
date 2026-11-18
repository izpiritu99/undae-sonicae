// undae-selftest <project.undae.json>: the editing path without a window. Live edits while playing, structural edits
// (graph swap), undo / redo, save and reload (unknown fields kept, identical render). Prints PASS / FAIL per check.
#include <iostream>
#include <juce_audio_formats/juce_audio_formats.h>
#include "app/Doc.h"
#include "engine/Analysis.h"
#include "engine/SoundDesign.h"
#include "engine/Effects.h"

static int fails = 0;
static void check(bool ok, const std::string& what) { std::cout << (ok ? "PASS  " : "FAIL  ") << what << "\n"; if (!ok) fails++; }

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
          juce::WavAudioFormat f; std::unique_ptr<juce::AudioFormatWriter> w(f.createWriterFor(new juce::FileOutputStream(wav), 48000, 1, 16, {}, 0)); w->writeFromAudioSampleBuffer(b, 0, 24000); }
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
    }
    std::cout << (fails ? "FAILED: " + std::to_string(fails) : std::string("all passed")) << "\n";
    return fails ? 1 : 0;
}
