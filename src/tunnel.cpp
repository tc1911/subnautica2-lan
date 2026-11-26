#include "tunnel.h"

#include <ws2tcpip.h>

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <sstream>

#include "log.h"

namespace sn2lan {

namespace {

std::string trimCopy(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && (s[begin] == ' ' || s[begin] == '\t')) {
        ++begin;
    }
    size_t end = s.size();
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t')) {
        --end;
    }
    return s.substr(begin, end - begin);
}

std::string endpointToString(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, const_cast<in_addr*>(&addr.sin_addr), ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

bool sameAddress(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

}  // namespace

Tunnel::~Tunnel() {
    stop();
}

uint64_t Tunnel::nowMs() {
    return GetTickCount64();
}

bool Tunnel::configure(const Options& options) {
    options_ = options;
    options_.mtu = std::min<size_t>(std::max<size_t>(options_.mtu, 512), 2000);
    isHost_ = options.isHost;
    hostReady_.store(false);

    // Provisional identity for HELLO frames: the real EOS id is only known
    // after the game sends its first P2P packet, which may be after we need
    // to talk to the host.
    std::random_device rd;
    char hex[33];
    for (int i = 0; i < 32; i += 8) {
        const uint32_t value = rd();
        std::snprintf(hex + i, 9, "%08x", value);
    }
    helloId_.assign(hex, 32);
    return true;
}

bool Tunnel::start(const Options& options) {
    if (running_.load()) {
        return true;
    }

    configure(options);

    WSADATA wsa{};
    const int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (wsaResult != 0) {
        SN2_LOG_ERROR("WSAStartup failed: %d", wsaResult);
        return false;
    }

    sockaddr_in bindAddr{};
    if (!resolveAddress(options_.bindAddress, options_.bindPort, &bindAddr)) {
        SN2_LOG_ERROR("cannot resolve bind address %s", options_.bindAddress.c_str());
        WSACleanup();
        return false;
    }

    socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) {
        SN2_LOG_ERROR("socket() failed: %d", WSAGetLastError());
        WSACleanup();
        return false;
    }

    if (::bind(socket_, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
        SN2_LOG_ERROR("bind(%s:%u) failed: %d", options_.bindAddress.c_str(), options_.bindPort, WSAGetLastError());
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    DWORD timeout = 250;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    sockaddr_in localAddr{};
    int localLen = sizeof(localAddr);
    getsockname(socket_, reinterpret_cast<sockaddr*>(&localAddr), &localLen);
    SN2_LOG_INFO("UDP socket bound to %s (role=%s, socket=%s, mtu=%zu)",
                 endpointToString(localAddr).c_str(),
                 isHost_ ? "host" : "client",
                 options_.socketName.c_str(),
                 options_.mtu);

    if (!isHost_) {
        if (!resolveAddress(options_.hostAddress, options_.hostPort, &hostAddr_)) {
            SN2_LOG_ERROR("cannot resolve host address %s:%u", options_.hostAddress.c_str(), options_.hostPort);
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            WSACleanup();
            return false;
        }
        hostAddrValid_ = true;
        SN2_LOG_INFO("tunnelling game traffic to host %s", endpointToString(hostAddr_).c_str());
    } else {
        SN2_LOG_INFO("waiting for clients on UDP %u (map this port with frp)", options_.bindPort);
    }

    running_.store(true);
    recvThread_ = std::thread(&Tunnel::recvLoop, this);
    return true;
}

void Tunnel::stop() {
    if (!running_.exchange(false)) {
        std::lock_guard<std::mutex> lock(sendMutex_);
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            WSACleanup();
        }
        return;
    }

    {
        // A game thread may be inside sendto right now; closing under the same
        // mutex keeps the handle alive until that call returns.
        std::lock_guard<std::mutex> lock(sendMutex_);
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
    }
    if (recvThread_.joinable()) {
        recvThread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queue_.clear();
        queuedBytes_ = 0;
        reassembler_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(peerMutex_);
        peers_.clear();
    }

    // Reset liveness so a later start() re-announces instead of believing the
    // old host address is still reachable.
    hostAlive_.store(false, std::memory_order_relaxed);
    lastHelloMs_.store(0, std::memory_order_relaxed);
    hostAddrValid_ = false;
    msgCounter_.store(0, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        WSACleanup();
    }
    SN2_LOG_INFO("tunnel stopped");
}

bool Tunnel::handlesSocket(const char* name) const {
    if (name == nullptr) {
        return false;
    }
    std::string list = options_.socketName;
    std::stringstream ss(list);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trimCopy(item);
        if (item == "*" || item == name) {
            return true;
        }
    }
    return false;
}

