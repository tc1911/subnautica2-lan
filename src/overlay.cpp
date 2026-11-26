#include "overlay.h"

#include <windows.h>
#include <windowsx.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "log.h"

namespace sn2lan {

namespace {

constexpr int kIdTarget = 1001;
constexpr int kIdJoin = 1002;
constexpr int kIdHide = 1003;
constexpr UINT kMsgStop = WM_APP + 8;
constexpr UINT kHotkeyToggle = 1;
constexpr int kWidth = 470;
constexpr int kHeight = 262;

std::mutex g_stateMutex;
OverlayState g_state;
OverlayJoinHandler g_handler = nullptr;

HWND g_window = nullptr;
HWND g_title = nullptr;
HWND g_status = nullptr;
HWND g_targetEdit = nullptr;
HWND g_joinButton = nullptr;
HWND g_hideButton = nullptr;
HWND g_message = nullptr;
HFONT g_font = nullptr;
HFONT g_titleFont = nullptr;
HBRUSH g_background = nullptr;

std::thread g_thread;
std::atomic<bool> g_running{false};
std::atomic<DWORD> g_threadId{0};
bool g_visible = true;
bool g_prefilled = false;
HWND g_previousForeground = nullptr;

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

void showWindow(bool show) {
    if (g_window == nullptr) {
        return;
    }
    if (show) {
        g_previousForeground = GetForegroundWindow();
        ShowWindow(g_window, SW_SHOW);
        SetForegroundWindow(g_window);
        g_visible = true;
    } else {
        ShowWindow(g_window, SW_HIDE);
        g_visible = false;
        if (g_previousForeground != nullptr && IsWindow(g_previousForeground)) {
            SetForegroundWindow(g_previousForeground);
        }
    }
}

void refreshLabels() {
    if (g_window == nullptr) {
        return;
    }
    OverlayState state;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        state = g_state;
    }
    const std::wstring status = widen(state.line1 + "\r\n" + state.line2 + "\r\n" + state.line3);
    SetWindowTextW(g_status, status.c_str());
    SetWindowTextW(g_message, widen(state.message).c_str());
    EnableWindow(g_joinButton, !state.isHost);
    if (!g_prefilled && !state.target.empty()) {
        SetWindowTextW(g_targetEdit, widen(state.target).c_str());
        g_prefilled = true;
    }
    (void)g_title;
}

bool validTarget(const std::string& text) {
    if (text.empty() || text.find(' ') != std::string::npos) {
        return false;
    }
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon + 1 >= text.size()) {
        return false;
    }
    for (size_t i = colon + 1; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
    }
    return true;
}

