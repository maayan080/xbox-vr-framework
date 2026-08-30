#pragma once

#include <windows.h>

#include <exception>
#include <string>

#include "xvr/Log.h"

namespace xvr {

// Thrown by XVR_CHECK. Carries the failing expression and site so the log line
// is actionable without a debugger attached - which is the normal case on Xbox.
class HresultException : public std::exception
{
public:
    HresultException(HRESULT hr, std::string message)
        : m_hr(hr), m_message(std::move(message))
    {
    }

    HRESULT Code() const noexcept { return m_hr; }
    const char* what() const noexcept override { return m_message.c_str(); }

private:
    HRESULT m_hr;
    std::string m_message;
};

[[noreturn]] void ThrowHresult(HRESULT hr, const char* expression, const char* file, int line);

inline void CheckHr(HRESULT hr, const char* expression, const char* file, int line)
{
    if (FAILED(hr))
    {
        ThrowHresult(hr, expression, file, line);
    }
}

} // namespace xvr

#define XVR_CHECK(expr) ::xvr::CheckHr((expr), #expr, __FILE__, __LINE__)
