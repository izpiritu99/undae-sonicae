// Plug-ins through the existing host processes (native/PROTOCOL.md): `undae-host` for CLAP and Linux VST3,
// `undae-vsthost.exe` / `undae-vsthost32.exe` under Wine for Windows VST2 / VST3. Each plug-in instance is one local
// WebSocket connection. Unlike the browser, the native app sends an audio block and waits for the answer in the same
// audio callback (a local round trip takes ~0.1-0.4 ms), so plug-ins add no buffering delay. A plug-in that crashes takes
// its host process down, not the app.
#pragma once
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <juce_core/juce_core.h>

namespace us {

// where a host listens: this computer (port + token) or a computer sharing its plug-ins (host + port + path with the code)
struct HostEndpoint { int port = 0; std::string token, folder, host = "127.0.0.1", path; bool ok() const { return port > 0; } };

// The host processes. start() returns at once; the Wine hosts come up in the background (the first time Wine builds its
// prefix, which takes a while).
class HostSet {
public:
    static HostSet& get();
    void start(const juce::File& binDir, bool serial = false);
    void stop();
    HostEndpoint clap() const; HostEndpoint win(int bits = 64) const;
    bool winPending() const { return pending.load() > 0; }
    bool juceHost() const { return juceOn.load(); }   // the JUCE host (src/pluginhost) runs: macOS, or UNDAE_PLUGIN_HOST=juce
    // Use another computer's plug-ins (the desktop app's "Share this computer's plug-ins"): "host[:port]" and the pairing
    // code; "" goes back to this computer's hosts. Returns "" or what went wrong; `name` gets the computer's name.
    std::string useServer(const std::string& addr, const std::string& code, std::string* name = nullptr);
    bool remote() const { std::lock_guard<std::mutex> lk(m); return remoteOn; }
    juce::File binDir() const { return bin; }
    static juce::File findBinDir();   // native/bin next to the app (packaged) or up from the build folder (development)
private:
#if defined(_WIN32)
    struct Proc { void* proc = nullptr; void* in = nullptr; HostEndpoint ep; };   // process HANDLE, write end of its stdin
#else
    struct Proc { int pid = -1, in = -1; HostEndpoint ep; };
#endif
    mutable std::mutex m; Proc clapP, win64, win32; bool remoteOn = false; HostEndpoint rClap, rWin64, rWin32; std::atomic<int> pending{ 0 }; juce::File bin; bool started = false;
    std::atomic<bool> juceOn{ false };
    void startJuceHost(const std::string& serial);
    static Proc spawn(const std::string& exe, const std::vector<std::string>& args, const std::vector<std::pair<std::string, std::string>>& env, double timeoutSec);
};

// One plug-in instance in a host.
class PluginInstance {
public:
    struct Param { int id; std::string name, module, text; double min, max, def, value; bool stepped, hidden, readonly; };
    struct Info { std::string name, vendor; int inputs = 2, outputs = 2; bool notes = false, gui = false; std::vector<Param> params; };
    ~PluginInstance();
    // connect and load; blocking (call off the audio thread). slot: { path, pid, fmt, windows, bits, state, v<id>... }
    bool load(const juce::var& slot, double sampleRate, std::string& error);
    bool ready() const { return loaded.load(); }
    const Info& info() const { return inf; }
    // audio thread: one block through the plug-in; frame = the block's start on the engine clock. false: no answer in time
    bool process(float* L, float* R, int n, uint32_t frame, int timeoutMs = 8);
    void note(bool on, int key, double vel, uint32_t at);
    void midi(int s, int d1, int d2, uint32_t at);
    void param(int id, double v);
    void panic();
    bool attach(const HostEndpoint& ep) { return connect(ep); }   // a connection with no plug-in (scanning)
    juce::var request(const juce::var& msg, int timeoutMs = 15000);   // control call with a reply (message / background thread)
    std::string readState();                  // base64, or ""
    bool showWindow(bool show);
    // events from the plug-in (its own window changed parameters, the state changed, the window closed)
    struct Event { int kind; std::vector<std::pair<int, double>> changes; bool open = false; };   // kind 0 param changes, 1 dirty, 2 gui
    std::vector<Event> takeEvents();
    double lastSent(int id) const { std::lock_guard<std::mutex> l(pm); auto it = sent.find(id); return it == sent.end() ? NAN : it->second; }
private:
    std::intptr_t fd = -1; std::thread reader; std::atomic<bool> running{ false }, loaded{ false }; Info inf;
    bool remote = false, primed = false; int underruns = 0;   // remote: blocks are pipelined through a small buffer instead of awaited
    std::mutex wm;                             // writes
    std::mutex am; std::condition_variable acv; std::deque<std::vector<float>> replies;   // audio answers
    std::mutex rm; std::map<int, juce::var> answers; std::condition_variable rcv; int seq = 0;
    std::mutex em; std::vector<Event> events;
    mutable std::mutex pm; std::map<int, double> sent;
    bool connect(const HostEndpoint& ep);
    bool sendFrame(int op, const void* data, size_t n);
    void sendJson(const juce::var& v);
    void readLoop();
};

// Scan every host for plug-ins: { name, vendor, path, id, format, instrument, windows, bits }
std::vector<juce::var> scanPlugins();
// Which host a slot needs
HostEndpoint endpointFor(const juce::var& slot);

}   // namespace us
