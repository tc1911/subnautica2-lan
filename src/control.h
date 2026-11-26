#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace sn2lan {
namespace control {

// Single-port control channel: TCP on the same port number the game uses for
// UDP, so a host only has to publish one port (TCP + UDP) through frp.
constexpr uint16_t kProtocolVersion = 1;
constexpr uint16_t kDefaultPort = 7777;

enum class Status : uint8_t {
    Ready = 0,
    NotReady = 1,
    VersionMismatch = 2,
};

// Host side. Answers one handshake per connection, then closes.
class Server {
public:
    Server() = default;
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start(uint16_t port, std::function<bool()> readyProvider);
    void stop();
    bool running() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// Client side. TCP connect + HELLO + WELCOME with short timeouts.
bool probe(const std::string& address, uint16_t port, Status* status, std::string* error);

}  // namespace control
}  // namespace sn2lan
