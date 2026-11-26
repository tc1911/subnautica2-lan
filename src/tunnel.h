#pragma once

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "protocol.h"

namespace sn2lan {

class Tunnel {
public:
    struct Options {
        bool isHost = false;
        std::string socketName = "GameNetDriver";
        std::string bindAddress = "0.0.0.0";
        uint16_t bindPort = 28015;
        std::string hostAddress;
        uint16_t hostPort = 28015;
        size_t mtu = 1400;
        bool debug = false;
        bool passthroughUntilSeen = true;
        uint32_t peerTimeoutMs = 10000;
        bool forceIpDriver = false;
        std::string autoConnect;
        std::string loginOptions;
        uint32_t autoConnectDelaySec = 20;
    };

    struct Stats {
        uint64_t datagramsSent = 0;
        uint64_t datagramsReceived = 0;
        uint64_t bytesSent = 0;
        uint64_t bytesReceived = 0;
        uint64_t parseErrors = 0;
        uint64_t queueDrops = 0;
    };

    Tunnel() = default;
    ~Tunnel();

    // Stores options (and a provisional HELLO id) without opening the UDP
    // socket. Used in IP mode, where TCP 7777 carries the handshake.
    bool configure(const Options& options);
    bool start(const Options& options);
    void stop();

    bool running() const { return running_.load(std::memory_order_relaxed); }
    bool isHost() const { return isHost_; }
    const std::string& socketName() const { return options_.socketName; }
    bool passthroughUntilSeen() const { return options_.passthroughUntilSeen; }
    bool forceIpDriver() const { return options_.forceIpDriver; }
    const std::string& autoConnect() const { return options_.autoConnect; }
    const std::string& loginOptions() const { return options_.loginOptions; }
    uint32_t autoConnectDelaySec() const { return options_.autoConnectDelaySec; }
    // Host side: set when the game starts its listen server, so clients know
    // they can stop waiting and connect.
    void setHostReady(bool ready);
    bool hostReady() const { return hostReady_.load(std::memory_order_relaxed); }

    // True if `name` is one of the socket names we tunnel (comma separated list,
    // "*" matches everything).
    bool handlesSocket(const char* name) const;

    // Sends a game payload to `remotePuid`. Returns false when the packet was
    // not tunnelled (caller should fall back to EOS).
    bool send(const char* localPuid, const char* remotePuid, uint8_t channel, const void* data, size_t len);

    // Pops the next tunnelled packet. `peek` is used by
    // EOS_P2P_GetNextReceivedPacketSize, `pop` by EOS_P2P_ReceivePacket.
    bool peek(uint32_t* size, const uint8_t* channel);
    bool pop(const uint8_t* filterChannel, std::string* puid, uint8_t* channel, void* out, uint32_t maxLen,
             uint32_t* written);

    // True if a direct packet from `puid` arrived within the timeout window.
    bool recentlySeen(const char* puid) const;

    Stats stats() const;
    size_t peerCount() const;

private:
    struct Peer {
        sockaddr_in addr{};
        uint64_t lastSeenMs = 0;
        uint64_t packets = 0;
    };

    struct Inbound {
        std::string puid;
        uint8_t channel = 0;
        std::vector<uint8_t> data;
    };

    void recvLoop();
    void handleDatagram(const uint8_t* data, size_t len, const sockaddr_in& from);
    void maybeSendHello(uint64_t nowMs);
    void sendHelloTo(const sockaddr_in& to, const std::string& senderPuid, uint8_t flags = 0);
    bool sendFrame(const std::vector<uint8_t>& frame, const sockaddr_in& to);
    bool resolveAddress(const std::string& host, uint16_t port, sockaddr_in* out) const;

    static uint64_t nowMs();

    Options options_;
    bool isHost_ = false;
    SOCKET socket_ = INVALID_SOCKET;
    sockaddr_in hostAddr_{};
    bool hostAddrValid_ = false;

    std::atomic<bool> running_{false};
    std::thread recvThread_;
    std::atomic<bool> hostAlive_{false};
    std::atomic<bool> hostReady_{false};
    std::atomic<uint64_t> lastHelloMs_{0};
    std::atomic<uint32_t> msgCounter_{0};

    mutable std::mutex peerMutex_;
    std::unordered_map<std::string, Peer> peers_;
    std::string localPuid_;
    // Stable per-process fallback id used for HELLO frames before the game
    // has revealed our real EOS id through EOS_P2P_SendPacket.
    std::string helloId_;

    mutable std::mutex queueMutex_;
    std::deque<Inbound> queue_;
    size_t queuedBytes_ = 0;
    Reassembler reassembler_;

    std::mutex sendMutex_;

    std::atomic<uint64_t> datagramsSent_{0};
    std::atomic<uint64_t> datagramsReceived_{0};
    std::atomic<uint64_t> bytesSent_{0};
    std::atomic<uint64_t> bytesReceived_{0};
    std::atomic<uint64_t> parseErrors_{0};
    std::atomic<uint64_t> queueDrops_{0};

    static constexpr size_t kMaxQueuedBytes = 16 * 1024 * 1024;
};

}  // namespace sn2lan
