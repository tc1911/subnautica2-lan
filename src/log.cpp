#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <atomic>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace sn2lan {

namespace {

std::mutex g_mutex;
std::ofstream g_file;
std::atomic<bool> g_debug{false};
bool g_initialized = false;

const char* levelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO ";
        case LogLevel::Warn:
            return "WARN ";
        case LogLevel::Error:
            return "ERROR";
    }
    return "?????";
}

void rotateIfNeeded(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size < 8u * 1024u * 1024u) {
        return;
    }
    std::filesystem::path old = path;
    old += L".old";
    std::filesystem::remove(old, ec);
    std::filesystem::rename(path, old, ec);
}

}  // namespace

bool logInit(const std::wstring& filePath, bool debugEnabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::filesystem::path path(filePath);
    rotateIfNeeded(path);

    g_file.open(path, std::ios::out | std::ios::app | std::ios::binary);
    g_debug.store(debugEnabled, std::memory_order_relaxed);
    g_initialized = g_file.is_open();

    char stamp[64] = {0};
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u.%03u", st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (g_initialized) {
        g_file << "\n===== SN2Lan session " << stamp << " =====\n";
        g_file.flush();
    }
    return g_initialized;
}

void logShutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file.is_open()) {
        g_file.flush();
        g_file.close();
    }
    g_initialized = false;
}

void logSetDebug(bool enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_debug.store(enabled, std::memory_order_relaxed);
}

void logWrite(LogLevel level, const char* file, int line, const char* fmt, ...) {
    if (level == LogLevel::Debug && !g_debug.load(std::memory_order_relaxed)) {
        return;
    }

    char message[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char stamp[64];
    std::snprintf(stamp, sizeof(stamp), "%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    const char* base = file ? std::strrchr(file, '/') : nullptr;
    const char* base2 = file ? std::strrchr(file, '\\') : nullptr;
    if (base2 && (!base || base2 > base)) {
        base = base2;
    }
    const char* shortFile = base ? base + 1 : (file ? file : "?");

    char line_buf[2400];
    std::snprintf(line_buf, sizeof(line_buf), "[%s][%s] %s (%s:%d)\n", stamp, levelName(level), message, shortFile, line);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_initialized) {
            g_file << line_buf;
            g_file.flush();
        }
    }

    OutputDebugStringA(line_buf);
    if (level == LogLevel::Error) {
        std::fputs(line_buf, stderr);
        std::fflush(stderr);
    }
}

}  // namespace sn2lan
