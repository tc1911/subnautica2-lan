#include <windows.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "config.h"
#include "control.h"
#include "hooks.h"
#include "log.h"
#include "crashlog.h"
#include "game_overlay.h"
#include "overlay.h"
#include "tunnel.h"

namespace {

std::mutex g_mutex;
std::atomic<bool> g_started{false};
std::atomic<bool> g_stopRequested{false};
std::atomic<bool> g_enabled{false};

// Allocated once and never destroyed: the EOS detours keep a raw pointer to
// it, so freeing it on stop would be a use-after-free. stop() makes it inert.
sn2lan::Tunnel* g_tunnel = nullptr;
sn2lan::Config g_config;
std::wstring g_moduleDir;
std::wstring g_configPath;
std::wstring g_logPath;
std::thread g_supervisor;
sn2lan::control::Server g_control;

std::atomic<int> g_probeStatus{0};  // 0=unknown, 1=not ready, 2=ready

std::mutex g_noticeMutex;
std::string g_notice;
std::string g_noticeEn;
uint64_t g_noticeUntil = 0;

void setNotice(const std::string& text, const std::string& textEn, uint64_t ms = 5000) {
    std::lock_guard<std::mutex> lock(g_noticeMutex);
    g_notice = text;
    g_noticeEn = textEn;
    g_noticeUntil = GetTickCount64() + ms;
}

bool parseHostPort(const std::string& target, std::string* host, uint16_t* port) {
    const size_t colon = target.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= target.size()) {
        if (target.empty()) {
            return false;
        }
        *host = target;
    } else {
        *host = target.substr(0, colon);
        const long value = std::strtol(target.c_str() + colon + 1, nullptr, 10);
        if (value <= 0 || value > 65535) {
            return false;
        }
        *port = static_cast<uint16_t>(value);
    }
    return !host->empty();
}

std::wstring envPath(const wchar_t* name) {
    wchar_t buffer[32768] = {0};
    const DWORD len = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    if (len == 0 || len >= std::size(buffer)) {
        return std::wstring();
    }
    return std::wstring(buffer, len);
}

std::wstring moduleDir() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&moduleDir), &module);
    wchar_t path[MAX_PATH] = {0};
    if (module == nullptr || GetModuleFileNameW(module, path, MAX_PATH) == 0) {
        return L".";
    }
    std::wstring dir(path);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L".";
    }
    return dir.substr(0, slash);
}

std::wstring joinPath(const std::wstring& dir, const wchar_t* name) {
    std::wstring out = dir;
    if (!out.empty() && out.back() != L'\\' && out.back() != L'/') {
        out += L'\\';
    }
    out += name;
    return out;
}

std::string identitySummary() {
    const std::string preview = sn2lan::loginOptionsPreview();
    if (!preview.empty()) {
        return "身份: 已读取账号登录信息（含 SonarPlayerId/SteamID）";
    }
    return "身份: 等待账号登录，暂时仅 SteamID（物品仍保留）";
}

void pushOverlayState() {
    if (g_tunnel == nullptr) {
        return;
    }
    sn2lan::OverlayState state;
    state.isHost = g_config.isHost;
    state.ipMode = g_config.forceIpDriver;
    state.controlPort = g_config.controlPort;
    state.hostAddress = g_config.hostAddress;
    const bool ready = g_tunnel->hostReady();
    const size_t peers = g_tunnel->peerCount();
    const bool issued = sn2lan::joinIssued();
    state.hostReady = ready;
    state.joinIssued = issued;
    state.peerCount = static_cast<int>(peers);
    state.probeStatus = g_probeStatus.load(std::memory_order_relaxed);
    state.identityReady = !sn2lan::loginOptionsPreview().empty();

    if (g_config.isHost) {
        state.target = "127.0.0.1:" + std::to_string(g_config.controlPort);
        state.line1 = g_config.forceIpDriver
                          ? "角色: 主机 · TCP/UDP " + std::to_string(g_config.controlPort)
                          : "角色: 主机 · 游戏 UDP 7777 · 隧道 UDP 28015";
        state.line2 = ready ? "状态: 房间已开放，等待玩家加入" : "状态: 请在游戏内点「创建多人游戏」";
        state.line2 += " · 直连客户端 " + std::to_string(peers);
    } else {
        state.target = g_config.autoConnect.empty()
                           ? g_config.hostAddress + ":" + std::to_string(g_config.controlPort)
                           : g_config.autoConnect;
        state.line1 = "角色: 客户端 · 目标 " + state.target;
        const int probe = g_probeStatus.load(std::memory_order_relaxed);
        if (issued) {
            state.line2 = "状态: 已发送连接请求，正在载入…";
        } else if (probe == 2 || ready) {
            state.line2 = "状态: 主机已就绪，可以连接";
        } else if (probe == 1) {
            state.line2 = "状态: 主机在线，但还没开房";
        } else {
            state.line2 = "状态: 未检测到主机（也可以直接尝试连接）";
        }
        state.line2 += " · 隧道 peer " + std::to_string(peers);
    }
    state.line3 = identitySummary();
    state.message = "F8 显示/隐藏 · 主机请先在游戏内创建多人游戏";
    {
        std::lock_guard<std::mutex> lock(g_noticeMutex);
        if (!g_notice.empty() && GetTickCount64() < g_noticeUntil) {
            state.message = g_notice;
            state.messageEn = g_noticeEn;
        }
    }
    sn2lan::overlaySetState(state);
    sn2lan::gameOverlaySetState(state);
}

