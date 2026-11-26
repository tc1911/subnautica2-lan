#include <cassert>
// Integration test for the UDP tunnel: two Tunnel instances in one process
// exchange real datagrams over loopback. Windows only (winsock); run it under
// Wine from Linux: scripts/test-tunnel-wine.sh
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "tunnel.h"
#include "control.h"

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::printf("ok   %s\n", what);
    } else {
        std::printf("FAIL %s\n", what);
        ++g_failures;
    }
}

bool waitPop(sn2lan::Tunnel& tunnel, std::string* puid, std::vector<uint8_t>* data, int timeoutMs, uint8_t expectedChannel = 0) {
    std::vector<uint8_t> buffer(65536);
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < static_cast<DWORD>(timeoutMs)) {
        uint32_t size = 0;
        uint8_t channel = 0;
        uint32_t written = 0;
        if (tunnel.peek(&size, nullptr) &&
            tunnel.pop(nullptr, puid, &channel, buffer.data(), static_cast<uint32_t>(buffer.size()), &written)) {
            if (channel == expectedChannel) {
                buffer.resize(written);
                *data = std::move(buffer);
                return true;
            }
        }
        Sleep(20);
    }
    return false;
}

}  // namespace

void testControlChannel() {
    sn2lan::control::Server server;
    bool ready = false;
    assert(server.start(27777, [&ready]() { return ready; }));

    sn2lan::control::Status status = sn2lan::control::Status::Ready;
    std::string error;
    assert(sn2lan::control::probe("127.0.0.1", 27777, &status, &error));
    assert(status == sn2lan::control::Status::NotReady);

    ready = true;
    assert(sn2lan::control::probe("127.0.0.1", 27777, &status, &error));
    assert(status == sn2lan::control::Status::Ready);

    server.stop();
    assert(!server.running());
    assert(!sn2lan::control::probe("127.0.0.1", 27777, &status, &error));
    std::printf("ok   control channel not-ready/ready, listener stops\n");
}

int main() {
    const std::string hostPuid(32, 'a');
    const std::string clientPuid(32, 'b');

    sn2lan::Tunnel host;
    sn2lan::Tunnel client;

    sn2lan::Tunnel::Options hostOptions;
    hostOptions.isHost = true;
    hostOptions.bindAddress = "127.0.0.1";
    hostOptions.bindPort = 27098;
    hostOptions.mtu = 1400;

    sn2lan::Tunnel::Options clientOptions;
    clientOptions.isHost = false;
    clientOptions.bindAddress = "127.0.0.1";
    clientOptions.bindPort = 27097;
    clientOptions.hostAddress = "127.0.0.1";
    clientOptions.hostPort = 27098;
    clientOptions.mtu = 1400;

    check(host.start(hostOptions), "host tunnel starts");
    check(client.start(clientOptions), "client tunnel starts");

    // Small packet client -> host.
    const char small[] = "hello tunnel";
    bool sent = false;
    for (int i = 0; i < 50 && !sent; ++i) {
        sent = client.send(clientPuid.c_str(), hostPuid.c_str(), 0, small, sizeof(small));
        if (!sent) {
            Sleep(20);
        }
    }
    check(sent, "client can send to host");

    std::string from;
    std::vector<uint8_t> received(65536);
    check(waitPop(host, &from, &received, 3000), "host receives the packet");
    check(from == clientPuid, "host sees the client puid as sender");
    check(received.size() == sizeof(small) && std::memcmp(received.data(), small, sizeof(small)) == 0,
          "payload arrives intact");

    // Host knows the client's address now and can reply.
    bool replied = false;
    for (int i = 0; i < 50 && !replied; ++i) {
        replied = host.send(hostPuid.c_str(), clientPuid.c_str(), 0, "world", 5);
        if (!replied) {
            Sleep(20);
        }
    }
    check(replied, "host can send back to the learned client address");
    check(waitPop(client, &from, &received, 3000), "client receives the reply");
    check(from == hostPuid, "client sees the host puid as sender");
    check(received.size() == 5 && std::memcmp(received.data(), "world", 5) == 0, "reply payload intact");
    check(client.recentlySeen(hostPuid.c_str()), "client marks the host as directly reachable");
    check(host.recentlySeen(clientPuid.c_str()), "host marks the client as directly reachable");

    // Fragmented packet: 5000 bytes is four frames at mtu 1400.
    std::vector<uint8_t> big(5000);
    for (size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<uint8_t>(i * 31 + 7);
    }
    check(client.send(clientPuid.c_str(), hostPuid.c_str(), 3, big.data(), big.size()), "client sends 5000 bytes");
    check(waitPop(host, &from, &received, 3000, 3), "host reassembles the fragmented packet");
    check(received.size() == big.size() && std::memcmp(received.data(), big.data(), big.size()) == 0,
          "fragmented payload intact");

    const sn2lan::Tunnel::Stats stats = client.stats();
    check(stats.datagramsSent >= 5, "client sent multiple datagrams");
    check(stats.parseErrors == 0, "no parse errors");

    client.stop();
    host.stop();

    testControlChannel();

    std::printf(g_failures == 0 ? "all tunnel tests passed\n" : "%d tunnel test(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
