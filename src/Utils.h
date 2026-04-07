#pragma once

#include <string>
#include <vector>
#include <windows.h>

inline std::wstring ToWide(const std::string& value)
{
    if (value.empty())
    {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

inline std::string ToUtf8(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

inline std::wstring FileNameWithoutExtension(const std::wstring& path)
{
    const auto slash = path.find_last_of(L"\\/");
    const auto start = slash == std::wstring::npos ? 0 : slash + 1;
    const auto dot = path.find_last_of(L'.');

    if (dot == std::wstring::npos || dot < start)
    {
        return path.substr(start);
    }

    return path.substr(start, dot - start);
}
