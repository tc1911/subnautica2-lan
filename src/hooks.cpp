#include "hooks.h"

#include "game_overlay.h"

#include <filesystem>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

#include "MinHook.h"
#include "eos_abi.h"
#include "log.h"
#include "tunnel.h"

namespace sn2lan {

namespace {

constexpr const wchar_t* kEosModuleName = L"EOSSDK-Win64-Shipping.dll";
constexpr int32_t kSocketIdApiVersion = 1;

std::atomic<Tunnel*> g_tunnel{nullptr};
// Serialises installation; the supervisor thread and startInternal() both call
// installHooks() during startup.
std::mutex g_installMutex;
std::atomic<bool> g_hooksCreated{false};

// Manual join requested from the overlay.
std::mutex g_joinMutex;
std::string g_joinTarget;
std::atomic<bool> g_joinRequested{false};

EosApi g_api;
PFN_EOS_P2P_SendPacket g_originalSendPacket = nullptr;
PFN_EOS_P2P_ReceivePacket g_originalReceivePacket = nullptr;
PFN_EOS_P2P_GetNextReceivedPacketSize g_originalGetNextReceivedPacketSize = nullptr;

// --- engine net driver substitution (build 533) -------------------------
// CreateNetDriver_Local loads the driver class through StaticLoadClass with
// UNetDriver::StaticClass() as the base. Intercepting that single call and
// returning UIpNetDriver lets the engine's own construction/registration path
// run unchanged, which is far safer than reimplementing NetDriver creation.
//
// StaticLoadClass signature (from the call at 0x144FBC5DA):
//   UClass* (UClass* BaseClass, UObject* InOuter,
//            const TStringView* ClassName, const TStringView* Filename,
//            uint32 LoadFlags, UPackageMap* Sandbox)
using VoiceChatTickFn = void (*)(void* self, float deltaSeconds);

using StaticLoadClassFn = void* (*)(void* baseClass, void* outer, const void* className, const void* filename,
                                    uint32_t loadFlags, void* sandbox);
StaticLoadClassFn g_originalStaticLoadClass = nullptr;
void* g_staticLoadClassTarget = nullptr;
VoiceChatTickFn g_originalVoiceChatTick = nullptr;
void* g_voiceChatTickTarget = nullptr;
std::atomic<void*> g_unetDriverClass{nullptr};
std::atomic<bool> g_loggedRequestedDriverClass{false};

// Image-base relative addresses from the build 533 PDB.
constexpr uintptr_t kStaticLoadClassRva = 0x1878D50;
constexpr uintptr_t kUNetDriverClassGetterRva = 0x49665A0;
constexpr uintptr_t kIpNetDriverStaticClassRva = 0x5947B00;
// UUWEVoiceChatWorldSubsystem::Tick - crashes when there is no EOS voice
// session, which is always the case on a plain UDP (frp) connection.
constexpr uintptr_t kVoiceChatTickRva = 0x682B990;
constexpr int32_t kStringViewLenOffset = 8;

// --- client auto-connect (plain UDP mode) -------------------------------
// UWorld::Tick runs every frame and hands us the world pointer directly.
// SetClientTravel only stores the pending URL; the world processes it at the
// end of the frame, so calling it from here is safe.
using WorldTickFn = void (*)(void* world, int32_t tickType, float deltaSeconds);

using ImGuiNewFrameFn = void (*)();
constexpr uintptr_t kImGuiNewFrameRva = 0x6083120;
ImGuiNewFrameFn g_originalImGuiNewFrame = nullptr;
void* g_imGuiNewFrameTarget = nullptr;
WorldTickFn g_originalWorldTick = nullptr;
void* g_engineTickTarget = nullptr;
std::atomic<bool> g_connectIssued{false};
std::chrono::steady_clock::time_point g_hooksInstalledAt{};

constexpr uintptr_t kWorldTickRva = 0x4880370;
constexpr uintptr_t kSetClientTravelRva = 0x4FEE970;
constexpr uintptr_t kGEngineRva = 0xDC3BCD8;
// USN2Statics::GetLocalPlayer(UObject* WorldContext) -> USN2LocalPlayer*
constexpr uintptr_t kGetLocalPlayerRva = 0x7B2B480;
// USN2LocalPlayer::GetGameLoginOptions() const -> FString (sret: rcx=buffer, rdx=this)
constexpr uintptr_t kGetGameLoginOptionsRva = 0x7A52700;
// UEngine::GetFirstLocalPlayerController(const UWorld*) -> APlayerController*
constexpr uintptr_t kGetFirstLocalPlayerControllerRva = 0x4FC70B0;
// APlayerController::GetLocalPlayer() -> ULocalPlayer*
constexpr uintptr_t kPlayerControllerGetLocalPlayerRva = 0x4B52F60;
constexpr int32_t kTravelAbsolute = 0;


// EOS ids are interned by the SDK, but calling FromString on every packet is
// both wasteful and an SDK call from an arbitrary game thread. Cache the
// mapping so pointer identity also stays stable for the session.
std::mutex g_puidCacheMutex;
std::unordered_map<std::string, EOS_ProductUserId> g_puidCache;

EOS_ProductUserId puidFromStringCached(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(g_puidCacheMutex);
        const auto it = g_puidCache.find(id);
        if (it != g_puidCache.end()) {
            return it->second;
        }
    }
    if (g_api.puidFromString == nullptr) {
        return nullptr;
    }
    EOS_ProductUserId handle = g_api.puidFromString(id.c_str());
    if (handle != nullptr) {
        std::lock_guard<std::mutex> lock(g_puidCacheMutex);
        g_puidCache.emplace(id, handle);
    }
    return handle;
}

std::atomic<uint64_t> g_directSends{0};
std::atomic<uint64_t> g_fallbackSends{0};
std::atomic<uint64_t> g_directReceives{0};

HMODULE selfModule() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&selfModule), &module);
    return module;
}

