#include "Project.h"
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace us {

static double num(const juce::var& o, const char* k, double def) {
    const juce::var v = o.getProperty(k, juce::var());
    if (v.isDouble() || v.isInt() || v.isInt64()) return (double)v;
    if (v.isBool()) return (bool)v ? 1 : 0;
    return def;
}
static bool flag(const juce::var& o, const char* k, bool def) { const juce::var v = o.getProperty(k, juce::var()); return v.isVoid() || v.isUndefined() ? def : (bool)v; }
static std::string str(const juce::var& o, const char* k, const std::string& def) { const juce::var v = o.getProperty(k, juce::var()); return v.isString() ? v.toString().toStdString() : def; }
static bool has(const juce::var& o, const char* k) { return o.hasProperty(k); }

double FxSlot::num(const char* k, double def) const { const juce::var* v = p.getVarPointer(k); return v && (v->isDouble() || v->isInt() || v->isInt64()) ? (double)*v : def; }
std::string FxSlot::str(const char* k, const std::string& def) const { const juce::var* v = p.getVarPointer(k); return v && v->isString() ? v->toString().toStdString() : def; }

static EnvP envOf(const juce::var& o, EnvP d) { if (!o.isObject()) return d; return { num(o, "a", d.a), num(o, "d", d.d), num(o, "s", d.s), num(o, "r", d.r) }; }

static SynthP synthOf(const juce::var& o) {
    SynthP s;
    if (auto* arr = o.getProperty("osc", juce::var()).getArray())
        for (int i = 0; i < 3 && i < arr->size(); i++) {
            const juce::var& x = (*arr)[i]; OscP& O = s.osc[i];
            O.on = flag(x, "on", i == 0); O.wave = str(x, "wave", O.wave); O.wave2 = str(x, "wave2", "sine");
            O.oct = num(x, "oct", 0); O.semi = num(x, "semi", 0); O.fine = num(x, "fine", 0); O.level = num(x, "level", 0.8); O.morph = num(x, "morph", 0); O.pan = num(x, "pan", 0);
        }
    s.uni = (int)num(o, "uni", 1); s.detune = num(o, "detune", 14); s.spread = num(o, "spread", 0.5); s.drift = num(o, "drift", 0);
    s.sub = num(o, "sub", 0); s.subWave = str(o, "subWave", "sine"); s.subOct = num(o, "subOct", -1); s.noise = num(o, "noise", 0); s.noiseTone = num(o, "noiseTone", 1);
    s.fm = num(o, "fm", 0); s.ring = num(o, "ring", 0);
    auto filt = [](const juce::var& f, FilterP d) { if (!f.isObject()) { d.on = false; return d; } d.on = flag(f, "on", d.on); d.type = str(f, "type", d.type); d.cutoff = num(f, "cutoff", d.cutoff); d.q = num(f, "q", d.q); d.env = num(f, "env", 0); d.key = num(f, "key", 0); d.drive = num(f, "drive", 0); return d; };
    s.f1 = filt(o.getProperty("filter", juce::var()), FilterP{});
    FilterP f2d; f2d.on = false; f2d.type = "highpass"; f2d.cutoff = 200; s.f2 = filt(o.getProperty("filter2", juce::var()), f2d);
    s.froute = str(o, "froute", "serial");
    const juce::var sh = o.getProperty("shaper", juce::var()); s.shaperType = str(sh, "type", "off"); s.shaperAmt = num(sh, "amt", 0.3);
    s.aenv = envOf(o.getProperty("aenv", juce::var()), s.aenv); s.fenv = envOf(o.getProperty("fenv", juce::var()), s.fenv);
    s.menv = envOf(o.getProperty("menv", juce::var()), s.menv); s.menv2 = envOf(o.getProperty("menv2", juce::var()), s.menv2);
    if (auto* arr = o.getProperty("lfo", juce::var()).getArray())
        for (int i = 0; i < 3 && i < arr->size(); i++) { const juce::var& x = (*arr)[i]; s.lfo[i] = { str(x, "wave", "sine"), num(x, "rate", 1), num(x, "sync", 0), num(x, "fade", 0) }; }
    if (auto* arr = o.getProperty("mod", juce::var()).getArray())
        for (auto& x : *arr) s.mod.push_back({ str(x, "src", "none"), str(x, "dst", "none"), num(x, "amt", 0) });
    if (auto* arr = o.getProperty("macro", juce::var()).getArray()) for (int i = 0; i < 4 && i < arr->size(); i++) s.macro[i] = (double)(*arr)[i];
    s.mode = str(o, "mode", "poly"); s.glide = num(o, "glide", 0); s.voices = (int)num(o, "voices", 8); s.vel = num(o, "vel", 0.6); s.gain = num(o, "gain", 0.7); s.bendRange = num(o, "bendRange", 2);
    return s;
}

void songFromVar(Project& p, const juce::var& song) {
    p.clips.clear(); p.audio.clear(); p.autos.clear(); p.markers.clear();
    p.tracks = (int)num(song, "tracks", 6);
    if (auto* arr = song.getProperty("clips", juce::var()).getArray())
        for (auto& c : *arr) p.clips.push_back({ str(c, "id", ""), str(c, "pat", ""), str(c, "color", ""), (int)num(c, "track", 0), num(c, "start", 0), num(c, "len", 1), num(c, "off", 0), flag(c, "mute", false), str(c, "group", "") });
    if (auto* arr = song.getProperty("audio", juce::var()).getArray())
        for (auto& a : *arr) {
            AudioClip x; x.id = str(a, "id", ""); x.sample = str(a, "sample", ""); x.name = str(a, "name", ""); x.bus = str(a, "bus", "master");
            x.track = (int)num(a, "track", 0); x.start = num(a, "start", 0); x.len = num(a, "len", 1); x.off = num(a, "off", 0); x.gain = num(a, "gain", 1);
            x.bpm = num(a, "bpm", 0); x.stretch = flag(a, "stretch", true); x.mute = flag(a, "mute", false); x.color = str(a, "color", ""); x.group = str(a, "group", "");
            p.audio.push_back(std::move(x));
        }
    if (auto* arr = song.getProperty("auto", juce::var()).getArray())
        for (auto& l : *arr) {
            AutoLane lane; lane.id = str(l, "id", ""); if (lane.id.empty()) lane.id = newId(); lane.target = str(l, "target", "");
            if (auto* pts = l.getProperty("pts", juce::var()).getArray()) for (auto& p : *pts) lane.pts.push_back({ num(p, "t", 0), num(p, "v", 0) });
            p.autos.push_back(std::move(lane));
        }
    if (auto* arr = song.getProperty("markers", juce::var()).getArray()) for (auto& m : *arr) p.markers.push_back({ str(m, "id", newId()), str(m, "name", ""), num(m, "t", 0) });
}

Project parseProject(const juce::var& root) {
    juce::var P = root.hasProperty("project") ? root.getProperty("project", juce::var()) : root;
    if (!P.isObject() || !P.getProperty("channels", juce::var()).isArray()) throw std::runtime_error("That is not an Undae Sonicae project file");
    Project pr; pr.raw = juce::JSON::parse(juce::JSON::toString(P));   // a private deep copy
    pr.name = str(P, "name", "Untitled"); pr.bpm = num(P, "bpm", 120); pr.swing = num(P, "swing", 0); pr.swingDiv = (int)num(P, "swingDiv", 16); pr.masterPitch = num(P, "masterPitch", 0);
    const juce::var key = P.getProperty("key", juce::var()); pr.keyRoot = (int)num(key, "root", 0); pr.keyScale = str(key, "scale", "chromatic");
    const juce::var sig = P.getProperty("sig", juce::var()); pr.sigN = (int)num(sig, "n", 4); pr.sigD = (int)num(sig, "d", 4);
    for (auto& c : *P.getProperty("channels", juce::var()).getArray()) {
        Channel ch;
        ch.id = str(c, "id", ""); ch.name = str(c, "name", ""); ch.type = str(c, "type", "sample"); ch.color = str(c, "color", "#5aa9ff");
        ch.vol = num(c, "vol", 0.8); ch.pan = num(c, "pan", 0); ch.mute = flag(c, "mute", false); ch.solo = flag(c, "solo", false);
        ch.bus = str(c, "bus", "master"); ch.root = (int)num(c, "root", 60); ch.sample = str(c, "sample", "inst:keys");
        ch.tune = num(c, "tune", 0); ch.fine = num(c, "fine", 0); ch.start = num(c, "start", 0); ch.att = num(c, "att", 0); ch.rel = num(c, "rel", 0.08);
        ch.gate = flag(c, "gate", false); ch.rev = flag(c, "rev", false); ch.cut = flag(c, "cut", false); ch.choke = (int)num(c, "choke", 0);
        const juce::var sy = c.getProperty("synth", juce::var()); if (sy.isObject()) ch.synth = synthOf(sy);
        if (const juce::var gr = c.getProperty("gran", juce::var()); gr.isObject()) { auto& G = ch.gran; G.on = flag(gr, "on", false); G.pos = num(gr, "pos", 0.3); G.scan = num(gr, "scan", 0); G.size = num(gr, "size", 0.08);
            G.density = num(gr, "density", 24); G.jitter = num(gr, "jitter", 0.03); G.spray = num(gr, "spray", 0); G.spread = num(gr, "spread", 0.6); G.rev = num(gr, "rev", 0); }
        if (auto* zs = c.getProperty("zones", juce::var()).getArray()) for (auto& z : *zs) {
            Zone Z; Z.sample = str(z, "sample", ""); Z.lo = (int)num(z, "lo", 0); Z.hi = (int)num(z, "hi", 127); Z.root = (int)num(z, "root", 60); Z.vlo = (int)num(z, "vlo", 0); Z.vhi = (int)num(z, "vhi", 127); Z.gain = num(z, "gain", 0);
            if (!Z.sample.empty()) ch.zones.push_back(Z);
        }
        ch.plug = c.getProperty("plug", juce::var()); ch.group = str(c, "group", "");
        pr.channels.push_back(std::move(ch));
    }
    for (auto& p : *P.getProperty("patterns", juce::var()).getArray()) {
        Pattern pt; pt.id = str(p, "id", ""); pt.name = str(p, "name", ""); pt.color = str(p, "color", ""); pt.bars = num(p, "bars", 1);
        const juce::var lp = p.getProperty("loop", juce::var()); if (lp.isObject()) pt.loop = { flag(lp, "on", false), num(lp, "a", 0), num(lp, "b", 0) };
        if (auto* obj = p.getProperty("notes", juce::var()).getDynamicObject())
            for (auto& kv : obj->getProperties()) {
                auto& list = pt.notes[kv.name.toString().toStdString()];
                if (auto* arr = kv.value.getArray())
                    for (auto& n : *arr) {
                        Note x; x.id = str(n, "id", ""); if (x.id.empty()) x.id = newId(); x.t = num(n, "t", 0); x.d = num(n, "d", STEP); x.n = (int)num(n, "n", 60); x.v = num(n, "v", 0.8);
                        x.pan = num(n, "pan", 0); x.fine = num(n, "fine", 0); x.rel = num(n, "rel", 0); x.cut = num(n, "cut", 0); x.res = num(n, "res", 0);
                        x.hasCut = has(n, "cut"); x.hasRes = has(n, "res"); x.mute = flag(n, "mute", false); x.slide = flag(n, "slide", false);
                        list.push_back(x);
                    }
            }
        pr.patterns.push_back(std::move(pt));
    }
    for (auto& b : *P.getProperty("buses", juce::var()).getArray()) {
        Bus bu; bu.id = str(b, "id", ""); bu.name = str(b, "name", ""); bu.kind = str(b, "kind", "insert");
        bu.vol = num(b, "vol", 1); bu.pan = num(b, "pan", 0); bu.width = num(b, "width", 1);
        bu.mute = flag(b, "mute", false); bu.solo = flag(b, "solo", false); bu.mono = flag(b, "mono", false); bu.inv = flag(b, "inv", false); bu.toMaster = flag(b, "toMaster", true);
        bu.input = str(b, "input", ""); bu.arm = flag(b, "arm", false); bu.mon = flag(b, "mon", false); bu.stereo = flag(b, "stereo", false);
        if (auto* arr = b.getProperty("sends", juce::var()).getArray()) for (auto& s : *arr) bu.sends.push_back({ str(s, "to", ""), num(s, "amt", 0) });
        if (auto* arr = b.getProperty("fx", juce::var()).getArray())
            for (auto& f : *arr) {
                FxSlot fx; fx.id = str(f, "id", ""); fx.type = str(f, "type", ""); fx.on = flag(f, "on", true);
                if (auto* po = f.getProperty("p", juce::var()).getDynamicObject()) fx.p = po->getProperties();
                bu.fx.push_back(std::move(fx));
            }
        pr.buses.push_back(std::move(bu));
    }
    if (pr.buses.empty()) { Bus m; m.id = "master"; m.name = "Master"; m.kind = "master"; pr.buses.push_back(m); }
    songFromVar(pr, P.getProperty("song", juce::var()));
    if (auto* arr = P.getProperty("arrs", juce::var()).getArray()) for (auto& a : *arr) pr.arrs.push_back({ str(a, "id", newId()), str(a, "name", "Arrangement"), a.getProperty("song", juce::var()) });
    pr.arrCur = str(P, "arrCur", "");
    if (auto* arr = P.getProperty("links", juce::var()).getArray()) for (auto& l : *arr) pr.links.push_back({ str(l, "id", newId()), str(l, "target", ""), (int)num(l, "ch", 0), (int)num(l, "cc", 0) });
    pr.rackGroup = str(P, "rackGroup", "");
    if (auto* arr = root.getProperty("samples", juce::var()).getArray())
        for (auto& s : *arr) {
            juce::MemoryOutputStream mo; juce::Base64::convertFromBase64(mo, s.getProperty("data", "").toString());
            const std::string id = str(s, "id", ""); pr.embedded->data[id] = mo.getMemoryBlock(); pr.embedded->names[id] = str(s, "name", id);
        }
    return pr;
}

Project loadProject(const juce::File& file) {
    if (!file.existsAsFile()) throw std::runtime_error("File not found: " + file.getFullPathName().toStdString());
    juce::var root;
    const juce::Result r = juce::JSON::parse(file.loadFileAsString(), root);
    if (r.failed()) throw std::runtime_error("Not a readable project file (" + r.getErrorMessage().toStdString() + ")");
    return parseProject(root);
}

int Project::songBars() const {
    double m = 0;
    for (auto& c : clips) m = std::max(m, c.start + c.len);
    for (auto& a : audio) m = std::max(m, a.start + a.len);
    return (int)std::ceil(m - 1e-6);
}
const Pattern* Project::pattern(const std::string& id) const { for (auto& p : patterns) if (p.id == id) return &p; return nullptr; }
const Bus* Project::findBus(const std::string& id) const { for (auto& b : buses) if (b.id == id) return &b; return nullptr; }
const Channel* Project::findChannel(const std::string& id) const { for (auto& c : channels) if (c.id == id) return &c; return nullptr; }

}   // namespace us

