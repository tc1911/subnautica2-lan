#include "game_overlay.h"

#include <windows.h>
#include <windowsx.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "log.h"

namespace sn2lan {

namespace {

// RVAs in Subnautica2-Win64-Test.exe (build 533), resolved from the PDB.
constexpr uintptr_t kBeginRva = 0x5FDEA10;
constexpr uintptr_t kEndRva = 0x603F380;
constexpr uintptr_t kTextUnformattedRva = 0x60D2860;
constexpr uintptr_t kButtonRva = 0x5FEBDB0;
constexpr uintptr_t kInputTextRva = 0x60747F0;
constexpr uintptr_t kSameLineRva = 0x60982F0;
constexpr uintptr_t kSetNextWindowPosRva = 0x609DD40;
constexpr uintptr_t kSetNextWindowSizeRva = 0x609DD80;
constexpr uintptr_t kIsWindowHoveredRva = 0x60799F0;
constexpr uintptr_t kGetMousePosRva = 0x60698B0;
constexpr uintptr_t kGetCursorScreenPosRva = 0x6067B30;
constexpr uintptr_t kIsItemActiveRva = 0x6078A70;
constexpr uintptr_t kIsItemHoveredRva = 0x6078B10;
constexpr uintptr_t kSetNextItemWidthRva = 0x609DCC0;
constexpr uintptr_t kGetIoRva = 0x6068340;
constexpr uintptr_t kAddMousePosEventRva = 0x5FDA180;
constexpr uintptr_t kAddMouseButtonEventRva = 0x5FD9FC0;

struct Vec2 {
    float x;
    float y;
};

using BeginFn = bool (*)(const char* name, bool* open, int flags);
using EndFn = void (*)();
using TextFn = void (*)(const char* text, const char* textEnd);
using ButtonFn = bool (*)(const char* label, const Vec2* size);
using InputTextFn = bool (*)(const char* label, char* buffer, size_t bufferSize, int flags, void* callback,
                             void* userData);
using SameLineFn = void (*)(float offset, float spacing);
using SetNextWindowPosFn = void (*)(const Vec2* pos, int condition, const Vec2* pivot);
using SetNextWindowSizeFn = void (*)(const Vec2* size, int condition);
using IsWindowHoveredFn = bool (*)(int flags);
using GetMousePosFn = Vec2 (*)();
using GetCursorScreenPosFn = void (*)(Vec2*);
using IsItemActiveFn = bool (*)();
using IsItemHoveredFn = bool (*)(int);
using GetIoFn = void* (*)();
using AddMousePosEventFn = void (*)(void* io, float x, float y);
using AddMouseButtonEventFn = void (*)(void* io, int button, bool down);
using SetNextItemWidthFn = void (*)(float width);

BeginFn g_begin = nullptr;
EndFn g_end = nullptr;
TextFn g_text = nullptr;
ButtonFn g_button = nullptr;
InputTextFn g_inputText = nullptr;
SameLineFn g_sameLine = nullptr;
SetNextWindowPosFn g_setNextWindowPos = nullptr;
SetNextWindowSizeFn g_setNextWindowSize = nullptr;
IsWindowHoveredFn g_isWindowHovered = nullptr;
GetMousePosFn g_getMousePos = nullptr;
GetCursorScreenPosFn g_getCursorScreenPos = nullptr;
IsItemActiveFn g_isItemActive = nullptr;
IsItemHoveredFn g_isItemHovered = nullptr;
GetIoFn g_getIo = nullptr;
AddMousePosEventFn g_addMousePosEvent = nullptr;
AddMouseButtonEventFn g_addMouseButtonEvent = nullptr;
SetNextItemWidthFn g_setNextItemWidth = nullptr;

std::atomic<uint64_t> g_frames{0};
uintptr_t g_exeBase = 0;
uint64_t g_lastMouseLog = 0;
bool g_ready = false;
std::atomic<bool> g_visible{false};
std::atomic<int> g_editField{0};
std::mutex g_keyMutex;
std::vector<int> g_keyQueue;
HHOOK g_kbHook = nullptr;
HHOOK g_mouseHook = nullptr;
HANDLE g_hookThread = nullptr;
std::atomic<bool> g_hooksStarted{false};
std::atomic<bool> g_clickPending{false};
std::atomic<int> g_clickX{0};
std::atomic<int> g_clickY{0};
struct RectI { float x = 0, y = 0, w = 0, h = 0; };
RectI g_widgetRects[4];  // 0=host 1=port 2=connect 3=hide
std::mutex g_rectMutex;

// On Wine/Xwayland, Win32 client coordinates do not track the compositor's real
// window position (niri can move the window without Wine noticing), so map the
// cursor through X11: imgui = root_pointer - root_origin_of_window_under_pointer.
using XOpenDisplayFn = void* (*)(const char*);
using XDefaultRootWindowFn = unsigned long (*)(void*);
using XQueryPointerFn = int (*)(void*, unsigned long, unsigned long*, unsigned long*, int*, int*, int*, int*, unsigned int*);
using XTranslateCoordinatesFn = int (*)(void*, unsigned long, unsigned long, int, int, int*, int*, unsigned long*);
struct X11Api {
    void* dpy = nullptr;
    XDefaultRootWindowFn rootWindow = nullptr;
    XQueryPointerFn queryPointer = nullptr;
    XTranslateCoordinatesFn translate = nullptr;
    bool ok = false;
};
X11Api& x11Api() {
    static X11Api api = [] {
        X11Api a;
        static const char* kCandidates[] = {"libX11.so.6", "/usr/lib/libX11.so.6",
                                            "/usr/lib/x86_64-linux-gnu/libX11.so.6", "libX11.so"};
        HMODULE mod = nullptr;
        for (const char* name : kCandidates) {
            mod = LoadLibraryA(name);
            if (mod != nullptr) {
                SN2_LOG_INFO("overlay pointer mapping: loaded %s", name);
                break;
            }
            SN2_LOG_DEBUG("overlay pointer mapping: LoadLibrary(%s) failed err=%lu", name, GetLastError());
        }
        if (mod == nullptr) return a;
        auto open = reinterpret_cast<XOpenDisplayFn>(GetProcAddress(mod, "XOpenDisplay"));
        a.rootWindow = reinterpret_cast<XDefaultRootWindowFn>(GetProcAddress(mod, "XDefaultRootWindow"));
        a.queryPointer = reinterpret_cast<XQueryPointerFn>(GetProcAddress(mod, "XQueryPointer"));
        a.translate = reinterpret_cast<XTranslateCoordinatesFn>(GetProcAddress(mod, "XTranslateCoordinates"));
        if (open == nullptr || a.rootWindow == nullptr || a.queryPointer == nullptr || a.translate == nullptr) {
            return a;
        }
        a.dpy = open(nullptr);
        a.ok = a.dpy != nullptr;
        return a;
    }();
    return api;
}
bool x11ImguiPos(int& ix, int& iy) {
    X11Api& api = x11Api();
    if (!api.ok) return false;
    const unsigned long root = api.rootWindow(api.dpy);
    unsigned long child = 0;
    unsigned long rootRet = 0;
    int rx = 0, ry = 0, wx = 0, wy = 0;
    unsigned int mask = 0;
    if (!api.queryPointer(api.dpy, root, &rootRet, &child, &rx, &ry, &wx, &wy, &mask)) return false;
    if (child == 0) return false;
    int ox = 0, oy = 0;
    unsigned long ignored = 0;
    if (!api.translate(api.dpy, child, root, 0, 0, &ox, &oy, &ignored)) return false;
    ix = rx - ox;
    iy = ry - oy;
    return ix > -8 && iy > -8 && ix < 8192 && iy < 8192;
}

std::atomic<long> g_clientX{0};
std::atomic<long> g_clientY{0};
std::atomic<bool> g_clientValid{false};
HWND g_gameHwnd = nullptr;
WNDPROC g_oldWndProc = nullptr;

static LRESULT CALLBACK gameWndProc(HWND hwnd, UINT msg, WPARAM, LPARAM lp);

void ensureGameWndProc() {
    if (g_oldWndProc != nullptr) {
        return;
    }
    HWND hwnd = FindWindowW(L"UnrealWindow", nullptr);
    if (hwnd == nullptr) {
        return;
    }
    WNDPROC old = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(gameWndProc)));
    if (old == nullptr) {
        return;
    }
    g_gameHwnd = hwnd;
    g_oldWndProc = old;
    SN2_LOG_INFO("game window subclassed for exact client input (hwnd=%p)", reinterpret_cast<void*>(hwnd));
}

