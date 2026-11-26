#include "config.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace sn2lan {

namespace {

std::string trim(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool parseBool(const std::string& value, bool* out) {
    const std::string v = lower(trim(value));
    if (v == "1" || v == "true" || v == "yes" || v == "on") {
        *out = true;
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off") {
        *out = false;
        return true;
    }
    return false;
}

bool parsePort(const std::string& value, uint16_t* out) {
    try {
        const int v = std::stoi(trim(value));
        if (v < 0 || v > 65535) {
            return false;
        }
        *out = static_cast<uint16_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

bool parseSize(const std::string& value, size_t* out) {
    try {
        const long long v = std::stoll(trim(value));
        if (v <= 0) {
            return false;
        }
        *out = static_cast<size_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

bool loadConfig(const std::wstring& path, Config* out, std::string* error) {
    const std::filesystem::path file(path);
    std::ifstream in(file);
    if (!in.is_open()) {
        if (error) {
            *error = "config file not found";
        }
        return false;
    }

    Config cfg;
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#' || (trimmed[0] == '[' && trimmed.back() == ']')) {
            continue;
        }

        const size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            if (error) {
                *error = "line " + std::to_string(lineNo) + ": expected key=value";
            }
            return false;
        }

        const std::string key = lower(trim(trimmed.substr(0, eq)));
        const std::string value = trim(trimmed.substr(eq + 1));

        bool ok = true;
        if (key == "enabled") {
            ok = parseBool(value, &cfg.enabled);
        } else if (key == "role") {
            const std::string role = lower(value);
            if (role == "host" || role == "server") {
                cfg.isHost = true;
            } else if (role == "client") {
                cfg.isHost = false;
            } else {
                ok = false;
            }
        } else if (key == "socket_name") {
            cfg.socketName = value;
        } else if (key == "bind_address" || key == "listen_address") {
            cfg.bindAddress = value;
        } else if (key == "bind_port" || key == "listen_port") {
            ok = parsePort(value, &cfg.bindPort);
        } else if (key == "host" || key == "host_address") {
            cfg.hostAddress = value;
        } else if (key == "host_port") {
            ok = parsePort(value, &cfg.hostPort);
        } else if (key == "mtu") {
            ok = parseSize(value, &cfg.mtu);
        } else if (key == "overlay") {
            ok = parseBool(value, &cfg.overlay);
        } else if (key == "overlay_mode") {
            const std::string mode = lower(value);
            if (mode == "window" || mode == "imgui" || mode == "none") {
                cfg.overlayMode = mode;
            } else {
                cfg.overlayMode = "imgui";
            }
        } else if (key == "debug") {
            ok = parseBool(value, &cfg.debug);
        } else if (key == "passthrough_until_seen") {
            ok = parseBool(value, &cfg.passthroughUntilSeen);
        } else if (key == "force_ip_driver") {
            ok = parseBool(value, &cfg.forceIpDriver);
        } else if (key == "login_options") {
            cfg.loginOptions = value;
        } else if (key == "control_port") {
            ok = parsePort(value, &cfg.controlPort);
        } else if (key == "auto_connect" || key == "connect") {
            cfg.autoConnect = value;
        } else if (key == "auto_connect_delay") {
            size_t v = 0;
            ok = parseSize(value, &v);
            if (ok) {
                cfg.autoConnectDelaySec = static_cast<uint32_t>(std::min<size_t>(std::max<size_t>(v, 1), 600));
            }
        } else if (key == "peer_timeout_ms") {
            size_t v = 0;
            ok = parseSize(value, &v);
            if (ok) {
                cfg.peerTimeoutMs = static_cast<uint32_t>(std::min<size_t>(v, 600000));
            }
        } else {
            // Ignore unknown keys so older configs keep working.
            continue;
        }

        if (!ok) {
            if (error) {
                *error = "line " + std::to_string(lineNo) + ": invalid value for " + key + ": " + value;
            }
            return false;
        }
    }

    if (cfg.socketName.empty()) {
        cfg.socketName = "GameNetDriver";
    }
    if (cfg.mtu < 512 || cfg.mtu > 2000) {
        cfg.mtu = 1400;
    }
    if (!cfg.isHost && (cfg.hostAddress.empty() || cfg.hostPort == 0)) {
        if (error) {
            *error = "client mode requires host=<public ip> and host_port=<port>";
        }
        return false;
    }

    *out = cfg;
    return true;
}

}  // namespace sn2lan
