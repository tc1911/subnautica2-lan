#pragma once

#include <cstdint>
#include <string>

namespace sn2lan {

// Text shown in the in-game overlay. The mod fills this in from the tunnel
// state; the UI thread only renders it.
struct OverlayState {
    bool isHost = false;
    std::string target;   // "ip:port" pre-filled in the join box
    std::string line1;    // role / listen port
    std::string line2;    // host ready / peer count
    std::string line3;    // identity state
    std::string message;  // transient hint at the bottom
    std::string messageEn;

    // Raw values so a caller can draw its own (ASCII) layout.
    bool ipMode = true;
    uint16_t controlPort = 7777;
    std::string hostAddress;
    bool hostReady = false;
    bool joinIssued = false;
    int peerCount = 0;
    int probeStatus = 0;  // 0=unknown, 1=host online but not hosting, 2=ready
    bool identityReady = false;
};

using OverlayJoinHandler = void (*)(const std::string& target);

// Starts the overlay window on its own thread. Safe to call more than once.
bool overlayStart(OverlayJoinHandler handler);
void overlayStop();

// Thread-safe; the window refreshes itself a few times per second.
void overlaySetState(const OverlayState& state);
void overlayMessage(const std::string& message);
void overlayHide();

}  // namespace sn2lan
