#include "Engine.h"
#include <functional>
#include <map>
#include <set>

namespace us {

// --- sequencing ----------------------------------------------------------------------------------------------------
// Swing as a time warp over each pair of grid cells: the off-beat lands late and everything around it stretches to match.
double swingShift(const Project& P, double t) {
    if (!P.swing) return 0;
    const double g = P.swingDiv == 8 ? STEP * 2 : STEP, s = P.swing * g * 0.66, x = std::fmod(t, 2 * g);
    return x < g ? x * s / g : s * (2 * g - x) / g;
}

std::vector<Event> collect(const Project& P, Mode mode, const Pattern* pt, double a, double b, bool once) {
    std::vector<Event> out;
    const int BAR = P.bar();
    bool solo = false; for (auto& c : P.channels) solo |= c.solo;
    std::vector<const Channel*> chans; for (auto& c : P.channels) if (!(c.mute || (solo && !c.solo))) chans.push_back(&c);
    auto add = [&](const Pattern& p, double base, double limit, double lo) {
        const double pl = p.bars * BAR;
        for (auto* c : chans) {
            auto it = p.notes.find(c->id); if (it == p.notes.end()) continue;
            for (auto& n : it->second) {
                if (n.mute) continue;
                const double tk = base + n.t;
                if (tk >= a && tk >= lo && tk < b && tk < limit && n.t < pl)
                    out.push_back({ c, n.n, n.v, std::min(n.d, limit - tk), tk, swingShift(P, n.t), n.pan, n.fine, n.rel, n.cut, n.res, n.slide, n.hasCut, n.hasRes });
            }
        }
    };
    constexpr double NEG = -1e300, INF = 1e300;
    if (mode != Mode::Song) {
        if (!pt) return out;
        if (pt->loop.on && pt->loop.b > pt->loop.a) {   // Piano Roll loop: only that part, over and over
            const double W = pt->loop.b - pt->loop.a; Pattern sub = *pt; sub.notes.clear(); sub.bars = W / BAR;
            for (auto& [id, ns] : pt->notes) for (auto& n : ns) if (n.t >= pt->loop.a && n.t < pt->loop.b) { Note m = n; m.t = n.t - pt->loop.a; m.d = std::min(n.d, pt->loop.b - n.t); sub.notes[id].push_back(m); }
            for (double k = std::floor(a / W); k * W < b; k++) add(sub, k * W, (k + 1) * W, NEG);
            return out;
        }
        const double L = pt->bars * BAR; if (L <= 0) return out;
        for (double k = std::floor(a / L); k * L < b; k++) add(*pt, k * L, INF, NEG);
        return out;
    }
    const double L = (double)P.songBars() * BAR; if (L <= 0) return out;
    if (once && a >= L) return out;
    for (double k = std::floor(a / L); k * L < b && !(once && k > 0); k++)
        for (auto& cl : P.clips) {
            const Pattern* p = P.pattern(cl.pat); if (!p || cl.mute) continue;
            const double cs = k * L + std::round(cl.start * BAR), ce = cs + std::round(cl.len * BAR), pl = p->bars * BAR;
            if (pl <= 0 || ce <= a || cs >= b) continue;
            const double off = std::fmod(std::round(cl.off * BAR), pl);
            for (double m = std::max(0.0, std::floor((a - cs + off) / pl)); cs - off + m * pl < std::min(b, ce); m++) add(*p, cs - off + m * pl, ce, cs);
        }
    return out;
}

// --- time stretch (WSOLA) ------------------------------------------------------------------------------------------
std::shared_ptr<Sample> stretchSample(const Sample& src, double ratio) {
    auto out = std::make_shared<Sample>(); out->id = src.id; out->name = src.name; out->sr = src.sr;
    const size_t nIn = src.length(), chs = src.ch.size(); const int N = src.sr > 60000 ? 4096 : 2048, H = N / 2, D = N / 4;
    const size_t nOut = std::max<size_t>(1, (size_t)std::floor(nIn * ratio));
    out->ch.assign(chs, std::vector<float>(nOut, 0.f));
    if (nIn < (size_t)N * 2) { for (size_t c = 0; c < chs; c++) for (size_t i = 0; i < nOut; i++) out->ch[c][i] = src.ch[c][std::min(nIn - 1, (size_t)std::floor(i / ratio))]; return out; }
    std::vector<float> mono = src.ch[0]; if (chs > 1) for (size_t i = 0; i < nIn; i++) mono[i] = (mono[i] + src.ch[1][i]) * 0.5f;
    std::vector<float> win(N); for (int i = 0; i < N; i++) win[i] = (float)(0.5 - 0.5 * std::cos(2 * PI * i / N));
    const size_t frames = (nOut + H - 1) / H; const long last = (long)nIn - N; std::vector<long> pos(frames, 0);
    for (size_t k = 1, prev = 0; k < frames; k++) {
        const long ideal = std::min(last, (long)std::llround(k * H / ratio)), want = (long)prev + H; long best = ideal;
        if (want <= last) {
            const long lo = std::max(0L, ideal - D), hi = std::min(last, ideal + D); double bs = -1e300;
            for (long p = lo; p <= hi; p += 2) { double s = 0; for (int i = 0; i < H; i += 8) s += mono[p + i] * mono[want + i]; if (s > bs) { bs = s; best = p; } }
        }
        pos[k] = best; prev = (size_t)best;
    }
    for (size_t c = 0; c < chs; c++) {
        const auto& s = src.ch[c]; auto& d = out->ch[c];
        for (size_t k = 0; k < frames; k++) { const size_t o = k * H; const long p = pos[k]; const size_t n = std::min<size_t>(N, nOut - o); for (size_t i = 0; i < n; i++) d[o + i] += s[p + i] * win[i]; }
        for (size_t i = 0; i < (size_t)H && i < nOut; i++) d[i] = s[i];
    }
    return out;
}

// --- plug-ins in the graph --------------------------------------------------------------------------------------------
// the slot as the hosts' JSON (a plug-in effect keeps it in its parameters)
static juce::var slotVar(const FxSlot& f) { auto* o = new juce::DynamicObject(); for (auto& kv : f.p) o->setProperty(kv.name, kv.value); return juce::var(o); }
// send parameter values the project has and the plug-in has not heard yet
static void pushParams(PluginInstance& pi, const juce::NamedValueSet& vals) {
    for (auto& kv : vals) { const auto k = kv.name.toString(); if (!k.startsWith("v") || !(kv.value.isDouble() || kv.value.isInt())) continue;
        const int id = k.substring(1).getIntValue(); const double v = (double)kv.value; const double last = pi.lastSent(id); if (std::isnan(last) || std::abs(last - v) > 1e-9) pi.param(id, v); }
}
class PluginFx : public Effect {
public:
    std::shared_ptr<PluginInstance> inst; int timeout = 8;
    void set(const FxSlot& f) override { if (inst && inst->ready()) pushParams(*inst, f.p); }
    void process(float* L, float* R, int n) override { if (inst && inst->ready()) inst->process(L, R, n, frame, timeout); }
};
// a note playing in a plug-in instrument: only its timing lives here
class PlugVoice : public Voice {
public:
    PlugVoice(std::shared_ptr<PluginInstance> p, int key, double vel, double t, double dur, double sr) : pi(std::move(p)), k(key), rate(sr) {
        note = key; t0 = t; pi->note(true, key, vel, fr(t));
        if (dur >= 0) { pi->note(false, key, 0, fr(t + dur)); end = t + dur; } else end = 1e30;
    }
    bool stereo() const override { return true; }
    void render(float*, float*, float*, int, double, double) override {}
    void release(double t) override { if (released) return; released = true; if (end > t) { pi->note(false, k, 0, fr(t)); end = t; } }
    void kill(double t) override { release(t); }
    void slide(int, double, double) override {}
    void setBend(double cents) override { const int v = juce::jlimit(0, 16383, (int)std::lround(8192 + cents / 200.0 * 8192)); pi->midi(0xE0, v & 127, v >> 7, 0); }
    void setWheel(double w) override { pi->midi(0xB0, 1, juce::jlimit(0, 127, (int)std::lround(w * 127)), 0); }
private:
    std::shared_ptr<PluginInstance> pi; int k; double rate;
    uint32_t fr(double t) const { return (uint32_t)std::llround(t * rate); }
};

// --- the graph -----------------------------------------------------------------------------------------------------
struct Engine::BusNode {
    const Bus* b = nullptr; bool isMaster = false; size_t index = 0;
    std::vector<std::unique_ptr<Effect>> fx; std::vector<FxSlot> slots;   // slots: live copies automation can change
    std::vector<float> L, R, tapL, tapR;
    double vol = 1, pan = 0;
    struct SendT { BusNode* to; double amt; }; std::vector<SendT> sends;
    std::vector<std::pair<Effect*, BusNode*>> sides;   // effects that listen to another bus
};
struct Engine::Strip {
    const Channel* c = nullptr; BusNode* bus = nullptr; std::shared_ptr<PluginInstance> plug;
    std::vector<std::unique_ptr<Voice>> voices; std::vector<bool> stolen; int last = -1;
    std::vector<float> L, R, M; double vol = 0.8, pan = 0;
};
struct Engine::ClipPlay { SampleRef s; BusNode* bus; double t0, off, dur, gain; double ratio; };

struct Engine::Graph {
    std::atomic<float>* peaks = nullptr; std::atomic<float>* waveLo = nullptr; std::atomic<float>* waveHi = nullptr; size_t npeaks = 0; bool offline = false; int plugTimeout = 8;
    std::function<std::shared_ptr<PluginInstance>(const std::string&, const juce::var&)> plugFor;   // a loaded instance for a slot (pool or fresh)
    std::shared_ptr<Project> P; double sr = 48000; int block = 128; SampleBank* bank = nullptr;
    double tickAt = 0, timeAt = 0; bool rolling = false;   // the transport: song tick `tickAt` was at engine time `timeAt` (tempo-aware effects)
    std::vector<std::unique_ptr<Strip>> strips; std::vector<std::unique_ptr<BusNode>> buses; std::vector<BusNode*> order; BusNode* master = nullptr;
    std::map<std::string, Strip*> byChan; std::map<std::string, BusNode*> byBus;
    std::vector<ClipPlay> clips; std::map<std::string, SampleRef> stretched;
    double masterPitch = 0; bool anyBusSolo = false, anyChSolo = false;
    const float* const* inPtr = nullptr; int inCount = 0, inOff = 0;   // the device input for this block (monitoring)
    struct Held { Strip* s; int note; Voice* v; bool sustained = false; }; std::vector<Held> held;

