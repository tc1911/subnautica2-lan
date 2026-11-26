#include "control.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "log.h"

namespace sn2lan {
namespace control {

namespace {

constexpr uint32_t kMagic = 0x43324E53;  // "SN2C"
constexpr uint16_t kTypeHello = 1;
constexpr uint16_t kTypeWelcome = 2;
constexpr uint32_t kMaxPayload = 64;
constexpr uint64_t kHandshakeTimeoutMs = 5000;

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t length;
};
struct Hello {
    uint16_t protocol;
    uint16_t reserved;
};
struct Welcome {
    uint8_t status;
    uint8_t reserved[3];
};
#pragma pack(pop)

static_assert(sizeof(Header) == 12, "control header layout");
static_assert(sizeof(Hello) == 4, "control hello layout");
static_assert(sizeof(Welcome) == 4, "control welcome layout");

class WsaGuard {
public:
    WsaGuard() {
        WSADATA data{};
        ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WsaGuard() {
        if (ok_) {
            WSACleanup();
        }
    }
    bool ok() const { return ok_; }

private:
    bool ok_ = false;
};

bool sendAll(SOCKET sock, const void* data, size_t size) {
    const char* cursor = static_cast<const char*>(data);
    while (size > 0) {
        const int sent = ::send(sock, cursor, static_cast<int>(size), 0);
        if (sent <= 0) {
            return false;
        }
        cursor += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

std::string endpoint(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, const_cast<in_addr*>(&addr.sin_addr), ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

}  // namespace

struct Server::Impl {
    struct Conn {
        SOCKET sock = INVALID_SOCKET;
        uint64_t deadlineMs = 0;
        std::vector<uint8_t> buffer;
    };

    SOCKET listenSock = INVALID_SOCKET;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    std::function<bool()> ready;
    std::vector<Conn> conns;
};

Server::~Server() {
    stop();
}

bool Server::running() const {
    return impl_ != nullptr && impl_->running.load();
}

bool Server::start(uint16_t port, std::function<bool()> readyProvider) {
    if (impl_ != nullptr && impl_->running.load()) {
        impl_->ready = std::move(readyProvider);
        return true;
    }
    stop();

    auto* impl = new Impl();
    impl->ready = std::move(readyProvider);

    // Winsock is initialised once for the lifetime of the server; the tunnel
    // does the same in EOS mode, so both paths keep their own reference.
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        SN2_LOG_WARN("control: WSAStartup failed");
        delete impl;
        return false;
    }

    SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        SN2_LOG_WARN("control: socket() failed: %d", WSAGetLastError());
        delete impl;
        return false;
    }

    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        SN2_LOG_WARN("control: bind TCP %u failed: %d (handshake disabled)", port, WSAGetLastError());
        closesocket(sock);
        delete impl;
        return false;
    }
    if (::listen(sock, 8) == SOCKET_ERROR) {
        SN2_LOG_WARN("control: listen TCP %u failed: %d", port, WSAGetLastError());
        closesocket(sock);
        delete impl;
        return false;
    }

    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);

    impl->listenSock = sock;
    impl->running.store(true);
    impl_ = impl;

