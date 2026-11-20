#if defined(_WIN32)
 #include <winsock2.h>
 #include <ws2tcpip.h>
 #include <windows.h>
#endif
#include "PluginHost.h"
#include <cmath>
#include <csignal>
#if defined(__APPLE__)
 #include <sys/sysctl.h>
#endif
#if !defined(_WIN32)
 #include <arpa/inet.h>
 #include <netdb.h>
 #include <fcntl.h>
 #include <netinet/in.h>
 #include <netinet/tcp.h>
 #include <poll.h>
 #include <sys/socket.h>
 #include <sys/wait.h>
 #include <unistd.h>
#endif

namespace us {

// --- sockets: the few calls that differ between Winsock and BSD sockets --------------------------------------------
#if defined(_WIN32)
using SockT = SOCKET;
static constexpr int kSendFlags = 0;   // Winsock raises no SIGPIPE
static void closeSock(std::intptr_t s) { closesocket((SOCKET)s); }
static void shutSock(std::intptr_t s) { shutdown((SOCKET)s, SD_BOTH); }
static void setTimeout(std::intptr_t s, int opt, int sec) { DWORD ms = (DWORD)sec * 1000; setsockopt((SOCKET)s, SOL_SOCKET, opt, (const char*)&ms, sizeof ms); }
static void netInit() { static const bool ok = [] { WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) == 0; }(); (void)ok; }
static void noSigPipe(std::intptr_t) {}
#else
using SockT = int;
#if defined(__APPLE__)
static constexpr int kSendFlags = 0;   // macOS has no MSG_NOSIGNAL: the socket gets SO_NOSIGPIPE instead (noSigPipe)
static void noSigPipe(std::intptr_t s) { int one = 1; setsockopt((int)s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#else
static constexpr int kSendFlags = MSG_NOSIGNAL;
static void noSigPipe(std::intptr_t) {}
#endif
static void closeSock(std::intptr_t s) { close((int)s); }
static void shutSock(std::intptr_t s) { shutdown((int)s, SHUT_RDWR); }
static void setTimeout(std::intptr_t s, int opt, int sec) { timeval tv{ sec, 0 }; setsockopt((int)s, SOL_SOCKET, opt, &tv, sizeof tv); }
static void netInit() {}
#endif
static constexpr const char* kHostMarker =
#if defined(_WIN32)
    "undae-vsthost.exe";   // no CLAP host on Windows yet: the VST host marks the folder
#else
    "undae-host";
#endif

// --- host processes ------------------------------------------------------------------------------------------------
HostSet& HostSet::get() { static HostSet h; return h; }

juce::File HostSet::findBinDir() {
    auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    for (auto d = exe.getParentDirectory(); d.getFullPathName().length() > 1; d = d.getParentDirectory()) {
        if (d.getChildFile(kHostMarker).existsAsFile()) return d;
        if (d.getChildFile("native/bin").getChildFile(kHostMarker).existsAsFile()) return d.getChildFile("native/bin");
        if (d.isRoot()) break;
    }
    auto cwd = juce::File::getCurrentWorkingDirectory();
    for (auto d = cwd; d.getFullPathName().length() > 1; d = d.getParentDirectory()) { if (d.getChildFile("native/bin").getChildFile(kHostMarker).existsAsFile()) return d.getChildFile("native/bin"); if (d.isRoot()) break; }
    return {};
}

#if defined(_WIN32)
// Windows: CreateProcess with stdout and stdin on pipes, and the extra variables added to a copy of this environment.
static std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\""; size_t bs = 0;
    for (wchar_t c : a) { if (c == L'\\') { bs++; continue; } if (c == L'"') q.append(bs * 2 + 1, L'\\'); else q.append(bs, L'\\'); bs = 0; q += c; }
    q.append(bs * 2, L'\\'); return q + L"\"";
}
HostSet::Proc HostSet::spawn(const std::string& exe, const std::vector<std::string>& args, const std::vector<std::pair<std::string, std::string>>& env, double timeoutSec) {
    Proc p; auto W = [](const std::string& s) { return std::wstring(juce::String::fromUTF8(s.c_str()).toWideCharPointer()); };
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE outR = nullptr, outW = nullptr, inR = nullptr, inW = nullptr;
    if (!CreatePipe(&outR, &outW, &sa, 0)) return p;
    if (!CreatePipe(&inR, &inW, &sa, 0)) { CloseHandle(outR); CloseHandle(outW); return p; }
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);   // the app's ends stay here
    // environment block: this process's variables, minus the ones being set, then the new ones
    std::vector<std::wstring> vars;
    if (wchar_t* blk = GetEnvironmentStringsW()) {
        for (const wchar_t* e = blk; *e; e += wcslen(e) + 1) {
            std::wstring v(e); bool replaced = false;
            for (auto& [k, val] : env) { std::wstring kw = W(k) + L"="; if (_wcsnicmp(v.c_str(), kw.c_str(), kw.size()) == 0) replaced = true; }
            if (!replaced) vars.push_back(v);
        }
        FreeEnvironmentStringsW(blk);
    }
    for (auto& [k, v] : env) vars.push_back(W(k) + L"=" + W(v));
    std::wstring envBlock; for (auto& v : vars) { envBlock += v; envBlock += L'\0'; } envBlock += L'\0';
    std::wstring cmd = quoteArg(W(exe)); for (auto& a : args) cmd += L" " + quoteArg(W(a));
    STARTUPINFOW si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = outW; si.hStdInput = inR; si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, envBlock.data(), nullptr, &si, &pi);
    CloseHandle(outW); CloseHandle(inR);
    if (!ok) { CloseHandle(outR); CloseHandle(inW); return p; }
    CloseHandle(pi.hThread);
    // the first line of stdout: {"port":N,"token":"..."}
    std::string line; const double t0 = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - t0 < timeoutSec * 1000) {
        DWORD avail = 0; if (!PeekNamedPipe(outR, nullptr, 0, nullptr, &avail, nullptr)) break;   // fails once the host has exited
        if (!avail) { Sleep(20); continue; }
        char c; DWORD k = 0; if (!ReadFile(outR, &c, 1, &k, nullptr) || !k) break;
        if (c == '\n') break; if (c != '\r') line += c;
    }
    CloseHandle(outR);
    juce::var j; juce::JSON::parse(juce::String(line), j);
    if ((int)j.getProperty("port", 0) > 0) { p.proc = pi.hProcess; p.in = inW; p.ep.port = (int)j.getProperty("port", 0); p.ep.token = j.getProperty("token", "").toString().toStdString(); }
    else { CloseHandle(inW); TerminateProcess(pi.hProcess, 1); CloseHandle(pi.hProcess); }
    return p;
}
#else
HostSet::Proc HostSet::spawn(const std::string& exe, const std::vector<std::string>& args, const std::vector<std::pair<std::string, std::string>>& env, double timeoutSec) {
    Proc p; int out[2], in[2];
    if (pipe(out) || pipe(in)) return p;
    const pid_t pid = fork();
    if (pid == 0) {   // child: stdout to the pipe, stdin from the other (the hosts exit when it closes), stderr kept
        dup2(out[1], 1); dup2(in[0], 0); close(out[0]); close(out[1]); close(in[0]); close(in[1]);
        for (auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);
        std::vector<char*> av; av.push_back(const_cast<char*>(exe.c_str())); for (auto& a : args) av.push_back(const_cast<char*>(a.c_str())); av.push_back(nullptr);
        execvp(exe.c_str(), av.data()); _exit(127);
    }
    close(out[1]); close(in[0]);
    if (pid < 0) { close(out[0]); close(in[1]); return p; }
    // the first line of stdout: {"port":N,"token":"..."}
    std::string line; const double t0 = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - t0 < timeoutSec * 1000) {
        pollfd pf{ out[0], POLLIN, 0 }; if (poll(&pf, 1, 200) <= 0) continue;
        char c; const ssize_t k = read(out[0], &c, 1); if (k <= 0) break;
        if (c == '\n') break; line += c;
    }
    close(out[0]);   // nothing else is read from it; the host writes no more to stdout
    juce::var j; juce::JSON::parse(juce::String(line), j);
    if ((int)j.getProperty("port", 0) > 0) { p.pid = pid; p.in = in[1]; p.ep.port = (int)j.getProperty("port", 0); p.ep.token = j.getProperty("token", "").toString().toStdString(); }
    else { close(in[1]); kill(pid, SIGKILL); waitpid(pid, nullptr, 0); }
    return p;
}
#endif

