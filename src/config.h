#pragma once

#include <cstdint>
#include <string>

namespace sn2lan {

struct Config {
    bool enabled = false;
    bool isHost = false;
    std::string socketName = "GameNetDriver";

    // Host: address to bind. Client: local bind address (0.0.0.0 is fine).
    std::string bindAddress = "0.0.0.0";
    uint16_t bindPort = 28015;

    // Client only: where the host's UDP port is reachable (public IP / frp).
    std::string hostAddress;
    uint16_t hostPort = 28015;

    size_t mtu = 1400;
    bool debug = false;

    // Keep sending through EOS until the peer has proven that the direct UDP
    // path works. Makes joining robust when UDP is blocked.
    bool passthroughUntilSeen = true;

    // After this long without a direct packet a peer is considered gone and we
    // fall back to EOS again.
    uint32_t peerTimeoutMs = 10000;

    // EXPERIMENTAL (build 533 only): replace the game's NetDriverEOS with the
    // engine's plain-UDP IpNetDriver so the host listens on a normal port.
    bool forceIpDriver = false;

    // Client only: "ip:port" to ClientTravel to once the game world is up.
    // Empty disables it. Used for the plain-UDP (IpNetDriver) join path.
    std::string autoConnect;
    std::string loginOptions;

    // In-game overlay (F8) with a manual "join IP:port" box.
    bool overlay = true;
    std::string overlayMode = "imgui";

    // IP mode: TCP port for the join handshake (same number as the game's UDP
    // port, so only one port has to be published).
    uint16_t controlPort = 7777;

    // Seconds to wait after the mod starts before issuing the ClientTravel.
    uint32_t autoConnectDelaySec = 20;
};

// Reads config.ini. Missing file => returns false and leaves `out` at defaults.
// `error` receives a human readable reason on parse errors.
bool loadConfig(const std::wstring& path, Config* out, std::string* error);

}  // namespace sn2lan