void handleJoinRequest(const std::string& target);

void supervisorLoop() {
    int attempt = 0;
    const uint64_t startedAt = GetTickCount64();
    bool overlayFallbackStarted = false;
    bool overlayFallbackResolved = false;
    uint64_t lastHealthLog = GetTickCount64();
    while (!g_stopRequested.load(std::memory_order_relaxed)) {
        if (g_enabled.load(std::memory_order_relaxed) && !sn2lan::hooksInstalled()) {
            ++attempt;
            if (sn2lan::installHooks(g_tunnel)) {
                SN2_LOG_INFO("EOS hooks ready after %d attempt(s)", attempt);
            } else {
                SN2_LOG_WARN("hook attempt %d failed, retrying in 1s", attempt);
            }
        }

        const uint64_t now = GetTickCount64();
        if (g_enabled.load(std::memory_order_relaxed) && now - lastHealthLog >= 60000 && g_tunnel) {
            lastHealthLog = now;
            const sn2lan::Tunnel::Stats stats = g_tunnel->stats();
            SN2_LOG_INFO("health: sent=%llu recv=%llu bytes_out=%llu bytes_in=%llu parse_errors=%llu queue_drops=%llu %s",
                         static_cast<unsigned long long>(stats.datagramsSent),
                         static_cast<unsigned long long>(stats.datagramsReceived),
                         static_cast<unsigned long long>(stats.bytesSent),
                         static_cast<unsigned long long>(stats.bytesReceived),
                         static_cast<unsigned long long>(stats.parseErrors),
                         static_cast<unsigned long long>(stats.queueDrops),
                         sn2lan::hookStatus().c_str());
        }

        if (g_config.overlay && g_config.overlayMode == "imgui" && !overlayFallbackResolved) {
            const uint64_t frames = sn2lan::gameOverlayFrameCount();
            if (!overlayFallbackStarted && now - startedAt >= 60000 && frames == 0) {
                overlayFallbackStarted = true;
                SN2_LOG_WARN("no ImGui frames after 60s; falling back to the desktop overlay window");
                sn2lan::overlayStart(&handleJoinRequest);
            } else if (overlayFallbackStarted && frames > 0) {
                overlayFallbackResolved = true;
                SN2_LOG_INFO("ImGui frames arrived; closing the desktop overlay window");
                sn2lan::overlayStop();
            } else if (frames > 0) {
                overlayFallbackResolved = true;
            }
        }

        // Retry hook installation quickly while the EOS SDK is still missing,
        // then settle into a slow health-check cadence.
        if (g_enabled.load(std::memory_order_relaxed)) {
            pushOverlayState();
        }

        const int steps = sn2lan::hooksInstalled() ? 50 : 10;
        for (int i = 0; i < steps && !g_stopRequested.load(std::memory_order_relaxed); ++i) {
            Sleep(100);
        }
    }
}