static LRESULT CALLBACK gameWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP: {
            g_clientX.store(GET_X_LPARAM(lparam), std::memory_order_relaxed);
            g_clientY.store(GET_Y_LPARAM(lparam), std::memory_order_relaxed);
            g_clientValid.store(true, std::memory_order_relaxed);
            break;
        }
        default:
            break;
    }
    return CallWindowProcW(g_oldWndProc, hwnd, msg, wparam, lparam);
}
std::atomic<int> g_focus{2};
std::atomic<bool> g_keyConnect{false};
std::atomic<int> g_offsetX{0};
std::atomic<int> g_offsetY{62};
std::atomic<bool> g_calibrating{false};
std::atomic<bool> g_calibHavePoint{false};
std::atomic<int> g_calibX{0};
std::atomic<int> g_calibY{0};
bool g_f8WasDown = false;
bool g_f9WasDown = false;

std::mutex g_mutex;
OverlayState g_state;
OverlayJoinHandler g_joinHandler = nullptr;
char g_target[96] = {0};
char g_host[64] = {0};
char g_port[8] = "7777";
std::string g_targetSource;
bool g_hoverWas = false;
bool g_itemHoverWas = false;
bool g_inputWasActive = false;


void pushKey(int ch) {
    std::lock_guard<std::mutex> lock(g_keyMutex);
    if (g_keyQueue.size() < 64) {
        g_keyQueue.push_back(ch);
    }
}