bool Tunnel::resolveAddress(const std::string& host, uint16_t port, sockaddr_in* out) const {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0 || result == nullptr) {
        return false;
    }
    std::memcpy(out, result->ai_addr, sizeof(sockaddr_in));
    freeaddrinfo(result);
    return true;
}

bool Tunnel::sendFrame(const std::vector<uint8_t>& frame, const sockaddr_in& to) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (socket_ == INVALID_SOCKET) {
        return false;
    }
    const int sent = sendto(socket_, reinterpret_cast<const char*>(frame.data()), static_cast<int>(frame.size()), 0,
                            reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    if (sent == SOCKET_ERROR) {
        SN2_LOG_DEBUG("sendto(%s) failed: %d", endpointToString(to).c_str(), WSAGetLastError());
        return false;
    }
    datagramsSent_.fetch_add(1, std::memory_order_relaxed);
    bytesSent_.fetch_add(static_cast<uint64_t>(sent), std::memory_order_relaxed);
    return true;
}

void Tunnel::sendHelloTo(const sockaddr_in& to, const std::string& senderPuid, uint8_t flags) {
    FrameHeader header{};
    header.magic = kMagic;
    header.version = kVersion;
    header.type = kTypeHello;
    header.flags = flags;
    header.msgId = 0;
    header.fragCount = 1;
    std::memcpy(header.sender, senderPuid.data(), std::min(senderPuid.size(), kPuidLen));
    header.sender[kPuidLen] = '\0';
    header.payloadLen = 0;

    std::vector<uint8_t> frame(sizeof(FrameHeader));
    std::memcpy(frame.data(), &header, sizeof(FrameHeader));
    sendFrame(frame, to);
}

void Tunnel::setHostReady(bool ready) {
    if (!isHost_) {
        return;
    }
    const bool wasReady = hostReady_.exchange(ready);
    if (!ready || wasReady) {
        return;
    }
    std::string local;
    std::vector<sockaddr_in> targets;
    {
        std::lock_guard<std::mutex> lock(peerMutex_);
        local = localPuid_.size() == kPuidLen ? localPuid_ : helloId_;
        for (const auto& entry : peers_) {
            targets.push_back(entry.second.addr);
        }
    }
    if (local.size() != kPuidLen) {
        // No packet has been sent yet, so we do not know our own id. The
        // synthetic sender is only used to wake clients; it is never learnt.
        local.assign(kPuidLen, '0');
    }
    for (const auto& addr : targets) {
        sendHelloTo(addr, local, kFlagHostReady);
    }
    SN2_LOG_INFO("host ready signal sent to %zu peer(s)", targets.size());
}

void Tunnel::maybeSendHello(uint64_t now) {
    if (isHost_ || !running_.load()) {
        return;
    }
    if (hostAlive_.load(std::memory_order_relaxed)) {
        return;
    }
    std::string local;
    {
        std::lock_guard<std::mutex> lock(peerMutex_);
        local = localPuid_.size() == kPuidLen ? localPuid_ : helloId_;
    }
    if (local.size() != kPuidLen) {
        return;
    }
    const uint64_t last = lastHelloMs_.load(std::memory_order_relaxed);
    if (last != 0 && now - last < 2000) {
        return;
    }
    lastHelloMs_.store(now, std::memory_order_relaxed);
    if (hostAddrValid_) {
        sendHelloTo(hostAddr_, local);
    }
}

