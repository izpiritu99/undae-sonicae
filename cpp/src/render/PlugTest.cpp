// undae-plugtest: start the plug-in hosts, scan, load plug-ins and run audio through them synchronously.
#include <iostream>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "engine/PluginHost.h"

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI init;
    if (argc > 3 && juce::String(argv[1]) == "--server") {   // the plug-ins of a computer sharing them, pipelined
        std::string name; const auto err = us::HostSet::get().useServer(argv[2], argv[3], &name);
        if (!err.empty()) { std::cout << "FAIL  server: " << err << "\n"; return 1; }
        std::cout << "server: " << name << "\n";
        auto list = us::scanPlugins(); juce::var slot; for (auto& p : list) if (p.getProperty("name", "").toString() == "Surge XT") slot = p;
        if (slot.isVoid()) { std::cout << "FAIL  Surge XT not offered by the server (" << list.size() << " plug-ins)\n"; return 1; }
        juce::var s(new juce::DynamicObject()); s.getDynamicObject()->setProperty("path", slot.getProperty("path", "")); s.getDynamicObject()->setProperty("pid", slot.getProperty("id", "")); s.getDynamicObject()->setProperty("fmt", slot.getProperty("format", ""));
        us::PluginInstance pi; std::string e; if (!pi.load(s, 48000, e)) { std::cout << "FAIL  load over the network: " << e << "\n"; return 1; }
        std::vector<float> L(128), R(128); float pk = 0; int firstSound = -1; uint32_t frame = 0;
        for (int b = 0; b < 600; b++) {   // paced like a sound card: one 128-frame block every 2.67 ms
            std::fill(L.begin(), L.end(), 0.f); std::fill(R.begin(), R.end(), 0.f);
            if (b == 5) pi.note(true, 60, 0.9, frame); if (b == 400) pi.note(false, 60, 0, frame);
            pi.process(L.data(), R.data(), 128, frame); frame += 128;
            float bp = 0; for (float v : L) bp = std::max(bp, std::abs(v)); if (bp > 0.01f && firstSound < 0) firstSound = b; pk = std::max(pk, bp);
            juce::Thread::sleep(2);
        }
        const bool ok = pk > 0.01f;
        std::cout << (ok ? "PASS  " : "FAIL  ") << "Surge XT over the plug-in server: " << list.size() << " plug-ins offered, peak " << pk << ", first sound " << (firstSound - 5) << " blocks after the note (" << (firstSound - 5) * 128 / 48.0 << " ms)\n";
        return ok ? 0 : 1;
    }
    auto bin = us::HostSet::findBinDir(); std::cout << "hosts in " << bin.getFullPathName() << "\n";
    us::HostSet::get().start(bin);
    for (int i = 0; i < 600 && us::HostSet::get().winPending(); i++) juce::Thread::sleep(100);
    std::cout << "CLAP host port " << us::HostSet::get().clap().port << ", Wine 64-bit " << us::HostSet::get().win(64).port << ", Wine 32-bit " << us::HostSet::get().win(32).port << "\n";
    auto list = us::scanPlugins();
    if (argc > 1 && juce::String(argv[1]) == "--scan-json") { juce::Array<juce::var> a; for (auto& p : list) a.add(p); std::cout << juce::JSON::toString(a, true) << "\n"; us::HostSet::get().stop(); return 0; }
    for (auto& p : list) std::cout << "  found: " << p.getProperty("name", "").toString() << " [" << p.getProperty("format", "").toString() << ((bool)p.getProperty("instrument", false) ? ", instrument" : "") << ((bool)p.getProperty("windows", false) ? ", Windows" : "") << ((int)p.getProperty("bits", 64) == 32 ? " 32-bit" : "") << "]\n";
    int fails = 0;
    auto tryOne = [&](const juce::String& want, int instWanted) {   // instWanted -1: as the scan says; "Name@au" picks a format
        const auto nm = want.upToFirstOccurrenceOf("@", false, false), fmt = want.fromFirstOccurrenceOf("@", false, false);
        juce::var slot; for (auto& p : list) if (p.getProperty("name", "").toString() == nm && (fmt.isEmpty() || p.getProperty("format", "").toString() == fmt)) slot = p;
        if (slot.isVoid()) { std::cout << "SKIP  " << want << " not found\n"; return; }
        const bool inst = instWanted < 0 ? (bool)slot.getProperty("instrument", false) : instWanted != 0;
        juce::var s(new juce::DynamicObject()); auto* o = s.getDynamicObject();
        o->setProperty("path", slot.getProperty("path", "")); o->setProperty("pid", slot.getProperty("id", "")); o->setProperty("fmt", slot.getProperty("format", ""));
        if ((bool)slot.getProperty("windows", false)) o->setProperty("windows", true); if ((int)slot.getProperty("bits", 64) == 32) o->setProperty("bits", 32);
        us::PluginInstance pi; std::string err; const double t0 = juce::Time::getMillisecondCounterHiRes();
        if (!pi.load(s, 48000, err)) { std::cout << "FAIL  " << want << ": " << err << "\n"; fails++; return; }
        const double loadMs = juce::Time::getMillisecondCounterHiRes() - t0;
        std::vector<float> L(128), R(128); float pk = 0; double worst = 0, total = 0; int missed = 0; uint32_t frame = 0;
        for (int b = 0; b < 400; b++) {
            for (int i = 0; i < 128; i++) { const float v = inst ? 0.f : 0.4f * (float)std::sin((frame + i) * 2 * 3.14159265 * 220 / 48000); L[(size_t)i] = R[(size_t)i] = v; }
            if (inst && b == 10) pi.note(true, 60, 0.9, frame + 5);
            if (inst && b == 300) pi.note(false, 60, 0, frame);
            const double a = juce::Time::getMillisecondCounterHiRes(); if (!pi.process(L.data(), R.data(), 128, frame, 50)) missed++; const double d = juce::Time::getMillisecondCounterHiRes() - a;
            if (b > 20) { worst = std::max(worst, d); total += d; }
            if (b > 20) for (int i = 0; i < 128; i++) pk = std::max(pk, std::abs(L[(size_t)i]));
            frame += 128;
        }
        const bool ok = pk > 0.01f && missed == 0;
        std::cout << (ok ? "PASS  " : "FAIL  ") << want << ": loaded in " << (int)loadMs << " ms, " << pi.info().params.size() << " parameters, peak " << pk << ", round trip avg " << total / 379 << " ms, worst " << worst << " ms (a block lasts 2.67 ms), missed " << missed << "\n";
        if (!ok) fails++;
    };
    // plug-in names on the command line are tested instead of the usual set (an instrument gets a note, an effect a tone)
    juce::StringArray names; for (int i = 1; i < argc; i++) if (!juce::String(argv[i]).startsWith("--")) names.add(argv[i]);
    if (names.isEmpty()) { tryOne("Undae Test Sine", 1); tryOne("Surge XT", 1); tryOne("Undae Test Gain", 0); tryOne("FB-3300", 1); tryOne("TAL BitCrusher - TAL", 0); }
    for (auto& n : names) tryOne(n, -1);
    us::HostSet::get().stop();
    std::cout << (fails ? "FAILED" : "all passed") << "\n"; return fails ? 1 : 0;
}