// --- the JUCE plug-in host (src/pluginhost): the only host on macOS; elsewhere used when UNDAE_PLUGIN_HOST=juce ---------
static bool wantJuceHost() {
#if JUCE_MAC
    return true;
#else
    return juce::SystemStats::getEnvironmentVariable("UNDAE_PLUGIN_HOST", "") == "juce";
#endif
}
static juce::File juceHostExe() {
    const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
#if JUCE_MAC
    const char* name = "undae-pluginhost.app/Contents/MacOS/undae-pluginhost";
    { const auto f = exe.getParentDirectory().getSiblingFile("Helpers").getChildFile(name); if (f.existsAsFile()) return f; }   // inside Undae Sonicae.app
#elif defined(_WIN32)
    const char* name = "undae-pluginhost.exe";
#else
    const char* name = "undae-pluginhost";
#endif
    if (exe.getSiblingFile(name).existsAsFile()) return exe.getSiblingFile(name);
    for (auto d = exe.getParentDirectory(); !d.isRoot() && d.getParentDirectory() != d; d = d.getParentDirectory())   // development: cpp/build*/undae-pluginhost_artefacts/<config>/
        for (auto cfg : { "Release", "Debug", "RelWithDebInfo" }) { const auto f = d.getChildFile("undae-pluginhost_artefacts").getChildFile(cfg).getChildFile(name); if (f.existsAsFile()) return f; }
    return {};
}
// Folders the JUCE host scans besides the standard ones: UNDAE_PLUGIN_PATH (or UNDAE_VST_PATH), separated by ';'.
static std::string extraPluginFolders() {
    auto env = juce::SystemStats::getEnvironmentVariable("UNDAE_PLUGIN_PATH", ""); if (env.isEmpty()) env = juce::SystemStats::getEnvironmentVariable("UNDAE_VST_PATH", "");
    juce::StringArray dirs; for (auto& f : juce::StringArray::fromTokens(env, ";", "")) if (f.trim().isNotEmpty()) dirs.add(f.trim());
    return dirs.joinIntoString("\n").toStdString();
}
#if defined(__APPLE__)
// An Apple Silicon Mac with Rosetta: the (universal) host is started a second time as Intel code, for Intel-only
// plug-ins, which cannot load into an arm64 process. Each copy lists only the plug-ins it should serve (HostMain.cpp).
static bool appleSiliconWithRosetta() {
    int arm = 0; size_t n = sizeof arm;
    return sysctlbyname("hw.optional.arm64", &arm, &n, nullptr, 0) == 0 && arm == 1 && juce::File("/Library/Apple/usr/libexec/oah/libRosettaRuntime").exists();
}
#endif
void HostSet::startJuceHost(const std::string& ser) {   // with m held
    const auto exe = juceHostExe(); if (!exe.existsAsFile()) return;
    juceOn = true; pending++;
    std::thread([this, exe, ser] {
        Proc p = spawn(exe.getFullPathName().toStdString(), { "--child" }, { { "UNDAE_SERIAL", ser } }, 30);
        p.ep.folder = extraPluginFolders();
        { std::lock_guard<std::mutex> l(m); clapP = p; }
        pending--;
    }).detach();
#if defined(__APPLE__)
    if (appleSiliconWithRosetta()) {
        pending++;
        std::thread([this, exe, ser] {
            Proc p = spawn("/usr/bin/arch", { "-x86_64", exe.getFullPathName().toStdString(), "--child" }, { { "UNDAE_SERIAL", ser } }, 60);
            p.ep.folder = extraPluginFolders();
            { std::lock_guard<std::mutex> l(m); win64 = p; }
            pending--;
        }).detach();
    }
#endif
}

