// Standalone loader.
//
// The game imports dwmapi.dll, so dropping this file next to
// Subnautica2-Win64-Test.exe gives us code execution inside the game process
// without UE4SS. That matters because no UE4SS build currently supports the
// game's 0.2 build (533): the SN2 fork is stuck on the May build and upstream
// v4.0.0 crashes the game shortly after the lobby loads.
//
// Only the four functions the executable imports are implemented. They are
// deliberately minimal no-ops that report a composited desktop, which is what
// Wine's builtin dwmapi effectively does for this game; Unreal only uses them
// to pick a presentation mode. The actual mod (hooks, tunnel) lives in
// mod.cpp and is started from DllMain on a worker thread.

#include <windows.h>
#include <dwmapi.h>

extern "C" __declspec(dllexport) HRESULT WINAPI DwmIsCompositionEnabled(BOOL* enabled) {
    if (enabled != nullptr) {
        *enabled = TRUE;
    }
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DwmSetWindowAttribute(HWND /*hwnd*/, DWORD /*attribute*/,
                                                                     LPCVOID /*value*/, DWORD /*size*/) {
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DwmFlush(void) {
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DwmGetCompositionTimingInfo(HWND /*hwnd*/, DWM_TIMING_INFO* info) {
    if (info != nullptr) {
        ZeroMemory(info, sizeof(*info));
    }
    return S_OK;
}
