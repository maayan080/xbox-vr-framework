#pragma once

// Logging for the Xbox-side host. Writes to the app's LocalState folder so that
// build/test tooling can read runtime output back off the device (or off a local
// sideload install) without anyone copy-pasting console text by hand.

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace xvr {

enum class LogLevel { Info, Warn, Error };

// Opens <LocalState>/<fileName>, truncating any previous run's log.
void LogInit(std::wstring_view fileName);
void LogRaw(LogLevel level, std::wstring_view message);
void LogFlush();
void LogShutdown();

// Absolute path of the app's LocalState folder (where the log and any capture
// files land). Empty string if LogInit hasn't run.
std::wstring LogDirectory();

template <typename... Args>
void LogInfo(std::wformat_string<Args...> fmt, Args&&... args)
{
    LogRaw(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void LogWarn(std::wformat_string<Args...> fmt, Args&&... args)
{
    LogRaw(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void LogError(std::wformat_string<Args...> fmt, Args&&... args)
{
    LogRaw(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace xvr
