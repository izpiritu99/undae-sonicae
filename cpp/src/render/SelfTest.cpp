// undae-selftest <project.undae.json>: the editing path without a window. Live edits while playing, structural edits
// (graph swap), undo / redo, save and reload (unknown fields kept, identical render). Prints PASS / FAIL per check.
#include <iostream>
#include <juce_audio_formats/juce_audio_formats.h>
#include "app/Doc.h"
#include "engine/Analysis.h"
#include "engine/SoundDesign.h"

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
    std::cout << (fails ? "FAILED: " + std::to_string(fails) : std::string("all passed")) << "\n";
    return fails ? 1 : 0;
}