void handleJoinRequest(const std::string& target) {
    if (!g_config.forceIpDriver) {
        sn2lan::requestJoin(target);
        return;
    }
    std::thread([target]() {
        std::string host;
        uint16_t port = g_config.controlPort;
        if (!parseHostPort(target, &host, &port)) {
            setNotice("地址格式应为 ip:端口", "Use the ip:port format");
            return;
        }
        setNotice("正在连接 " + host + ":" + std::to_string(port) + " …",
                  "Connecting to " + host + ":" + std::to_string(port) + " ...", 15000);
        sn2lan::control::Status status = sn2lan::control::Status::NotReady;
        std::string error;
        if (!sn2lan::control::probe(host, port, &status, &error)) {
            SN2_LOG_WARN("handshake with %s:%u failed: %s", host.c_str(), port, error.c_str());
            g_probeStatus.store(0);
            setNotice("连接失败: " + error, "Connect failed: " + error, 8000);
            return;
        }
        if (status == sn2lan::control::Status::NotReady) {
            g_probeStatus.store(1);
            setNotice("主机还没点「创建多人游戏」", "Host is online but has not created a game yet", 8000);
            return;
        }
        if (status == sn2lan::control::Status::VersionMismatch) {
            setNotice("主机与客户端 mod 版本不一致", "Host and client mod versions differ", 8000);
            return;
        }
        SN2_LOG_INFO("handshake ok, joining %s:%u", host.c_str(), port);
        g_probeStatus.store(2);
        setNotice("握手成功，正在进图…", "Handshake OK, joining ...", 8000);
        sn2lan::overlayHide();
        sn2lan::gameOverlayHide();
        sn2lan::requestJoin(host + ":" + std::to_string(port));
    }).detach();
}

bool startInternal() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_started.load(std::memory_order_relaxed)) {
        return true;
    }

    g_moduleDir = moduleDir();
    // Env overrides keep multiple instances (and the Wine integration test)
    // from sharing one config/log file.
    g_configPath = envPath(L"SN2LAN_CONFIG");
    if (g_configPath.empty()) {
        g_configPath = joinPath(g_moduleDir, L"config.ini");
    }
    g_logPath = envPath(L"SN2LAN_LOG");
    if (g_logPath.empty()) {
        g_logPath = joinPath(g_moduleDir, L"sn2lan.log");
    }

    std::string error;
    const bool configOk = sn2lan::loadConfig(g_configPath, &g_config, &error);
    sn2lan::logInit(g_logPath, g_config.debug);
    sn2lan::installCrashLogger();
    SN2_LOG_INFO("crash logger installed");

    SN2_LOG_INFO("SN2LAN 0.1.0 (build " __DATE__ " " __TIME__ ") loading from %ls", g_moduleDir.c_str());

    if (!configOk) {
        SN2_LOG_ERROR("cannot load config.ini: %s", error.c_str());
        return false;
    }
    if (!g_config.enabled) {
        SN2_LOG_INFO("tunnel disabled in config.ini, staying idle");
        g_started.store(true);
        return true;
    }

    sn2lan::Tunnel::Options options;
    options.isHost = g_config.isHost;
    options.socketName = g_config.socketName;
    options.bindAddress = g_config.bindAddress;
    options.bindPort = g_config.bindPort;
    options.hostAddress = g_config.hostAddress;
    options.hostPort = g_config.hostPort;
    options.mtu = g_config.mtu;
    options.debug = g_config.debug;
    options.passthroughUntilSeen = g_config.passthroughUntilSeen;
    options.peerTimeoutMs = g_config.peerTimeoutMs;
    options.forceIpDriver = g_config.forceIpDriver;
    options.autoConnect = g_config.autoConnect;
    options.loginOptions = g_config.loginOptions;
    options.autoConnectDelaySec = g_config.autoConnectDelaySec;

    if (g_tunnel == nullptr) {
        g_tunnel = new sn2lan::Tunnel();
    }
    if (g_config.forceIpDriver) {
        // IP mode: TCP 7777 is the control channel, UDP 7777 belongs to the
        // game's IpNetDriver. The tunnel only carries options and identity.
        g_tunnel->configure(options);
    } else if (!g_tunnel->start(options)) {
        SN2_LOG_ERROR("tunnel failed to start");
        return false;
    }

    g_stopRequested.store(false);
    g_enabled.store(true);
    g_started.store(true);
    g_supervisor = std::thread(supervisorLoop);
    // Try once synchronously; the supervisor keeps retrying if EOS is not up yet.
    sn2lan::installHooks(g_tunnel);
    if (g_config.forceIpDriver && g_config.isHost) {
        const bool ok = g_control.start(g_config.controlPort, []() {
            return g_tunnel != nullptr && g_tunnel->hostReady();
        });
        if (ok) {
            SN2_LOG_INFO("control server listening on TCP %u", g_config.controlPort);
        } else {
            SN2_LOG_WARN("control server failed to bind TCP %u (frp/firewall?)", g_config.controlPort);
        }
    }

    if (g_config.overlay) {
        sn2lan::gameOverlaySetJoinHandler(&handleJoinRequest);
        if (g_config.overlayMode == "window") {
            sn2lan::overlayStart(&handleJoinRequest);
        } else if (g_config.overlayMode == "imgui") {
            SN2_LOG_INFO("in-game overlay enabled (F8); waiting for ImGui frames");
        }
        pushOverlayState();
    }
    SN2_LOG_INFO("SN2LAN started (role=%s, mode=%s, passthrough_until_seen=%d)",
                 g_config.isHost ? "host" : "client", g_config.forceIpDriver ? "ip" : "eos",
                 g_config.passthroughUntilSeen ? 1 : 0);
    return true;
}

