// An audio file writer through JUCE 8's options API (the older createWriterFor overloads are deprecated).
#pragma once
#include <memory>
#include <juce_audio_formats/juce_audio_formats.h>

namespace us {

// A writer for `stream`, which it then owns; nullptr when the format cannot write these settings. 32 bits in WAV is
// IEEE float, as before ("automatic" sample format keeps the old behaviour).
inline std::unique_ptr<juce::AudioFormatWriter> makeWriter(juce::AudioFormat& format, std::unique_ptr<juce::OutputStream> stream, double sampleRate, int channels, int bits, int quality = 0) {
    const auto o = juce::AudioFormatWriterOptions{}.withSampleRate(sampleRate).withNumChannels(channels).withBitsPerSample(bits).withQualityOptionIndex(quality);
    return format.createWriterFor(stream, o);
}

}   // namespace us
