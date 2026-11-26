#include "crashlog.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace sn2lan {

namespace {

LONG WINAPI crashFilter(EXCEPTION_POINTERS* info) {
    const DWORD code = info != nullptr && info->ExceptionRecord != nullptr ? info->ExceptionRecord->ExceptionCode : 0;
    const void* address =
        info != nullptr && info->ExceptionRecord != nullptr ? info->ExceptionRecord->ExceptionAddress : nullptr;

    HMODULE exe = GetModuleHandleW(nullptr);
    const auto base = reinterpret_cast<uintptr_t>(exe);
    const auto fault = reinterpret_cast<uintptr_t>(address);

    char body[8192];
    int used = 0;
    used += std::snprintf(body + used, sizeof(body) - used, "fatal exception 0x%08lX at exe+0x%llX (raw %p)\n",
                          static_cast<unsigned long>(code),
                          static_cast<unsigned long long>(fault >= base ? fault - base : 0), address);

    void* frames[40] = {};
    const USHORT count = CaptureStackBackTrace(0, 40, frames, nullptr);
    for (USHORT i = 0; i < count && used < static_cast<int>(sizeof(body)) - 64; ++i) {
        const auto pc = reinterpret_cast<uintptr_t>(frames[i]);
        if (pc >= base && pc < base + 0x10000000u) {
            used += std::snprintf(body + used, sizeof(body) - used, "  #%u exe+0x%llX\n", i,
                                  static_cast<unsigned long long>(pc - base));
        } else {
            used += std::snprintf(body + used, sizeof(body) - used, "  #%u %p\n", i, frames[i]);
        }
    }

    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) > 0) {
        char* slash = std::strrchr(path, '\\');
        if (slash != nullptr) {
            std::snprintf(slash + 1, static_cast<size_t>(path + MAX_PATH - slash - 1), "sn2lan_crash.txt");
        }
    }

    HANDLE file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, body, static_cast<DWORD>(used), &written, nullptr);
        CloseHandle(file);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void installCrashLogger() {
    SetUnhandledExceptionFilter(crashFilter);
}

}  // namespace sn2lan
