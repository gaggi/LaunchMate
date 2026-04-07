#include "App.h"
#include "MainWindow.h"

#include <algorithm>
#include <commctrl.h>
#include <cwchar>
#include <windows.h>
#include <shellapi.h>
#include <string>

namespace
{
    constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\LaunchMate.SingleInstance";

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

                wchar_t* end = nullptr;
                const unsigned long value = std::wcstoul(argv[++index], &end, 10);
                if (end != nullptr && *end == L'\0')
                {
                    options.processPollIntervalMs = static_cast<DWORD>(std::clamp<unsigned long>(value, 100, 60000));
                }
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