void handleJoin() {
    wchar_t buffer[256] = {0};
    GetWindowTextW(g_targetEdit, buffer, static_cast<int>(std::size(buffer)));
    std::string target = narrow(buffer);
    while (!target.empty() && target.back() == ' ') {
        target.pop_back();
    }
    if (!validTarget(target)) {
        overlayMessage("格式应为 IP:端口，例如 1.2.3.4:7777");
        return;
    }
    if (g_handler != nullptr) {
        g_handler(target);
    }
    overlayMessage("正在连接 " + target + " ...");
    showWindow(false);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            g_titleFont = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                      L"Microsoft YaHei UI");
            g_background = CreateSolidBrush(RGB(248, 248, 250));

            g_title = CreateWindowExW(0, L"STATIC", L"SN2LAN 联机助手", WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 10, 300,
                                      28, window, nullptr, nullptr, nullptr);
            g_status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 44, kWidth - 32, 62,
                                       window, nullptr, nullptr, nullptr);
            g_targetEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                                                           ES_AUTOHSCROLL,
                                           16, 114, 262, 26, window, reinterpret_cast<HMENU>(kIdTarget), nullptr,
                                           nullptr);
            g_joinButton = CreateWindowExW(0, L"BUTTON", L"连接", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                           288, 114, 82, 26, window, reinterpret_cast<HMENU>(kIdJoin), nullptr,
                                           nullptr);
            g_hideButton = CreateWindowExW(0, L"BUTTON", L"隐藏 (F8)", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                                                          BS_PUSHBUTTON,
                                           376, 114, 78, 26, window, reinterpret_cast<HMENU>(kIdHide), nullptr,
                                           nullptr);
            g_message = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 148, kWidth - 32, 62,
                                        window, nullptr, nullptr, nullptr);

            for (HWND control : {g_title, g_status, g_targetEdit, g_joinButton, g_hideButton, g_message}) {
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
            }
            SendMessageW(g_title, WM_SETFONT, reinterpret_cast<WPARAM>(g_titleFont), TRUE);

            if (!RegisterHotKey(window, kHotkeyToggle, MOD_NOREPEAT, VK_F8)) {
                RegisterHotKey(window, kHotkeyToggle, 0, VK_F8);
            }
            SetTimer(window, 1, 400, nullptr);
            refreshLabels();
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == kIdJoin) {
                handleJoin();
            } else if (id == kIdHide) {
                showWindow(false);
            }
            return 0;
        }
        case WM_HOTKEY:
            if (wParam == kHotkeyToggle) {
                showWindow(!g_visible);
            }
            return 0;
        case WM_TIMER:
            refreshLabels();
            return 0;
        case WM_CLOSE:
            showWindow(false);
            return 0;
        case WM_NCHITTEST: {
            const LRESULT hit = DefWindowProcW(window, message, wParam, lParam);
            if (hit == HTCLIENT) {
                POINT point{};
                point.x = GET_X_LPARAM(lParam);
                point.y = GET_Y_LPARAM(lParam);
                ScreenToClient(window, &point);
                if (point.y < 40) {
                    return HTCAPTION;
                }
            }
            return hit;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(30, 30, 35));
            return reinterpret_cast<LRESULT>(g_background);
        }
        case WM_ERASEBKGND: {
            RECT rect{};
            GetClientRect(window, &rect);
            FillRect(reinterpret_cast<HDC>(wParam), &rect, g_background);
            return 1;
        }
        case WM_DESTROY:
            KillTimer(window, 1);
            UnregisterHotKey(window, kHotkeyToggle);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

void runUi() {
    g_threadId.store(GetCurrentThreadId());

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.style = CS_HREDRAW | CS_VREDRAW;
    cls.lpfnWndProc = windowProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    cls.lpszClassName = L"SN2LANOverlayWindow";
    cls.hbrBackground = nullptr;
    RegisterClassExW(&cls);

    g_window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, cls.lpszClassName, L"SN2LAN",
                               WS_POPUP | WS_CLIPCHILDREN, 80, 80, kWidth, kHeight, nullptr, nullptr, cls.hInstance,
                               nullptr);
    if (g_window == nullptr) {
        SN2_LOG_ERROR("overlay: CreateWindowEx failed (%lu)", GetLastError());
        g_running.store(false);
        return;
    }

    // Visible without stealing focus from the game at startup; F8 (or a click)
    // activates it.
    ShowWindow(g_window, SW_SHOWNOACTIVATE);
    g_visible = true;

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == kMsgStop) {
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (IsWindow(g_window)) {
        DestroyWindow(g_window);
    }
    g_window = nullptr;
    if (g_background != nullptr) {
        DeleteObject(g_background);
        g_background = nullptr;
    }
    if (g_font != nullptr) {
        DeleteObject(g_font);
        g_font = nullptr;
    }
    if (g_titleFont != nullptr) {
        DeleteObject(g_titleFont);
        g_titleFont = nullptr;
    }
    g_threadId.store(0);
    SN2_LOG_INFO("overlay stopped");
}

}  // namespace

bool overlayStart(OverlayJoinHandler handler) {
    if (g_running.exchange(true)) {
        g_handler = handler;
        return true;
    }
    g_handler = handler;
    g_thread = std::thread(runUi);
    SN2_LOG_INFO("overlay started (F8 to toggle)");
    return true;
}

void overlayStop() {
    if (!g_running.exchange(false)) {
        return;
    }
    for (int i = 0; i < 50; ++i) {
        const DWORD threadId = g_threadId.load();
        if (threadId != 0 && PostThreadMessageW(threadId, kMsgStop, 0, 0)) {
            break;
        }
        Sleep(10);
    }
    if (g_thread.joinable()) {
        g_thread.join();
    }
}

void overlaySetState(const OverlayState& state) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_state = state;
}

void overlayMessage(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_state.message = message;
}

void overlayHide() {
    if (g_window != nullptr) {
        PostMessageW(g_window, WM_COMMAND, kIdHide, 0);
    }
}

}  // namespace sn2lan