#if defined(_WIN32)
// The standard VST folders; the hosts take them as one string, one folder per line (scanPlugins splits it).
static std::string windowsPluginFolders(int bits) {
    juce::StringArray dirs;
    for (auto var : { bits == 64 ? "ProgramW6432" : "ProgramFiles(x86)" }) {
        const auto pf = juce::SystemStats::getEnvironmentVariable(var, bits == 64 ? "C:\\Program Files" : "C:\\Program Files (x86)");
        for (auto sub : { "Common Files\\VST3", "VSTPlugins", "Steinberg\\VSTPlugins", "Common Files\\VST2", "Common Files\\Steinberg\\VST2" }) {
            const juce::File d = juce::File(pf).getChildFile(sub); if (d.isDirectory()) dirs.add(d.getFullPathName());
        }
    }
    // more folders, separated by ';' (each host skips the plug-ins of the other bitness, so both can scan the same ones)
    for (auto& f : juce::StringArray::fromTokens(juce::SystemStats::getEnvironmentVariable("UNDAE_VST_PATH", ""), ";", ""))
        if (f.trim().isNotEmpty() && juce::File(f.trim()).isDirectory()) dirs.addIfNotAlreadyThere(juce::File(f.trim()).getFullPathName());
    return dirs.joinIntoString("\n").toStdString();
}