    static bool reaches(const Project& P, const std::string& from, const std::string& to, std::set<std::string>& seen) {
        if (from == to) return true;
        if (seen.count(from)) return false;
        seen.insert(from);
        const Bus* b = P.findBus(from); if (!b) return false;
        for (auto& s : b->sends) if (reaches(P, s.to, to, seen)) return true;
        return false;
    }

    void build() {
        const Project& p = *P;
        for (auto& b : p.buses) {
            auto n = std::make_unique<BusNode>(); n->b = &b; n->vol = b.vol; n->pan = b.pan; n->index = buses.size();
            n->L.assign(block, 0); n->R.assign(block, 0); n->tapL.assign(block, 0); n->tapR.assign(block, 0);
            for (auto& f : b.fx) {
                std::unique_ptr<Effect> e;
                if (f.type == "plugin") { auto pf = std::make_unique<PluginFx>(); pf->timeout = plugTimeout; if (plugFor) pf->inst = plugFor("fx:" + b.id + ":" + f.id, slotVar(f)); e = std::move(pf); }
                else e = makeEffect(f.type);
                e->prepare(sr, block); e->set(f); if (!offline) e->attach(f.id); n->fx.push_back(std::move(e)); n->slots.push_back(f);
            }
            byBus[b.id] = n.get(); buses.push_back(std::move(n));
        }
        master = buses[0].get(); master->isMaster = true;
        for (auto& n : buses) {
            for (auto& s : n->b->sends) {
                auto it = byBus.find(s.to); std::set<std::string> seen;
                if (it == byBus.end() || it->second == master || it->second == n.get() || reaches(p, s.to, n->b->id, seen)) continue;
                n->sends.push_back({ it->second, s.amt });
            }
            for (size_t i = 0; i < n->fx.size(); i++) { const std::string src = n->fx[i]->sideSource(); auto it = byBus.find(src); if (!src.empty() && it != byBus.end() && it->second != n.get()) n->sides.push_back({ n->fx[i].get(), it->second }); }
        }
        // processing order: a bus after everything that sends to it or feeds its side chain; the master last
        std::map<BusNode*, int> indeg; std::map<BusNode*, std::vector<BusNode*>> next;
        for (auto& n : buses) if (n.get() != master) indeg[n.get()];
        for (auto& n : buses) {
            if (n.get() == master) continue;
            for (auto& s : n->sends) if (s.to != master) { next[n.get()].push_back(s.to); indeg[s.to]++; }
            for (auto& [e, src] : n->sides) if (src != master) { next[src].push_back(n.get()); indeg[n.get()]++; }
        }
        std::vector<BusNode*> q; for (auto& [n, d] : indeg) if (!d) q.push_back(n);
        std::set<BusNode*> done;
        while (!q.empty()) { BusNode* n = q.back(); q.pop_back(); order.push_back(n); done.insert(n); for (auto* m : next[n]) if (--indeg[m] == 0) q.push_back(m); }
        for (auto& n : buses) if (n.get() != master && !done.count(n.get())) order.push_back(n.get());   // a loop: project order
        order.push_back(master);
        for (auto& c : p.channels) {
            auto s = std::make_unique<Strip>(); s->c = &c; s->vol = c.vol; s->pan = c.pan;
            auto it = byBus.find(c.bus); s->bus = it == byBus.end() ? master : it->second;
            s->L.assign(block, 0); s->R.assign(block, 0); s->M.assign(block, 0);
            if (c.type == "plugin" && plugFor && c.plug.isObject()) s->plug = plugFor("ch:" + c.id, c.plug);
            byChan[c.id] = s.get(); strips.push_back(std::move(s));
        }
        masterPitch = p.masterPitch;
        for (auto& a : p.audio) clipBuf(a);   // time-stretch now, not on the audio thread
        for (auto& b : p.buses) anyBusSolo |= b.solo;
        for (auto& c : p.channels) anyChSolo |= c.solo;
    }

