#include "StartupRegistration.h"

#include <iterator>
#include <string>
#include <vector>
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>

namespace
{
    bool RemoveRunKey()
    {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
        LONG result = RegDeleteValueW(key, L"LaunchMate");
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
        RegCloseKey(key);
        return result == ERROR_SUCCESS;
    }

    std::wstring CurrentUserSid()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
        DWORD bytes = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
        std::vector<BYTE> buffer(bytes);
        const bool read = bytes != 0 && GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes);
        CloseHandle(token);
        if (!read) return {};
        LPWSTR text = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) return {};
        std::wstring result(text);
        LocalFree(text);
        return result;
    }

    bool RunSchtasks(const std::wstring& parameters)
    {
        std::wstring command = L"\"C:\\Windows\\System32\\schtasks.exe\" " + parameters;
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process)) return false;
        WaitForSingleObject(process.hProcess, 10000);
        DWORD result = 1;
        GetExitCodeProcess(process.hProcess, &result);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return result == 0;
    }

    std::wstring QuoteArgument(const std::wstring& value)
    {
        return L"\"" + value + L"\"";
    }

    bool IsElevated()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        TOKEN_ELEVATION elevation{};
        DWORD bytes = 0;
        const bool read = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes) != FALSE;
        CloseHandle(token);
        return read && elevation.TokenIsElevated != 0;
    }

    bool ApplyLogonTask(bool enabled, bool startAsAdministrator)
    {
        const auto sid = CurrentUserSid();
        if (sid.empty()) return false;
        const auto taskName = L"LaunchMate Startup " + sid;
        if (!enabled)
        {
            RunSchtasks(L"/Delete /TN " + QuoteArgument(taskName) + L" /F");
            return true;
        }

        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        if (!length || length == std::size(path)) return false;
        const auto command = L"\"" + std::wstring(path, length) + L"\"";
        std::wstring parameters = L"/Create /TN " + QuoteArgument(taskName) + L" /TR " +
            QuoteArgument(command) + L" /SC ONLOGON";
        if (startAsAdministrator) parameters += L" /RL HIGHEST";
        parameters += L" /F";
        return RunSchtasks(parameters);
    }

    void RemoveLegacyElevatedStartupTask()
    {
        const auto sid = CurrentUserSid();
        if (sid.empty()) return;
        RunSchtasks(L"/Delete /TN " + QuoteArgument(L"LaunchMate Elevated Startup " + sid) + L" /F");
    }

    bool RunElevatedStartupConfiguration(bool startWithWindows, bool startAsAdministrator)
    {
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        const auto sid = CurrentUserSid();
        if (!length || length == std::size(path) || sid.empty()) return false;

        const std::wstring parameters = L"--launchmate-configure-startup " +
            std::wstring(startWithWindows ? L"1" : L"0") + L" " +
            std::wstring(startAsAdministrator ? L"1" : L"0") + L" " + QuoteArgument(sid);
        SHELLEXECUTEINFOW execute{sizeof(execute)};
        execute.fMask = SEE_MASK_NOCLOSEPROCESS;
        execute.lpVerb = L"runas";
        execute.lpFile = path;
        execute.lpParameters = parameters.c_str();
        execute.nShow = SW_HIDE;
        if (!ShellExecuteExW(&execute) || !execute.hProcess) return false;
        WaitForSingleObject(execute.hProcess, 60000);
        DWORD result = 1;
        GetExitCodeProcess(execute.hProcess, &result);
        CloseHandle(execute.hProcess);
        return result == 0;
    }
}

bool StartupRegistration::Apply(bool startWithWindows, bool startAsAdministrator)
{
    if (!IsElevated()) return RunElevatedStartupConfiguration(startWithWindows, startAsAdministrator);
    if (!RemoveRunKey()) return false;
    RemoveLegacyElevatedStartupTask();
    return ApplyLogonTask(startWithWindows, startAsAdministrator);
}

bool StartupRegistration::ConfigureElevatedStartup(bool startWithWindows, bool startAsAdministrator,
    const std::wstring& expectedUserSid)
{
    if (!IsElevated() || expectedUserSid.empty() || expectedUserSid != CurrentUserSid()) return false;
    if (!RemoveRunKey()) return false;
    RemoveLegacyElevatedStartupTask();
    return ApplyLogonTask(startWithWindows, startAsAdministrator);
}
