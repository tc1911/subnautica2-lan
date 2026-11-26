#pragma once

#include <string>

namespace sn2lan {

class Tunnel;

// Installs MinHook detours for the EOS P2P entry points. Must be called after
// the EOS SDK module is available (it is loaded on demand if needed).
bool installHooks(Tunnel* tunnel);

// Detaches the tunnel from the detours; the detours themselves stay installed
// (and simply forward to EOS) so game threads can never execute into removed
// trampolines while we are shutting down.
void removeHooks();

// Only for DLL_PROCESS_DETACH: really uninstalls MinHook. The process is going
// away, so no game thread can be inside a detour any more.
void unhookAllForUnload();

// Manual join from the overlay: "ip:port". Picked up by the world tick hook.
void requestJoin(const std::string& target);
std::string loginOptionsPreview();
bool joinIssued();

bool hooksInstalled();
std::string hookStatus();

}  // namespace sn2lan
