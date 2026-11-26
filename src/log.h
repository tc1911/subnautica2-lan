#pragma once

#include <string>

namespace sn2lan {

enum class LogLevel { Debug, Info, Warn, Error };

// Opens (appends to) the log file. Returns false if the file could not be opened.
bool logInit(const std::wstring& filePath, bool debugEnabled);
void logShutdown();
void logSetDebug(bool enabled);

void logWrite(LogLevel level, const char* file, int line, const char* fmt, ...);

}  // namespace sn2lan

#define SN2_LOG_DEBUG(...) ::sn2lan::logWrite(::sn2lan::LogLevel::Debug, __FILE__, __LINE__, __VA_ARGS__)
#define SN2_LOG_INFO(...) ::sn2lan::logWrite(::sn2lan::LogLevel::Info, __FILE__, __LINE__, __VA_ARGS__)
#define SN2_LOG_WARN(...) ::sn2lan::logWrite(::sn2lan::LogLevel::Warn, __FILE__, __LINE__, __VA_ARGS__)
#define SN2_LOG_ERROR(...) ::sn2lan::logWrite(::sn2lan::LogLevel::Error, __FILE__, __LINE__, __VA_ARGS__)