    int activeCount(Strip& s, double t) { int k = 0; for (size_t i = 0; i < s.voices.size(); i++) if (!s.stolen[i] && s.voices[i]->end > t) k++; return k; }
    void killOldest(Strip& s, double t) { for (size_t i = 0; i < s.voices.size(); i++) if (!s.stolen[i] && s.voices[i]->end > t) { s.voices[i]->kill(t); s.stolen[i] = true; return; } }
    void killActive(Strip& s, double t) { for (size_t i = 0; i < s.voices.size(); i++) { auto& v = s.voices[i]; if (v->t0 <= t && v->end > t) { v->kill(t); s.stolen[i] = true; } } }

    // Graph.trigger in the browser version. dur < 0: held until released.
    Voice* trigger(Strip& s, int note, double vel, double t, double dur, const Event* e, bool slide, double liveBend = 0, double wheel = 0) {
        const Channel& c = *s.c;
        for (size_t i = 0; i < s.voices.size();) { if (s.voices[i]->end < t) { s.voices.erase(s.voices.begin() + i); s.stolen.erase(s.stolen.begin() + i); } else i++; }
        if (c.choke) for (auto& o : strips) if (o.get() != &s && o->c->choke == c.choke) killActive(*o, t);
        if (slide) {
            bool any = false;
            for (size_t i = 0; i < s.voices.size(); i++) { auto& v = s.voices[i]; if (!s.stolen[i] && v->t0 < t && v->end > t && !v->released) { v->slide(note, t, dur > 0 ? dur : 0.05); any = true; } }
            if (any) return nullptr;
        }
        NoteOpts o; o.bpm = P->bpm; o.wheel = wheel; o.bend = masterPitch * 100 + (e ? e->fine : 0) + liveBend * bendRange(c) * 100; if (e) { o.pan = e->pan; o.rel = e->rel; o.cut = e->cut; o.res = e->res; }
        std::unique_ptr<Voice> v;
        if (c.type == "synth" && c.synth) {
            const SynthP& Sp = *c.synth;
            if (Sp.mode != "poly") killActive(s, t);
            else { const int mx = std::min(Sp.voices, 16); while (activeCount(s, t) >= mx && mx > 0) killOldest(s, t); }
            o.glideFrom = s.last; s.last = note;
            v = makeSynthVoice(Sp, note, vel, t, dur, o, sr);
        } else if (c.type == "plugin") {   // the note goes to the host stamped with its frame; the release likewise
            if (!s.plug || !s.plug->ready()) return nullptr;
            v = std::make_unique<PlugVoice>(s.plug, juce::jlimit(0, 127, note + (int)std::round(masterPitch)), vel, t, dur, sr);
        } else if (c.type == "sample") {
            if (c.cut) killActive(s, t);
            while (activeCount(s, t) >= 24) killOldest(s, t);
            v = makeSampleVoice(bank->get(c.sample), c, note, vel, t, dur, o, sr);
        }
        if (!v) return nullptr;
        Voice* raw = v.get(); s.voices.push_back(std::move(v)); s.stolen.push_back(false);
        return raw;
    }

    static double bendRange(const Channel& c) { return c.type == "synth" && c.synth ? c.synth->bendRange : 2.0; }
    bool alive(Strip* s, Voice* v) const { for (auto& x : s->voices) if (x.get() == v) return true; return false; }
    void killAll(double t) { for (auto& s : strips) { for (auto& v : s->voices) v->kill(t); if (s->plug) s->plug->panic(); } held.clear(); clips.clear(); }