// --- writing -------------------------------------------------------------------------------------------------------
namespace us {

std::string newId() {   // 8 base-36 characters, like util.js uid()
    static juce::Random r(juce::Time::currentTimeMillis());
    const char* d = "0123456789abcdefghijklmnopqrstuvwxyz"; std::string s;
    for (int i = 0; i < 8; i++) s += d[r.nextInt(36)];
    return s;
}

namespace {
juce::var clone(const juce::var& v) { return v.isVoid() || v.isUndefined() ? juce::var() : juce::JSON::parse(juce::JSON::toString(v)); }
juce::DynamicObject* objOf(juce::var& v) { if (!v.getDynamicObject()) v = juce::var(new juce::DynamicObject()); return v.getDynamicObject(); }
// the item of an array with this id, cloned, or a new object
juce::var byId(const juce::var& arr, const std::string& id) {
    if (auto* a = arr.getArray()) for (auto& x : *a) if (x.getProperty("id", "").toString().toStdString() == id) return clone(x);
    return juce::var(new juce::DynamicObject());
}
juce::var S(const std::string& s) { return juce::var(juce::String(s)); }
juce::var envVar(juce::var base, const EnvP& e) { auto* o = objOf(base); o->setProperty("a", e.a); o->setProperty("d", e.d); o->setProperty("s", e.s); o->setProperty("r", e.r); return base; }

juce::var synthVar(const SynthP& s, juce::var base) {
    auto* o = objOf(base);
    juce::Array<juce::var> osc; juce::var oldOsc = base.getProperty("osc", juce::var());
    for (int i = 0; i < 3; i++) {
        juce::var x = oldOsc.getArray() && i < oldOsc.getArray()->size() ? clone((*oldOsc.getArray())[i]) : juce::var(new juce::DynamicObject());
        auto* xo = objOf(x); const OscP& O = s.osc[i];
        xo->setProperty("on", O.on); xo->setProperty("wave", S(O.wave)); xo->setProperty("wave2", S(O.wave2)); xo->setProperty("oct", O.oct); xo->setProperty("semi", O.semi);
        xo->setProperty("fine", O.fine); xo->setProperty("level", O.level); xo->setProperty("morph", O.morph); xo->setProperty("pan", O.pan);
        osc.add(x);
    }
    o->setProperty("osc", osc);
    o->setProperty("uni", s.uni); o->setProperty("detune", s.detune); o->setProperty("spread", s.spread); o->setProperty("drift", s.drift); o->setProperty("sub", s.sub);
    o->setProperty("subWave", S(s.subWave)); o->setProperty("subOct", s.subOct); o->setProperty("noise", s.noise); o->setProperty("noiseTone", s.noiseTone); o->setProperty("fm", s.fm); o->setProperty("ring", s.ring);
    auto filt = [&](const char* k, const FilterP& f) { juce::var v = clone(base.getProperty(k, juce::var())); auto* fo = objOf(v); fo->setProperty("on", f.on); fo->setProperty("type", S(f.type)); fo->setProperty("cutoff", f.cutoff); fo->setProperty("q", f.q); fo->setProperty("env", f.env); fo->setProperty("key", f.key); if (!std::strcmp(k, "filter")) fo->setProperty("drive", f.drive); o->setProperty(k, v); };
    filt("filter", s.f1); filt("filter2", s.f2); o->setProperty("froute", S(s.froute));
    juce::var sh(new juce::DynamicObject()); sh.getDynamicObject()->setProperty("type", S(s.shaperType)); sh.getDynamicObject()->setProperty("amt", s.shaperAmt); o->setProperty("shaper", sh);
    o->setProperty("aenv", envVar(clone(base.getProperty("aenv", juce::var())), s.aenv)); o->setProperty("fenv", envVar(clone(base.getProperty("fenv", juce::var())), s.fenv));
    o->setProperty("menv", envVar(clone(base.getProperty("menv", juce::var())), s.menv)); o->setProperty("menv2", envVar(clone(base.getProperty("menv2", juce::var())), s.menv2));
    juce::Array<juce::var> lfo; for (auto& l : s.lfo) { juce::var v(new juce::DynamicObject()); auto* lo = v.getDynamicObject(); lo->setProperty("wave", S(l.wave)); lo->setProperty("rate", l.rate); lo->setProperty("sync", l.sync); lo->setProperty("fade", l.fade); lfo.add(v); }
    o->setProperty("lfo", lfo);
    juce::Array<juce::var> mod; for (auto& m : s.mod) { juce::var v(new juce::DynamicObject()); auto* mo = v.getDynamicObject(); mo->setProperty("src", S(m.src)); mo->setProperty("dst", S(m.dst)); mo->setProperty("amt", m.amt); mod.add(v); }
    o->setProperty("mod", mod);
    juce::Array<juce::var> mac; for (double m : s.macro) mac.add(m); o->setProperty("macro", mac);
    o->setProperty("mode", S(s.mode)); o->setProperty("glide", s.glide); o->setProperty("voices", s.voices); o->setProperty("vel", s.vel); o->setProperty("gain", s.gain); o->setProperty("bendRange", s.bendRange);
    return base;
}
}   // namespace

juce::var songToVar(const Project& p) {
    juce::var song = clone(p.raw.getProperty("song", juce::var())); auto* so = objOf(song);
    so->setProperty("tracks", p.tracks);
    const juce::var oldClips = song.getProperty("clips", juce::var()), oldAudio = song.getProperty("audio", juce::var());
    juce::Array<juce::var> clips;
    for (auto& c : p.clips) { juce::var v = byId(oldClips, c.id); auto* co = v.getDynamicObject(); co->setProperty("id", S(c.id)); co->setProperty("pat", S(c.pat)); co->setProperty("track", c.track); co->setProperty("start", c.start); co->setProperty("len", c.len);
        if (c.off != 0) co->setProperty("off", c.off); else co->removeProperty("off"); if (c.mute) co->setProperty("mute", true); else co->removeProperty("mute"); if (!c.color.empty()) co->setProperty("color", S(c.color)); else co->removeProperty("color"); if (!c.group.empty()) co->setProperty("group", S(c.group)); else co->removeProperty("group"); clips.add(v); }
    so->setProperty("clips", clips);
    juce::Array<juce::var> audio;
    for (auto& a : p.audio) { juce::var v = byId(oldAudio, a.id); auto* ao = v.getDynamicObject(); ao->setProperty("id", S(a.id)); ao->setProperty("sample", S(a.sample)); ao->setProperty("name", S(a.name)); ao->setProperty("track", a.track);
        ao->setProperty("start", a.start); ao->setProperty("len", a.len); ao->setProperty("off", a.off); ao->setProperty("bus", S(a.bus)); ao->setProperty("gain", a.gain); ao->setProperty("bpm", a.bpm); ao->setProperty("stretch", a.stretch); ao->setProperty("mute", a.mute);
        if (!a.color.empty()) ao->setProperty("color", S(a.color)); else ao->removeProperty("color"); if (!a.group.empty()) ao->setProperty("group", S(a.group)); else ao->removeProperty("group"); audio.add(v); }
    so->setProperty("audio", audio);
    juce::Array<juce::var> autos;
    for (auto& l : p.autos) { juce::var v(new juce::DynamicObject()); v.getDynamicObject()->setProperty("id", S(l.id.empty() ? newId() : l.id)); v.getDynamicObject()->setProperty("target", S(l.target)); juce::Array<juce::var> pts;
        for (auto& q : l.pts) { juce::var pv(new juce::DynamicObject()); pv.getDynamicObject()->setProperty("t", q.t); pv.getDynamicObject()->setProperty("v", q.v); pts.add(pv); }
        v.getDynamicObject()->setProperty("pts", pts); autos.add(v); }
    so->setProperty("auto", autos);
    juce::Array<juce::var> marks; for (auto& m : p.markers) { juce::var v(new juce::DynamicObject()); v.getDynamicObject()->setProperty("id", S(m.id)); v.getDynamicObject()->setProperty("name", S(m.name)); v.getDynamicObject()->setProperty("t", m.t); marks.add(v); }
    so->setProperty("markers", marks);
    return song;
}

juce::var projectToVar(const Project& p) {
    juce::var P = clone(p.raw); auto* o = objOf(P);
    o->setProperty("v", 1); o->setProperty("name", S(p.name)); o->setProperty("bpm", p.bpm); o->setProperty("swing", p.swing); o->setProperty("swingDiv", p.swingDiv);
    if (p.masterPitch != 0 || o->hasProperty("masterPitch")) o->setProperty("masterPitch", p.masterPitch);
    juce::var key(new juce::DynamicObject()); key.getDynamicObject()->setProperty("root", p.keyRoot); key.getDynamicObject()->setProperty("scale", S(p.keyScale)); o->setProperty("key", key);
    juce::var sig(new juce::DynamicObject()); sig.getDynamicObject()->setProperty("n", p.sigN); sig.getDynamicObject()->setProperty("d", p.sigD); o->setProperty("sig", sig);
    const juce::var oldCh = p.raw.getProperty("channels", juce::var()), oldPat = p.raw.getProperty("patterns", juce::var()), oldBus = p.raw.getProperty("buses", juce::var());
    juce::Array<juce::var> chans;
    for (auto& c : p.channels) {
        juce::var v = byId(oldCh, c.id); auto* co = v.getDynamicObject();
        co->setProperty("id", S(c.id)); co->setProperty("name", S(c.name)); co->setProperty("type", S(c.type)); co->setProperty("color", S(c.color));
        co->setProperty("vol", c.vol); co->setProperty("pan", c.pan); co->setProperty("mute", c.mute); co->setProperty("solo", c.solo); co->setProperty("bus", S(c.bus));
        co->setProperty("root", c.root); co->setProperty("sample", S(c.sample)); co->setProperty("tune", c.tune); co->setProperty("fine", c.fine); co->setProperty("start", c.start);
        co->setProperty("att", c.att); co->setProperty("rel", c.rel); co->setProperty("gate", c.gate); co->setProperty("rev", c.rev); co->setProperty("cut", c.cut); co->setProperty("choke", c.choke);
        co->setProperty("synth", c.synth ? synthVar(*c.synth, clone(v.getProperty("synth", juce::var()))) : juce::var());
        if (c.gran.on || co->hasProperty("gran")) { juce::var g(new juce::DynamicObject()); auto* go = g.getDynamicObject(); const auto& G = c.gran;
            go->setProperty("on", G.on); go->setProperty("pos", G.pos); go->setProperty("scan", G.scan); go->setProperty("size", G.size); go->setProperty("density", G.density);
            go->setProperty("jitter", G.jitter); go->setProperty("spray", G.spray); go->setProperty("spread", G.spread); go->setProperty("rev", G.rev); co->setProperty("gran", g); }
        if (!c.zones.empty() || co->hasProperty("zones")) { juce::Array<juce::var> zs; for (auto& Z : c.zones) { juce::var z(new juce::DynamicObject()); auto* zo = z.getDynamicObject();
                zo->setProperty("sample", S(Z.sample)); zo->setProperty("lo", Z.lo); zo->setProperty("hi", Z.hi); zo->setProperty("root", Z.root); zo->setProperty("vlo", Z.vlo); zo->setProperty("vhi", Z.vhi); zo->setProperty("gain", Z.gain); zs.add(z); }
            co->setProperty("zones", zs); }
        if (!c.plug.isVoid()) co->setProperty("plug", c.plug);
        if (!c.group.empty()) co->setProperty("group", S(c.group)); else co->removeProperty("group");
        chans.add(v);
    }
    o->setProperty("channels", chans);
    juce::Array<juce::var> pats;
    for (auto& pt : p.patterns) {
        juce::var v = byId(oldPat, pt.id); auto* po = v.getDynamicObject();
        po->setProperty("id", S(pt.id)); po->setProperty("name", S(pt.name)); po->setProperty("bars", pt.bars); if (!pt.color.empty()) po->setProperty("color", S(pt.color));
        const juce::var oldNotes = v.getProperty("notes", juce::var()); juce::var notes(new juce::DynamicObject());
        for (auto& [cid, list] : pt.notes) {
            const juce::var oldList = oldNotes.getProperty(juce::Identifier(cid), juce::var()); juce::Array<juce::var> arr;
            for (auto& n : list) {
                juce::var nv = byId(oldList, n.id); auto* no = nv.getDynamicObject();
                no->setProperty("id", S(n.id)); no->setProperty("t", n.t); no->setProperty("d", n.d); no->setProperty("n", n.n); no->setProperty("v", n.v);
                auto opt = [&](const char* k, double val, bool keep) { if (val != 0 || keep) no->setProperty(k, val); else no->removeProperty(k); };
                opt("pan", n.pan, false); opt("fine", n.fine, false); opt("rel", n.rel, false); opt("cut", n.cut, n.hasCut); opt("res", n.res, n.hasRes);
                if (n.mute) no->setProperty("mute", true); else no->removeProperty("mute");
                if (n.slide) no->setProperty("slide", true); else no->removeProperty("slide");
                arr.add(nv);
            }
            notes.getDynamicObject()->setProperty(juce::Identifier(cid), arr);
        }
        po->setProperty("notes", notes);
        if (pt.loop.on || pt.loop.b > pt.loop.a) { juce::var lp(new juce::DynamicObject()); lp.getDynamicObject()->setProperty("on", pt.loop.on); lp.getDynamicObject()->setProperty("a", pt.loop.a); lp.getDynamicObject()->setProperty("b", pt.loop.b); po->setProperty("loop", lp); }
        pats.add(v);
    }
    o->setProperty("patterns", pats);
    juce::Array<juce::var> buses;
    for (auto& b : p.buses) {
        juce::var v = byId(oldBus, b.id); auto* bo = v.getDynamicObject();
        bo->setProperty("id", S(b.id)); bo->setProperty("name", S(b.name)); bo->setProperty("kind", S(b.kind)); bo->setProperty("vol", b.vol); bo->setProperty("pan", b.pan); bo->setProperty("width", b.width);
        bo->setProperty("mute", b.mute); bo->setProperty("solo", b.solo); bo->setProperty("mono", b.mono); bo->setProperty("inv", b.inv); bo->setProperty("toMaster", b.toMaster);
        juce::Array<juce::var> sends; for (auto& s : b.sends) { juce::var sv(new juce::DynamicObject()); sv.getDynamicObject()->setProperty("to", S(s.to)); sv.getDynamicObject()->setProperty("amt", s.amt); sends.add(sv); }
        bo->setProperty("sends", sends);
        bo->setProperty("input", S(b.input)); bo->setProperty("arm", b.arm); bo->setProperty("mon", b.mon); bo->setProperty("stereo", b.stereo);
        const juce::var oldFx = v.getProperty("fx", juce::var()); juce::Array<juce::var> fx;
        for (auto& f : b.fx) {
            juce::var fv = byId(oldFx, f.id); auto* fo = fv.getDynamicObject();
            fo->setProperty("id", S(f.id)); fo->setProperty("type", S(f.type)); fo->setProperty("on", f.on);
            juce::var pv(new juce::DynamicObject()); for (auto& kv : f.p) pv.getDynamicObject()->setProperty(kv.name, kv.value); fo->setProperty("p", pv);
            fx.add(fv);
        }
        bo->setProperty("fx", fx);
        buses.add(v);
    }
    o->setProperty("buses", buses);
    o->setProperty("song", songToVar(p));
    juce::Array<juce::var> arrs; for (auto& a : p.arrs) { juce::var v(new juce::DynamicObject()); v.getDynamicObject()->setProperty("id", S(a.id)); v.getDynamicObject()->setProperty("name", S(a.name)); if (!a.song.isVoid() && a.id != p.arrCur) v.getDynamicObject()->setProperty("song", a.song); arrs.add(v); }
    if (!p.arrs.empty()) { o->setProperty("arrs", arrs); o->setProperty("arrCur", S(p.arrCur)); }
    juce::Array<juce::var> links; for (auto& l : p.links) { juce::var v(new juce::DynamicObject()); auto* lo = v.getDynamicObject(); lo->setProperty("id", S(l.id)); lo->setProperty("target", S(l.target)); lo->setProperty("ch", l.ch); lo->setProperty("cc", l.cc); links.add(v); }
    o->setProperty("links", links);
    if (!p.rackGroup.empty()) o->setProperty("rackGroup", S(p.rackGroup)); else o->removeProperty("rackGroup");
    return P;
}

void saveProject(const Project& p, const juce::File& file) {
    juce::var root(new juce::DynamicObject()); auto* r = root.getDynamicObject();
    r->setProperty("app", "undae-sonicae"); r->setProperty("project", projectToVar(p));
    juce::Array<juce::var> smp;
    for (auto& [id, bytes] : p.embedded->data) { juce::var s(new juce::DynamicObject()); s.getDynamicObject()->setProperty("id", S(id)); s.getDynamicObject()->setProperty("name", S(p.embedded->names.count(id) ? p.embedded->names.at(id) : id)); s.getDynamicObject()->setProperty("data", juce::Base64::toBase64(bytes.getData(), bytes.getSize())); smp.add(s); }
    r->setProperty("samples", smp);
    juce::TemporaryFile tmp(file);
    if (!tmp.getFile().replaceWithText(juce::JSON::toString(root, true)) || !tmp.overwriteTargetFileWithTemporary()) throw std::runtime_error("Could not write " + file.getFullPathName().toStdString());
}

}   // namespace us
