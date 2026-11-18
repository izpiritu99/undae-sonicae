// Mixer effects (app/js/audio/fx.js): EQ, filter, compressor, limiter, reverb, delay, chorus, phaser, distortion,
// bitcrush and the peak controller ("duck"). Plug-in slots pass audio through until plug-in hosting is ported.
#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include "Dsp.h"
#include "Project.h"

namespace us {

// Live readings of a Meters slot, shared between the audio thread (writes) and the mixer panel (reads), found by slot id.
// No locks: a torn read of the sample ring only costs one frame of a display.
struct MeterTap {
    static constexpr int RING = 16384;
    std::array<float, RING> l{}, r{}; std::atomic<uint32_t> w{ 0 }; std::atomic<double> rate{ 48000 };
    std::atomic<float> momentary{ -200 }, shortTerm{ -200 }, integrated{ -200 }, truePeak{ -200 }, corr{ 0 };
    std::atomic<uint32_t> resetReq{ 0 };                    // the panel bumps it to restart integrated loudness and the peak hold
    void last(int n, std::vector<float>& L, std::vector<float>& R) const;   // the newest n samples, oldest first
    static std::shared_ptr<MeterTap> forSlot(const std::string& slotId);
};

class Effect {
public:
    virtual ~Effect() = default;
    virtual void prepare(double sr, int maxBlock) { this->sr = sr; (void)maxBlock; }
    virtual void set(const FxSlot& f) = 0;                  // read every parameter from the slot (cheap when unchanged)
    virtual void process(float* L, float* R, int n) = 0;    // in place
    virtual std::string sideSource() const { return {}; }   // a bus whose post-effects signal this one listens to
    virtual void side(const float*, const float*, int) {}   // that signal, before process() for the same block
    virtual void offlineReady() {}
    virtual void attach(const std::string& slotId) { (void)slotId; }   // live graphs only: the slot this effect sits in                          // offline renders: finish any background preparation first
    uint32_t frame = 0;                                     // where the coming block starts on the engine clock (plug-ins need it)
    double bpm = 120;                                       // the project tempo
    double beat = -1;                                       // song position of the block start in quarter notes; -1 when stopped
protected:
    double sr = 48000;
};

std::unique_ptr<Effect> makeEffect(const std::string& type);

// Web Audio's DynamicsCompressorNode, approximated: soft knee, attack / release in the dB domain, 6 ms look-ahead and
// the spec's automatic make-up gain ((1 / gain at 0 dBFS) ^ 0.6). Shared by the compressor and the limiter.
class Compressor {
public:
    double thr = -24, knee = 30, ratio = 12, att = 0.003, rel = 0.25;
    void prepare(double sr);
    void process(float* L, float* R, int n);
    double reductionDb() const { return gs; }
private:
    double sr = 48000, gs = 0; DelayLine dl, dr; size_t look = 288;
    // running maximum of the level over the look-ahead window (monotonic queue in fixed rings: no allocation on the audio thread)
    std::vector<double> qv; std::vector<int64_t> qi; size_t qh = 0, qt = 0; int64_t idx = 0;
    double curve(double xdb) const;
};

}   // namespace us
