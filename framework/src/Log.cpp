#include "xvr/Log.h"

#include <windows.h>
#include <winrt/Windows.Storage.h>

#include <chrono>
#include <fstream>
#include <mutex>

namespace xvr {
namespace {

std::mutex g_mutex;
std::ofstream g_file;
std::wstring g_directory;
std::chrono::steady_clock::time_point g_start;

std::string ToUtf8(std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }

    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                             nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                          needed, nullptr, nullptr);
    return result;
}

const wchar_t* LevelTag(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Warn:  return L"WARN ";
    case LogLevel::Error: return L"ERROR";
    default:              return L"INFO ";
    }
}

} // namespace

void LogInit(std::wstring_view fileName)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    g_start = std::chrono::steady_clock::now();

    try
    {
        g_directory = winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str();
    }
    catch (...)
    {
        // Not running packaged (or no app data) - fall back to the working directory
        // so the framework stays usable outside a UWP host.
        g_directory = L".";
    }

    std::wstring path = g_directory;
    path += L'\\';
    path += fileName;

    g_file.open(path.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
}

void LogRaw(LogLevel level, std::wstring_view message)
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - g_start)
                             .count();

    std::wstring line = std::format(L"[{:>8}ms] {} {}\r\n", elapsed, LevelTag(level), message);

    ::OutputDebugStringW(line.c_str());

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file.is_open())
    {
        const std::string utf8 = ToUtf8(line);
        g_file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
        // Flushed eagerly: a crash mid-run is exactly when the log matters most.
        g_file.flush();
    }
}

void LogFlush()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file.is_open())
    {
        g_file.flush();
    }
}

void LogShutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file.is_open())
    {
        g_file.flush();
        g_file.close();
    }
}

std::wstring LogDirectory()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_directory;
}

} // namespace xvr
