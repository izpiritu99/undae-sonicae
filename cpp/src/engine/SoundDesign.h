// Sound Design (app/js/ui/design.js): a one-shot built from a swept tone (with FM), filtered noise and / or an existing
// sample, then drive, a filter sweep, attack / fade, normalising and a chain of mixer effects with a tail.
#pragma once
#include <string>
#include <vector>
#include "Project.h"
#include "Samples.h"

namespace us {

struct DesignP {
    double len = 0.5;
    struct { bool on = true; std::string wave = "sine"; double f0 = 180, f1 = 50, sweep = 0.08, level = 0.9, dec = 0.3, fm = 0, ratio = 2; } tone;
    struct { bool on = false; std::string type = "highpass"; double cut = 4000, q = 0.7, level = 0.5, dec = 0.1; } noise;
    struct { bool on = false; std::string id; double start = 0, semi = 0, level = 0.8; bool rev = false; } smp;
    double drive = 0; struct { bool on = false; double cut = 8000, cut2 = 8000, q = 1; } filt;
    double att = 0.001, fade = 0.02, tail = 1; bool norm = true;
    std::vector<FxSlot> fx;
};
const std::vector<std::pair<const char*, DesignP>>& designPresets();
// 44.1 kHz; stereo when effects are on (they get `tail` seconds to ring out), mono otherwise
std::shared_ptr<Sample> renderDesign(const DesignP& d, const SampleBank& bank);
juce::MemoryBlock wavBytes(const Sample& s, int bits = 24);

}   // namespace us