std::wstring exeDirectory() {
    wchar_t path[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) {
        return {};
    }
    std::wstring exe(path);
    const size_t slash = exe.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    return exe.substr(0, slash);
}

HMODULE loadFromPath(const std::wstring& path) {
    wchar_t full[MAX_PATH] = {0};
    if (GetFullPathNameW(path.c_str(), MAX_PATH, full, nullptr) == 0) {
        return nullptr;
    }
    return LoadLibraryW(full);
}

HMODULE ensureEosModule() {
    HMODULE module = GetModuleHandleW(kEosModuleName);
    if (module != nullptr) {
        return module;
    }

    // 1) next to our own DLL (handy for tests / manual installs)
    wchar_t path[MAX_PATH] = {0};
    HMODULE self = selfModule();
    if (self != nullptr && GetModuleFileNameW(self, path, MAX_PATH) > 0) {
        std::wstring full(path);
        const size_t slash = full.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            full.resize(slash + 1);
            full += kEosModuleName;
            module = LoadLibraryW(full.c_str());
            if (module != nullptr) {
                SN2_LOG_INFO("loaded EOS SDK from mod directory");
                return module;
            }
        }
    }

    // 2) where the game keeps it: <game>\Engine\Binaries\Win64, with the
    //    executable living in <game>\Subnautica2\Binaries\Win64.
    const std::wstring exeDir = exeDirectory();
    if (!exeDir.empty()) {
        const std::wstring enginePath =
            exeDir + L"\\..\\..\\..\\Engine\\Binaries\\Win64\\" + kEosModuleName;
        module = loadFromPath(enginePath);
        if (module != nullptr) {
            SN2_LOG_INFO("loaded EOS SDK from Engine\\Binaries\\Win64");
            return module;
        }
    }

    // 3) plain name (PATH / current directory)
    module = LoadLibraryW(kEosModuleName);
    if (module != nullptr) {
        SN2_LOG_INFO("loaded EOS SDK from the default search path");
    }
    return module;
}

bool puidToString(EOS_ProductUserId id, char out[40]) {
    if (id == nullptr || g_api.puidToString == nullptr) {
        return false;
    }
    int32_t length = 40;
    std::memset(out, 0, 40);
    if (g_api.puidToString(id, out, &length) != kEosSuccess) {
        return false;
    }
    out[39] = '\0';
    return isValidPuid(out);
}

const char* modifierKey(int32_t value) {
    switch (value) {
        case 0:
            return "unreliable-unordered";
        case 1:
            return "reliable-unordered";
        case 2:
            return "reliable-ordered";
        default:
            return "unknown";
    }
}

EOS_EResult detourSendPacket(EOS_HP2P handle, const EOS_P2P_SendPacketOptions* options) {
    Tunnel* tunnel = g_tunnel.load(std::memory_order_relaxed);
    if (tunnel == nullptr || !tunnel->running() || options == nullptr || options->SocketId == nullptr) {
        return g_originalSendPacket(handle, options);
    }
    if (!tunnel->handlesSocket(options->SocketId->SocketName)) {
        return g_originalSendPacket(handle, options);
    }

    char local[40] = {0};
    char remote[40] = {0};
    if (!puidToString(options->LocalUserId, local) || !puidToString(options->RemoteUserId, remote)) {
        return g_originalSendPacket(handle, options);
    }

    const bool direct = tunnel->recentlySeen(remote);
    const bool sent = tunnel->send(local, remote, options->Channel, options->Data, options->DataLengthBytes);
    if (sent && (direct || !tunnel->passthroughUntilSeen())) {
        const uint64_t count = g_directSends.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count % 5000 == 0) {
            SN2_LOG_DEBUG("tunnel send #%llu -> %s channel=%u bytes=%u reliability=%s", count, remote,
                          options->Channel, options->DataLengthBytes, modifierKey(options->Reliability));
        }
        return kEosSuccess;
    }

    const uint64_t count = g_fallbackSends.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count % 5000 == 0) {
        SN2_LOG_DEBUG("EOS fallback send #%llu -> %s (direct=%d sent=%d)", count, remote, direct ? 1 : 0,
                      sent ? 1 : 0);
    }
    return g_originalSendPacket(handle, options);
}