void HostSet::start(const juce::File& binDir, bool serial) {
    std::lock_guard<std::mutex> lk(m); if (started) return; started = true; bin = binDir;
    const std::string ser = serial ? "1" : "0";
    if (wantJuceHost()) { startJuceHost(ser); return; }   // testing the macOS host on Windows: it takes every plug-in
    clapP.ep.folder = bin.getFullPathName().toStdString();   // no CLAP host on Windows yet; CLAP slots pass audio through
    for (int bits : { 64, 32 }) {   // the VST hosts run directly; the 32-bit one under WoW64
        const juce::File exe = bin.getChildFile(bits == 64 ? "undae-vsthost.exe" : "undae-vsthost32.exe"); if (!exe.existsAsFile()) continue;
        pending++;
        std::thread([this, exe, bits, ser] {
            const std::string tok = juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()).toStdString() + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()).toStdString();
            Proc p = spawn(exe.getFullPathName().toStdString(), { "--child" }, { { "UNDAE_HOST_TOKEN", tok }, { "UNDAE_SERIAL", ser } }, 30);
            p.ep.folder = windowsPluginFolders(bits);
            { std::lock_guard<std::mutex> l(m); (bits == 64 ? win64 : win32) = p; }
            pending--;
        }).detach();
    }
}
void HostSet::stop() {
    std::lock_guard<std::mutex> lk(m);
    for (Proc* p : { &clapP, &win64, &win32 }) if (p->proc) {
        CloseHandle((HANDLE)p->in);   // the hosts exit when their stdin closes; give them a moment, then end them
        if (WaitForSingleObject((HANDLE)p->proc, 500) != WAIT_OBJECT_0) TerminateProcess((HANDLE)p->proc, 0);
        CloseHandle((HANDLE)p->proc); p->proc = p->in = nullptr;
    }
}
#else
void HostSet::start(const juce::File& binDir, bool serial) {
    std::lock_guard<std::mutex> lk(m); if (started) return; started = true; bin = binDir;
#if JUCE_ANDROID
    return;   // no host processes on Android: plug-ins come from a computer sharing them (the Wi-Fi plug-in server)
#endif
    signal(SIGPIPE, SIG_IGN);   // a host going away must not kill the app
    const std::string ser = serial ? "1" : "0";
    if (wantJuceHost()) { startJuceHost(ser); return; }   // macOS: Audio Units, VST3 and VST2 all go through this one host
    if (bin.getChildFile("undae-host").existsAsFile()) clapP = spawn(bin.getChildFile("undae-host").getFullPathName().toStdString(), { "--child" }, { { "UNDAE_SERIAL", ser } }, 10);
    clapP.ep.folder = bin.getFullPathName().toStdString();   // bundled test plug-ins live next to the host
    if (system("which wine > /dev/null 2>&1") != 0) return;
    const juce::File prefix = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(".local/share/undae-sonicae/wine"); prefix.getParentDirectory().createDirectory();
    const std::string folder = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(".vst-win").getFullPathName().toStdString();
    for (int bits : { 64, 32 }) {
        const juce::File exe = bin.getChildFile(bits == 64 ? "undae-vsthost.exe" : "undae-vsthost32.exe"); if (!exe.existsAsFile()) continue;
        pending++;
        std::thread([this, exe, bits, prefix, folder, ser] {
            const std::string tok = juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()).toStdString() + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()).toStdString();
            Proc p = spawn("wine", { exe.getFullPathName().toStdString(), "--child" }, { { "WINEPREFIX", prefix.getFullPathName().toStdString() }, { "WINEDEBUG", "-all" }, { "WINEDLLOVERRIDES", "mscoree,mshtml=" }, { "UNDAE_HOST_TOKEN", tok }, { "UNDAE_SERIAL", ser } }, 120);
            p.ep.folder = folder;
            { std::lock_guard<std::mutex> l(m); (bits == 64 ? win64 : win32) = p; }
            pending--;
        }).detach();
    }
}
void HostSet::stop() {
    std::lock_guard<std::mutex> lk(m);
    for (Proc* p : { &clapP, &win64, &win32 }) if (p->pid > 0) { close(p->in); kill(p->pid, SIGTERM); waitpid(p->pid, nullptr, WNOHANG); p->pid = -1; }
}
#endif
HostEndpoint HostSet::clap() const { std::lock_guard<std::mutex> lk(m); return remoteOn ? rClap : clapP.ep; }
HostEndpoint HostSet::win(int bits) const { std::lock_guard<std::mutex> lk(m); if (remoteOn) return bits == 32 ? rWin32 : rWin64; return bits == 32 ? win32.ep : win64.ep; }