LRESULT CALLBACK kbProc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && g_visible.load(std::memory_order_relaxed) &&
        g_editField.load(std::memory_order_relaxed) == 0) {
        const auto* nav = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
        if ((wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN) && g_gameHwnd != nullptr &&
            GetForegroundWindow() == g_gameHwnd) {
            const DWORD vk = nav->vkCode;
            if (vk == VK_TAB || vk == VK_DOWN) {
                g_focus.store((g_focus.load(std::memory_order_relaxed) + 1) % 4, std::memory_order_relaxed);
                return 1;
            }
            if (vk == VK_UP) {
                g_focus.store((g_focus.load(std::memory_order_relaxed) + 3) % 4, std::memory_order_relaxed);
                return 1;
            }
            if (vk == VK_RETURN) {
                const int f = g_focus.load(std::memory_order_relaxed);
                if (f == 0 || f == 1) {
                    g_editField.store(f + 1);
                } else if (f == 2) {
                    g_keyConnect.store(true, std::memory_order_relaxed);
                } else {
                    g_visible.store(false);
                }
                return 1;
            }
            if (vk == VK_ESCAPE) {
                g_visible.store(false);
                return 1;
            }
        }
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }
    if (code == HC_ACTION && g_visible.load(std::memory_order_relaxed) &&
        g_editField.load(std::memory_order_relaxed) != 0) {
        const auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
        if (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN) {
            if (kb->vkCode == VK_BACK) {
                pushKey(-1);
                return 1;
            }
            if (kb->vkCode == VK_RETURN) {
                pushKey(-2);
                return 1;
            }
            if (kb->vkCode == VK_ESCAPE) {
                pushKey(-3);
                return 1;
            }
            if (kb->vkCode == VK_F8 || kb->vkCode == VK_F9) {
                return CallNextHookEx(nullptr, code, wparam, lparam);
            }
            BYTE keys[256];
            if (GetKeyboardState(keys)) {
                wchar_t buf[8] = {0};
                const int n = ToUnicode(kb->vkCode, kb->scanCode, keys, buf, 8, 0);
                for (int i = 0; i < n; ++i) {
                    if (buf[i] >= 32 && buf[i] < 127) {
                        pushKey(static_cast<int>(buf[i]));
                    }
                }
            }
            return 1;
        }
        if (wparam == WM_KEYUP || wparam == WM_SYSKEYUP) {
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK mouseProc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && g_visible.load(std::memory_order_relaxed)) {
        if (wparam == WM_LBUTTONDOWN || wparam == WM_LBUTTONUP) {
            int ix = 0;
            int iy = 0;
            if (!x11ImguiPos(ix, iy)) {
                POINT pt{0, 0};
                if (g_clientValid.load(std::memory_order_relaxed)) {
                    pt.x = g_clientX.load(std::memory_order_relaxed);
                    pt.y = g_clientY.load(std::memory_order_relaxed);
                } else {
                    GetCursorPos(&pt);
                    HWND hwndFallback = FindWindowW(L"UnrealWindow", nullptr);
                    if (hwndFallback != nullptr) {
                        ScreenToClient(hwndFallback, &pt);
                    }
                }
                ix = pt.x - g_offsetX.load(std::memory_order_relaxed);
                iy = pt.y - g_offsetY.load(std::memory_order_relaxed);
            }
            if (wparam == WM_LBUTTONDOWN) {
                g_clickX.store(ix, std::memory_order_relaxed);
                g_clickY.store(iy, std::memory_order_relaxed);
                SN2_LOG_DEBUG("mouse hook: click imgui=%d,%d calib=%d", ix, iy,
                              g_calibrating.load(std::memory_order_relaxed) ? 1 : 0);
            }
            if (g_calibrating.load(std::memory_order_relaxed)) {
                if (wparam == WM_LBUTTONDOWN) {
                    g_calibX.store(ix, std::memory_order_relaxed);
                    g_calibY.store(iy, std::memory_order_relaxed);
                    g_calibHavePoint.store(true, std::memory_order_relaxed);
                }
                return 1;
            }
            bool inside = false;
            {
                std::lock_guard<std::mutex> lock(g_rectMutex);
                for (const RectI& r : g_widgetRects) {
                    if (r.w > 0 && ix >= r.x - 6 && ix <= r.x + r.w + 6 && iy >= r.y - 6 &&
                        iy <= r.y + r.h + 6) {
                        inside = true;
                        break;
                    }
                }
            }
            if (inside) {
                if (wparam == WM_LBUTTONDOWN) {
                    g_clickPending.store(true, std::memory_order_relaxed);
                }
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

DWORD WINAPI hookThreadProc(void*) {
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, kbProc, nullptr, 0);
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, nullptr, 0);
    SN2_LOG_INFO("overlay input hooks: keyboard=%p mouse=%p", reinterpret_cast<void*>(g_kbHook),
                 reinterpret_cast<void*>(g_mouseHook));
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

void startInputHooks() {
    bool expected = false;
    if (g_hooksStarted.compare_exchange_strong(expected, true)) {
        g_hookThread = CreateThread(nullptr, 0, hookThreadProc, nullptr, 0, nullptr);
    }
}


std::wstring uiStatePath() {
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&uiStatePath), &mod);
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(mod, path, MAX_PATH);
    std::wstring str(path);
    const size_t slash = str.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        str.resize(slash + 1);
    }
    str += L"sn2lan_ui.txt";
    return str;
}