bool firstSocketName(const std::string& list, std::string* out) {
    const size_t comma = list.find(',');
    std::string name = comma == std::string::npos ? list : list.substr(0, comma);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
        name.pop_back();
    }
    if (name.empty() || name == "*") {
        return false;
    }
    *out = name;
    return true;
}

EOS_EResult detourGetNextReceivedPacketSize(EOS_HP2P handle,
                                            const EOS_P2P_GetNextReceivedPacketSizeOptions* options,
                                            uint32_t* outSize) {
    Tunnel* tunnel = g_tunnel.load(std::memory_order_relaxed);
    if (tunnel != nullptr && tunnel->running() && outSize != nullptr) {
        const uint8_t* channel = options != nullptr ? options->RequestedChannel : nullptr;
        uint32_t size = 0;
        if (tunnel->peek(&size, channel)) {
            *outSize = size;
            return kEosSuccess;
        }
    }
    return g_originalGetNextReceivedPacketSize(handle, options, outSize);
}

EOS_EResult detourReceivePacket(EOS_HP2P handle,
                                const EOS_P2P_ReceivePacketOptions* options,
                                EOS_ProductUserId* outPeerId,
                                EOS_P2P_SocketId* outSocketId,
                                uint8_t* outChannel,
                                void* outData,
                                uint32_t* outBytesWritten) {
    Tunnel* tunnel = g_tunnel.load(std::memory_order_relaxed);
    if (tunnel != nullptr && tunnel->running() && options != nullptr && outPeerId != nullptr) {
        const uint8_t* channel = options->RequestedChannel;
        for (int attempt = 0; attempt < 32; ++attempt) {
            uint32_t size = 0;
            if (!tunnel->peek(&size, channel)) {
                break;
            }

            std::string puid;
            uint8_t packetChannel = 0;
            uint32_t written = 0;
            if (size > options->MaxDataSizeBytes) {
                // Never hand a truncated packet to the game: drop it and try
                // the next one instead.
                tunnel->pop(channel, &puid, &packetChannel, nullptr, 0, nullptr);
                SN2_LOG_WARN("dropping %u byte tunnelled packet, game buffer is %u bytes", size,
                             options->MaxDataSizeBytes);
                continue;
            }
            if (!tunnel->pop(channel, &puid, &packetChannel, outData, options->MaxDataSizeBytes, &written)) {
                break;
            }

            EOS_ProductUserId peer = puidFromStringCached(puid);
            if (peer == nullptr) {
                SN2_LOG_WARN("cannot reconstruct EOS user id for %s, dropping tunnelled packet", puid.c_str());
                continue;
            }

            *outPeerId = peer;
            if (outSocketId != nullptr) {
                std::string name;
                if (!firstSocketName(tunnel->socketName(), &name)) {
                    // "*" is a matcher, not a real socket; the game only ever
                    // uses GameNetDriver for the net driver socket.
                    name = "GameNetDriver";
                }
                outSocketId->ApiVersion = kSocketIdApiVersion;
                std::memset(outSocketId->SocketName, 0, sizeof(outSocketId->SocketName));
                std::memcpy(outSocketId->SocketName, name.data(), std::min(name.size(), sizeof(outSocketId->SocketName) - 1));
            }
            if (outChannel != nullptr) {
                *outChannel = packetChannel;
            }
            if (outBytesWritten != nullptr) {
                *outBytesWritten = written;
            }

            const uint64_t count = g_directReceives.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count == 1 || count % 5000 == 0) {
                SN2_LOG_DEBUG("tunnel receive #%llu <- %s channel=%u bytes=%u", count, puid.c_str(), packetChannel,
                              written);
            }
            return kEosSuccess;
        }
    }

    return g_originalReceivePacket(handle, options, outPeerId, outSocketId, outChannel, outData, outBytesWritten);
}