static std::intptr_t dial(const std::string& host, int port) {   // TCP to host:port (a name or an address); -1 on failure
    netInit();
    addrinfo hints{}, *res = nullptr; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) || !res) return -1;
    const SockT s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
#if defined(_WIN32)
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return -1; }
#else
    if (s < 0) { freeaddrinfo(res); return -1; }
#endif
    const std::intptr_t fd = (std::intptr_t)s;
    noSigPipe(fd); setTimeout(fd, SO_SNDTIMEO, 5);
    if (::connect(s, res->ai_addr, (int)res->ai_addrlen)) { closeSock(fd); freeaddrinfo(res); return -1; }
    freeaddrinfo(res); return fd;
}
std::string HostSet::useServer(const std::string& addrIn, const std::string& code, std::string* name) {
    std::string addr = addrIn; const auto sch = addr.find("://"); if (sch != std::string::npos) addr = addr.substr(sch + 3); if (addr.find('/') != std::string::npos) addr = addr.substr(0, addr.find('/'));
    if (addr.empty()) { std::lock_guard<std::mutex> lk(m); remoteOn = false; return {}; }
    std::string host = addr; int port = 47800; const auto c = addr.rfind(':'); if (c != std::string::npos) { host = addr.substr(0, c); port = std::atoi(addr.substr(c + 1).c_str()); }
    const std::intptr_t fd = dial(host, port); if (fd < 0) return "No answer from " + host + ":" + std::to_string(port) + ". Is sharing on there, and are both on the same network?";
    setTimeout(fd, SO_RCVTIMEO, 5);
    const std::string req = "GET /share.json?code=" + code + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n"; send((SockT)fd, req.data(), (int)req.size(), kSendFlags);
    std::string resp; char buf[4096]; for (;;) { const auto k = recv((SockT)fd, buf, (int)sizeof buf, 0); if (k <= 0) break; resp.append(buf, (size_t)k); } closeSock(fd);
    if (resp.find(" 403 ") != std::string::npos) return "Wrong pairing code";
    if (resp.find(" 429 ") != std::string::npos) return "Too many wrong codes: wait a minute";
    const auto b0 = resp.find('{'), b1 = resp.rfind('}'); juce::var j;   // the JSON body (Node sends it chunked)
    if (b0 != std::string::npos && b1 != std::string::npos && b1 > b0) juce::JSON::parse(juce::String(resp.substr(b0, b1 - b0 + 1)), j);
    if (!(bool)j.getProperty("ok", false)) return "That computer answered, but not as a plug-in server";
    auto ep = [&](const char* which, bool have, const juce::var& folder) { HostEndpoint e; if (!have) return e; e.host = host; e.port = port; e.path = std::string("/") + which + "?code=" + code; e.folder = folder.toString().toStdString(); return e; };
    std::lock_guard<std::mutex> lk(m);
    rClap = ep("clap", (bool)j.getProperty("clap", false), j.getProperty("bundled", ""));
    rWin64 = ep("win", j.getProperty("win", juce::var()).isObject(), j.getProperty("win", juce::var()).getProperty("folder", ""));
    rWin32 = ep("win32", j.getProperty("win32", juce::var()).isObject(), j.getProperty("win32", juce::var()).getProperty("folder", ""));
    remoteOn = true; if (name) *name = j.getProperty("name", "").toString().toStdString();
    return {};
}

static bool isWindowsSlot(const juce::var& s) {   // true: the second host (Windows VSTs; with the JUCE host, Intel-only plug-ins under Rosetta)
    if (HostSet::get().juceHost()) return (bool)s.getProperty("rosetta", false);   // the JUCE host takes every format
    const auto fmt = s.getProperty("fmt", "").toString();
    if (fmt == "vst2") return true;
#if defined(_WIN32)
    if (fmt == "vst3") return true;   // every VST3 here is a Windows one, in the VST host
#endif
    if (fmt == "vst3") { const auto w = s.getProperty("windows", juce::var()); return w.isVoid() ? s.getProperty("path", "").toString().contains("/.vst-win/") : (bool)w; }
    return false;
}
HostEndpoint endpointFor(const juce::var& s) { return isWindowsSlot(s) ? HostSet::get().win((int)s.getProperty("bits", 64) == 32 ? 32 : 64) : HostSet::get().clap(); }