void Tunnel::recvLoop() {
    // Frames can be as large as the configured MTU; a fixed 2 KB buffer would
    // silently truncate them and every big packet would fail to parse.
    std::vector<uint8_t> buffer(std::max<size_t>(options_.mtu, 2048) + 64);
    while (running_.load(std::memory_order_relaxed)) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        const SOCKET sock = socket_;
        const int received = recvfrom(sock, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0,
                                      reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (received == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            if (!running_.load(std::memory_order_relaxed)) {
                break;
            }
            if (err == WSAETIMEDOUT) {
                maybeSendHello(nowMs());
                continue;
            }
            if (err == WSAENOTSOCK || err == WSAEINTR) {
                break;
            }
            SN2_LOG_DEBUG("recvfrom failed: %d", err);
            Sleep(10);
            continue;
        }
        if (received > 0) {
            handleDatagram(buffer.data(), static_cast<size_t>(received), from);
        }
        maybeSendHello(nowMs());
    }
}

void Tunnel::handleDatagram(const uint8_t* data, size_t len, const sockaddr_in& from) {
    datagramsReceived_.fetch_add(1, std::memory_order_relaxed);
    bytesReceived_.fetch_add(len, std::memory_order_relaxed);

    FrameHeader header{};
    const uint8_t* payload = nullptr;
    size_t payloadLen = 0;
    if (!parseFrame(data, len, header, payload, payloadLen)) {
        parseErrors_.fetch_add(1, std::memory_order_relaxed);
        SN2_LOG_DEBUG("dropped malformed datagram from %s (%zu bytes)", endpointToString(from).c_str(), len);
        return;
    }

    const std::string sender(header.sender, kPuidLen);
    {
        std::lock_guard<std::mutex> lock(peerMutex_);
        auto it = peers_.find(sender);
        if (it == peers_.end()) {
            Peer peer;
            peer.addr = from;
            peer.lastSeenMs = nowMs();
            peer.packets = 1;
            peers_.emplace(sender, peer);
            SN2_LOG_INFO("peer %s learned at %s", sender.c_str(), endpointToString(from).c_str());
        } else {
            if (!sameAddress(it->second.addr, from)) {
                SN2_LOG_INFO("peer %s moved to %s", sender.c_str(), endpointToString(from).c_str());
            }
            it->second.addr = from;
            it->second.lastSeenMs = nowMs();
            it->second.packets += 1;
        }
    }

    if (!isHost_ && hostAddrValid_ && sameAddress(from, hostAddr_)) {
        if (!hostAlive_.exchange(true)) {
            SN2_LOG_INFO("direct tunnel to host is alive");
        }
    }

    if (header.type == kTypeHello) {
        if (isHost_) {
            std::string local;
            {
                std::lock_guard<std::mutex> lock(peerMutex_);
                local = localPuid_.size() == kPuidLen ? localPuid_ : helloId_;
            }
            // Before the first EOS packet we only have a provisional id; the
            // client accepts it for HELLO replies and keeps retrying.
            if (local.size() == kPuidLen) {
                sendHelloTo(from, local, hostReady_.load(std::memory_order_relaxed) ? kFlagHostReady : 0);
            }
        } else if ((header.flags & kFlagHostReady) != 0 && !hostReady_.exchange(true)) {
            SN2_LOG_INFO("host signalled that its listen server is up");
        }
        return;
    }

    std::vector<uint8_t> message;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        reassembler_.gc(nowMs());
        if (!reassembler_.add(header, payload, payloadLen, nowMs(), message)) {
            return;
        }
        if (queue_.size() >= 4096 || queuedBytes_ + message.size() > kMaxQueuedBytes) {
            queueDrops_.fetch_add(1, std::memory_order_relaxed);
            SN2_LOG_WARN("receive queue full, dropping packet from %s", sender.c_str());
            return;
        }
        Inbound inbound;
        inbound.puid = sender;
        inbound.channel = header.channel;
        inbound.data = std::move(message);
        queuedBytes_ += inbound.data.size();
        queue_.push_back(std::move(inbound));
    }
}