bool containsNoCase(const wchar_t* text, int32_t len, const wchar_t* needle) {
    if (text == nullptr || len <= 0) {
        return false;
    }
    const int32_t needleLen = static_cast<int32_t>(wcslen(needle));
    if (needleLen <= 0 || needleLen > len) {
        return false;
    }
    for (int32_t i = 0; i + needleLen <= len; ++i) {
        int32_t j = 0;
        for (; j < needleLen; ++j) {
            wchar_t a = text[i + j];
            wchar_t b = needle[j];
            if (a >= L'A' && a <= L'Z') {
                a = static_cast<wchar_t>(a - L'A' + L'a');
            }
            if (b >= L'A' && b <= L'Z') {
                b = static_cast<wchar_t>(b - L'A' + L'a');
            }
            if (a != b) {
                break;
            }
        }
        if (j == needleLen) {
            return true;
        }
    }
    return false;
}

// Reads the SteamID64 of the local Steam user from the game's steam_api64.dll.
static uint64_t localSteamUserId() {
    static std::atomic<uint64_t> cached{0};
    if (cached.load() != 0) {
        return cached.load();
    }
    HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
    if (steam == nullptr) {
        wchar_t exePath[MAX_PATH] = {0};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
            return 0;
        }
        const std::filesystem::path path =
            std::filesystem::path(exePath).parent_path().parent_path().parent_path() /
            L"Engine/Binaries/ThirdParty/Steamworks/Steamv161/Win64/steam_api64.dll";
        steam = LoadLibraryW(path.c_str());
    }
    if (steam == nullptr) {
        return 0;
    }
    using GetUserFn = void* (*)();
    const char* versions[] = {"SteamAPI_SteamUser_v023", "SteamAPI_SteamUser_v022", "SteamAPI_SteamUser_v021"};
    void* user = nullptr;
    for (const char* version : versions) {
        auto get = reinterpret_cast<GetUserFn>(GetProcAddress(steam, version));
        if (get != nullptr && (user = get()) != nullptr) {
            break;
        }
    }
    auto getSteamId = reinterpret_cast<uint64_t (*)(void*)>(GetProcAddress(steam, "SteamAPI_ISteamUser_GetSteamID"));
    if (user == nullptr || getSteamId == nullptr) {
        return 0;
    }
    const uint64_t id = getSteamId(user);
    cached.store(id);
    SN2_LOG_INFO("steam user id: %llu", static_cast<unsigned long long>(id));
    return id;
}

void* detourStaticLoadClass(void* baseClass, void* outer, const void* className, const void* filename,
                            uint32_t loadFlags, void* sandbox) {
    StaticLoadClassFn original = g_originalStaticLoadClass;
    if (original == nullptr) {
        return nullptr;
    }

    Tunnel* tunnel = g_tunnel.load(std::memory_order_relaxed);
    if (tunnel == nullptr || !tunnel->forceIpDriver() || className == nullptr) {
        return original(baseClass, outer, className, filename, loadFlags, sandbox);
    }

    HMODULE exe = GetModuleHandleW(nullptr);
    if (exe == nullptr) {
        return original(baseClass, outer, className, filename, loadFlags, sandbox);
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(exe);
    void* unetClass = g_unetDriverClass.load(std::memory_order_acquire);
    if (unetClass == nullptr) {
        auto getClass = reinterpret_cast<void* (*)()>(base + kUNetDriverClassGetterRva);
        unetClass = getClass();
        g_unetDriverClass.store(unetClass, std::memory_order_release);
    }
    if (unetClass == nullptr || baseClass != unetClass) {
        return original(baseClass, outer, className, filename, loadFlags, sandbox);
    }

    const auto* view = reinterpret_cast<const uint8_t*>(className);
    const wchar_t* name = *reinterpret_cast<const wchar_t* const*>(view);
    const int32_t nameLen = *reinterpret_cast<const int32_t*>(view + kStringViewLenOffset);
    if (!g_loggedRequestedDriverClass.exchange(true)) {
        SN2_LOG_INFO("StaticLoadClass: game requested net driver class %.*ls", static_cast<int>(nameLen), name);
    }
    if (!containsNoCase(name, nameLen, L"eos")) {
        return original(baseClass, outer, className, filename, loadFlags, sandbox);
    }

    auto ipNetDriverClass = reinterpret_cast<void* (*)()>(base + kIpNetDriverStaticClassRva);
    void* cls = ipNetDriverClass();
    if (cls == nullptr) {
        SN2_LOG_ERROR("StaticLoadClass: UIpNetDriver::StaticClass() returned null");
        return original(baseClass, outer, className, filename, loadFlags, sandbox);
    }

    SN2_LOG_INFO("StaticLoadClass: substituted UIpNetDriver for %.*ls", static_cast<int>(nameLen), name);
    tunnel->setHostReady(true);
    return cls;
}

// The game prints its own login string once EOS login completes:
//   GetGameLoginOptions returning '?SonarPlayerId=...?PlatformUserId=...'
// Reusing it gives each account its real identity (SonarPlayerId) without
// having to call into game code whose lifetime we do not control.
std::wstring gameLoginOptions() {
    static std::wstring cached;
    if (!cached.empty()) {
        return cached;
    }

    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&gameLoginOptions), &self)) {
        return {};
    }
    wchar_t modulePath[MAX_PATH] = {0};
    const DWORD length = GetModuleFileNameW(self, modulePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }

    const std::filesystem::path logPath = std::filesystem::path(modulePath).parent_path() / L".." / L".." / L".." /
                                          L"Saved" / L"Logs" / L"Subnautica2.log";
    std::ifstream file(logPath, std::ios::in | std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return {};
    }
    const std::streamoff size = file.tellg();
    const std::streamoff window = 512 * 1024;
    const std::streamoff start = size > window ? size - window : 0;
    file.seekg(start, std::ios::beg);
    std::string content(static_cast<size_t>(size - start), '\0');
    file.read(content.data(), static_cast<std::streamsize>(content.size()));

    const std::string marker = "GetGameLoginOptions returning '";
    const size_t markerPos = content.rfind(marker);
    if (markerPos == std::string::npos) {
        return {};
    }
    const size_t valueStart = markerPos + marker.size();
    const size_t valueEnd = content.find('\'', valueStart);
    if (valueEnd == std::string::npos || valueEnd == valueStart) {
        return {};
    }
    const std::string value = content.substr(valueStart, valueEnd - valueStart);
    if (value[0] != '?' || value.find("SonarPlayerId") == std::string::npos) {
        return {};
    }
    cached.assign(value.begin(), value.end());
    SN2_LOG_INFO("game login options: %s", value.c_str());
    return cached;
}