// --- WebSocket client ----------------------------------------------------------------------------------------------
static bool writeAll(std::intptr_t fd, const void* p, size_t n) { auto* c = (const char*)p; while (n) { const auto k = send((SockT)fd, c, (int)std::min<size_t>(n, 1 << 30), kSendFlags); if (k <= 0) return false; c += k; n -= (size_t)k; } return true; }
static bool readAll(std::intptr_t fd, void* p, size_t n) { auto* c = (char*)p; while (n) { const auto k = recv((SockT)fd, c, (int)std::min<size_t>(n, 1 << 30), 0); if (k <= 0) return false; c += k; n -= (size_t)k; } return true; }

bool PluginInstance::connect(const HostEndpoint& ep) {
    fd = dial(ep.host, ep.port); if (fd < 0) { fd = -1; return false; }
    remote = ep.host != "127.0.0.1";
    int one = 1; setsockopt((SockT)fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);   // small audio blocks: no batching
    const std::string req = "GET " + (ep.path.empty() ? "/?t=" + ep.token : ep.path) + " HTTP/1.1\r\nHost: " + ep.host + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    if (!writeAll(fd, req.data(), req.size())) return false;
    std::string resp; char c;
    while (resp.find("\r\n\r\n") == std::string::npos && resp.size() < 4096) { if (recv((SockT)fd, &c, 1, 0) <= 0) return false; resp += c; }
    if (resp.find(" 101 ") == std::string::npos) { closeSock(fd); fd = -1; return false; }
    running = true; reader = std::thread([this] { readLoop(); });
    return true;
}
bool PluginInstance::sendFrame(int op, const void* data, size_t n) {   // client frames are masked (with a zero key)
    uint8_t h[14]; size_t hn = 2; h[0] = (uint8_t)(0x80 | op);
    if (n < 126) h[1] = (uint8_t)(0x80 | n); else if (n < 65536) { h[1] = 0x80 | 126; h[2] = (uint8_t)(n >> 8); h[3] = (uint8_t)n; hn = 4; }
    else { h[1] = 0x80 | 127; for (int i = 0; i < 8; i++) h[2 + i] = (uint8_t)((uint64_t)n >> (8 * (7 - i))); hn = 10; }
    std::memset(h + hn, 0, 4); hn += 4;
    std::lock_guard<std::mutex> lk(wm); if (fd < 0) return false;
    return writeAll(fd, h, hn) && writeAll(fd, data, n);
}
void PluginInstance::sendJson(const juce::var& v) { const auto s = juce::JSON::toString(v, true).toStdString(); sendFrame(1, s.data(), s.size()); }
void PluginInstance::readLoop() {
    std::vector<uint8_t> msg; int msgOp = 0;
    while (running) {
        uint8_t h[2], ext[8]; if (!readAll(fd, h, 2)) break;
        const bool fin = h[0] & 0x80; const int op = h[0] & 15; uint64_t n = h[1] & 127;
        if (n == 126) { if (!readAll(fd, ext, 2)) break; n = (uint64_t)ext[0] << 8 | ext[1]; }
        else if (n == 127) { if (!readAll(fd, ext, 8)) break; n = 0; for (int i = 0; i < 8; i++) n = n << 8 | ext[i]; }
        if (n > (64u << 20)) break;
        const size_t at = msg.size(); msg.resize(at + n); if (n && !readAll(fd, msg.data() + at, n)) break;
        if (op == 8) break;
        if (op != 0) msgOp = op;
        if (!fin) continue;
        if (msgOp == 2 && msg.size() >= 12) {
            uint32_t cnt; std::memcpy(&cnt, msg.data() + 8, 4); std::vector<float> f((size_t)cnt * 2); std::memcpy(f.data(), msg.data() + 12, std::min(f.size() * 4, msg.size() - 12));
            { std::lock_guard<std::mutex> lk(am); replies.push_back(std::move(f)); } acv.notify_all();
        } else if (msgOp == 1) {
            juce::var j; juce::JSON::parse(juce::String::fromUTF8((const char*)msg.data(), (int)msg.size()), j);
            const int s = (int)j.getProperty("seq", 0);
            if (s) { { std::lock_guard<std::mutex> lk(rm); answers[s] = j; } rcv.notify_all(); }
            else {
                const auto ev = j.getProperty("ev", "").toString(); Event e{ -1, {}, false };
                if (ev == "param") { e.kind = 0; if (auto* a = j.getProperty("changes", juce::var()).getArray()) for (auto& c : *a) { const int id = (int)c.getProperty("id", 0); const double v = (double)c.getProperty("value", 0.0); e.changes.push_back({ id, v }); std::lock_guard<std::mutex> l(pm); sent[id] = v; } }
                else if (ev == "dirty") e.kind = 1;
                else if (ev == "gui") { e.kind = 2; e.open = (bool)j.getProperty("open", false); }
                if (e.kind >= 0) { std::lock_guard<std::mutex> lk(em); events.push_back(std::move(e)); }
            }
        }
        msg.clear();
    }
    running = false; acv.notify_all(); rcv.notify_all();
}

