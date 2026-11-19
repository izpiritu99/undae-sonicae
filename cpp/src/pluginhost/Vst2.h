// The VST2 binary interface, written from its public description (struct layout and opcode numbers), as in
// native/win/vsthost.c. No Steinberg SDK code is used.
#pragma once
#include <cstdint>

namespace vst2 {
struct AEffect;
using MasterFn = intptr_t (*)(AEffect*, int32_t op, int32_t index, intptr_t value, void* ptr, float opt);
struct AEffect {
    int32_t magic;   // 'VstP'
    intptr_t (*dispatcher)(AEffect*, int32_t op, int32_t index, intptr_t value, void* ptr, float opt);
    void (*process)(AEffect*, float** in, float** out, int32_t frames);   // old style: adds to the output
    void (*setParameter)(AEffect*, int32_t index, float value);
    float (*getParameter)(AEffect*, int32_t index);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities; float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect*, float** in, float** out, int32_t frames);
    void (*processDoubleReplacing)(AEffect*, double** in, double** out, int32_t frames);
    char future[56];
};
constexpr int32_t kMagic = 0x56737450;
enum : int32_t { effOpen = 0, effClose = 1, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8, effSetSampleRate = 10, effSetBlockSize = 11,
                 effMainsChanged = 12, effEditGetRect = 13, effEditOpen = 14, effEditClose = 15, effEditIdle = 19, effGetChunk = 23, effSetChunk = 24,
                 effProcessEvents = 25, effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48, effCanDo = 51, effStartProcess = 71, effStopProcess = 72 };
enum : int32_t { flagHasEditor = 1, flagCanReplacing = 1 << 4, flagProgramChunks = 1 << 5, flagIsSynth = 1 << 8 };
enum : int32_t { amAutomate = 0, amVersion = 1, amGetTime = 7, amSizeWindow = 15, amGetSampleRate = 16, amGetBlockSize = 17, amGetProcessLevel = 23,
                 amGetVendorString = 32, amGetProductString = 33, amGetVendorVersion = 34, amCanDo = 37, amGetLanguage = 38, amBeginEdit = 43, amEndEdit = 44 };
struct TimeInfo { double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos; int32_t sigNum, sigDen, smpteOffset, smpteRate, toNextClock, flags; };
struct MidiEvent { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; char midi[4]; char detune, offVelocity, r1, r2; };
constexpr int kMaxEvents = 256;
struct Events { int32_t numEvents; intptr_t reserved; MidiEvent* events[kMaxEvents]; };
struct ERect { int16_t top, left, bottom, right; };
}   // namespace vst2
