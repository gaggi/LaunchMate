#pragma once

#include <string>
#include <vector>
#include <windows.h>

struct LaunchProgram
{
    std::wstring displayName;
    std::wstring filePath;
    std::wstring arguments;
    bool closeWhenGameStops{true};
    int waitTimeMilliseconds{0};
};

struct WatchedProcessRule
{
    std::wstring displayName;
    std::wstring processName;
    std::wstring executablePath;
    bool enabled{true};
    std::vector<LaunchProgram> programsToLaunch;
};

struct AppConfiguration
{
    bool minimizeToTray{true};
    bool closeToTray{true};
    bool startWithWindows{false};
    bool startInTray{false};
    bool startMonitoringOnLaunch{false};
    bool checkForUpdatesOnStartup{true};
    int windowWidth{1210};
    int windowHeight{730};
    int windowLeft{CW_USEDEFAULT};
    int windowTop{CW_USEDEFAULT};
    bool hasWindowPlacement{false};
    bool startMaximized{false};
    std::vector<LaunchProgram> globalPrograms;
    std::vector<WatchedProcessRule> watchedProcesses;

    static AppConfiguration CreateDefault()
    {
        AppConfiguration config;
        return config;
    }
};