PluginInstance::~PluginInstance() {
    running = false;
    if (fd >= 0) { shutSock(fd); }
    if (reader.joinable()) reader.join();
    if (fd >= 0) closeSock(fd);
}

juce::var PluginInstance::request(const juce::var& msg, int timeoutMs) {
    int s; { std::lock_guard<std::mutex> lk(rm); s = ++seq; }
    juce::var m = juce::JSON::parse(juce::JSON::toString(msg)); m.getDynamicObject()->setProperty("seq", s);
    sendJson(m);
    std::unique_lock<std::mutex> lk(rm);
    rcv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return answers.count(s) || !running; });
    auto it = answers.find(s); if (it == answers.end()) return {};
    juce::var r = it->second; answers.erase(it); return r;
}

static juce::var obj(std::initializer_list<std::pair<const char*, juce::var>> kv) { auto* o = new juce::DynamicObject(); for (auto& [k, v] : kv) o->setProperty(k, v); return juce::var(o); }

bool PluginInstance::load(const juce::var& slot, double sr, std::string& error) {
    const HostEndpoint ep = endpointFor(slot);
    if (!ep.ok()) {
        if (HostSet::get().juceHost() && isWindowsSlot(slot)) error = HostSet::get().winPending() ? "The Intel plug-in host is still starting" : "This Intel-only plug-in needs Rosetta: run softwareupdate --install-rosetta, then restart Undae Sonicae";
        else error = isWindowsSlot(slot) ? (HostSet::get().winPending() ? "Wine is still starting" : "Windows plug-ins need Wine and the Windows plug-in host") : "The plug-in host is not running";
        return false;
    }
    if (!connect(ep)) { error = "The plug-in host is not reachable"; return false; }
    juce::var r = request(obj({ { "op", "load" }, { "path", slot.getProperty("path", "") }, { "id", slot.getProperty("pid", "") }, { "sampleRate", sr } }), 60000);
    if (!(bool)r.getProperty("ok", false)) { error = r.getProperty("error", "The plug-in could not be loaded").toString().toStdString(); return false; }
    inf.name = r.getProperty("name", "").toString().toStdString(); inf.vendor = r.getProperty("vendor", "").toString().toStdString();
    inf.inputs = (int)r.getProperty("inputs", 2); inf.outputs = (int)r.getProperty("outputs", 2); inf.notes = (bool)r.getProperty("notes", false); inf.gui = (bool)r.getProperty("gui", false);
    if (auto* a = r.getProperty("params", juce::var()).getArray())
        for (auto& q : *a) inf.params.push_back({ (int)q.getProperty("id", 0), q.getProperty("name", "").toString().toStdString(), q.getProperty("module", "").toString().toStdString(), q.getProperty("text", "").toString().toStdString(),
            (double)q.getProperty("min", 0.0), (double)q.getProperty("max", 1.0), (double)q.getProperty("def", 0.0), (double)q.getProperty("value", 0.0), (bool)q.getProperty("stepped", false), (bool)q.getProperty("hidden", false), (bool)q.getProperty("readonly", false) });
    const auto st = slot.getProperty("state", "").toString(); if (st.isNotEmpty()) request(obj({ { "op", "state" }, { "data", st } }), 30000);
    for (auto& q : inf.params) { const juce::var v = slot.getProperty(juce::Identifier("v" + juce::String(q.id)), juce::var()); if (v.isDouble() || v.isInt()) { param(q.id, (double)v); q.value = (double)v; } else { std::lock_guard<std::mutex> l(pm); sent[q.id] = q.value; } }
    loaded = true; return true;
}

