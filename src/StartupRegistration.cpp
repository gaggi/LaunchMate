#include "StartupRegistration.h"

#include <string>
#include <windows.h>

bool StartupRegistration::Apply(bool enabled)
{
    HKEY key = nullptr;
    const auto result = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE,
        nullptr,
        &key,
        nullptr);

    if (result != ERROR_SUCCESS)
    {
        return false;
    }

    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);

    LONG writeResult = ERROR_SUCCESS;
    if (enabled)
    {
        std::wstring command = L"\"";
        command += modulePath;
        command += L"\"";
        writeResult = RegSetValueExW(
            key,
            L"LaunchMate",
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }
    else
    {
        writeResult = RegDeleteValueW(key, L"LaunchMate");
        if (writeResult == ERROR_FILE_NOT_FOUND)
        {
            writeResult = ERROR_SUCCESS;
        }
    }

    RegCloseKey(key);
    return writeResult == ERROR_SUCCESS;
}