    // seconds-based clip playback (Graph.playClip): 4 ms fades at both ends
    void addClip(const AudioClip& a, SampleRef buf, double t, double off, double dur) {
        if (!buf || dur <= 0.005) return;
        auto it = byBus.find(a.bus); clips.push_back({ buf, it == byBus.end() ? master : it->second, t, off, dur, a.gain, 1 });
    }
    SampleRef clipBuf(const AudioClip& a) {
        SampleRef s = bank->get(a.sample); if (!s) return nullptr;
        const double r = ratio(a); if (std::abs(r - 1) < 0.002) return s;
        const std::string key = a.sample + ":" + std::to_string(r);
        auto it = stretched.find(key); if (it != stretched.end()) return it->second;
        return stretched[key] = stretchSample(*s, r);
    }
    double ratio(const AudioClip& a) const { return !a.stretch || !a.bpm ? 1.0 : clampd(a.bpm / P->bpm, 0.25, 4); }
    double clipDur(const AudioClip& a) { SampleRef s = bank->get(a.sample); return s ? std::min(a.len * P->bar() * P->secondsPerTick(), (s->seconds() - a.off) * ratio(a)) : 0; }

    // song automation at tick position `tick` (song mode only)
    void applyAuto(double tick) {
        for (auto& l : P->autos) {
            if (l.pts.empty()) continue;
            double v;
            if (tick <= l.pts[0].t) v = l.pts[0].v;
            else { v = l.pts.back().v; for (size_t i = 1; i < l.pts.size(); i++) if (tick < l.pts[i].t) { const auto& a = l.pts[i - 1]; const auto& b = l.pts[i]; v = a.v + (b.v - a.v) * (tick - a.t) / (b.t - a.t ? b.t - a.t : 1); break; } }
            juce::StringArray parts; parts.addTokens(juce::String(l.target), ":", ""); if (parts.size() < 2) continue;
            const std::string kind = parts[0].toStdString(), id = parts[1].toStdString(), key = parts.size() > 2 ? parts[2].toStdString() : "";
            if (kind == "master") masterPitch = v;
            else if (kind == "bus") { auto it = byBus.find(id); if (it == byBus.end() || it->second->b->mute) continue; (key == "pan" ? it->second->pan : it->second->vol) = v; }
            else if (kind == "ch") { auto it = byChan.find(id); if (it == byChan.end() || it->second->c->mute) continue; (key == "pan" ? it->second->pan : it->second->vol) = v; }
            else if (kind == "fx" && parts.size() > 3) {
                auto it = byBus.find(id); if (it == byBus.end()) continue; BusNode& n = *it->second;
                for (size_t i = 0; i < n.slots.size(); i++) if (n.slots[i].id == key && n.slots[i].type != "plugin") { n.slots[i].p.set(juce::Identifier(parts[3]), v); n.fx[i]->set(n.slots[i]); }
            }
        }
    }