bool PluginInstance::process(float* L, float* R, int n, uint32_t frame, int timeoutMs) {
    if (!loaded || !running) return false;
    std::vector<uint8_t> buf(12 + (size_t)n * 8); const uint32_t hd[3] = { 1, frame, (uint32_t)n }; std::memcpy(buf.data(), hd, 12);
    float* f = (float*)(buf.data() + 12); for (int i = 0; i < n; i++) { f[i * 2] = L[i]; f[i * 2 + 1] = R[i]; }
    if (!sendFrame(2, buf.data(), buf.size())) return false;
    if (remote) {   // over the network: answers arrive a few blocks later; play from a buffer of about 8 blocks, re-filled after a gap
        constexpr size_t K = 8;
        std::vector<float> r;
        { std::lock_guard<std::mutex> lk(am); if (!primed && replies.size() >= K) primed = true;
          if (primed && !replies.empty()) { r = std::move(replies.front()); replies.pop_front(); } else if (primed) { primed = false; underruns++; }
          while (replies.size() > 4 * K) replies.pop_front(); }   // fell far behind: drop the oldest so the delay does not grow
        if (r.empty()) { std::fill_n(L, n, 0.f); std::fill_n(R, n, 0.f); return true; }
        const int m = std::min(n, (int)r.size() / 2); for (int i = 0; i < m; i++) { L[i] = r[(size_t)i * 2]; R[i] = r[(size_t)i * 2 + 1]; }
        return true;
    }
    std::unique_lock<std::mutex> lk(am);
    if (!acv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return !replies.empty() || !running; }) || replies.empty()) return false;
    auto r = std::move(replies.front()); replies.pop_front(); lk.unlock();
    const int m = std::min(n, (int)r.size() / 2); for (int i = 0; i < m; i++) { L[i] = r[(size_t)i * 2]; R[i] = r[(size_t)i * 2 + 1]; }
    // a reply that came after the timeout last time would now be one block late: drop stale ones
    std::lock_guard<std::mutex> l2(am); while (replies.size() > 0) replies.pop_front();
    return true;
}
void PluginInstance::note(bool on, int key, double vel, uint32_t at) { if (loaded) sendJson(obj({ { "op", "note" }, { "on", on }, { "key", key }, { "vel", vel }, { "at", (juce::int64)at } })); }
void PluginInstance::midi(int s, int d1, int d2, uint32_t at) { if (!loaded) return; juce::Array<juce::var> d{ s, d1, d2 }; sendJson(obj({ { "op", "midi" }, { "data", d }, { "at", (juce::int64)at } })); }
void PluginInstance::param(int id, double v) { { std::lock_guard<std::mutex> l(pm); sent[id] = v; } sendJson(obj({ { "op", "param" }, { "id", id }, { "value", v } })); }
void PluginInstance::panic() { if (loaded) sendJson(obj({ { "op", "panic" } })); }
std::string PluginInstance::readState() { auto r = request(obj({ { "op", "state" } }), 10000); return (bool)r.getProperty("ok", false) ? r.getProperty("data", "").toString().toStdString() : std::string(); }
bool PluginInstance::showWindow(bool show) { auto r = request(obj({ { "op", "gui" }, { "show", show } }), 10000); return (bool)r.getProperty("open", false); }
std::vector<PluginInstance::Event> PluginInstance::takeEvents() { std::lock_guard<std::mutex> lk(em); std::vector<Event> e; e.swap(events); return e; }

// --- scanning ------------------------------------------------------------------------------------------------------
std::vector<juce::var> scanPlugins() {
    std::vector<juce::var> out; std::mutex om;
    auto run = [&](HostEndpoint ep, bool windows, int bits, int timeout) {
        if (!ep.ok()) return;
        PluginInstance c; if (!c.attach(ep)) return;
        juce::Array<juce::var> paths;   // one folder, or several, one per line (the Windows VST folders)
        for (auto& f : juce::StringArray::fromLines(juce::String::fromUTF8(ep.folder.c_str()))) if (f.isNotEmpty()) paths.add(f);
        auto r = c.request(obj({ { "op", "scan" }, { "paths", paths } }), timeout);
        if (auto* a = r.getProperty("plugins", juce::var()).getArray())
            for (auto& pl : *a) {
                juce::var x = juce::JSON::parse(juce::JSON::toString(pl)); auto* o = x.getDynamicObject(); if (!o) continue;
                if (windows && HostSet::get().juceHost()) o->setProperty("rosetta", true);   // from the Intel copy of the JUCE host
                else if (windows) { o->setProperty("windows", true); if (bits == 32) o->setProperty("bits", 32); }
                std::lock_guard<std::mutex> lk(om); out.push_back(x);
            }
    };
    // (the JUCE host scans every installed AU / VST3 / VST2, so it gets longer)
    std::thread a([&] { run(HostSet::get().clap(), false, 64, HostSet::get().juceHost() ? 300000 : 30000); }), b([&] { run(HostSet::get().win(64), true, 64, 120000); }), c([&] { run(HostSet::get().win(32), true, 32, 120000); });
    a.join(); b.join(); c.join();
    return out;
}

}   // namespace us
