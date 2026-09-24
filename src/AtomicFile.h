#pragma once

#include <filesystem>
#include <string_view>
#include <windows.h>

// Stage in the destination directory so the final rename stays on one volume.
// Never truncate the existing file before the replacement has been flushed.
inline bool WriteFileAtomically(const std::filesystem::path& path, std::string_view contents)
{
    std::wstring temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 32; ++attempt)
    {
        temporary = path.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt);
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) return false;
    }
    if (file == INVALID_HANDLE_VALUE) return false;
    bool success = true;
    size_t offset = 0;
    while (offset < contents.size())
    {
        const auto remaining = contents.size() - offset;
        const DWORD count = static_cast<DWORD>(remaining > 1024 * 1024 ? 1024 * 1024 : remaining);
        DWORD written = 0;
        if (!WriteFile(file, contents.data() + offset, count, &written, nullptr) || written == 0)
        {
            success = false;
            break;
        }
        offset += written;
    }
    if (success) success = FlushFileBuffers(file) != FALSE;
    if (!CloseHandle(file)) success = false;
    if (success) success = MoveFileExW(temporary.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!success) DeleteFileW(temporary.c_str());
    return success;
}
