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
    int closeDelayMilliseconds{0};
};

struct ProcessStopAction
{
    std::wstring displayName;
    std::wstring processName;
    std::wstring executablePath;
    bool gracefulCloseFirst{true};
    int forceAfterMilliseconds{3000};
    bool restartAfterWatchProcessEnds{false};
    int restartDelayMilliseconds{0};
};

struct HomeAssistantAction
{
    std::wstring displayName;
    std::wstring webhookUrl;
    std::wstring jsonPayload{L"{}"};
    int waitTimeMilliseconds{0};
};

struct CatalogProgram
{
    std::wstring displayName;
    std::wstring filePath;
};

struct MonitorPowerSetup
{
    struct DisplayPath
    {
        std::wstring displayName;
        std::wstring monitorName;
        DWORD sourceAdapterLowPart{0};
        LONG sourceAdapterHighPart{0};
        UINT sourceId{0};
        DWORD targetAdapterLowPart{0};
        LONG targetAdapterHighPart{0};
        UINT targetId{0};
        LONG positionX{0};
        LONG positionY{0};
        UINT width{0};
        bool enabled{true};
        bool isPrimary{false};
    };

    std::wstring name;
    std::vector<DisplayPath> displayPaths;
    UINT hotkeyModifiers{0};
    UINT hotkeyVirtualKey{0};
};

struct WatchedProcessRule
{
    std::wstring displayName;
    std::wstring processName;
    std::wstring executablePath;
    bool enabled{true};
    std::vector<LaunchProgram> programsToLaunch;
    std::vector<ProcessStopAction> processesToStop;
    std::vector<HomeAssistantAction> homeAssistantActions;
    std::wstring monitorPowerSetupName;
    bool restoreMonitorPowerSetupOnExit{true};
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
    std::vector<CatalogProgram> catalogPrograms;
    std::vector<MonitorPowerSetup> monitorPowerSetups;
    std::vector<WatchedProcessRule> watchedProcesses;

    static AppConfiguration CreateDefault()
    {
        AppConfiguration config;
        return config;
    }
};
