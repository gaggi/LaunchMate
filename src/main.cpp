#include "App.h"
#include "ConfigStore.h"
#include "MainWindow.h"
#include "StartupRegistration.h"

#include <algorithm>
#include <commctrl.h>
#include <cwchar>
#include <windows.h>
#include <shellapi.h>
#include <string>

#if defined(_MSC_VER)
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace
{
    constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\LaunchMate.SingleInstance";
    constexpr DWORD kMinimumPollIntervalMs = 100;
    constexpr DWORD kMaximumPollIntervalMs = 300000;

    int ConfigureStartupTaskFromCommandLine()
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) return -1;
        int result = -1;
        if (argc >= 2)
        {
            const std::wstring command = argv[1];
            if (command == L"--configure-elevated-startup=on" || command == L"--configure-elevated-startup=off")
            {
                result = argc == 3 && StartupRegistration::ConfigureElevatedTask(
                    command == L"--configure-elevated-startup=on", argv[2]) ? 0 : 1;
            }
        }
        LocalFree(argv);
        return result;
    }

    bool RelaunchAsAdministrator()
    {
        std::wstring path(MAX_PATH, L'\0');
        DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        while (length != 0 && length == path.size() && path.size() < 32768)
        {
            path.resize(path.size() * 2);
            length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        }
        if (!length || length == path.size()) return false;
        path.resize(length);
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS;
        info.lpVerb = L"runas";
        info.lpFile = path.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info)) return false;
        if (info.hProcess) CloseHandle(info.hProcess);
        return true;
    }

    DWORD ParsePollIntervalArgument(const wchar_t* value, DWORD fallback)
    {
        wchar_t* end = nullptr;
        const unsigned long parsed = std::wcstoul(value, &end, 10);
        if (end == nullptr || *end != L'\0')
        {
            return fallback;
        }

        return static_cast<DWORD>(std::clamp<unsigned long>(parsed, kMinimumPollIntervalMs, kMaximumPollIntervalMs));
    }

    AppLaunchOptions ParseLaunchOptions()
    {
        AppLaunchOptions options;

        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
        {
            return options;
        }

        for (int index = 1; index < argc; ++index)
        {
            const std::wstring argument = argv[index];
            if (argument == L"--log")
            {
                options.loggingEnabled = true;
                continue;
            }

            if (argument == L"--poll-interval")
            {
                if (index + 1 >= argc)
                {
                    continue;
                }

                options.pollIntervalMs = ParsePollIntervalArgument(argv[++index], options.pollIntervalMs);
                continue;
            }

            if (argument == L"--active-poll-interval")
            {
                if (index + 1 >= argc)
                {
                    continue;
                }

                options.activePollIntervalMs = ParsePollIntervalArgument(argv[++index], options.activePollIntervalMs);
            }
        }

        LocalFree(argv);
        return options;
    }

    void RestoreExistingInstance()
    {
        const auto existingWindow = FindWindowW(MainWindow::kWindowClassName, nullptr);
        if (!existingWindow)
        {
            return;
        }

        PostMessageW(existingWindow, MainWindow::kRestoreRequestMessage, 0, 0);
    }
}

int WINAPI wWinMain(HINSTANCE instanceHandle, HINSTANCE, PWSTR, int showCommand)
{
    const int startupTaskResult = ConfigureStartupTaskFromCommandLine();
    if (startupTaskResult >= 0) return startupTaskResult;

    const bool alwaysRunAsAdministrator = ConfigStore().Load().alwaysRunAsAdministrator;
    const auto instanceMutex = CreateMutexW(nullptr, FALSE, kSingleInstanceMutexName);
    if (!instanceMutex)
    {
        if (GetLastError() == ERROR_ACCESS_DENIED && alwaysRunAsAdministrator &&
            !StartupRegistration::IsElevated() && StartupRegistration::CanElevateCurrentUser())
            return RelaunchAsAdministrator() ? 0 : 1;
        return -1;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        RestoreExistingInstance();
        CloseHandle(instanceMutex);
        return 0;
    }

    if (alwaysRunAsAdministrator && !StartupRegistration::IsElevated())
    {
        CloseHandle(instanceMutex);
        if (!StartupRegistration::CanElevateCurrentUser())
        {
            MessageBoxW(nullptr, L"LaunchMate's administrator mode requires the signed-in Windows account to be an administrator. It will not start as a different user's profile.", L"LaunchMate", MB_OK | MB_ICONERROR);
            return 1;
        }
        if (!RelaunchAsAdministrator())
        {
            if (GetLastError() != ERROR_CANCELLED)
                MessageBoxW(nullptr, L"LaunchMate could not request administrator rights.", L"LaunchMate", MB_OK | MB_ICONERROR);
            return 1;
        }
        return 0;
    }

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&controls);

    App app(instanceHandle, ParseLaunchOptions());
    const auto result = app.Run(showCommand);
    CloseHandle(instanceMutex);
    return result;
}
