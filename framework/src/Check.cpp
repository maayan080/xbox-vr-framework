#include "xvr/Check.h"

#include <format>

namespace xvr {
namespace {

std::wstring Widen(const char* text)
{
    std::string narrow(text ? text : "");
    if (narrow.empty())
    {
        return {};
    }

    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, narrow.data(), static_cast<int>(narrow.size()),
                                             nullptr, 0);
    std::wstring result(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, narrow.data(), static_cast<int>(narrow.size()), result.data(),
                          needed);
    return result;
}

const char* FileNameOnly(const char* path)
{
    const char* name = path;
    for (const char* p = path; p && *p; ++p)
    {
        if (*p == '\\' || *p == '/')
        {
            name = p + 1;
        }
    }
    return name;
}

} // namespace

void ThrowHresult(HRESULT hr, const char* expression, const char* file, int line)
{
    const char* shortFile = FileNameOnly(file);

    LogError(L"HRESULT 0x{:08X} from `{}` at {}:{}", static_cast<uint32_t>(hr), Widen(expression),
             Widen(shortFile), line);

    throw HresultException(hr, std::format("HRESULT 0x{:08X} from `{}` at {}:{}",
                                           static_cast<uint32_t>(hr), expression, shortFile, line));
}

} // namespace xvr