bool loadOffsets(int& ox, int& oy) {
    FILE* f = _wfopen(uiStatePath().c_str(), L"rb");
    if (f == nullptr) {
        return false;
    }
    int x = 0;
    int y = 0;
    const bool ok = std::fscanf(f, "%d %d", &x, &y) == 2;
    std::fclose(f);
    if (!ok || x < -2000 || x > 2000 || y < -2000 || y > 2000) {
        return false;
    }
    ox = x;
    oy = y;
    return true;
}

void saveOffsets(int ox, int oy) {
    FILE* f = _wfopen(uiStatePath().c_str(), L"wb");
    if (f != nullptr) {
        std::fprintf(f, "%d %d\n", ox, oy);
        std::fclose(f);
    }
}

}  // namespace

bool gameOverlayResolve(void* exeBase) {
    if (exeBase == nullptr) {
        return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(exeBase);
    g_exeBase = base;
    g_begin = reinterpret_cast<BeginFn>(base + kBeginRva);
    g_end = reinterpret_cast<EndFn>(base + kEndRva);
    g_text = reinterpret_cast<TextFn>(base + kTextUnformattedRva);
    g_button = reinterpret_cast<ButtonFn>(base + kButtonRva);
    g_inputText = reinterpret_cast<InputTextFn>(base + kInputTextRva);
    g_sameLine = reinterpret_cast<SameLineFn>(base + kSameLineRva);
    g_setNextWindowPos = reinterpret_cast<SetNextWindowPosFn>(base + kSetNextWindowPosRva);
    g_setNextWindowSize = reinterpret_cast<SetNextWindowSizeFn>(base + kSetNextWindowSizeRva);
    g_getCursorScreenPos = reinterpret_cast<GetCursorScreenPosFn>(base + kGetCursorScreenPosRva);
    g_ready = true;
    g_visible = true;
    int ox = 0;
    int oy = 62;
    if (x11Api().ok) {
        g_calibrating.store(false);
        SN2_LOG_INFO("overlay pointer mapping: x11 (client coordinates from XQueryPointer)");
    } else if (loadOffsets(ox, oy)) {
        g_offsetX.store(ox);
        g_offsetY.store(oy);
        SN2_LOG_INFO("overlay alignment restored: offset=(%d,%d)", ox, oy);
    } else {
        g_calibrating.store(true);
        SN2_LOG_INFO("overlay alignment: first run, calibration needed");
    }
    SN2_LOG_INFO("in-game overlay ready (ImGui Begin=%p)", reinterpret_cast<void*>(g_begin));
    return true;
}

void gameOverlaySetJoinHandler(OverlayJoinHandler handler) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_joinHandler = handler;
}

