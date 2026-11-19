#include "Mp3.h"
#if defined(_WIN32)
 #include <windows.h>
#else
 #include <dlfcn.h>
#endif
#include <stdexcept>
#include <vector>

namespace us {

namespace {
#if defined(_WIN32)
// libmp3lame.dll next to the exe first, then wherever Windows looks for DLLs
static void* openLame() {
    const auto dir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    for (const char* n : { "libmp3lame.dll", "lame_enc.dll", "mp3lame.dll" }) {
        if (auto h = LoadLibraryW(dir.getChildFile(n).getFullPathName().toWideCharPointer())) return (void*)h;
        if (auto h = LoadLibraryA(n)) return (void*)h;
    }
    return nullptr;
}
static void* lameSym(void* h, const char* s) { return (void*)GetProcAddress((HMODULE)h, s); }
#elif defined(__APPLE__)
// inside the app bundle (Contents/Frameworks) first, then Homebrew on Apple Silicon and on Intel
static void* openLame() {
    const auto fw = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory().getSiblingFile("Frameworks");
    for (auto n : { fw.getChildFile("libmp3lame.0.dylib").getFullPathName(), fw.getChildFile("libmp3lame.dylib").getFullPathName(),
                    juce::String("/opt/homebrew/lib/libmp3lame.0.dylib"), juce::String("/usr/local/lib/libmp3lame.0.dylib"), juce::String("libmp3lame.0.dylib"), juce::String("libmp3lame.dylib") })
        if (void* h = dlopen(n.toRawUTF8(), RTLD_NOW | RTLD_LOCAL)) return h;
    return nullptr;
}
#else
static void* openLame() { for (const char* n : { "libmp3lame.so.0", "libmp3lame.so" }) if (void* h = dlopen(n, RTLD_NOW | RTLD_LOCAL)) return h; return nullptr; }
static void* lameSym(void* h, const char* s) { return dlsym(h, s); }
#endif
struct Lame {
    void* h = nullptr;
    void* (*init)() = nullptr; int (*setRate)(void*, int) = nullptr; int (*setOutRate)(void*, int) = nullptr; int (*setChannels)(void*, int) = nullptr; int (*setBrate)(void*, int) = nullptr;
    int (*setQuality)(void*, int) = nullptr; int (*initParams)(void*) = nullptr; int (*encode)(void*, const float*, const float*, int, unsigned char*, int) = nullptr;
    int (*flush)(void*, unsigned char*, int) = nullptr; int (*close)(void*) = nullptr;
    Lame() {
        h = openLame();
        if (!h) return;
        auto f = [this](const char* s) { return lameSym(h, s); };
        init = (void* (*)())f("lame_init"); setRate = (int (*)(void*, int))f("lame_set_in_samplerate"); setOutRate = (int (*)(void*, int))f("lame_set_out_samplerate");
        setChannels = (int (*)(void*, int))f("lame_set_num_channels"); setBrate = (int (*)(void*, int))f("lame_set_brate"); setQuality = (int (*)(void*, int))f("lame_set_quality");
        initParams = (int (*)(void*))f("lame_init_params"); encode = (int (*)(void*, const float*, const float*, int, unsigned char*, int))f("lame_encode_buffer_ieee_float");
        flush = (int (*)(void*, unsigned char*, int))f("lame_encode_flush"); close = (int (*)(void*))f("lame_close");
    }
    bool ok() const { return h && init && setRate && setOutRate && setChannels && setBrate && setQuality && initParams && encode && flush && close; }
};
Lame& lame() { static Lame l; return l; }
}   // namespace

bool mp3Available() { return lame().ok(); }

juce::MemoryBlock encodeMp3(const juce::AudioBuffer<float>& buf, double sr, int kbps) {
    Lame& L = lame(); if (!L.ok()) throw std::runtime_error("MP3 export needs LAME (libmp3lame), which is not installed");
    void* g = L.init(); if (!g) throw std::runtime_error("LAME could not start");
    L.setRate(g, (int)sr); L.setOutRate(g, (int)sr); L.setChannels(g, 2); L.setBrate(g, kbps); L.setQuality(g, 2);
    if (L.initParams(g) < 0) { L.close(g); throw std::runtime_error("LAME does not accept these settings (sample rate " + std::to_string((int)sr) + ")"); }
    juce::MemoryBlock out; const int N = 8192; std::vector<unsigned char> mp3((size_t)(N * 5 / 4 + 7200));
    const float* l = buf.getReadPointer(0); const float* r = buf.getReadPointer(buf.getNumChannels() > 1 ? 1 : 0);
    for (int i = 0; i < buf.getNumSamples(); i += N) {
        const int n = std::min(N, buf.getNumSamples() - i), k = L.encode(g, l + i, r + i, n, mp3.data(), (int)mp3.size());
        if (k < 0) { L.close(g); throw std::runtime_error("MP3 encoding failed"); }
        out.append(mp3.data(), (size_t)k);
    }
    const int k = L.flush(g, mp3.data(), (int)mp3.size()); if (k > 0) out.append(mp3.data(), (size_t)k);
    L.close(g);
    return out;
}

}   // namespace us
