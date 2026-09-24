#include "IRacingServices.h"

#include <algorithm>
#include <array>
#include <vector>
#include <windows.h>

namespace
{
    constexpr wchar_t kRecoveryKey[] = L"Software\\LaunchMate\\IRacingServiceRecovery";
    constexpr DWORD kSnapshotMagic = 0x4c4d5331; // LMS1

    struct ServiceSnapshot
    {
        DWORD magic{kSnapshotMagic};
        DWORD startType{};
        DWORD delayedAutoStart{};
        DWORD delayedAutoStartKnown{};
        DWORD wasRunning{};
    };

    bool QueryState(SC_HANDLE service, DWORD& state)
    {
        SERVICE_STATUS_PROCESS status{};
        DWORD bytes = 0;
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes)) return false;
        state = status.dwCurrentState;
        return true;
    }

    bool ReadSnapshot(SC_HANDLE service, ServiceSnapshot& snapshot)
    {
        DWORD bytes = 0;
        QueryServiceConfigW(service, nullptr, 0, &bytes);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) return false;
        std::vector<BYTE> buffer(bytes);
        if (!QueryServiceConfigW(service, reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data()), bytes, &bytes)) return false;
        const auto* config = reinterpret_cast<const QUERY_SERVICE_CONFIGW*>(buffer.data());
        snapshot.startType = config->dwStartType;
        SERVICE_DELAYED_AUTO_START_INFO delayed{};
        if (QueryServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
            reinterpret_cast<BYTE*>(&delayed), sizeof(delayed), &bytes))
        {
            snapshot.delayedAutoStart = delayed.fDelayedAutostart != FALSE;
            snapshot.delayedAutoStartKnown = 1;
        }
        DWORD state = 0;
        if (!QueryState(service, state)) return false;
        snapshot.wasRunning = state != SERVICE_STOPPED;
        return true;
    }

    bool WaitForState(SC_HANDLE service, DWORD wanted)
    {
        for (int attempt = 0; attempt < 50; ++attempt)
        {
            DWORD state = 0;
            if (!QueryState(service, state)) return false;
            if (state == wanted) return true;
            Sleep(100);
        }
        return false;
    }

    bool StopOne(SC_HANDLE service)
    {
        DWORD state = 0;
        if (!QueryState(service, state)) return false;
        if (state == SERVICE_STOPPED) return true;
        if (state != SERVICE_STOP_PENDING)
        {
            SERVICE_STATUS status{};
            if (!ControlService(service, SERVICE_CONTROL_STOP, &status) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) return false;
        }
        return WaitForState(service, SERVICE_STOPPED);
    }

    bool RestoreOne(SC_HANDLE service, const ServiceSnapshot& snapshot, bool restoreStartup)
    {
        if (restoreStartup && !ChangeServiceConfigW(service, SERVICE_NO_CHANGE, snapshot.startType, SERVICE_NO_CHANGE,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) return false;
        if (restoreStartup && snapshot.startType == SERVICE_AUTO_START && snapshot.delayedAutoStartKnown)
        {
            SERVICE_DELAYED_AUTO_START_INFO delayed{snapshot.delayedAutoStart != 0};
            if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed)) return false;
        }
        if (snapshot.wasRunning)
        {
            DWORD state = 0;
            if (!QueryState(service, state)) return false;
            if (state == SERVICE_RUNNING) return true;
            if (state != SERVICE_START_PENDING && !StartServiceW(service, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) return false;
            return WaitForState(service, SERVICE_RUNNING);
        }
        return true;
    }

    HKEY OpenRecoveryKey(bool create)
    {
        HKEY key = nullptr;
        if (create)
        {
            if (RegCreateKeyExW(HKEY_CURRENT_USER, kRecoveryKey, 0, nullptr, 0,
                KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return nullptr;
        }
        else if (RegOpenKeyExW(HKEY_CURRENT_USER, kRecoveryKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) return nullptr;
        return key;
    }

    bool ReadRecovery(HKEY key, const wchar_t* name, ServiceSnapshot& snapshot)
    {
        DWORD type = 0;
        DWORD bytes = sizeof(snapshot);
        return RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&snapshot), &bytes) == ERROR_SUCCESS &&
            type == REG_BINARY && bytes == sizeof(snapshot) && snapshot.magic == kSnapshotMagic;
    }

    bool SaveRecovery(HKEY key, const wchar_t* name, const ServiceSnapshot& snapshot)
    {
        return RegSetValueExW(key, name, 0, REG_BINARY, reinterpret_cast<const BYTE*>(&snapshot), sizeof(snapshot)) == ERROR_SUCCESS;
    }
}

const std::vector<IRacingServiceOption>& IRacingServiceOptions()
{
    static const std::vector<IRacingServiceOption> options{
        {L"wuauserv", L"Windows Update", true},
        {L"UsoSvc", L"Update Orchestrator", true},
        {L"WSearch", L"Windows Search", true},
        {L"Spooler", L"Print Spooler", false},
        {L"WlanSvc", L"Wi-Fi", false},
        {L"BthAvctpSvc", L"Bluetooth AVCTP", false},
        {L"BTAGService", L"Bluetooth Audio Gateway", false},
        {L"bthserv", L"Bluetooth Support", false},
        {L"XblAuthManager", L"Xbox Live Auth", false},
        {L"bzserv", L"Backblaze", false}};
    return options;
}