void detourImGuiNewFrame() {
    sn2lan::gameOverlayFeedInput();
    if (g_originalImGuiNewFrame != nullptr) {
        g_originalImGuiNewFrame();
    }
    static std::atomic<int> calls{0};
    const int count = calls.fetch_add(1, std::memory_order_relaxed);
    if (count < 3) {
        SN2_LOG_INFO("ImGui::NewFrame observed (#%d)", count + 1);
    }
    sn2lan::gameOverlayDraw();
}

void detourVoiceChatTick(void* self, float deltaSeconds) {
    Tunnel* tunnel = g_tunnel.load(std::memory_order_relaxed);
    if (tunnel != nullptr && tunnel->forceIpDriver()) {
        return;  // no EOS voice session exists in IP mode; ticking it faults
    }
    if (g_originalVoiceChatTick != nullptr) {
        g_originalVoiceChatTick(self, deltaSeconds);
    }
}

void detourWorldTick(void* world, int32_t tickType, float deltaSeconds) {
    if (g_originalWorldTick != nullptr) {
        g_originalWorldTick(world, tickType, deltaSeconds);
    }
    static std::atomic<bool> loggedTick{false};
    if (!loggedTick.exchange(true)) {
        SN2_LOG_INFO("world tick observed (world=%p)", world);
    }

    Tunnel* tunnel = g_tunnel.load(std::memory_order_relaxed);
    if (tunnel == nullptr || tunnel->isHost() || world == nullptr) {
        return;
    }

    // The overlay can ask for a join at any time; auto_connect keeps working
    // for unattended clients.
    std::string target;
    if (g_joinRequested.exchange(false)) {
        std::lock_guard<std::mutex> lock(g_joinMutex);
        target = g_joinTarget;
    }
    const bool manual = !target.empty();
    if (!manual) {
        if (g_connectIssued.load(std::memory_order_relaxed) || tunnel->autoConnect().empty() ||
            !tunnel->hostReady()) {
            return;
        }
        if (std::chrono::steady_clock::now() - g_hooksInstalledAt <
            std::chrono::seconds(tunnel->autoConnectDelaySec())) {
            return;
        }
        target = tunnel->autoConnect();
    }

    HMODULE exe = GetModuleHandleW(nullptr);
    if (exe == nullptr) {
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(exe);
    void* engine = *reinterpret_cast<void**>(base + kGEngineRva);
    if (engine == nullptr) {
        return;
    }

    std::string options = tunnel->loginOptions();
    if (options.empty()) {
        const std::wstring gameOptions = gameLoginOptions();
        if (!gameOptions.empty()) {
            options.assign(gameOptions.begin(), gameOptions.end());
        }
    }
    // Prefer the account's real identity. The game only logs it once EOS login
    // finishes (up to a minute after launch), so give it time before falling
    // back to the bare Steam id. A manual join never waits.
    if (options.empty() && !manual) {
        static std::chrono::steady_clock::time_point readySince{};
        static std::chrono::steady_clock::time_point lastNotice{};
        const auto now = std::chrono::steady_clock::now();
        if (readySince.time_since_epoch().count() == 0) {
            readySince = now;
            SN2_LOG_INFO("auto_connect: waiting for the account login string before joining");
        }
        if (now - readySince < std::chrono::seconds(45)) {
            if (now - lastNotice > std::chrono::seconds(15)) {
                lastNotice = now;
                SN2_LOG_INFO("auto_connect: still waiting for account login (%llds)",
                             static_cast<long long>(
                                 std::chrono::duration_cast<std::chrono::seconds>(now - readySince).count()));
            }
            return;
        }
        SN2_LOG_WARN("auto_connect: no account login string after 45s, falling back to the Steam id");
    }
    if (options.empty()) {
        const uint64_t steamId = localSteamUserId();
        if (steamId != 0) {
            options = "?PlatformUserId=" + std::to_string(steamId) + "?PlatformProvider=STEAM";
        }
    }

    std::string url = target;
    url += options;
    std::wstring wide(url.begin(), url.end());

    if (!manual && g_connectIssued.exchange(true)) {
        return;
    }
    g_connectIssued.store(true);
    auto setClientTravel = reinterpret_cast<void (*)(void*, void*, const wchar_t*, int32_t)>(base + kSetClientTravelRva);
    SN2_LOG_INFO("%s: ClientTravel to %ls (world=%p engine=%p)", manual ? "manual join" : "auto_connect",
                 wide.c_str(), world, engine);
    setClientTravel(engine, world, wide.c_str(), kTravelAbsolute);
}

bool createHook(const char* name, void* target, void* detour, void** original) {
    if (target == nullptr) {
        SN2_LOG_ERROR("cannot hook %s: export not found", name);
        return false;
    }
    const MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK) {
        SN2_LOG_ERROR("MH_CreateHook(%s) failed: %s", name, MH_StatusToString(status));
        return false;
    }
    return true;
}

}  // namespace