bool Tunnel::send(const char* localPuid, const char* remotePuid, uint8_t channel, const void* data, size_t len) {
    if (!running_.load(std::memory_order_relaxed)) {
        return false;
    }
    if (!isValidPuid(localPuid) || !isValidPuid(remotePuid)) {
        return false;
    }
    if (len == 0) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(peerMutex_);
        if (localPuid_ != localPuid) {
            localPuid_ = localPuid;
        }
    }

    const uint32_t msgId = msgCounter_.fetch_add(1, std::memory_order_relaxed) + 1;
    std::vector<std::vector<uint8_t>> frames;
    if (!buildFrames(localPuid, channel, data, len, options_.mtu, msgId, frames)) {
        SN2_LOG_WARN("cannot frame %zu byte packet (mtu=%zu)", len, options_.mtu);
        return false;
    }

    sockaddr_in destination{};
    if (isHost_) {
        std::lock_guard<std::mutex> lock(peerMutex_);
        const auto it = peers_.find(remotePuid);
        if (it == peers_.end()) {
            SN2_LOG_DEBUG("no direct address for peer %s yet, falling back to EOS", remotePuid);
            return false;
        }
        destination = it->second.addr;
    } else {
        if (!hostAddrValid_) {
            return false;
        }
        destination = hostAddr_;
    }

    bool allSent = true;
    for (const auto& frame : frames) {
        allSent = sendFrame(frame, destination) && allSent;
    }
    return allSent;
}

bool Tunnel::peek(uint32_t* size, const uint8_t* channel) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    // Scan instead of only looking at the front: a packet for another channel
    // at the head must not block the channel the game is asking for.
    for (const auto& inbound : queue_) {
        if (channel != nullptr && inbound.channel != *channel) {
            continue;
        }
        if (size != nullptr) {
            *size = static_cast<uint32_t>(inbound.data.size());
        }
        return true;
    }
    return false;
}

bool Tunnel::pop(const uint8_t* filterChannel, std::string* puid, uint8_t* channel, void* out, uint32_t maxLen,
                 uint32_t* written) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    auto match = queue_.end();
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (filterChannel == nullptr || it->channel == *filterChannel) {
            match = it;
            break;
        }
    }
    if (match == queue_.end()) {
        return false;
    }
    // Never hand a truncated payload to the game; leave the packet queued and
    // let the caller drop it explicitly with out == nullptr.
    if (out != nullptr && match->data.size() > maxLen) {
        return false;
    }

    Inbound inbound = std::move(*match);
    queue_.erase(match);
    queuedBytes_ -= std::min(queuedBytes_, inbound.data.size());

    const size_t copyLen = out != nullptr ? inbound.data.size() : 0;
    if (copyLen > 0) {
        std::memcpy(out, inbound.data.data(), copyLen);
    }

    if (puid) {
        *puid = std::move(inbound.puid);
    }
    if (channel) {
        *channel = inbound.channel;
    }
    if (written) {
        *written = static_cast<uint32_t>(copyLen);
    }
    return true;
}

bool Tunnel::recentlySeen(const char* puid) const {
    if (puid == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(peerMutex_);
    const auto it = peers_.find(puid);
    if (it == peers_.end()) {
        return false;
    }
    const uint64_t now = nowMs();
    const uint64_t last = it->second.lastSeenMs;
    return now >= last && (now - last) <= options_.peerTimeoutMs;
}

size_t Tunnel::peerCount() const {
    std::lock_guard<std::mutex> lock(peerMutex_);
    return peers_.size();
}

Tunnel::Stats Tunnel::stats() const {
    Stats s;
    s.datagramsSent = datagramsSent_.load(std::memory_order_relaxed);
    s.datagramsReceived = datagramsReceived_.load(std::memory_order_relaxed);
    s.bytesSent = bytesSent_.load(std::memory_order_relaxed);
    s.bytesReceived = bytesReceived_.load(std::memory_order_relaxed);
    s.parseErrors = parseErrors_.load(std::memory_order_relaxed);
    s.queueDrops = queueDrops_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace sn2lan
