#include "StartupRegistration.h"

#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <taskschd.h>
#include <wrl/client.h>

namespace
{
    using Microsoft::WRL::ComPtr;

    class ScopedBstr
    {
    public:
        explicit ScopedBstr(const std::wstring& text) : value_(SysAllocStringLen(text.data(), static_cast<UINT>(text.size()))) {}
        ~ScopedBstr() { SysFreeString(value_); }
        operator BSTR() const { return value_; }
        bool Valid() const { return value_ != nullptr; }

    private:
        BSTR value_{};
    };

    std::wstring ModulePath()
    {
        std::wstring path(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (!length) return {};
            if (length < path.size()) { path.resize(length); return path; }
            if (path.size() >= 32768) return {};
            path.resize(path.size() * 2);
        }
    }

    std::wstring CurrentUserSid()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> buffer(size);
        const bool read = size != 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
        CloseHandle(token);
        if (!read) return {};
        LPWSTR sidText = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sidText)) return {};
        std::wstring sid(sidText);
        LocalFree(sidText);
        return sid;
    }

    bool ApplyRunKey(bool enabled)
    {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
        LONG result = ERROR_SUCCESS;
        if (enabled)
        {
            const std::wstring path = ModulePath();
            if (path.empty()) result = ERROR_FILE_NOT_FOUND;
            else
            {
                const std::wstring command = L"\"" + path + L"\"";
                result = RegSetValueExW(key, L"LaunchMate", 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(command.c_str()),
                    static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
            }
        }
        else
        {
            result = RegDeleteValueW(key, L"LaunchMate");
            if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
        }
        RegCloseKey(key);
        return result == ERROR_SUCCESS;
    }

    bool RunElevatedTaskConfiguration(bool enabled)
    {
        const std::wstring path = ModulePath();
        const std::wstring sid = CurrentUserSid();
        if (path.empty() || sid.empty()) return false;
        const std::wstring arguments = std::wstring(enabled ? L"--configure-elevated-startup=on " : L"--configure-elevated-startup=off ") + sid;
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS;
        info.lpVerb = L"runas";
        info.lpFile = path.c_str();
        info.lpParameters = arguments.c_str();
        info.nShow = SW_HIDE;
        if (!ShellExecuteExW(&info) || !info.hProcess) return false;
        const DWORD wait = WaitForSingleObject(info.hProcess, 60000);
        DWORD exitCode = 1;
        const bool succeeded = wait == WAIT_OBJECT_0 && GetExitCodeProcess(info.hProcess, &exitCode) && exitCode == 0;
        CloseHandle(info.hProcess);
        return succeeded;
    }

    bool ConfigureTask(bool enabled)
    {
        const std::wstring sid = CurrentUserSid();
        if (sid.empty()) return false;
        const std::wstring taskName = L"LaunchMate Elevated Startup " + sid;
        const std::wstring path = ModulePath();
        if (enabled && path.empty()) return false;

        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitialize = SUCCEEDED(initialized);
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return false;

        bool success = false;
        do
        {
            ComPtr<ITaskService> service;
            if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&service)))) break;
            VARIANT empty{};
            VariantInit(&empty);
            if (FAILED(service->Connect(empty, empty, empty, empty))) break;
            ComPtr<ITaskFolder> folder;
            ScopedBstr root(L"\\");
            ScopedBstr name(taskName);
            if (!root.Valid() || !name.Valid() || FAILED(service->GetFolder(root, &folder))) break;
            if (!enabled)
            {
                const HRESULT removed = folder->DeleteTask(name, 0);
                success = SUCCEEDED(removed) || removed == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
                    removed == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
                break;
            }

            ComPtr<ITaskDefinition> definition;
            if (FAILED(service->NewTask(0, &definition))) break;
            ComPtr<IPrincipal> principal;
            if (FAILED(definition->get_Principal(&principal))) break;
            ScopedBstr user(sid);
            if (!user.Valid() || FAILED(principal->put_UserId(user)) ||
                FAILED(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN)) ||
                FAILED(principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST))) break;

            ComPtr<ITriggerCollection> triggers;
            ComPtr<ITrigger> trigger;
            ComPtr<ILogonTrigger> logonTrigger;
            if (FAILED(definition->get_Triggers(&triggers)) ||
                FAILED(triggers->Create(TASK_TRIGGER_LOGON, &trigger)) ||
                FAILED(trigger.As(&logonTrigger)) ||
                FAILED(logonTrigger->put_UserId(user))) break;

            ComPtr<IActionCollection> actions;
            ComPtr<IAction> action;
            ComPtr<IExecAction> executable;
            ScopedBstr executablePath(path);
            ScopedBstr workingDirectory(std::filesystem::path(path).parent_path().wstring());
            if (!executablePath.Valid() || !workingDirectory.Valid() ||
                FAILED(definition->get_Actions(&actions)) ||
                FAILED(actions->Create(TASK_ACTION_EXEC, &action)) ||
                FAILED(action.As(&executable)) ||
                FAILED(executable->put_Path(executablePath)) ||
                FAILED(executable->put_WorkingDirectory(workingDirectory))) break;

            ComPtr<ITaskSettings> settings;
            ScopedBstr unlimited(L"PT0S");
            if (!unlimited.Valid() || FAILED(definition->get_Settings(&settings)) ||
                FAILED(settings->put_ExecutionTimeLimit(unlimited)) ||
                FAILED(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE)) ||
                FAILED(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE)) ||
                FAILED(settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW))) break;

            ComPtr<IRegisteredTask> registered;
            success = SUCCEEDED(folder->RegisterTaskDefinition(name, definition.Get(), TASK_CREATE_OR_UPDATE,
                empty, empty, TASK_LOGON_INTERACTIVE_TOKEN, empty, &registered));
        } while (false);

        if (uninitialize) CoUninitialize();
        return success;
    }
}

bool StartupRegistration::IsElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

bool StartupRegistration::CanElevateCurrentUser()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION_TYPE type{};
    DWORD size = 0;
    const bool canElevate = GetTokenInformation(token, TokenElevationType, &type, sizeof(type), &size) &&
        type == TokenElevationTypeLimited;
    CloseHandle(token);
    return canElevate;
}

bool StartupRegistration::ConfigureElevatedTask(bool enabled, const std::wstring& expectedUserSid)
{
    return IsElevated() && !expectedUserSid.empty() && expectedUserSid == CurrentUserSid() && ConfigureTask(enabled);
}

bool StartupRegistration::Apply(bool startWithWindows, bool alwaysRunAsAdministrator)
{
    if (startWithWindows && alwaysRunAsAdministrator)
    {
        const bool configured = IsElevated() ? ConfigureTask(true) :
            CanElevateCurrentUser() && RunElevatedTaskConfiguration(true);
        return configured && ApplyRunKey(false);
    }

    bool removed = ConfigureTask(false);
    if (!removed && !IsElevated())
        removed = CanElevateCurrentUser() && RunElevatedTaskConfiguration(false);
    return removed && ApplyRunKey(startWithWindows && !alwaysRunAsAdministrator);
}