bool installHooks(Tunnel* tunnel) {
    std::lock_guard<std::mutex> lock(g_installMutex);
    if (g_hooksCreated.load()) {
        g_tunnel.store(tunnel);
        return true;
    }
    if (tunnel == nullptr) {
        return false;
    }

    const bool ipMode = tunnel->forceIpDriver();
    if (!ipMode) {
        HMODULE eos = ensureEosModule();
        if (eos == nullptr) {
            SN2_LOG_ERROR("EOS SDK module not found; cannot install hooks");
            return false;
        }

        g_api.sendPacket = reinterpret_cast<PFN_EOS_P2P_SendPacket>(GetProcAddress(eos, "EOS_P2P_SendPacket"));
        g_api.receivePacket = reinterpret_cast<PFN_EOS_P2P_ReceivePacket>(GetProcAddress(eos, "EOS_P2P_ReceivePacket"));
        g_api.getNextReceivedPacketSize = reinterpret_cast<PFN_EOS_P2P_GetNextReceivedPacketSize>(
            GetProcAddress(eos, "EOS_P2P_GetNextReceivedPacketSize"));
        g_api.puidToString = reinterpret_cast<PFN_EOS_ProductUserId_ToString>(
            GetProcAddress(eos, "EOS_ProductUserId_ToString"));
        g_api.puidFromString = reinterpret_cast<PFN_EOS_ProductUserId_FromString>(
            GetProcAddress(eos, "EOS_ProductUserId_FromString"));

        if (g_api.sendPacket == nullptr || g_api.receivePacket == nullptr ||
            g_api.getNextReceivedPacketSize == nullptr || g_api.puidToString == nullptr ||
            g_api.puidFromString == nullptr) {
            SN2_LOG_ERROR("EOS P2P exports missing (send=%p recv=%p size=%p toString=%p fromString=%p)",
                          reinterpret_cast<void*>(g_api.sendPacket), reinterpret_cast<void*>(g_api.receivePacket),
                          reinterpret_cast<void*>(g_api.getNextReceivedPacketSize),
                          reinterpret_cast<void*>(g_api.puidToString), reinterpret_cast<void*>(g_api.puidFromString));
            return false;
        }
    }

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
        SN2_LOG_ERROR("MH_Initialize failed: %s", MH_StatusToString(initStatus));
        return false;
    }

    bool ok = true;
    // Best effort: the ImGui hook only exists in the real game executable, not
    // in test harnesses. A failure here must not disable the tunnel hooks.
    HMODULE exeModule = GetModuleHandleW(nullptr);
    if (exeModule != nullptr) {
        void* imguiTarget = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(exeModule) + kImGuiNewFrameRva);
        const MH_STATUS imguiStatus = MH_CreateHook(imguiTarget, reinterpret_cast<void*>(&detourImGuiNewFrame),
                                                    reinterpret_cast<void**>(&g_originalImGuiNewFrame));
        if (imguiStatus == MH_OK) {
            g_imGuiNewFrameTarget = imguiTarget;
            sn2lan::gameOverlayResolve(exeModule);
        } else {
            SN2_LOG_WARN("in-game overlay unavailable (ImGui::NewFrame hook: %s)",
                         MH_StatusToString(imguiStatus));
            g_originalImGuiNewFrame = nullptr;
        }
    } else {
        SN2_LOG_WARN("main module not found; in-game overlay disabled");
    }

    if (!ipMode) {
        ok = createHook("EOS_P2P_SendPacket", reinterpret_cast<void*>(g_api.sendPacket),
                        reinterpret_cast<void*>(&detourSendPacket),
                        reinterpret_cast<void**>(&g_originalSendPacket)) &&
             ok;
        ok = createHook("EOS_P2P_ReceivePacket", reinterpret_cast<void*>(g_api.receivePacket),
                        reinterpret_cast<void*>(&detourReceivePacket),
                        reinterpret_cast<void**>(&g_originalReceivePacket)) &&
             ok;
        ok = createHook("EOS_P2P_GetNextReceivedPacketSize",
                        reinterpret_cast<void*>(g_api.getNextReceivedPacketSize),
                        reinterpret_cast<void*>(&detourGetNextReceivedPacketSize),
                        reinterpret_cast<void**>(&g_originalGetNextReceivedPacketSize)) &&
             ok;
    } else {
        HMODULE exe = GetModuleHandleW(nullptr);
        if (exe == nullptr) {
            SN2_LOG_ERROR("force_ip_driver: main module not found");
            ok = false;
        } else {
            g_staticLoadClassTarget = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(exe) + kStaticLoadClassRva);
            ok = createHook("StaticLoadClass", g_staticLoadClassTarget,
                            reinterpret_cast<void*>(&detourStaticLoadClass),
                            reinterpret_cast<void**>(&g_originalStaticLoadClass)) &&
                 ok;

            if (ok) {
                g_voiceChatTickTarget = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(exe) + kVoiceChatTickRva);
                ok = createHook("UUWEVoiceChatWorldSubsystem::Tick", g_voiceChatTickTarget,
                                reinterpret_cast<void*>(&detourVoiceChatTick),
                                reinterpret_cast<void**>(&g_originalVoiceChatTick)) &&
                     ok;
            }

            if (ok && !tunnel->isHost()) {
                g_engineTickTarget = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(exe) + kWorldTickRva);
                ok = createHook("UWorld::Tick", g_engineTickTarget,
                                reinterpret_cast<void*>(&detourWorldTick),
                                reinterpret_cast<void**>(&g_originalWorldTick)) &&
                     ok;
            }
        }
    }

    if (!ok) {
        MH_DisableHook(MH_ALL_HOOKS);
        if (g_api.sendPacket != nullptr) {
            MH_RemoveHook(reinterpret_cast<void*>(g_api.sendPacket));
            MH_RemoveHook(reinterpret_cast<void*>(g_api.receivePacket));
            MH_RemoveHook(reinterpret_cast<void*>(g_api.getNextReceivedPacketSize));
        }
        if (g_staticLoadClassTarget != nullptr) {
            MH_RemoveHook(g_staticLoadClassTarget);
            g_staticLoadClassTarget = nullptr;
            g_originalStaticLoadClass = nullptr;
        }
        if (g_engineTickTarget != nullptr) {
            MH_RemoveHook(g_engineTickTarget);
            g_engineTickTarget = nullptr;
            g_originalWorldTick = nullptr;
        }
        if (g_voiceChatTickTarget != nullptr) {
            MH_RemoveHook(g_voiceChatTickTarget);
            g_voiceChatTickTarget = nullptr;
            g_originalVoiceChatTick = nullptr;
        }
        if (g_imGuiNewFrameTarget != nullptr) {
            MH_RemoveHook(g_imGuiNewFrameTarget);
            g_imGuiNewFrameTarget = nullptr;
            g_originalImGuiNewFrame = nullptr;
        }
        MH_Uninitialize();
        return false;
    }

    g_tunnel.store(tunnel);
    g_hooksInstalledAt = std::chrono::steady_clock::now();
    const MH_STATUS enableStatus = MH_EnableHook(MH_ALL_HOOKS);
    if (enableStatus != MH_OK) {
        SN2_LOG_ERROR("MH_EnableHook failed: %s", MH_StatusToString(enableStatus));
        g_tunnel.store(nullptr);
        if (g_api.sendPacket != nullptr) {
            MH_RemoveHook(reinterpret_cast<void*>(g_api.sendPacket));
            MH_RemoveHook(reinterpret_cast<void*>(g_api.receivePacket));
            MH_RemoveHook(reinterpret_cast<void*>(g_api.getNextReceivedPacketSize));
        }
        if (g_staticLoadClassTarget != nullptr) {
            MH_RemoveHook(g_staticLoadClassTarget);
            g_staticLoadClassTarget = nullptr;
            g_originalStaticLoadClass = nullptr;
        }
        if (g_engineTickTarget != nullptr) {
            MH_RemoveHook(g_engineTickTarget);
            g_engineTickTarget = nullptr;
            g_originalWorldTick = nullptr;
        }
        if (g_voiceChatTickTarget != nullptr) {
            MH_RemoveHook(g_voiceChatTickTarget);
            g_voiceChatTickTarget = nullptr;
            g_originalVoiceChatTick = nullptr;
        }
        if (g_imGuiNewFrameTarget != nullptr) {
            MH_RemoveHook(g_imGuiNewFrameTarget);
            g_imGuiNewFrameTarget = nullptr;
            g_originalImGuiNewFrame = nullptr;
        }
        MH_Uninitialize();
        return false;
    }

    g_hooksCreated.store(true);
    SN2_LOG_INFO("%s hooks installed (socket filter: %s)", ipMode ? "engine" : "EOS P2P",
                 tunnel->socketName().c_str());
    if (g_staticLoadClassTarget != nullptr) {
        SN2_LOG_INFO("net driver hook installed: StaticLoadClass -> UIpNetDriver (build 533)");
        SN2_LOG_INFO("voice chat tick suppressed while force_ip_driver=1");
        if (g_engineTickTarget != nullptr) {
            SN2_LOG_INFO("manual join ready; auto_connect: %s",
                         tunnel->autoConnect().empty() ? "(disabled)" : tunnel->autoConnect().c_str());
        }
    }
    return true;
}

