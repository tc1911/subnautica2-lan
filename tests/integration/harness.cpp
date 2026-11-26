// End-to-end test driver: talks to the fake EOSSDK through the same exported
// functions the real game uses, so the MinHook detours and the UDP tunnel are
// exercised exactly as in game.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct SocketId {
    int32_t apiVersion;
    char name[33];
};
struct SendOptions {
    int32_t apiVersion;
    void* localUserId;
    void* remoteUserId;
    const SocketId* socketId;
    uint8_t channel;
    uint32_t dataLengthBytes;
    const void* data;
    int32_t allowDelayedDelivery;
    int32_t reliability;
    int32_t disableAutoAcceptConnection;
};
struct ReceiveOptions {
    int32_t apiVersion;
    void* localUserId;
    uint32_t maxDataSizeBytes;
    const uint8_t* requestedChannel;
};
struct SizeOptions {
    int32_t apiVersion;
    void* localUserId;
    const uint8_t* requestedChannel;
};

constexpr const char* kHostPuid = "11111111111111111111111111111111";
constexpr const char* kClientPuid = "22222222222222222222222222222222";

using FromStringFn = void* (*)(const char*);
using ToStringFn = int (*)(void*, char*, int32_t*);
using SendFn = int (*)(void*, const void*);
using ReceiveFn = int (*)(void*, const void*, void**, void*, uint8_t*, void*, uint32_t*);
using SizeFn = int (*)(void*, const void*, uint32_t*);

struct Api {
    FromStringFn fromString = nullptr;
    ToStringFn toString = nullptr;
    SendFn send = nullptr;
    ReceiveFn receive = nullptr;
    SizeFn nextSize = nullptr;
};

void* loadExport(HMODULE module, const char* name) {
    void* fn = reinterpret_cast<void*>(GetProcAddress(module, name));
    if (fn == nullptr) {
        std::printf("missing export %s\n", name);
    }
    return fn;
}

bool receiveOne(const Api& api, void* local, std::vector<uint8_t>& buffer, std::string* from, uint8_t* channel) {
    SizeOptions sizeOptions{1, local, nullptr};
    uint32_t size = 0;
    if (api.nextSize(nullptr, &sizeOptions, &size) != 0) {
        return false;
    }
    if (size > buffer.size()) {
        buffer.resize(size);
    }
    ReceiveOptions options{1, local, static_cast<uint32_t>(buffer.size()), nullptr};
    void* peer = nullptr;
    SocketId socket{1, {0}};
    uint32_t written = 0;
    if (api.receive(nullptr, &options, &peer, &socket, channel, buffer.data(), &written) != 0) {
        return false;
    }
    char text[40] = {0};
    int32_t length = sizeof(text);
    if (peer != nullptr && api.toString(peer, text, &length) == 0) {
        *from = text;
    }
    buffer.resize(written);
    return true;
}

int runHost(const Api& api, void* local) {
    std::vector<uint8_t> buffer(8192);
    int received = 0;
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < 8000) {
        std::string from;
        uint8_t channel = 0;
        if (!receiveOne(api, local, buffer, &from, &channel)) {
            Sleep(5);
            continue;
        }
        ++received;
        std::printf("host recv #%d from=%s bytes=%zu channel=%u\n", received, from.c_str(), buffer.size(), channel);
        if (received % 5 == 0) {
            SocketId socket{1, {0}};
            std::strncpy(socket.name, "GameNetDriver", sizeof(socket.name) - 1);
            const std::string reply = "pong";
            SendOptions options{1, local, api.fromString(from.c_str()), &socket, 0,
                                static_cast<uint32_t>(reply.size()), reply.data(), 1, 0, 1};
            api.send(nullptr, &options);
        }
    }
    std::printf("host done received=%d\n", received);
    return received >= 5 ? 0 : 2;
}

int runClient(const Api& api, void* local, void* host) {
    SocketId socket{1, {0}};
    std::strncpy(socket.name, "GameNetDriver", sizeof(socket.name) - 1);
    std::vector<uint8_t> payload(64, 'x');
    std::vector<uint8_t> buffer(8192);
    int sent = 0;
    int pongs = 0;
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < 10000) {
        if (GetTickCount() - start < 6000) {
            // Every tenth ping is larger than the MTU to exercise reassembly.
            if (sent % 10 == 0) {
                payload.assign(4000, 'y');
            } else {
                payload.assign(64, 'x');
            }
            SendOptions options{1, local, host, &socket, 0, static_cast<uint32_t>(payload.size()), payload.data(), 1,
                                0, 1};
            api.send(nullptr, &options);
            ++sent;
        }
        std::string from;
        uint8_t channel = 0;
        if (receiveOne(api, local, buffer, &from, &channel)) {
            ++pongs;
            std::printf("client recv #%d from=%s bytes=%zu\n", pongs, from.c_str(), buffer.size());
            if (pongs >= 3) {
                break;
            }
        }
        Sleep(20);
    }
    std::printf("client done sent=%d pongs=%d\n", sent, pongs);
    return pongs >= 3 ? 0 : 3;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string role = argc > 1 ? argv[1] : "host";
    HMODULE eos = LoadLibraryA("EOSSDK-Win64-Shipping.dll");
    HMODULE mod = LoadLibraryA("sn2lan.dll");
    if (eos == nullptr || mod == nullptr) {
        std::printf("LoadLibrary failed eos=%p mod=%p err=%lu\n", static_cast<void*>(eos), static_cast<void*>(mod),
                    GetLastError());
        return 1;
    }

    Api api;
    api.fromString = reinterpret_cast<FromStringFn>(loadExport(eos, "EOS_ProductUserId_FromString"));
    api.toString = reinterpret_cast<ToStringFn>(loadExport(eos, "EOS_ProductUserId_ToString"));
    api.send = reinterpret_cast<SendFn>(loadExport(eos, "EOS_P2P_SendPacket"));
    api.receive = reinterpret_cast<ReceiveFn>(loadExport(eos, "EOS_P2P_ReceivePacket"));
    api.nextSize = reinterpret_cast<SizeFn>(loadExport(eos, "EOS_P2P_GetNextReceivedPacketSize"));
    auto start = reinterpret_cast<void (*)()>(GetProcAddress(mod, "sn2lan_start"));
    auto stop = reinterpret_cast<void (*)()>(GetProcAddress(mod, "sn2lan_stop"));
    auto status = reinterpret_cast<const char* (*)()>(GetProcAddress(mod, "sn2lan_status"));

    start();
    Sleep(1200);  // let the hook retry loop find the (already loaded) EOS DLL

    void* local = api.fromString(role == "host" ? kHostPuid : kClientPuid);
    void* remote = api.fromString(role == "host" ? kClientPuid : kHostPuid);
    const int code = role == "host" ? runHost(api, local) : runClient(api, local, remote);
    std::printf("%s status: %s\n", role.c_str(), status());

    auto eosCount = reinterpret_cast<uint64_t (*)()>(GetProcAddress(eos, "sn2lan_fake_eos_send_count"));
    if (eosCount != nullptr) {
        std::printf("%s eos_sends=%llu\n", role.c_str(), static_cast<unsigned long long>(eosCount()));
    }

    stop();
    Sleep(200);
    FreeLibrary(mod);
    return code;
}