    // one block (n <= block) at engine time t into out
    void render(float* outL, float* outR, int n, double t) {
        for (auto& b : buses) { std::fill_n(b->L.data(), n, 0.f); std::fill_n(b->R.data(), n, 0.f); }
        for (auto& sp : strips) {
            Strip& s = *sp;
            if (s.plug) {   // a plug-in instrument: its host renders the block
                if (!s.plug->ready()) continue;
                std::fill_n(s.L.data(), n, 0.f); std::fill_n(s.R.data(), n, 0.f);
                if (!s.plug->process(s.L.data(), s.R.data(), n, (uint32_t)std::llround(t * sr), plugTimeout)) continue;
                for (size_t i = 0; i < s.voices.size();) { if (s.voices[i]->end <= t) { s.voices.erase(s.voices.begin() + (long)i); s.stolen.erase(s.stolen.begin() + (long)i); } else i++; }
                const double g = s.c->mute || (anyChSolo && !s.c->solo) ? 0 : s.vol;
                for (int i = 0; i < n; i++) { float l = s.L[(size_t)i] * (float)g, r = s.R[(size_t)i] * (float)g; panStereo(l, r, s.pan); s.bus->L[(size_t)i] += l; s.bus->R[(size_t)i] += r; }
                continue;
            }
            if (s.voices.empty()) continue;
            std::fill_n(s.L.data(), n, 0.f); std::fill_n(s.R.data(), n, 0.f); std::fill_n(s.M.data(), n, 0.f);
            bool st = false, mono = false;
            for (auto& v : s.voices) { if (v->end <= t || v->t0 >= t + (double)n / sr) continue; v->render(s.L.data(), s.R.data(), s.M.data(), n, t, sr); (v->stereo() ? st : mono) = true; }
            for (size_t i = 0; i < s.voices.size();) { if (s.voices[i]->end <= t) { s.voices.erase(s.voices.begin() + i); s.stolen.erase(s.stolen.begin() + i); } else i++; }
            const double g = s.c->mute || (anyChSolo && !s.c->solo) ? 0 : s.vol;
            float* bl = s.bus->L.data(); float* br = s.bus->R.data();
            if (st) { for (int i = 0; i < n; i++) { float l = (s.L[i] + s.M[i]) * (float)g, r = (s.R[i] + s.M[i]) * (float)g; panStereo(l, r, s.pan); bl[i] += l; br[i] += r; } }
            else if (mono) { for (int i = 0; i < n; i++) { float l, r; panMono(s.M[i] * (float)g, s.pan, l, r); bl[i] += l; br[i] += r; } }
        }
        const double dt = 1.0 / sr;
        for (size_t k = 0; k < clips.size();) {
            ClipPlay& c = clips[k];
            if (t >= c.t0 + c.dur) { clips.erase(clips.begin() + k); continue; }
            const size_t len = c.s->length(); const bool st = c.s->ch.size() > 1;
            for (int i = 0; i < n; i++) {
                const double tt = t + i * dt, tl = tt - c.t0; if (tl < 0 || tl >= c.dur) continue;
                double g = c.gain; if (tl < 0.004) g *= tl / 0.004; const double fe = std::max(0.005, c.dur - 0.006); if (tl > fe) g *= std::max(0.0, (c.dur - tl) / (c.dur - fe));
                const double p = (c.off + tl) * c.s->sr; const size_t j = (size_t)p; if (j + 1 >= len) continue; const double f = p - j;
                const float a = (float)(c.s->ch[0][j] + (c.s->ch[0][j + 1] - c.s->ch[0][j]) * f), b = st ? (float)(c.s->ch[1][j] + (c.s->ch[1][j + 1] - c.s->ch[1][j]) * f) : a;
                c.bus->L[i] += (float)(a * g); c.bus->R[i] += (float)(b * g);
            }
            k++;
        }
        if (inPtr && inCount > 0) for (auto& bp : buses) {   // monitored inputs: input "n" = device channel n (with n+1 when stereo)
            const Bus& B = *bp->b; if (!B.mon || B.input.empty()) continue; const int ch = juce::String(B.input).getIntValue() - 1; if (ch < 0 || ch >= inCount || !inPtr[ch]) continue;
            const float* a = inPtr[ch] + inOff; const float* b2 = B.stereo && ch + 1 < inCount && inPtr[ch + 1] ? inPtr[ch + 1] + inOff : a;
            for (int i = 0; i < n; i++) { bp->L[i] += a[i]; bp->R[i] += b2[i]; }
        }
        for (BusNode* bn : order) {
            BusNode& b = *bn; const Bus& B = *b.b; float* L = b.L.data(); float* R = b.R.data();
            const double w = B.mono ? 0 : B.width, inv = B.inv ? -1 : 1, d = (1 + w) / 2, x = (1 - w) / 2;
            if (inv != 1 || w != 1) for (int i = 0; i < n; i++) { const double l = L[i] * inv, r = R[i] * inv; L[i] = (float)(d * l + x * r); R[i] = (float)(x * l + d * r); }
            for (auto& [e, src] : b.sides) e->side(src->tapL.data(), src->tapR.data(), n);
            for (size_t i = 0; i < b.fx.size(); i++) if (b.slots[i].on) { auto& fx = *b.fx[i]; fx.frame = (uint32_t)std::llround(t * sr); fx.bpm = P->bpm; fx.beat = rolling ? (tickAt + (t - timeAt) / P->secondsPerTick()) / PPQ : -1; fx.process(L, R, n); }
            std::copy_n(L, n, b.tapL.data()); std::copy_n(R, n, b.tapR.data());
            const double out = B.mute || (anyBusSolo && !B.solo && B.kind == "insert") ? 0 : 1, g = b.vol * out;
            for (int i = 0; i < n; i++) { float l = L[i], r = R[i]; panStereo(l, r, b.pan); L[i] = l * (float)g; R[i] = r * (float)g; }
            if (peaks && b.index < npeaks) { float pk = 0, lo = 0, hi = 0; for (int i = 0; i < n; i++) { pk = std::max({ pk, std::abs(L[i]), std::abs(R[i]) }); const float m = (L[i] + R[i]) * 0.5f; lo = std::min(lo, m); hi = std::max(hi, m); } peaks[b.index] = pk;
                if (waveLo) { waveLo[b.index] = std::min(waveLo[b.index].load(), lo); waveHi[b.index] = std::max(waveHi[b.index].load(), hi); } }
            if (b.isMaster) { for (int i = 0; i < n; i++) { outL[i] = L[i]; outR[i] = R[i]; } continue; }
            if (B.toMaster) for (int i = 0; i < n; i++) { master->L[i] += L[i]; master->R[i] += R[i]; }
            for (auto& s : b.sends) for (int i = 0; i < n; i++) { s.to->L[i] += (float)(L[i] * s.amt); s.to->R[i] += (float)(R[i] * s.amt); }
        }
    }
    int voices() const { int k = 0; for (auto& s : strips) k += (int)s->voices.size(); return k; }
    // nothing here can make a sound: no notes, clips, held keys, monitored inputs or plug-ins (a plug-in may sound on its own)
    bool canRest() const {
        if (!clips.empty() || !held.empty()) return false;
        for (auto& s : strips) if (s->plug || !s->voices.empty()) return false;
        for (auto& b : buses) { if (b->b->mon && !b->b->input.empty()) return false; for (auto& f : b->slots) if (f.type == "plugin") return false; }
        return true;
    }
    // same structure, new values: point at the new snapshot (Engine::update decided the structure did not change)
    void sync(std::shared_ptr<Project> np) {
        auto keep = P; P = std::move(np); const Project& p = *P;   // keep: the old snapshot lives until every pointer has moved over
        for (size_t i = 0; i < strips.size() && i < p.channels.size(); i++) { strips[i]->c = &p.channels[i]; strips[i]->vol = p.channels[i].vol; strips[i]->pan = p.channels[i].pan; byChan[p.channels[i].id] = strips[i].get();
            if (strips[i]->plug && strips[i]->plug->ready() && p.channels[i].plug.getDynamicObject()) pushParams(*strips[i]->plug, p.channels[i].plug.getDynamicObject()->getProperties()); }
        for (size_t i = 0; i < buses.size() && i < p.buses.size(); i++) {
            BusNode& n = *buses[i]; const Bus& b = p.buses[i]; n.b = &b; n.vol = b.vol; n.pan = b.pan;
            for (size_t k = 0; k < n.fx.size() && k < b.fx.size(); k++) { n.slots[k] = b.fx[k]; n.fx[k]->set(n.slots[k]); }
            for (auto& ns : n.sends) { const size_t ti = ns.to->index; if (ti < p.buses.size()) for (auto& s : b.sends) if (s.to == p.buses[ti].id) ns.amt = s.amt; }
        }
        anyBusSolo = anyChSolo = false; for (auto& b : p.buses) anyBusSolo |= b.solo; for (auto& c : p.channels) anyChSolo |= c.solo;
        masterPitch = p.masterPitch;
    }
};

// --- engine --------------------------------------------------------------------------------------------------------
Engine::Engine(std::shared_ptr<SampleBank> b) : bank(std::move(b)) {}
Engine::~Engine() = default;

std::unique_ptr<Engine::Graph> Engine::build(std::shared_ptr<Project> p, double sampleRate, int block) {
    auto g = std::make_unique<Graph>(); g->P = std::move(p); g->sr = sampleRate; g->block = block; g->bank = bank.get(); g->peaks = busPeaks.data(); g->waveLo = busLo.data(); g->waveHi = busHi.data(); g->npeaks = busPeaks.size();
    g->plugFor = [this](const std::string& key, const juce::var&) { return plugin(key); };
    g->build();
    return g;
}

void Engine::prepare(double sampleRate, int block) { sr = sampleRate; maxBlock = std::max(block, 128); if (proj) setProject(proj); }

void Engine::setProject(std::shared_ptr<Project> p) {
    proj = std::move(p); liveKey = structureKey(*proj); syncPlugins(*proj);
    auto g = build(proj, sr, maxBlock);
    std::lock_guard<std::mutex> lk(liveLock);
    live = std::move(g); playing = false; posTicks = 0;
    if (Graph* old = pendingGraph.exchange(nullptr)) delete old;
    pendingProj.store({});
}

std::string Engine::structureKey(const Project& p) {
    std::string k;
    for (auto& c : p.channels) { k += c.id + "/" + c.type + "/" + c.bus; if (c.type == "plugin") k += "/" + c.plug.getProperty("path", "").toString().toStdString() + "#" + c.plug.getProperty("pid", "").toString().toStdString(); k += ";"; }
    k += "|";
    for (auto& b : p.buses) { k += b.id + "/" + b.kind + ":"; for (auto& f : b.fx) k += f.id + "=" + f.type + (f.type == "duck" || f.type == "gate" || f.type == "vocoder" ? "<" + f.str("src", "") : "") + (f.type == "plugin" ? "@" + f.str("path", "") + "#" + f.str("pid", "") : "") + ","; k += ">"; for (auto& s : b.sends) k += s.to + "+"; k += ";"; }
    return k;
}

void Engine::update(std::shared_ptr<Project> p) {
    if (!proj) { setProject(std::move(p)); return; }
    proj = p; syncPlugins(*p);
    const std::string key = structureKey(*p);
    if (key == liveKey) { pendingProj.store(std::move(p)); return; }
    liveKey = key;
    Graph* g = build(std::move(p), sr, maxBlock).release();
    if (Graph* old = pendingGraph.exchange(g)) delete old;   // never picked up: replaced by this one
    pendingProj.store({});
}

void Engine::syncPlugins(const Project& p) {
    struct Want { juce::var slot; std::string sig; };
    std::map<std::string, Want> want;
    auto sigOf = [this](const juce::var& s) { return (s.getProperty("path", "").toString() + "|" + s.getProperty("pid", "").toString() + "|" + s.getProperty("fmt", "").toString() + "|" + s.getProperty("bits", "").toString() + "|" + juce::String(sr)).toStdString(); };
    for (auto& c : p.channels) if (c.type == "plugin" && c.plug.isObject() && c.plug.getProperty("path", "").toString().isNotEmpty()) want["ch:" + c.id] = { c.plug, sigOf(c.plug) };
    for (auto& b : p.buses) for (auto& f : b.fx) if (f.type == "plugin") { auto s = slotVar(f); if (s.getProperty("path", "").toString().isNotEmpty()) want["fx:" + b.id + ":" + f.id] = { s, sigOf(s) }; }
    std::lock_guard<std::mutex> l(poolLock);
    for (auto it = pool.begin(); it != pool.end();) { auto w = want.find(it->first); if (w == want.end() || w->second.sig != it->second.sig) it = pool.erase(it); else ++it; }
    for (auto& [key, w] : want) {
        if (pool.count(key)) continue;
        Pooled pl; pl.inst = std::make_shared<PluginInstance>(); pl.sig = w.sig; pl.status = std::make_shared<std::string>("Loading...");
        auto inst = pl.inst; auto status = pl.status; const juce::var slot = juce::JSON::parse(juce::JSON::toString(w.slot)); const double rate = sr;
        std::thread([inst, status, slot, rate] { std::string err; for (int i = 0; i < 1200 && HostSet::get().winPending() && !endpointFor(slot).ok(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            *status = inst->load(slot, rate, err) ? std::string() : err; }).detach();
        pool[key] = pl;
    }
}

void Engine::reloadPlugins() { { std::lock_guard<std::mutex> l(poolLock); pool.clear(); } liveKey.clear(); if (proj) update(proj); }

void Engine::collectGarbage() { if (Graph* old = retiredGraph.exchange(nullptr)) delete old; }

juce::AudioBuffer<float> Engine::render(Mode m, const std::string& pid, int loops, double rate, std::function<void(double)> progress) {
    if (!proj) throw std::runtime_error("No project loaded");
    const Project& P = *proj; const Pattern* pt = P.pattern(pid); if (!pt && !P.patterns.empty()) pt = &P.patterns[0];
    const double spt = P.secondsPerTick(), L = (m == Mode::Song ? (double)P.songBars() * P.bar() : (pt ? pt->bars * P.bar() : 0)) * loops;
    if (L <= 0) throw std::runtime_error("Nothing to render: the song is empty");
    const int B = 128; globalRng() = Rng(20261116);   // exports are repeatable: the same project renders the same file
    auto g = std::make_unique<Graph>(); g->P = proj; g->sr = rate; g->block = B; g->bank = bank.get(); g->offline = true; g->plugTimeout = 10000;
    std::vector<std::string> plugErrors;
    g->plugFor = [&](const std::string&, const juce::var& slot) {   // exports load their own copy of each plug-in and wait for it
        auto pi = std::make_shared<PluginInstance>(); std::string err;
        for (int i = 0; i < 1200 && HostSet::get().winPending() && !endpointFor(slot).ok(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!pi->load(slot, rate, err)) { plugErrors.push_back(slot.getProperty("name", "plug-in").toString().toStdString() + " (" + err + ")"); return std::shared_ptr<PluginInstance>(); }
        return pi;
    };
    g->build(); g->rolling = true;   // an export plays from tick 0 at time 0
    const int frames = (int)std::ceil((L * spt + 2.5) * rate);
    auto evs = collect(P, m, pt, 0, L); std::sort(evs.begin(), evs.end(), [](const Event& a, const Event& b) { return a.tick + a.sw < b.tick + b.sw; });
    if (m == Mode::Song) for (auto& a : P.audio) if (!a.mute) g->addClip(a, g->clipBuf(a), std::round(a.start * P.bar()) * spt, a.off * g->ratio(a), g->clipDur(a));
    for (auto& b : g->buses) for (auto& e : b->fx) e->offlineReady();
    juce::AudioBuffer<float> out(2, frames); out.clear();
    size_t ei = 0;
    for (int pos = 0; pos < frames; pos += B) {
        const int n = std::min(B, frames - pos); const double t = pos / rate, te = (pos + n) / rate;
        if (m == Mode::Song && !P.autos.empty()) g->applyAuto(t / spt);
        for (; ei < evs.size() && (evs[ei].tick + evs[ei].sw) * spt < te; ei++) {
            const Event& e = evs[ei]; auto it = g->byChan.find(e.c->id); if (it == g->byChan.end()) continue;
            g->trigger(*it->second, e.n, e.v, (e.tick + e.sw) * spt, e.d * spt, &e, e.slide);
        }
        g->render(out.getWritePointer(0, pos), out.getWritePointer(1, pos), n, t);
        if (progress && (pos / B) % 512 == 0) progress((double)pos / frames);
    }
    return out;
}

void Engine::play(Mode m, const std::string& pid, double fromTick, double countInBars) {
    std::lock_guard<std::mutex> lk(liveLock);
    mode = m; patId = pid; startFrom = fromTick; countIn = countInBars; startReq = true; stopReq = false; playing = true;
}
void Engine::panic() { std::lock_guard<std::mutex> lk(queueLock); liveQueue.push_back({ {}, 0, 0, 6 }); }
// the metronome: a short sine blip, 1500 Hz on the bar, 1000 Hz on other beats (the browser version's click)
void Engine::renderClicks(float* L, float* R, int n, double t) {
    for (size_t k = 0; k < clicks.size();) {
        const Click& c = clicks[k]; const double f = c.accent ? 1500 : 1000, amp = c.accent ? 0.45 : 0.3, dur = 0.05;
        if (t > c.t0 + dur) { clicks.erase(clicks.begin() + (long)k); continue; }
        for (int i = 0; i < n; i++) { const double tl = t + i / sr - c.t0; if (tl < 0 || tl > dur) continue; const float v = (float)(amp * std::sin(2 * juce::MathConstants<double>::pi * f * tl) * std::exp(-tl * 70)); L[i] += v; R[i] += v; }
        k++;
    }
}
void Engine::stop() { std::lock_guard<std::mutex> lk(liveLock); stopReq = true; playing = false; }
void Engine::noteOn(const std::string& ch, int note, double vel) { std::lock_guard<std::mutex> lk(queueLock); liveQueue.push_back({ ch, note, vel, 1 }); }
void Engine::noteOff(const std::string& ch, int note) { std::lock_guard<std::mutex> lk(queueLock); liveQueue.push_back({ ch, note, 0, 0 }); }
void Engine::pitchBend(double v) { std::lock_guard<std::mutex> lk(queueLock); liveQueue.push_back({ {}, 0, v, 2 }); }
void Engine::modWheel(double v) { std::lock_guard<std::mutex> lk(queueLock); liveQueue.push_back({ {}, 0, v, 3 }); }
void Engine::sustain(bool down) { std::lock_guard<std::mutex> lk(queueLock); liveQueue.push_back({ {}, 0, down ? 1.0 : 0.0, 4 }); }
void Engine::previewSample(const std::string& id) { std::lock_guard<std::mutex> lk(queueLock); liveQueue.push_back({ id, 0, 1, 5 }); }
std::vector<Engine::RecNote> Engine::takeRecorded() { std::lock_guard<std::mutex> lk(queueLock); std::vector<RecNote> r; r.swap(recDone); return r; }

void Engine::process(float* L, float* R, int n) {
    std::unique_lock<std::mutex> lk(liveLock, std::try_to_lock);
    if (!lk.owns_lock() || !live) { std::fill_n(L, n, 0.f); std::fill_n(R, n, 0.f); return; }
    if (Graph* ng = pendingGraph.exchange(nullptr)) {   // a structural edit: the new graph takes over; the old one is freed by the message thread
        Graph* old = live.release(); live.reset(ng);
        if (Graph* prev = retiredGraph.exchange(old)) delete prev;   // (only if the message thread has fallen behind)
    }
    if (auto np = pendingProj.exchange({})) live->sync(std::move(np));
    if (patChanged) { std::unique_lock<std::mutex> q(queueLock, std::try_to_lock); if (q.owns_lock()) { patId = pendingPat; patChanged = false; } }
    Graph& g = *live; const Project& P = *g.P; const double spt = P.secondsPerTick();
    const Pattern* pt = P.pattern(patId); if (!pt && !P.patterns.empty()) pt = &P.patterns[0];
    if (stopReq) { stopReq = false; g.killAll(tNow); clicks.clear(); countingIn = false; }
    if (startReq) {
        startReq = false; g.killAll(tNow); aTick = startFrom; nextTick = startFrom; clicks.clear();
        const double beatSec = P.beat() * spt, pre = countIn > 0 ? countIn * P.bar() * spt : 0; aTime = tNow + pre;
        if (pre > 0) { countingIn = true; for (int k = 0; k < (int)std::round(countIn * P.sigN); k++) clicks.push_back({ tNow + k * beatSec, k % P.sigN == 0 }); }
        nextBeat = std::ceil(startFrom / P.beat() - 1e-9) * P.beat();
        if (mode == Mode::Song) for (auto& a : P.audio) { const double cs = std::round(a.start * P.bar()); if (!a.mute && cs < startFrom && cs + a.len * P.bar() > startFrom) { const double skip = (startFrom - cs) * spt; g.addClip(a, g.clipBuf(a), tNow, a.off * g.ratio(a) + skip, g.clipDur(a) - skip); } }
    }
    { std::unique_lock<std::mutex> q(queueLock, std::try_to_lock);
      if (q.owns_lock()) {
        // where a played note lands in the pattern (recording)
        const double loopL = pt ? (pt->loop.on && pt->loop.b > pt->loop.a ? pt->loop.b - pt->loop.a : pt->bars * P.bar()) : P.bar();
        const double recTick = std::fmod(aTick + (tNow - aTime) / spt, loopL > 0 ? loopL : P.bar());
        for (auto& e : liveQueue) {
            if (e.kind == 2) { liveBend = e.vel; for (auto& h : g.held) if (g.alive(h.s, h.v)) h.v->setBend(g.masterPitch * 100 + liveBend * Graph::bendRange(*h.s->c) * 100); continue; }
            if (e.kind == 3) { liveWheel = e.vel; for (auto& h : g.held) if (g.alive(h.s, h.v)) h.v->setWheel(liveWheel); continue; }
            if (e.kind == 4) { sustainDown = e.vel > 0.5; if (!sustainDown) for (size_t i = 0; i < g.held.size();) { if (g.held[i].sustained) { if (g.alive(g.held[i].s, g.held[i].v)) g.held[i].v->release(tNow); g.held.erase(g.held.begin() + (long)i); } else i++; } continue; }
            if (e.kind == 5) { if (auto s = bank->get(e.ch)) g.clips.push_back({ s, g.master, tNow, 0, s->seconds(), 0.8, 1 }); continue; }
            if (e.kind == 6) { g.killAll(tNow); clicks.clear(); continue; }
            auto it = g.byChan.find(e.ch); if (it == g.byChan.end()) continue;
            if (e.kind == 1) {
                Voice* v = g.trigger(*it->second, e.note, e.vel, tNow, -1, nullptr, false, liveBend, liveWheel); if (v) g.held.push_back({ it->second, e.note, v });
                if (recording && playing) recOpen[{ e.ch, e.note }] = { recTick, e.vel };
            } else {
                for (size_t i = 0; i < g.held.size();) {
                    auto& h = g.held[i];
                    if (h.s == it->second && h.note == e.note && !h.sustained) { if (sustainDown) { h.sustained = true; i++; continue; } if (g.alive(h.s, h.v)) h.v->release(tNow); g.held.erase(g.held.begin() + (long)i); }
                    else i++;
                }
                auto ro = recOpen.find({ e.ch, e.note });
                if (ro != recOpen.end()) { double d = recTick - ro->second.first; if (d <= 0) d += loopL; recDone.push_back({ e.ch, e.note, ro->second.second, ro->second.first, std::max(6.0, d) }); recOpen.erase(ro); }
            }
        }
        liveQueue.clear();
        if (!playing && !recOpen.empty()) recOpen.clear();
      } }
    for (int off = 0; off < n; off += maxBlock) {
        const int m = std::min(maxBlock, n - off); const double te = tNow + (double)m / sr;
        if (playing && te <= aTime) { countingIn = true; posTicks = startFrom; }   // counting in
        else if (playing) {
            countingIn = false;
            const double to = aTick + (te - aTime) / spt;
            if (metronome) { const double loopL0 = mode == Mode::Song ? (double)P.songBars() * P.bar() : 0; (void)loopL0;
                for (; nextBeat < to; nextBeat += P.beat()) { const double at = aTime + (nextBeat - aTick) * spt; if (at >= tNow - 0.01) {
                    double lt = nextBeat; const double L0 = mode == Mode::Song ? (double)P.songBars() * P.bar() : (pt ? (pt->loop.on && pt->loop.b > pt->loop.a ? pt->loop.b - pt->loop.a : pt->bars * P.bar()) : P.bar());
                    if (L0 > 0) lt = std::fmod(lt, L0); clicks.push_back({ at, std::fmod(lt, (double)P.bar()) < 1 }); } } }
            else nextBeat = std::ceil(to / P.beat()) * P.beat();
            if (mode == Mode::Song && !P.autos.empty()) { const double Ls = (double)P.songBars() * P.bar(); g.applyAuto(Ls > 0 ? std::fmod(aTick + (tNow - aTime) / spt, Ls) : 0); }
            for (auto& e : collect(P, mode, pt, nextTick, to)) {
                auto it = g.byChan.find(e.c->id); if (it == g.byChan.end()) continue;
                g.trigger(*it->second, e.n, e.v, std::max(tNow, aTime + (e.tick + e.sw - aTick) * spt), e.d * spt, &e, e.slide);
            }
            if (mode == Mode::Song) {
                const double Ls = (double)P.songBars() * P.bar();
                if (Ls > 0) for (double k = std::floor(nextTick / Ls); k * Ls < to; k++) for (auto& a : P.audio) {
                    const double cs = k * Ls + std::round(a.start * P.bar());
                    if (!a.mute && cs >= nextTick && cs < to) g.addClip(a, g.clipBuf(a), aTime + (cs - aTick) * spt, a.off * g.ratio(a), g.clipDur(a));
                }
            }
            nextTick = to;
            const double loopL = mode == Mode::Song ? (double)P.songBars() * P.bar() : (pt ? (pt->loop.on && pt->loop.b > pt->loop.a ? pt->loop.b - pt->loop.a : pt->bars * P.bar()) : 0);
            const double now = aTick + (tNow - aTime) / spt; posTicks = loopL > 0 ? std::fmod(now, loopL) : now;
        }
        // stopped, a second of silence and nothing able to sound: skip the effects (a phone's battery) until a note, preview or play
        const bool restable = !playing && clicks.empty() && g.canRest();
        if (restable && quietFrames >= sr) { std::fill_n(L + off, m, 0.f); std::fill_n(R + off, m, 0.f); tNow = te; continue; }
        g.inPtr = inPtr; g.inCount = inCount; g.inOff = off; g.rolling = playing && !countingIn; g.tickAt = aTick; g.timeAt = aTime;
        g.render(L + off, R + off, m, tNow);
        if (!clicks.empty()) renderClicks(L + off, R + off, m, tNow);
        if (restable) { float pk = 0; for (int i = off; i < off + m; i++) pk = std::max({ pk, std::abs(L[i]), std::abs(R[i]) }); quietFrames = pk < 1e-6f ? quietFrames + m : 0; } else quietFrames = 0;
        tNow = te;
    }
    float pl = 0, pr = 0; for (int i = 0; i < n; i++) { pl = std::max(pl, std::abs(L[i])); pr = std::max(pr, std::abs(R[i])); }
    peaks[0] = pl; peaks[1] = pr; voices = g.voices();
}

}   // namespace us