    impl->thread = std::thread([impl, port]() {
        SN2_LOG_INFO("control: handshake listener on TCP %u", port);

        while (!impl->stop.load(std::memory_order_relaxed)) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(impl->listenSock, &readSet);
            for (const Impl::Conn& conn : impl->conns) {
                FD_SET(conn.sock, &readSet);
            }
            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = 100000;
            const int selected = ::select(0, &readSet, nullptr, nullptr, &timeout);
            const uint64_t now = GetTickCount64();

            if (selected > 0 && FD_ISSET(impl->listenSock, &readSet)) {
                sockaddr_in peer{};
                int peerLen = sizeof(peer);
                SOCKET client = ::accept(impl->listenSock, reinterpret_cast<sockaddr*>(&peer), &peerLen);
                if (client != INVALID_SOCKET) {
                    if (impl->conns.size() >= 8) {
                        closesocket(client);
                    } else {
                        u_long nb = 1;
                        ioctlsocket(client, FIONBIO, &nb);
                        Impl::Conn conn;
                        conn.sock = client;
                        conn.deadlineMs = now + kHandshakeTimeoutMs;
                        impl->conns.push_back(std::move(conn));
                        SN2_LOG_DEBUG("control: handshake from %s", endpoint(peer).c_str());
                    }
                }
            }

            for (size_t i = 0; i < impl->conns.size();) {
                Impl::Conn& conn = impl->conns[i];
                bool closeConn = now > conn.deadlineMs;

                if (!closeConn && FD_ISSET(conn.sock, &readSet)) {
                    uint8_t chunk[256];
                    const int received = ::recv(conn.sock, reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
                    if (received > 0) {
                        conn.buffer.insert(conn.buffer.end(), chunk, chunk + received);
                    } else if (received == 0) {
                        closeConn = true;
                    } else {
                        const int error = WSAGetLastError();
                        if (error != WSAEWOULDBLOCK) {
                            closeConn = true;
                        }
                    }

                    if (!closeConn && conn.buffer.size() >= sizeof(Header)) {
                        Header header{};
                        std::memcpy(&header, conn.buffer.data(), sizeof(header));
                        if (header.magic != kMagic || header.version != kProtocolVersion ||
                            header.length > kMaxPayload) {
                            closeConn = true;
                        } else if (conn.buffer.size() >= sizeof(Header) + header.length) {
                            if (header.type == kTypeHello && header.length >= sizeof(Hello)) {
                                Hello hello{};
                                std::memcpy(&hello, conn.buffer.data() + sizeof(Header), sizeof(hello));
                                Welcome welcome{};
                                if (hello.protocol != kProtocolVersion) {
                                    welcome.status = static_cast<uint8_t>(Status::VersionMismatch);
                                } else if (impl->ready && impl->ready()) {
                                    welcome.status = static_cast<uint8_t>(Status::Ready);
                                } else {
                                    welcome.status = static_cast<uint8_t>(Status::NotReady);
                                }
                                Header response{kMagic, kProtocolVersion, kTypeWelcome, sizeof(welcome)};
                                sendAll(conn.sock, &response, sizeof(response));
                                sendAll(conn.sock, &welcome, sizeof(welcome));
                                SN2_LOG_DEBUG("control: handshake status=%u", static_cast<unsigned>(welcome.status));
                            }
                            closeConn = true;
                        }
                    }
                }

                if (closeConn) {
                    closesocket(conn.sock);
                    impl->conns.erase(impl->conns.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
        }

        for (Impl::Conn& conn : impl->conns) {
            closesocket(conn.sock);
        }
        impl->conns.clear();
        closesocket(impl->listenSock);
        impl->listenSock = INVALID_SOCKET;
        impl->running.store(false);
        SN2_LOG_INFO("control: listener stopped");
    });

    return true;
}

void Server::stop() {
    Impl* impl = impl_;
    if (impl == nullptr) {
        return;
    }
    impl->stop.store(true);
    if (impl->thread.joinable()) {
        impl->thread.join();
    }
    impl_ = nullptr;
    delete impl;
}

bool probe(const std::string& address, uint16_t port, Status* status, std::string* error,
           int* rttMs) {
    if (rttMs != nullptr) {
        *rttMs = 0;
    }
    WsaGuard wsa;
    if (!wsa.ok()) {
        if (error) {
            *error = "WSAStartup failed";
        }
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(address.c_str(), service.c_str(), &hints, &result) != 0 || result == nullptr) {
        if (error) {
            *error = "cannot resolve host";
        }
        return false;
    }

    SOCKET sock = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock == INVALID_SOCKET) {
        freeaddrinfo(result);
        if (error) {
            *error = "socket() failed";
        }
        return false;
    }

    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);
    const int connectResult = ::connect(sock, result->ai_addr, static_cast<int>(result->ai_addrlen));
    freeaddrinfo(result);

    if (connectResult == SOCKET_ERROR) {
        const int errorCode = WSAGetLastError();
        if (errorCode != WSAEWOULDBLOCK && errorCode != WSAEINPROGRESS) {
            closesocket(sock);
            if (error) {
                *error = "connect failed (" + std::to_string(errorCode) + ")";
            }
            return false;
        }
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(sock, &writeSet);
        timeval timeout{};
        timeout.tv_sec = 2;
        if (::select(0, nullptr, &writeSet, nullptr, &timeout) <= 0) {
            closesocket(sock);
            if (error) {
                *error = "connect timeout";
            }
            return false;
        }
        int soError = 0;
        int length = sizeof(soError);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &length);
        if (soError != 0) {
            closesocket(sock);
            if (error) {
                *error = "connect failed (" + std::to_string(soError) + ")";
            }
            return false;
        }
    }

    const auto sentAt = std::chrono::steady_clock::now();
    Header hello{kMagic, kProtocolVersion, kTypeHello, sizeof(Hello)};
    Hello payload{kProtocolVersion, 0};
    if (!sendAll(sock, &hello, sizeof(hello)) || !sendAll(sock, &payload, sizeof(payload))) {
        closesocket(sock);
        if (error) {
            *error = "send failed";
        }
        return false;
    }

    std::vector<uint8_t> buffer;
    uint64_t deadline = GetTickCount64() + 2000;
    while (GetTickCount64() < deadline) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock, &readSet);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;
        const int selected = ::select(0, &readSet, nullptr, nullptr, &timeout);
        if (selected <= 0) {
            continue;
        }
        uint8_t chunk[256];
        const int received = ::recv(sock, reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
        if (received <= 0) {
            break;
        }
        buffer.insert(buffer.end(), chunk, chunk + received);
        if (buffer.size() >= sizeof(Header)) {
            Header header{};
            std::memcpy(&header, buffer.data(), sizeof(header));
            if (header.magic != kMagic || header.version != kProtocolVersion || header.length > kMaxPayload) {
                break;
            }
            if (buffer.size() >= sizeof(Header) + header.length) {
                if (header.type != kTypeWelcome || header.length < sizeof(Welcome)) {
                    break;
                }
                Welcome welcome{};
                std::memcpy(&welcome, buffer.data() + sizeof(Header), sizeof(welcome));
                closesocket(sock);
                if (status) {
                    *status = static_cast<Status>(welcome.status);
                }
                if (rttMs != nullptr) {
                    const auto elapsed = std::chrono::steady_clock::now() - sentAt;
                    *rttMs = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
                }
                return true;
            }
        }
    }

    closesocket(sock);
    if (error) {
        *error = "no reply";
    }
    return false;
}

}  // namespace control
}  // namespace sn2lan