void gameOverlaySetState(const OverlayState& state) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state = state;
    if (!state.target.empty() && state.target != g_targetSource) {
        g_targetSource = state.target;
        std::strncpy(g_target, state.target.c_str(), sizeof(g_target) - 1);
        g_target[sizeof(g_target) - 1] = '\0';
        // Split host:port into the two editable fields.
        std::string t = state.target;
        const size_t colon = t.rfind(':');
        std::string host = t;
        std::string port = "7777";
        if (colon != std::string::npos && colon + 1 < t.size()) {
            host = t.substr(0, colon);
            port = t.substr(colon + 1);
        }
        std::strncpy(g_host, host.c_str(), sizeof(g_host) - 1);
        g_host[sizeof(g_host) - 1] = '\0';
        std::strncpy(g_port, port.c_str(), sizeof(g_port) - 1);
        g_port[sizeof(g_port) - 1] = '\0';
    }
}

void gameOverlayShow(bool show) {
    g_visible = show;
}

void gameOverlayHide() {
    g_visible = false;
}

void gameOverlayFeedInput() {
    if (g_exeBase == 0) {
        return;
    }
    const int field = g_editField.load(std::memory_order_relaxed);
    if (field != 0) {
        std::vector<int> keys;
        {
            std::lock_guard<std::mutex> lock(g_keyMutex);
            keys.swap(g_keyQueue);
        }
        char* buf = field == 1 ? g_host : g_port;
        const size_t cap = field == 1 ? sizeof(g_host) : sizeof(g_port);
        for (const int ch : keys) {
            size_t len = std::strlen(buf);
            if (ch == -1) {
                if (len > 0) {
                    buf[len - 1] = '\0';
                }
            } else if (ch == -2) {
                g_editField.store(0);
                break;
            } else if (ch == -3) {
                g_editField.store(0);
                break;
            } else if (ch >= 32 && ch < 127 && len + 1 < cap) {
                if (field == 2 && (ch < '0' || ch > '9')) {
                    continue;
                }
                buf[len] = static_cast<char>(ch);
                buf[len + 1] = '\0';
            }
        }
    }
    uintptr_t ctx = *reinterpret_cast<uintptr_t*>(g_exeBase + 0xDC72890);
    if (ctx == 0) {
        return;
    }
    ensureGameWndProc();
    int ix = 0;
    int iy = 0;
    if (!x11ImguiPos(ix, iy)) {
        POINT pt{0, 0};
        if (g_clientValid.load(std::memory_order_relaxed)) {
            pt.x = g_clientX.load(std::memory_order_relaxed);
            pt.y = g_clientY.load(std::memory_order_relaxed);
        } else {
            GetCursorPos(&pt);
            HWND hwndFallback = g_gameHwnd != nullptr ? g_gameHwnd : FindWindowW(L"UnrealWindow", nullptr);
            if (hwndFallback != nullptr) {
                ScreenToClient(hwndFallback, &pt);
            }
        }
        ix = pt.x - g_offsetX.load(std::memory_order_relaxed);
        iy = pt.y - g_offsetY.load(std::memory_order_relaxed);
    }
    const float fx = static_cast<float>(ix);
    const float fy = static_cast<float>(iy);
    *reinterpret_cast<float*>(ctx + 0xE0) = fx;
    *reinterpret_cast<float*>(ctx + 0xE4) = fy;
    const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    *reinterpret_cast<unsigned char*>(ctx + 0xE8) = down ? 1 : 0;
    static uint64_t last = 0;
    const uint64_t now = GetTickCount64();
    if (now - last > 3000) {
        last = now;
        SN2_LOG_DEBUG("feed cursor imgui=(%.0f,%.0f) hwnd=%p", fx, fy,
                     reinterpret_cast<void*>(g_gameHwnd));
    }
}

