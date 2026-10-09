#include "App.h"
#include "ConfigStore.h"
#include "IRacingServices.h"
#include "MainWindow.h"
#include "StartupRegistration.h"
#include "UpdateChecker.h"

#include <algorithm>
#include <commctrl.h>
#include <cwchar>
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace
{
    constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\LaunchMate.SingleInstance";
    constexpr DWORD kMinimumPollIntervalMs = 100;
    constexpr DWORD kMaximumPollIntervalMs = 300000;

    int RunElevatedCommand()
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) return -1;
        int result = -1;
        if (argc >= 2)
        {
            const std::wstring command = argv[1];
            std::wstring report;
            if (command == L"--launchmate-apply-services")
            {
                std::vector<std::wstring> services;
                for (int index = 2; index < argc; ++index) services.emplace_back(argv[index]);
                result = ApplyIRacingServices(services, report) ? 0 : 1;
            }
            else if (command == L"--launchmate-restore-services")
            {
                result = RestoreIRacingServices(report) ? 0 : 1;
            }
            else if (command == L"--launchmate-configure-startup" && argc == 5)
            {
                const bool startWithWindows = std::wstring(argv[2]) == L"1";
                const bool startAsAdministrator = std::wstring(argv[3]) == L"1";
                result = StartupRegistration::ConfigureElevatedStartup(startWithWindows,
                    startAsAdministrator, argv[4]) ? 0 : 1;
            }
        }
        LocalFree(argv);
        return result;
    }

    bool IsElevated()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        TOKEN_ELEVATION elevation{};
        DWORD bytes = 0;
        const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes) != FALSE &&
            elevation.TokenIsElevated != FALSE;
        CloseHandle(token);
        return elevated;
    }

    bool RelaunchAsAdministrator()
    {
        wchar_t executable[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        if (!length || length == std::size(executable)) return false;

        SHELLEXECUTEINFOW info{sizeof(info)};
        info.lpVerb = L"runas";
        info.lpFile = executable;
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::wstring parameters;
        if (argv != nullptr)
        {
            for (int index = 1; index < argc; ++index)
            {
                if (!parameters.empty()) parameters += L" ";
                // CommandLineToArgvW already supplies individual arguments.  The
                // launch options used by LaunchMate contain no embedded quotes;
                // retaining them here preserves flags such as --log.
                const std::wstring argument = argv[index];
                parameters += argument.find_first_of(L" \t") == std::wstring::npos
                    ? argument : L"\"" + argument + L"\"";
            }
            LocalFree(argv);
        }
        info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
        info.nShow = SW_SHOWNORMAL;
        return ShellExecuteExW(&info) != FALSE;
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

    bool RestoreExistingInstance()
    {
        const auto existingWindow = FindWindowW(MainWindow::kWindowClassName, nullptr);
        if (!existingWindow)
        {
            return false;
        }

        DWORD processId = 0;
        GetWindowThreadProcessId(existingWindow, &processId);
        AllowSetForegroundWindow(processId);
        return PostMessageW(existingWindow, MainWindow::kRestoreRequestMessage, 0, 0) != FALSE;
    }
}

int WINAPI wWinMain(HINSTANCE instanceHandle, HINSTANCE, PWSTR, int showCommand)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // A copy of LaunchMate started by the updater swaps in the new version and exits.
    const int updateResult = UpdateChecker::HandleSelfUpdateCommandLine();
    if (updateResult >= 0) return updateResult;
    const int elevatedCommandResult = RunElevatedCommand();
    if (elevatedCommandResult >= 0) return elevatedCommandResult;

    if (RestoreExistingInstance()) return 0;

    // This uses the same Shell "runas" verb as Explorer's "Run as administrator".
    // Do it before creating the mutex so the elevated replacement can become the primary instance.
    if (!IsElevated() && ConfigStore().Load().startAsAdministrator)
        return RelaunchAsAdministrator() ? 0 : 1;

    const auto instanceMutex = CreateMutexW(nullptr, FALSE, kSingleInstanceMutexName);
    if (!instanceMutex)
    {
        return -1;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        RestoreExistingInstance();
        CloseHandle(instanceMutex);
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
