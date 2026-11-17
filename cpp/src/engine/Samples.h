// Sample bank: the built-in 808 kit and stock instruments (synthesised at start-up, as app/js/audio/drums.js does), plus
// samples carried inside project files (any format JUCE reads: WAV, AIFF, FLAC, Ogg, MP3).
#pragma once
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "AtomicShared.h"
#include "Project.h"

namespace us {

struct Sample {
    std::string id, name, group;
    double sr = 44100;
    std::vector<std::vector<float>> ch;   // one vector per channel
    size_t length() const { return ch.empty() ? 0 : ch[0].size(); }
    double seconds() const { return length() / sr; }
    std::shared_ptr<const Sample> reversed() const;   // cached
private:
    mutable std::shared_ptr<const Sample> rev;
};
using SampleRef = std::shared_ptr<const Sample>;

class SampleBank {
public:
    SampleBank();                                       // builds the kit
    SampleRef get(const std::string& id) const;
    void addFromProject(const Project& p);              // decode what the project file carries
    SampleRef add(const std::string& id, const std::string& name, const juce::MemoryBlock& bytes);   // decode one file; null if unreadable
    std::vector<SampleRef> kit() const;                 // the built-in sounds, in library order
    void put(SampleRef s);                              // add or replace a decoded sample
    std::vector<SampleRef> imported() const;            // samples from project files and imports
private:
    // copy on write: the audio thread reads a snapshot of the map while the UI thread adds to a new one
    using Map = std::map<std::string, SampleRef>;
    AtomicShared<const Map> map{ std::make_shared<const Map>() }; std::vector<std::string> order;
};

}   // namespace us
