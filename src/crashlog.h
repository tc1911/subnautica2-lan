#pragma once

namespace sn2lan {

// Logs the faulting address and the call stack (as module RVAs) to
// sn2lan_crash.txt next to the game executable, then lets the normal crash
// handler continue.
void installCrashLogger();

}  // namespace sn2lan