void gameOverlayDraw() {
    g_frames.fetch_add(1, std::memory_order_relaxed);
    if (!g_ready) {
        return;
    }
    startInputHooks();
    if (g_exeBase != 0) {
        void* ctx = *reinterpret_cast<void**>(g_exeBase + 0xDC72890);
        const uint64_t now = GetTickCount64();
        if (ctx != nullptr && now - g_lastMouseLog > 2000) {
            g_lastMouseLog = now;
            const float* disp = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(ctx) + 0x08);
            SN2_LOG_DEBUG("io DisplaySize=%.0fx%.0f DisplayPos=%.0f,%.0f framebuffer=%.2fx%.2f", disp[0], disp[1],
                         disp[2], disp[3], disp[4], disp[5]);
            const float* mp = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(ctx) + 0xE0);
            POINT pt{0, 0};
            GetCursorPos(&pt);
            HWND fg = GetForegroundWindow();
            RECT rc{0, 0, 0, 0};
            if (fg) GetClientRect(fg, &rc);
            POINT cl = pt;
            if (fg) ScreenToClient(fg, &cl);
            SN2_LOG_DEBUG("io.MousePos=(%.1f,%.1f) cursor=(%ld,%ld) client=(%ld,%ld) size=%ldx%ld", mp[0], mp[1],
                         pt.x, pt.y, cl.x, cl.y, rc.right, rc.bottom);
        }
    }

    const bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (down && !g_f8WasDown) {
        g_visible = !g_visible;
    }
    g_f8WasDown = down;

    static bool f7WasDown = false;
    const bool f7Down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (f7Down && !f7WasDown) {
        if (!x11Api().ok) {
            g_calibrating.store(true);
            g_calibHavePoint.store(false);
            g_visible = true;
        }
    }
    f7WasDown = f7Down;

    if (g_calibrating.load(std::memory_order_relaxed)) {
        static const Vec2 kCalibSize{560.0f, 190.0f};
        static const Vec2 kPosition{40.0f, 90.0f};
        static const Vec2 kZero{0.0f, 0.0f};
        Vec2 target{0.0f, 0.0f};
        bool haveTarget = false;
        g_setNextWindowPos(&kPosition, 1, &kZero);
        g_setNextWindowSize(&kCalibSize, 1);
        if (g_begin("SN2LAN setup##calib", nullptr, 0)) {
            g_text("First run: align the panel with your screen.", nullptr);
            g_text("Click the + button below (once).", nullptr);
            if (g_getCursorScreenPos != nullptr) {
                g_getCursorScreenPos(&target);
                haveTarget = true;
            }
            static const Vec2 kTargetSize{48.0f, 48.0f};
            g_button("+##calibTarget", &kTargetSize);
            g_text("Press F7 later if the panel and the mouse drift apart.", nullptr);
        }
        g_end();
        if (haveTarget && g_calibHavePoint.load(std::memory_order_relaxed)) {
            const int clickX = g_calibX.load(std::memory_order_relaxed);
            const int clickY = g_calibY.load(std::memory_order_relaxed);
            const int ox = clickX - static_cast<int>(target.x + 24.0f);
            const int oy = clickY - static_cast<int>(target.y + 24.0f);
            if (ox > -2000 && ox < 2000 && oy > -2000 && oy < 2000) {
                g_offsetX.store(ox);
                g_offsetY.store(oy);
                g_calibHavePoint.store(false);
                g_calibrating.store(false);
                saveOffsets(ox, oy);
                SN2_LOG_INFO("overlay alignment calibrated: offset=(%d,%d)", ox, oy);
            }
        }
        return;
    }

    const bool connectDown = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    const bool connectPressed = connectDown && !g_f9WasDown;
    g_f9WasDown = connectDown;

    if (!g_visible) {
        return;
    }

    OverlayState state;
    OverlayJoinHandler handler = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        state = g_state;
        handler = g_joinHandler;
    }

    static const Vec2 kPosition{40.0f, 90.0f};
    static const Vec2 kSize{560.0f, 245.0f};
    static const Vec2 kZero{0.0f, 0.0f};
    static const Vec2 kConnectSize{110.0f, 0.0f};
    static const Vec2 kPortSize{70.0f, 0.0f};

    char line1[192];
    char line2[224];
    char line3[192];
    if (state.isHost) {
        std::snprintf(line1, sizeof(line1), "Role: host   game UDP %u / handshake TCP %u", state.controlPort,
                      state.controlPort);
        if (state.hostReady) {
            std::snprintf(line2, sizeof(line2), "Room: open, waiting for players (%d connected)", state.peerCount);
        } else {
            std::snprintf(line2, sizeof(line2), "Room: not open - click Create Multiplayer in game");
        }
    } else {
        std::snprintf(line1, sizeof(line1), "Role: client");
        if (state.joinIssued) {
            std::snprintf(line2, sizeof(line2), "Status: joining the host ...");
        } else if (state.probeStatus == 2 || state.hostReady) {
            std::snprintf(line2, sizeof(line2), "Status: host ready - press Connect");
        } else if (state.probeStatus == 1) {
            std::snprintf(line2, sizeof(line2), "Status: host online, room not open yet");
        } else {
            std::snprintf(line2, sizeof(line2), "Status: host not detected - you can still try");
        }
    }
    std::snprintf(line3, sizeof(line3), "Identity: %s",
                  state.identityReady ? "account login found" : "waiting (SteamID only)");

    if (connectPressed || g_keyConnect.exchange(false, std::memory_order_relaxed)) {
        std::snprintf(g_target, sizeof(g_target), "%s:%s", g_host, g_port[0] != '\0' ? g_port : "7777");
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_joinHandler != nullptr && g_host[0] != '\0') {
            g_joinHandler(std::string(g_target));
        }
        return;
    }

    g_setNextWindowPos(&kPosition, 1 /*Always*/, &kZero);
    g_setNextWindowSize(&kSize, 1);
    if (g_begin("SN2LAN##sn2lan", nullptr, 0)) {
        // The game's ImGui backend feeds mouse/keyboard to every window it owns;
        // log transitions so a non-interactive panel is obvious in sn2lan.log.
        if (g_isWindowHovered != nullptr) {
            const bool hoveredNow = g_isWindowHovered(0);
            if (hoveredNow != g_hoverWas) {
                g_hoverWas = hoveredNow;
                POINT pt;
                GetCursorPos(&pt);
                SN2_LOG_INFO("overlay: mouse %s panel (cursor %ld,%ld)", hoveredNow ? "over" : "left", pt.x, pt.y);
            }
        }

        g_text(line1, nullptr);
        g_text(line2, nullptr);
        g_text(line3, nullptr);
        g_text("Host address (IP or domain):", nullptr);
        static const Vec2 kFieldSize{548.0f, 0.0f};
        Vec2 tlHost{}, tlPort{}, tlConnect{}, tlHide{};
        static const Vec2 kPortSize{160.0f, 0.0f};
        const bool blink = ((g_frames.load(std::memory_order_relaxed) / 20) % 2) == 0;
        char label[128];
        std::snprintf(label, sizeof(label), "%s%s%s##host", (g_focus.load() == 0) ? "> " : "", g_host,
                      (g_editField.load() == 1 && blink) ? "|" : "");
        if (g_getCursorScreenPos != nullptr) g_getCursorScreenPos(&tlHost);
        if (g_button(label, &kFieldSize)) {
            g_editField.store(1);
        }
        { std::lock_guard<std::mutex> lock(g_rectMutex); g_widgetRects[0] = RectI{tlHost.x, tlHost.y, kFieldSize.x, 24.0f}; }
        std::snprintf(label, sizeof(label), "%s%s%s##port", (g_focus.load() == 1) ? "> " : "", g_port,
                      (g_editField.load() == 2 && blink) ? "|" : "");
        if (g_getCursorScreenPos != nullptr) g_getCursorScreenPos(&tlPort);
        if (g_button(label, &kPortSize)) {
            g_editField.store(2);
        }
        { std::lock_guard<std::mutex> lock(g_rectMutex); g_widgetRects[1] = RectI{tlPort.x, tlPort.y, kPortSize.x, 24.0f}; }
        g_sameLine(0.0f, 8.0f);
        g_sameLine(0.0f, 8.0f);
        if (g_getCursorScreenPos != nullptr) g_getCursorScreenPos(&tlConnect);
        const char* connectLabel = (g_focus.load() == 2) ? "> Connect##connect" : "Connect##connect";
        if (g_button(connectLabel, &kConnectSize) && handler != nullptr && g_host[0] != '\0') {
            std::snprintf(g_target, sizeof(g_target), "%s:%s", g_host, g_port[0] != '\0' ? g_port : "7777");
            SN2_LOG_INFO("overlay: Connect clicked for %s", g_target);
            handler(std::string(g_target));
        }
        if (g_isItemHovered != nullptr) {
            const bool itemNow = g_isItemHovered(0);
            if (itemNow != g_itemHoverWas) {
                g_itemHoverWas = itemNow;
                SN2_LOG_INFO("overlay: Connect item hover %d", itemNow ? 1 : 0);
            }
        }
        { std::lock_guard<std::mutex> lock(g_rectMutex); g_widgetRects[2] = RectI{tlConnect.x, tlConnect.y, kConnectSize.x, 24.0f}; }
        g_sameLine(0.0f, 6.0f);
        if (g_getCursorScreenPos != nullptr) g_getCursorScreenPos(&tlHide);
        if (g_button("Hide##hide", &kZero)) {
            g_visible = false;
        }
        { std::lock_guard<std::mutex> lock(g_rectMutex); g_widgetRects[3] = RectI{tlHide.x, tlHide.y, 64.0f, 24.0f}; }
        if (!state.messageEn.empty()) {
            g_text(state.messageEn.c_str(), nullptr);
        } else {
            g_text("F8 shows or hides this panel. Click a field to edit, then Connect.", nullptr);
        }
    }
    g_end();

    if (g_clickPending.exchange(false, std::memory_order_relaxed)) {
        const float cx = static_cast<float>(g_clickX.load(std::memory_order_relaxed));
        const float cy = static_cast<float>(g_clickY.load(std::memory_order_relaxed));
        int hit = -1;
        std::lock_guard<std::mutex> lock(g_rectMutex);
        for (int i = 0; i < 4; ++i) {
            const RectI& r = g_widgetRects[i];
            if (r.w > 0 && cx >= r.x && cx <= r.x + r.w && cy >= r.y && cy <= r.y + r.h) {
                hit = i;
                break;
            }
        }
        SN2_LOG_DEBUG("panel click imgui=(%.0f,%.0f) hit=%d rects=%0.f,%0.f,%0.f | %0.f,%0.f,%0.f | %0.f,%0.f,%0.f", cx, cy, hit,
                      g_widgetRects[0].x, g_widgetRects[0].y, g_widgetRects[0].w,
                      g_widgetRects[1].x, g_widgetRects[1].y, g_widgetRects[1].w,
                      g_widgetRects[2].x, g_widgetRects[2].y, g_widgetRects[2].w);
        if (hit == 0) {
            g_editField.store(1);
        } else if (hit == 1) {
            g_editField.store(2);
        } else if (hit == 2 && handler != nullptr && g_host[0] != '\0') {
            std::snprintf(g_target, sizeof(g_target), "%s:%s", g_host, g_port[0] != '\0' ? g_port : "7777");
            SN2_LOG_INFO("overlay: Connect clicked for %s", g_target);
            handler(std::string(g_target));
        } else if (hit == 3) {
            g_visible = false;
        }
    }
}

uint64_t gameOverlayFrameCount() {
    return g_frames.load(std::memory_order_relaxed);
}

}  // namespace sn2lan