void stopInternal() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_started.exchange(false, std::memory_order_relaxed)) {
        return;
    }
    g_enabled.store(false);
    g_stopRequested.store(true);
    if (g_supervisor.joinable()) {
        g_supervisor.join();
    }
    // Detach the detours from the tunnel but leave them installed, then stop
    // the socket. The Tunnel object stays alive until process exit.
    sn2lan::removeHooks();
    g_control.stop();
    sn2lan::overlayStop();
    if (g_tunnel != nullptr) {
        g_tunnel->stop();
    }
    SN2_LOG_INFO("SN2LAN stopped");
    sn2lan::logShutdown();
}

}  // namespace

extern "C" __declspec(dllexport) int luaopen_sn2lan(void* /*lua_State*/) {
    // Initialise without touching the Lua stack; the Lua mod only needs this
    // call to happen once UE4SS has loaded us.
    startInternal();
    return 0;
}

extern "C" __declspec(dllexport) void sn2lan_start() {
    startInternal();
}

extern "C" __declspec(dllexport) void sn2lan_stop() {
    stopInternal();
}

extern "C" __declspec(dllexport) const char* sn2lan_status() {
    static thread_local char buffer[512];
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool running = g_tunnel && g_tunnel->running();
    if (!g_started.load(std::memory_order_relaxed)) {
        std::snprintf(buffer, sizeof(buffer), "state=stopped enabled=%d", g_enabled.load() ? 1 : 0);
        return buffer;
    }
    if (!g_enabled.load(std::memory_order_relaxed)) {
        std::snprintf(buffer, sizeof(buffer), "state=idle (disabled in config)");
        return buffer;
    }
    const sn2lan::Tunnel::Stats stats = g_tunnel ? g_tunnel->stats() : sn2lan::Tunnel::Stats{};
    std::snprintf(buffer, sizeof(buffer),
                  "state=%s role=%s hooks=%d sent=%llu recv=%llu parse_errors=%llu queue_drops=%llu %s",
                  running ? "running" : "starting", g_config.isHost ? "host" : "client",
                  sn2lan::hooksInstalled() ? 1 : 0, static_cast<unsigned long long>(stats.datagramsSent),
                  static_cast<unsigned long long>(stats.datagramsReceived),
                  static_cast<unsigned long long>(stats.parseErrors),
                  static_cast<unsigned long long>(stats.queueDrops), sn2lan::hookStatus().c_str());
    return buffer;
}

extern "C" __declspec(dllexport) const char* sn2lan_version() {
    return "0.1.0";
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
#if defined(SN2LAN_STANDALONE)
        // Loaded as the dwmapi proxy while the process is starting. Do the
        // real work (file IO, sockets, MinHook) on a worker thread because
        // DllMain runs under the loader lock.
        CreateThread(nullptr, 0, [](LPVOID) -> DWORD { startInternal(); return 0; }, nullptr, 0, nullptr);
#endif
    } else if (reason == DLL_PROCESS_DETACH) {
        // Runs under the loader lock. Do not call MinHook or join threads here
        // (MH_DisableHook freezes every thread and would deadlock); just make
        // the detours inert and let process teardown reclaim everything.
        g_stopRequested.store(true);
        g_enabled.store(false);
        sn2lan::removeHooks();
    }
    return TRUE;
}
