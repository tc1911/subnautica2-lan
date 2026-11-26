#pragma once

#include <cstdint>

#include "overlay.h"

namespace sn2lan {

// In-game overlay drawn with the ImGui copy that ships inside the game
// executable. All functions are safe to call from any thread.
bool gameOverlayResolve(void* exeBase);
void gameOverlayDraw();
void gameOverlayFeedInput();
void gameOverlaySetState(const OverlayState& state);
void gameOverlaySetJoinHandler(OverlayJoinHandler handler);
void gameOverlayShow(bool show);
void gameOverlayHide();
uint64_t gameOverlayFrameCount();

}  // namespace sn2lan
