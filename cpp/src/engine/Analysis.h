// Audio analysis (app/js/audio/analysis.js): pitch tracking (YIN), melody and chord transcription, key detection
// (Krumhansl-Schmuckler), tempo estimate, and turning found notes into pattern notes.
#pragma once
#include <array>
#include <functional>
#include <string>
#include <vector>
#include "Project.h"
#include "Samples.h"

namespace us {

struct FoundNote { double s, e; int n; double v; };   // seconds
struct KeyResult { int root = 0; bool minor = false; double r = 0, conf = 0; std::string name, camelot; std::array<double, 12> chroma{}; std::vector<KeyResult> alts; };
using Progress = std::function<void(double)>;

std::vector<float> toMono(const Sample& s);
KeyResult detectKey(const std::vector<float>& x, double sr, Progress p = {});
KeyResult keyFromNotes(const std::vector<Note>& notes);
double detectBpm(const std::vector<float>& x, double sr);   // 0 when unsure
struct MelodyOpts { double gate = 0.08, minDur = 0.07, minHz = 38; };
std::vector<FoundNote> melodyNotes(const std::vector<float>& x, double sr, const MelodyOpts& o = {}, Progress p = {});
struct ChordOpts { double gate = 0.08; int maxPoly = 5; };
std::vector<FoundNote> chordNotes(const std::vector<float>& x, double sr, const ChordOpts& o = {}, Progress p = {});
// found notes to pattern notes at this tempo; q: grid in ticks (0 = free); align: start at the first note
std::vector<Note> toTicks(const std::vector<FoundNote>& notes, double bpm, int q, bool align);
const std::vector<std::pair<std::string, std::vector<int>>>& scales();
int snapToScale(int n, int root, const std::string& scale);
extern const char* const NOTE_NAMES[12];
double pitchOf(const std::vector<float>& x, double sr);
// hits in a sound (the beat slicer): start times in seconds, the first always 0; sens 0..1 (higher finds softer hits)
std::vector<double> detectOnsets(const std::vector<float>& x, double sr, double sens = 0.5, size_t maxHits = 64);   // live tuner: MIDI note (fractional) or -1

}   // namespace us
