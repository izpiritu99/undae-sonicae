// MP3 export through LAME, loaded at run time (libmp3lame.so.0, LGPL): nothing to build against, and the export is simply
// unavailable on a system without it.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>

namespace us {
bool mp3Available();
// stereo buffer at 32, 44.1 or 48 kHz -> MP3 bytes (constant bitrate); throws when LAME is missing or fails
juce::MemoryBlock encodeMp3(const juce::AudioBuffer<float>& buf, double sampleRate, int kbps = 320);
}
