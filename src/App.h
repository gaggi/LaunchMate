#pragma once

#include "ConfigStore.h"
#include "ProcessMonitor.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <windows.h>

class MainWindow;

struct AppLaunchOptions
{
    DWORD processPollIntervalMs{5000};
    bool loggingEnabled{false};
};

class App
{
public:
    App(HINSTANCE instanceHandle, AppLaunchOptions launchOptions);
    ~App();
    int Run(int showCommand);

    HINSTANCE InstanceHandle() const noexcept;
    ConfigStore& Config() noexcept;
    ProcessMonitor& Monitor() noexcept;
    AppConfiguration& Configuration() noexcept;
    const AppConfiguration& Configuration() const noexcept;
    void SetStatus(const std::wstring& text);

private:
    void LogMessage(const std::wstring& text);

    HINSTANCE instanceHandle_{nullptr};
    AppLaunchOptions launchOptions_{};
    ConfigStore configStore_;
    AppConfiguration configuration_;
    ProcessMonitor monitor_;
    std::unique_ptr<MainWindow> mainWindow_;
    std::filesystem::path logPath_;
    std::mutex logMutex_;
};
