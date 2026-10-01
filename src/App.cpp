#include "App.h"

#include "MainWindow.h"
#include "Utils.h"

#include <fstream>

namespace
{
    std::wstring BuildTimestamp()
    {
        SYSTEMTIME localTime{};
        GetLocalTime(&localTime);

        wchar_t buffer[64] = {};
        swprintf_s(
            buffer,
            L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
            localTime.wYear,
            localTime.wMonth,
            localTime.wDay,
            localTime.wHour,
            localTime.wMinute,
            localTime.wSecond,
            localTime.wMilliseconds);
        return buffer;
    }

    bool IsRunningElevated()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        TOKEN_ELEVATION elevation{};
        DWORD size = 0;
        const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE &&
            elevation.TokenIsElevated != 0;
        CloseHandle(token);
        return elevated;
    }
}

App::App(HINSTANCE instanceHandle, AppLaunchOptions launchOptions)
    : instanceHandle_(instanceHandle),
      launchOptions_(launchOptions),
      configuration_(configStore_.Load()),
      monitor_([this](const std::wstring& status) { SetStatus(status); })
{
    const bool lowCpuPrioritySet = SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS) != FALSE;
    const DWORD priorityError = lowCpuPrioritySet ? ERROR_SUCCESS : GetLastError();
    monitor_.SetPollInterval(launchOptions_.pollIntervalMs);
    monitor_.SetActivePollInterval(launchOptions_.activePollIntervalMs);
    monitor_.UpdateConfiguration(configuration_);

    if (launchOptions_.loggingEnabled)
    {
        logPath_ = configStore_.Path().parent_path() / L"launchmate.log";
        LogMessage(L"Logging enabled.");
        LogMessage(lowCpuPrioritySet ? L"LaunchMate CPU priority: Low."
            : L"Could not set LaunchMate CPU priority to Low (Windows error " + std::to_wstring(priorityError) + L").");
        LogMessage(L"Idle poll interval: " + std::to_wstring(launchOptions_.pollIntervalMs) + L" ms");
        LogMessage(L"Active poll interval: " + std::to_wstring(launchOptions_.activePollIntervalMs) + L" ms");
        LogMessage(IsRunningElevated()
            ? L"LaunchMate is running as administrator."
            : L"LaunchMate is running without administrator rights.");
    }
}

App::~App()
{
    // Stop callbacks before the window and logging mutex are destroyed.
    monitor_.Stop();
}

int App::Run(int showCommand)
{
    LogMessage(L"Application starting.");

    mainWindow_ = std::make_unique<MainWindow>(*this);
    if (!mainWindow_->Create(showCommand))
    {
        LogMessage(L"Failed to create main window.");
        return -1;
    }

    monitor_.RecoverIRacingServices();

    if (configuration_.startMonitoringOnLaunch)
    {
        monitor_.Start();
        mainWindow_->SyncMonitoringState();
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}

HINSTANCE App::InstanceHandle() const noexcept
{
    return instanceHandle_;
}

ConfigStore& App::Config() noexcept
{
    return configStore_;
}

ProcessMonitor& App::Monitor() noexcept
{
    return monitor_;
}

AppConfiguration& App::Configuration() noexcept
{
    return configuration_;
}

const AppConfiguration& App::Configuration() const noexcept
{
    return configuration_;
}

bool App::LoggingEnabled() const noexcept
{
    return launchOptions_.loggingEnabled;
}

void App::Log(const std::wstring& text)
{
    LogMessage(text);
}

void App::SetStatus(const std::wstring& text)
{
    if (mainWindow_)
    {
        mainWindow_->SetStatus(text);
    }

    LogMessage(text);
}

void App::LogMessage(const std::wstring& text)
{
    if (!launchOptions_.loggingEnabled || text.empty())
    {
        return;
    }

    std::scoped_lock lock(logMutex_);
    std::error_code directoryError;
    std::filesystem::create_directories(logPath_.parent_path(), directoryError);
    if (directoryError) return;

    std::ofstream stream(logPath_, std::ios::binary | std::ios::app);
    if (!stream)
    {
        return;
    }

    const auto line = ToUtf8(BuildTimestamp() + L"  " + text + L"\r\n");
    stream.write(line.data(), static_cast<std::streamsize>(line.size()));
}
