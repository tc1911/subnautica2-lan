// Minimal stand-in for EOSSDK-Win64-Shipping.dll used by the Wine integration
// test. Only the handful of exports SN2LAN touches are implemented: PUID
// conversion plus the three P2P calls, which deliberately never deliver
// anything so a successful round trip proves the tunnel did the work.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

namespace {

struct PuidDetails {
    std::string id;
};

std::mutex g_mutex;
std::map<std::string, PuidDetails*> g_ids;
uint64_t g_eosSends = 0;

}  // namespace

extern "C" {

__declspec(dllexport) int EOS_ProductUserId_ToString(void* id, char* buffer, int32_t* length) {
    if (id == nullptr || buffer == nullptr || length == nullptr) {
        return 3;  // EOS_InvalidParameters
    }
    const std::string& text = static_cast<PuidDetails*>(id)->id;
    if (*length < static_cast<int32_t>(text.size()) + 1) {
        *length = static_cast<int32_t>(text.size()) + 1;
        return 3;
    }
    std::snprintf(buffer, static_cast<size_t>(*length), "%s", text.c_str());
    *length = static_cast<int32_t>(text.size()) + 1;
    return 0;
}

__declspec(dllexport) void* EOS_ProductUserId_FromString(const char* text) {
    if (text == nullptr) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_ids.find(text);
    if (it != g_ids.end()) {
        return it->second;
    }
    auto* details = new PuidDetails{text};
    g_ids.emplace(text, details);
    return details;
}

__declspec(dllexport) int EOS_P2P_SendPacket(void* /*handle*/, const void* /*options*/) {
    __atomic_add_fetch(&g_eosSends, 1, __ATOMIC_RELAXED);
    return 0;  // pretend the EOS path accepted it (and drop it)
}

__declspec(dllexport) int EOS_P2P_ReceivePacket(void* /*handle*/, const void* /*options*/, void** /*peer*/,
                                                void* /*socket*/, unsigned char* /*channel*/, void* /*data*/,
                                                uint32_t* /*written*/) {
    return 5;  // EOS_NotFound: nothing from EOS, only the tunnel can deliver
}

__declspec(dllexport) int EOS_P2P_GetNextReceivedPacketSize(void* /*handle*/, const void* /*options*/,
                                                            uint32_t* /*size*/) {
    return 5;
}

__declspec(dllexport) uint64_t sn2lan_fake_eos_send_count() {
    return __atomic_load_n(&g_eosSends, __ATOMIC_RELAXED);
}

}  // extern "C"

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) {
    return TRUE;
}
