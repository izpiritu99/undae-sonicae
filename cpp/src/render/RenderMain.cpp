// undae-render: render a project to a WAV file without opening a window.
//   undae-render <project.undae.json> <out.wav> [--pattern <name or id>] [--loops N] [--rate 48000] [--bits 24]
// Without --pattern it renders the song (or, for a project with no song, its first pattern four times).
#include <iostream>
#include <juce_audio_formats/juce_audio_formats.h>
#include "../engine/Engine.h"

int main(int argc, char** argv) {
    if (argc < 3) { std::cerr << "usage: undae-render <project.undae.json> <out.wav> [--pattern NAME] [--loops N] [--rate HZ] [--bits 16|24|32]\n"; return 2; }
    std::string pat; int loops = 0; double rate = 48000; int bits = 24;
    for (int i = 3; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--pattern") pat = v; else if (k == "--loops") loops = std::stoi(v); else if (k == "--rate") rate = std::stod(v); else if (k == "--bits") bits = std::stoi(v);
    }
    try {
        auto t0 = juce::Time::getMillisecondCounterHiRes();
        auto proj = std::make_shared<us::Project>(us::loadProject(juce::File::getCurrentWorkingDirectory().getChildFile(argv[1])));
        auto bank = std::make_shared<us::SampleBank>(); bank->addFromProject(*proj);
        bool plugs = false; for (auto& c : proj->channels) plugs |= c.type == "plugin"; for (auto& b : proj->buses) for (auto& f : b.fx) plugs |= f.type == "plugin";
        if (plugs) us::HostSet::get().start(us::HostSet::findBinDir());   // plug-ins render through their hosts
        us::Engine eng(bank); eng.prepare(rate, 512); eng.setProject(proj);
        us::Mode mode = us::Mode::Song; std::string pid;
        if (!pat.empty() || proj->songBars() == 0) {
            mode = us::Mode::Pattern; if (loops == 0) loops = 4;
            for (auto& p : proj->patterns) if (p.name == pat || p.id == pat) pid = p.id;
            if (pid.empty() && !proj->patterns.empty()) pid = proj->patterns[0].id;
        }
        if (loops == 0) loops = 1;
        auto buf = eng.render(mode, pid, loops, rate);
        juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]); out.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(new juce::FileOutputStream(out), rate, 2, bits, {}, 0));
        if (!w) throw std::runtime_error("Cannot write " + out.getFullPathName().toStdString());
        w->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
        const double secs = buf.getNumSamples() / rate, took = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        std::cout << "Rendered \"" << proj->name << "\" (" << (mode == us::Mode::Song ? "song" : "pattern") << "): " << secs << " s of audio in " << took << " s ("
                  << (took > 0 ? secs / took : 0) << "x real time) -> " << out.getFullPathName() << "\n";
        us::HostSet::get().stop();
        return 0;
    } catch (const std::exception& e) { std::cerr << "error: " << e.what() << "\n"; return 1; }
}
