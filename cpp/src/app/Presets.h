// Synth presets (app/js/presets.js): each starts from the default patch and changes it.
#pragma once
#include <functional>
#include <utility>
#include <vector>
#include "Doc.h"

namespace ui {

inline const std::vector<std::pair<const char*, std::function<void(us::SynthP&)>>>& synthPresets() {
    using S = us::SynthP;
    auto mod = [](S& s, size_t i, const char* src, const char* dst, double amt) { if (s.mod.size() <= i) s.mod.resize(8); s.mod[i] = { src, dst, amt }; };
    auto osc = [](bool on, const char* wave, double oct, double semi, double fine, double level, double pan = 0) { return us::OscP{ on, wave, "sine", oct, semi, fine, level, 0, pan }; };
    static const std::vector<std::pair<const char*, std::function<void(S&)>>> list = {
        { "Init Saw", [](S&) {} },
        { "Sub Bass", [](S& s) { s.osc[0].wave = "sawtooth"; s.sub = 0.7; s.mode = "mono"; s.glide = 0.04; s.f1.cutoff = 420; s.f1.q = 2; s.f1.env = 26; s.fenv = { 0.003, 0.22, 0.15, 0.15 }; s.aenv = { 0.004, 0.3, 0.75, 0.12 }; } },
        { "Soft Lead", [=](S& s) { s.osc[0].wave = "square"; s.osc[1] = osc(true, "sawtooth", 0, 0, 9, 0.45); s.f1.cutoff = 2600; s.f1.q = 3; s.f1.env = 10; s.f1.key = 0.4; s.aenv = { 0.012, 0.25, 0.7, 0.3 }; s.lfo[0].rate = 5.5; mod(s, 0, "lfo1", "pitch", 0.008); mod(s, 1, "wheel", "cutoff", 0.5); } },
        { "Wide Pad", [=](S& s) { s.uni = 5; s.detune = 22; s.spread = 0.85; s.osc[1] = osc(true, "sawtooth", 1, 0, -6, 0.3); s.f1.cutoff = 1500; s.f1.q = 1.5; s.f1.env = 12; s.fenv = { 1.2, 2, 0.6, 1.5 }; s.aenv = { 0.6, 1, 0.85, 1.6 }; s.lfo[1].rate = 0.18; mod(s, 0, "lfo2", "cutoff", 0.12); mod(s, 1, "lfo2", "pan", 0.3); } },
        { "Pluck", [=](S& s) { s.f1.cutoff = 500; s.f1.q = 2.5; s.f1.env = 44; s.fenv = { 0.002, 0.16, 0, 0.16 }; s.aenv = { 0.002, 0.32, 0, 0.25 }; s.osc[1] = osc(true, "square", 1, 0, 4, 0.25); mod(s, 0, "vel", "cutoff", 0.3); } },
        { "FM Bell", [=](S& s) { s.osc[0].wave = "sine"; s.osc[1] = osc(false, "sine", 1, 7, 0, 0.5); s.fm = 0.45; s.f1.on = false; s.aenv = { 0.002, 1.6, 0, 1.2 }; s.menv = { 0.002, 0.5, 0, 0.4 }; mod(s, 0, "menv", "fm", 0.5); } },
        { "Acid Line", [](S& s) { s.mode = "mono"; s.glide = 0.06; s.f1.cutoff = 260; s.f1.q = 13; s.f1.env = 42; s.f1.drive = 0.35; s.fenv = { 0.002, 0.2, 0.05, 0.1 }; s.aenv = { 0.003, 0.2, 0.8, 0.08 }; } },
        { "Organ", [=](S& s) { s.osc[0] = osc(true, "sine", 0, 0, 0, 0.7); s.osc[1] = osc(true, "sine", 1, 0, 0, 0.45); s.osc[2] = osc(true, "sine", 1, 7, 0, 0.3); s.f1.on = false; s.aenv = { 0.006, 0.1, 1, 0.06 }; s.lfo[0].rate = 6.2; mod(s, 0, "lfo1", "amp", 0.15); } },
        { "Noise Sweep", [=](S& s) { s.osc[0].on = false; s.noise = 0.9; s.f1.type = "bandpass"; s.f1.cutoff = 900; s.f1.q = 6; s.aenv = { 0.4, 1, 0.8, 1.2 }; s.lfo[1].rate = 0.25; mod(s, 0, "lfo2", "cutoff", 0.5); } },
        { "Reese Bass", [=](S& s) { s.osc[1] = osc(true, "sawtooth", 0, 0, 19, 0.75); s.uni = 3; s.detune = 18; s.spread = 0.6; s.mode = "mono"; s.glide = 0.05; s.drift = 6; s.f1.type = "lowpass24"; s.f1.cutoff = 620; s.f1.q = 2.2; s.f1.drive = 0.25; s.aenv = { 0.01, 0.3, 0.9, 0.18 }; s.lfo[1].rate = 0.14; mod(s, 0, "lfo2", "cutoff", 0.18); mod(s, 1, "macro1", "cutoff", 0.6); } },
        { "Fold Lead", [=](S& s) { s.osc[0].wave = "sine"; s.osc[0].wave2 = "triangle"; s.shaperType = "fold"; s.shaperAmt = 0.5; s.f1.cutoff = 5200; s.f1.q = 1.2; s.aenv = { 0.008, 0.3, 0.75, 0.25 }; s.menv = { 0.005, 0.45, 0.2, 0.3 }; s.lfo[0].rate = 5.2; s.lfo[0].fade = 0.4; mod(s, 0, "menv", "oscA", 0.5); mod(s, 1, "lfo1", "pitch", 0.006); mod(s, 2, "wheel", "morph", 1); } },
        { "Vocal Pad", [=](S& s) { s.osc[0].wave = "vocal"; s.osc[0].wave2 = "organ"; s.osc[0].morph = 0.3; s.osc[1] = osc(true, "softsaw", 1, 0, -7, 0.25, 0.3); s.uni = 3; s.detune = 16; s.spread = 0.8; s.f1.cutoff = 2400; s.f1.q = 1; s.f2.on = true; s.f2.type = "highpass"; s.f2.cutoff = 220; s.f2.q = 0.7; s.aenv = { 0.7, 1.2, 0.85, 1.8 }; s.lfo[1].rate = 0.11; s.lfo[1].wave = "sine"; mod(s, 0, "lfo2", "morph", 0.45); mod(s, 1, "rand", "pan", 0.4); } },
        { "Glass Keys", [=](S& s) { s.osc[0].wave = "bell"; s.osc[1] = osc(true, "sine", 1, 0, 3, 0.3); s.f1.on = false; s.aenv = { 0.002, 1.4, 0, 0.9 }; s.vel = 0.8; mod(s, 0, "vel", "oscB", 0.4); } },
        { "S&H Bleeps", [=](S& s) { s.osc[0].wave = "pulse25"; s.f1.type = "bandpass"; s.f1.cutoff = 1200; s.f1.q = 3.5; s.gain = 1.4; s.aenv = { 0.003, 0.2, 0.6, 0.15 }; s.lfo[2] = { "sh", 8, 0.25, 0 }; mod(s, 0, "lfo3", "cutoff", 0.55); mod(s, 1, "lfo3", "pan", 0.5); } },
        { "Ring Metal", [=](S& s) { s.osc[0].wave = "sine"; s.osc[0].level = 0.3; s.osc[1] = osc(false, "sine", 1, 6, 0, 0.5); s.ring = 0.8; s.f1.cutoff = 7000; s.aenv = { 0.002, 0.9, 0.1, 0.7 }; s.menv = { 0.002, 0.6, 0, 0.4 }; mod(s, 0, "menv", "ring", 0.4); } },
        // wavetable patches (original): the position moves with an LFO, an envelope or the mod wheel
        { "Vowel Pad", [=](S& s) { s.osc[0].wave = "wt:formant"; s.osc[0].morph = 0.3; s.uni = 3; s.detune = 10; s.spread = 0.8; s.f1.cutoff = 5000; s.aenv = { 0.35, 0.6, 0.8, 1.2 };
            s.lfo[1].rate = 0.25; mod(s, 0, "lfo2", "morph", 0.35); mod(s, 1, "wheel", "morph", 0.4); } },
        { "Sync Lead", [=](S& s) { s.osc[0].wave = "wt:sync"; s.osc[0].morph = 0.1; s.mode = "mono"; s.glide = 0.05; s.f1.cutoff = 7000; s.aenv = { 0.005, 0.2, 0.8, 0.25 };
            s.menv = { 0.002, 0.45, 0.15, 0.3 }; mod(s, 0, "menv", "morph", 0.75); mod(s, 1, "lfo1", "pitch", 0.006); } },
        { "Fold Bass", [=](S& s) { s.osc[0].wave = "wt:fold"; s.osc[0].morph = 0.05; s.mode = "mono"; s.sub = 0.5; s.f1.cutoff = 3000; s.aenv = { 0.003, 0.3, 0.7, 0.15 };
            s.menv = { 0.001, 0.25, 0.1, 0.2 }; mod(s, 0, "menv", "morph", 0.5); mod(s, 1, "vel", "morph", 0.3); } },
        { "Crystal Keys", [=](S& s) { s.osc[0].wave = "wt:glass"; s.osc[0].morph = 0.6; s.osc[1] = osc(true, "sine", 1, 0, 3, 0.25); s.f1.cutoff = 9000; s.aenv = { 0.003, 1.2, 0.2, 0.8 };
            s.menv = { 0.001, 0.9, 0, 0.5 }; mod(s, 0, "menv", "morph", -0.4); } },
        { "PWM Strings", [=](S& s) { s.osc[0].wave = "wt:pulse"; s.osc[0].morph = 0.4; s.uni = 4; s.detune = 16; s.spread = 0.9; s.f1.cutoff = 3500; s.aenv = { 0.4, 0.5, 0.85, 0.9 };
            s.lfo[0].rate = 0.6; mod(s, 0, "lfo1", "morph", 0.3); } },
        // physical-model patches (original)
        { "Nylon Pluck", [=](S& s) { s.osc[0].on = false; s.pm = { "pluck", "bar", 2.5, 0.45, 0.18, 0.4, 0.9 }; s.f1.cutoff = 9000; s.aenv = { 0.002, 3, 1, 0.4 }; } },
        { "Steel Pluck", [=](S& s) { s.osc[0].on = false; s.pm = { "pluck", "bar", 4, 0.85, 0.12, 0.8, 0.85 }; s.f1.cutoff = 14000; s.aenv = { 0.002, 4, 1, 0.6 }; } },
        { "Pluck Bass", [=](S& s) { s.osc[0].on = false; s.pm = { "pluck", "bar", 1.2, 0.3, 0.3, 0.6, 1 }; s.sub = 0.25; s.mode = "mono"; s.f1.cutoff = 2500; s.aenv = { 0.002, 1.5, 1, 0.12 }; } },
        { "Vibes", [=](S& s) { s.osc[0].on = false; s.pm = { "mallet", "bar", 3, 0.5, 0.2, 0.45, 0.9 }; s.f1.cutoff = 12000; s.aenv = { 0.002, 3, 1, 1 }; s.lfo[0].rate = 5; mod(s, 0, "lfo1", "amp", 0.25); } },
        { "Glass Mallet", [=](S& s) { s.osc[0].on = false; s.pm = { "mallet", "glass", 2.2, 0.75, 0.2, 0.7, 0.85 }; s.f1.cutoff = 15000; s.aenv = { 0.002, 2.5, 1, 1.2 }; } },
        { "Wood Block", [=](S& s) { s.osc[0].on = false; s.pm = { "mallet", "wood", 1, 0.6, 0.2, 0.85, 1 }; s.f1.cutoff = 12000; s.aenv = { 0.001, 0.5, 1, 0.2 }; } },
        { "Tower Bell", [=](S& s) { s.osc[0].on = false; s.pm = { "mallet", "bell", 6, 0.7, 0.2, 0.75, 0.8 }; s.f1.cutoff = 16000; s.aenv = { 0.002, 6, 1, 3 }; } },
        // a general bank (original patches, named for what they do)
        { "Brass Section", [=](S& s) { s.osc[1] = osc(true, "sawtooth", 0, 0, 7, 0.6); s.uni = 2; s.detune = 8; s.f1.cutoff = 900; s.f1.q = 1.2; s.f1.env = 30; s.fenv = { 0.06, 0.4, 0.5, 0.3 }; s.aenv = { 0.05, 0.3, 0.85, 0.25 }; mod(s, 0, "vel", "cutoff", 0.3); } },
        { "Poly Strings", [=](S& s) { s.uni = 4; s.detune = 12; s.spread = 0.9; s.f1.cutoff = 3200; s.f1.q = 0.8; s.f2.on = true; s.f2.type = "highpass"; s.f2.cutoff = 180; s.aenv = { 0.35, 0.5, 0.9, 0.9 }; s.lfo[0].rate = 4.8; s.lfo[0].fade = 0.8; mod(s, 0, "lfo1", "pitch", 0.004); } },
        { "Square Lead", [=](S& s) { s.osc[0].wave = "square"; s.osc[1] = osc(true, "square", 1, 0, 6, 0.3); s.mode = "mono"; s.glide = 0.03; s.f1.cutoff = 3800; s.f1.q = 2; s.aenv = { 0.004, 0.2, 0.8, 0.15 }; s.lfo[0].rate = 5.6; s.lfo[0].fade = 0.5; mod(s, 0, "lfo1", "pitch", 0.007); } },
        { "Chip Lead", [=](S& s) { s.osc[0].wave = "pulse25"; s.f1.on = false; s.aenv = { 0.001, 0.1, 0.85, 0.05 }; s.lfo[0] = { "square", 12, 0, 0 }; mod(s, 0, "lfo1", "pitch", 0.01); } },
        { "Electric Piano", [=](S& s) { s.osc[0].wave = "sine"; s.osc[1] = osc(false, "sine", 0, 0, 2, 0.5); s.fm = 0.18; s.f1.on = false; s.aenv = { 0.002, 1.8, 0.15, 0.5 }; s.menv = { 0.001, 0.35, 0.05, 0.3 }; mod(s, 0, "menv", "fm", 0.35); mod(s, 1, "vel", "fm", 0.2); s.lfo[0].rate = 4.5; mod(s, 2, "lfo1", "pan", 0.25); } },
        { "Wobble Bass", [=](S& s) { s.osc[1] = osc(true, "square", -1, 0, 0, 0.6); s.mode = "mono"; s.sub = 0.4; s.f1.type = "lowpass24"; s.f1.cutoff = 300; s.f1.q = 3; s.f1.drive = 0.3; s.aenv = { 0.005, 0.3, 0.9, 0.1 }; s.lfo[0] = { "sine", 2, 0.5, 0 }; mod(s, 0, "lfo1", "cutoff", 0.6); } },
        { "Stab Chord", [=](S& s) { s.uni = 3; s.detune = 10; s.osc[1] = osc(true, "sawtooth", 0, 7, 0, 0.5); s.f1.cutoff = 700; s.f1.q = 2; s.f1.env = 40; s.fenv = { 0.002, 0.18, 0, 0.12 }; s.aenv = { 0.002, 0.25, 0, 0.15 }; } },
        { "Warm Bass", [=](S& s) { s.osc[0].wave = "triangle"; s.osc[1] = osc(true, "sawtooth", 0, 0, 3, 0.35); s.mode = "mono"; s.glide = 0.02; s.sub = 0.5; s.f1.cutoff = 700; s.f1.q = 1.2; s.f1.env = 10; s.aenv = { 0.004, 0.4, 0.8, 0.12 }; } },
        { "Hollow Pad", [=](S& s) { s.osc[0].wave = "pulse10"; s.osc[1] = osc(true, "square", -1, 0, -5, 0.4); s.uni = 3; s.detune = 14; s.spread = 0.8; s.f1.cutoff = 2000; s.f2.on = true; s.f2.type = "highpass"; s.f2.cutoff = 350; s.aenv = { 0.8, 1, 0.8, 1.5 }; s.lfo[1].rate = 0.2; mod(s, 0, "lfo2", "cutoff", 0.15); } },
        { "Saw Stack", [=](S& s) { s.uni = 7; s.detune = 26; s.spread = 1; s.osc[1] = osc(true, "sawtooth", 1, 0, 0, 0.3); s.f1.cutoff = 6500; s.f1.q = 0.9; s.aenv = { 0.01, 0.4, 0.85, 0.4 }; } },
        { "Breathy Flute", [=](S& s) { s.osc[0].wave = "sine"; s.osc[1] = osc(true, "triangle", 1, 0, 0, 0.15); s.noise = 0.12; s.noiseTone = 0.7; s.f1.cutoff = 4500; s.aenv = { 0.08, 0.3, 0.85, 0.2 }; s.lfo[0].rate = 5; s.lfo[0].fade = 0.6; mod(s, 0, "lfo1", "pitch", 0.005); mod(s, 1, "lfo1", "amp", 0.08); } },
        { "Sweep Pad", [=](S& s) { s.uni = 4; s.detune = 18; s.spread = 0.9; s.f1.type = "lowpass24"; s.f1.cutoff = 300; s.f1.q = 3.5; s.f1.env = 48; s.fenv = { 3, 3, 0.5, 2 }; s.aenv = { 1.2, 1.5, 0.9, 2 }; } },
        { "Arp Pluck", [=](S& s) { s.osc[0].wave = "sawtooth"; s.osc[1] = osc(true, "pulse25", 1, 0, 5, 0.3); s.f1.cutoff = 900; s.f1.q = 4; s.f1.env = 38; s.fenv = { 0.001, 0.12, 0, 0.1 }; s.aenv = { 0.001, 0.22, 0, 0.18 }; mod(s, 0, "vel", "cutoff", 0.4); } },
        { "Deep Sine Bass", [=](S& s) { s.osc[0].wave = "sine"; s.sub = 0.3; s.mode = "mono"; s.glide = 0.03; s.f1.on = false; s.shaperType = "soft"; s.shaperAmt = 0.25; s.aenv = { 0.003, 0.5, 0.9, 0.12 }; } },
        { "Choir Swell", [=](S& s) { s.osc[0].wave = "vocal"; s.osc[1] = osc(true, "vocal", 0, 0, 9, 0.6); s.uni = 3; s.detune = 12; s.spread = 0.9; s.f1.cutoff = 3000; s.aenv = { 1, 1, 0.9, 1.8 }; s.lfo[0].rate = 4.4; s.lfo[0].fade = 1; mod(s, 0, "lfo1", "pitch", 0.004); } },
    };
    return list;
}

}   // namespace ui