void requestJoin(const std::string& target) {
    if (target.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_joinMutex);
        g_joinTarget = target;
    }
    g_joinRequested.store(true);
    SN2_LOG_INFO("manual join requested: %s", target.c_str());
}

std::string loginOptionsPreview() {
    Tunnel* tunnel = g_tunnel.load(std::memory_order_relaxed);
    if (tunnel != nullptr && !tunnel->loginOptions().empty()) {
        return tunnel->loginOptions();
    }
    const std::wstring wide = gameLoginOptions();
    std::string out;
    out.reserve(wide.size());
    for (wchar_t ch : wide) {
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

bool joinIssued() {
    return g_connectIssued.load(std::memory_order_relaxed);
}

void removeHooks() {
    if (g_tunnel.exchange(nullptr) != nullptr) {
        SN2_LOG_INFO("EOS hooks detached (detours remain installed, forwarding to EOS)");
    }
}

void unhookAllForUnload() {
    std::lock_guard<std::mutex> lock(g_installMutex);
    g_tunnel.store(nullptr);
    if (!g_hooksCreated.exchange(false)) {
        return;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    if (g_api.sendPacket) {
        MH_RemoveHook(reinterpret_cast<void*>(g_api.sendPacket));
    }
    if (g_staticLoadClassTarget != nullptr) {
        MH_RemoveHook(g_staticLoadClassTarget);
        g_staticLoadClassTarget = nullptr;
        g_originalStaticLoadClass = nullptr;
    }
    if (g_engineTickTarget != nullptr) {
        MH_RemoveHook(g_engineTickTarget);
        g_engineTickTarget = nullptr;
        g_originalWorldTick = nullptr;
    }
    if (g_voiceChatTickTarget != nullptr) {
        MH_RemoveHook(g_voiceChatTickTarget);
        g_voiceChatTickTarget = nullptr;
        g_originalVoiceChatTick = nullptr;
    }
    if (g_imGuiNewFrameTarget != nullptr) {
        MH_RemoveHook(g_imGuiNewFrameTarget);
        g_imGuiNewFrameTarget = nullptr;
        g_originalImGuiNewFrame = nullptr;
    }
    if (g_api.receivePacket) {
        MH_RemoveHook(reinterpret_cast<void*>(g_api.receivePacket));
    }
    if (g_api.getNextReceivedPacketSize) {
        MH_RemoveHook(reinterpret_cast<void*>(g_api.getNextReceivedPacketSize));
    }
    MH_Uninitialize();
    SN2_LOG_INFO("EOS P2P hooks removed");
}

bool hooksInstalled() {
    return g_hooksCreated.load();
}

std::string hookStatus() {
    const uint64_t direct = g_directSends.load(std::memory_order_relaxed);
    const uint64_t fallback = g_fallbackSends.load(std::memory_order_relaxed);
    const uint64_t received = g_directReceives.load(std::memory_order_relaxed);
    return "direct_sends=" + std::to_string(direct) + " eos_fallback_sends=" + std::to_string(fallback) +
           " direct_receives=" + std::to_string(received);
}

}  // namespace sn2lan