bool IsIRacingServiceName(const std::wstring& name)
{
    const auto& options = IRacingServiceOptions();
    return std::any_of(options.begin(), options.end(), [&name](const IRacingServiceOption& option)
    {
        return _wcsicmp(option.name, name.c_str()) == 0;
    });
}

bool CanManageIRacingServices()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes) && elevation.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

std::wstring DescribeIRacingService(const wchar_t* name)
{
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return L"unavailable";
    SC_HANDLE service = OpenServiceW(manager, name, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (!service)
    {
        const DWORD error = GetLastError();
        CloseServiceHandle(manager);
        return error == ERROR_SERVICE_DOES_NOT_EXIST ? L"not installed" : L"unavailable";
    }
    ServiceSnapshot snapshot;
    const bool ok = ReadSnapshot(service, snapshot);
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (!ok) return L"unavailable";
    std::wstring result = snapshot.wasRunning ? L"running" : L"stopped";
    if (snapshot.startType == SERVICE_DISABLED) result += L", disabled";
    return result;
}

bool HasPendingIRacingServiceRestore()
{
    HKEY key = OpenRecoveryKey(false);
    if (!key) return false;
    bool pending = false;
    for (const auto& option : IRacingServiceOptions())
    {
        ServiceSnapshot snapshot;
        if (ReadRecovery(key, option.name, snapshot)) { pending = true; break; }
    }
    RegCloseKey(key);
    return pending;
}

bool RestoreIRacingServices(std::wstring& report)
{
    HKEY key = OpenRecoveryKey(false);
    if (!key) return true;
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) { RegCloseKey(key); report = L"Could not open Windows Service Control Manager."; return false; }
    bool success = true;
    for (const auto& option : IRacingServiceOptions())
    {
        ServiceSnapshot snapshot;
        if (!ReadRecovery(key, option.name, snapshot)) continue;
        const DWORD access = SERVICE_QUERY_STATUS | SERVICE_START |
            (option.disableWhileRacing ? SERVICE_CHANGE_CONFIG : 0);
        SC_HANDLE service = OpenServiceW(manager, option.name, access);
        if (!service || !RestoreOne(service, snapshot, option.disableWhileRacing))
        {
            success = false;
            report += std::wstring(option.name) + L" could not be restored; ";
        }
        else RegDeleteValueW(key, option.name);
        if (service) CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    RegCloseKey(key);
    return success;
}

bool ApplyIRacingServices(const std::vector<std::wstring>& selected, std::wstring& report)
{
    if (selected.empty()) return true;
    if (!CanManageIRacingServices())
    {
        report = L"Windows service actions need LaunchMate to run as Administrator.";
        return false;
    }
    if (HasPendingIRacingServiceRestore())
    {
        std::wstring recovery;
        if (!RestoreIRacingServices(recovery)) { report = L"Previous service state could not be restored: " + recovery; return false; }
    }
    HKEY key = OpenRecoveryKey(true);
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!key || !manager)
    {
        if (key) RegCloseKey(key);
        if (manager) CloseServiceHandle(manager);
        report = L"Could not prepare Windows service recovery.";
        return false;
    }
    bool success = true;
    for (const auto& name : selected)
    {
        const auto& options = IRacingServiceOptions();
        const auto option = std::find_if(options.begin(), options.end(), [&name](const IRacingServiceOption& item)
        {
            return _wcsicmp(item.name, name.c_str()) == 0;
        });
        if (option == options.end()) continue;
        const DWORD access = SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG | SERVICE_STOP | SERVICE_START |
            (option->disableWhileRacing ? SERVICE_CHANGE_CONFIG : 0);
        SC_HANDLE service = OpenServiceW(manager, option->name, access);
        if (!service)
        {
            if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST)
            {
                success = false;
                report += name + L" could not be opened; ";
            }
            continue;
        }
        ServiceSnapshot snapshot;
        if (!ReadSnapshot(service, snapshot))
        {
            success = false;
            report += name + L" could not be inspected; ";
            CloseServiceHandle(service);
            continue;
        }
        const bool changeStart = option->disableWhileRacing && snapshot.startType != SERVICE_DISABLED;
        if (!changeStart && !snapshot.wasRunning) { CloseServiceHandle(service); continue; }
        if (!SaveRecovery(key, option->name, snapshot))
        {
            success = false;
            report += name + L" could not be saved for recovery; ";
            CloseServiceHandle(service);
            continue;
        }
        bool applied = true;
        if (changeStart)
            applied = ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_DISABLED, SERVICE_NO_CHANGE,
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) != FALSE;
        if (applied && snapshot.wasRunning) applied = StopOne(service);
        if (!applied)
        {
            success = false;
            report += name + L" could not be stopped or disabled; ";
            if (RestoreOne(service, snapshot, option->disableWhileRacing)) RegDeleteValueW(key, option->name);
        }
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    RegCloseKey(key);
    return success;
}
