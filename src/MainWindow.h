#pragma once

#include "App.h"
#include "MonitorPowerController.h"
#include "TrayIcon.h"
#include "UpdateChecker.h"
#include "BackgroundTask.h"
#include <array>
#include <unordered_map>

#include <windows.h>
#include <commctrl.h>

class MainWindow
{
public:
    static constexpr wchar_t kWindowClassName[] = L"LaunchMateWindow";
    static constexpr UINT kRestoreRequestMessage = WM_APP + 2;
    static constexpr UINT kUpdateCheckResultMessage = WM_APP + 3;
    static constexpr UINT kApplyDownloadedUpdateMessage = WM_APP + 4;
    static constexpr UINT kUpdateErrorMessage = WM_APP + 5;

    explicit MainWindow(App& app);
    ~MainWindow();

    bool Create(int showCommand);
    void SetStatus(const std::wstring& text);
    void SyncMonitoringState();

private:
    enum ControlId
    {
        IdToggleMonitoring = 2001,
        IdMonitorPowerSetups,
        IdSaveConfig,
        IdCheckForUpdates,
        IdDetectInstalledApps,
        IdTransferCatalogProgram,
        IdAddCatalogProgram,
        IdRemoveCatalogProgram,
        IdAddWatchedProcess,
        IdRemoveWatchedProcess,
        IdRemoveRuleAction,
        IdEditRuleActions,
        IdSettingsMinimizeToTray,
        IdSettingsCloseToTray,
        IdSettingsStartWithWindows,
        IdSettingsStartAsAdministrator,
        IdSettingsUseEtw,
        IdSettingsStartInTray,
        IdSettingsStartMonitoringOnLaunch,
        IdSettingsCheckForUpdatesOnStartup,
        IdCatalogSearch,
        IdCatalogList,
        IdWatchedList,
        IdRuleProgramsList,
        IdSourceTabs,
        IdMpoSettings
    };

    static constexpr UINT kTrayCallbackMessage = WM_APP + 1;

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void CreateFonts();
    void CreateControls();
    void PopulateLists();
    void RefreshProcessStates();
    static constexpr UINT_PTR kProcessStateTimer = 82;
    void PopulateCatalogPrograms();
    void PopulateRunningProcesses();
    void PopulateDetectedProcesses();
    void SwitchSourceTab();
    void SyncCatalogProgramsFromConfiguration();
    void DetectInstalledApps();
    void PopulateRulePrograms();
    void SyncProcessStateTimer(bool visible);
    void ToggleMonitoring();
    void ManageMonitorPowerSetups();
    bool ApplyMonitorPowerSetup(size_t index, bool interactive);
    void SaveConfiguration();
    void RegisterMonitorHotkeys();
    void UnregisterMonitorHotkeys();
    void CaptureWindowPlacement();
    void RestoreWindowPlacement(int showCommand);
    void HideToTray();
    void ShowFromTray();
    void AddSelectedCatalogProgram();
    void TransferSelectedSource();
    void AddSelectedRunningProcess();
    void AddSelectedDetectedProcess();
    void AddCustomCatalogProgram();
    void RemoveSelectedCatalogProgram();
    void AddWatchedProcess();
    void EditRuleProgram();
    void EditRuleActions(int initialTab = 0, int initialActionIndex = -1);
    void RemoveWatchedProcess();
    void RemoveSelectedRuleAction();
    void HandleTrayCommand(UINT command);
    void StartUpdateCheck(bool interactive);
    void BeginUpdateInstall(UpdateReleaseInfo release);
    void UpdateSettingsFromUi();
    void UpdateSettingsUi();
    LaunchProgram SelectLaunchProgram();
    WatchedProcessRule SelectWatchedProcess();
    int SelectedWatchedIndex() const;

    App& app_;
    HWND windowHandle_{nullptr};
    HWND toggleButtonHandle_{nullptr};
    HWND catalogSearchHandle_{nullptr};
    HWND catalogListHandle_{nullptr};
    HWND sourceTabsHandle_{nullptr};
    HWND detectSourceButtonHandle_{nullptr};
    HWND addCatalogButtonHandle_{nullptr};
    HWND removeCatalogButtonHandle_{nullptr};
    HWND watchedListHandle_{nullptr};
    HWND ruleProgramsListHandle_{nullptr};
    HWND minimizeToTrayHandle_{nullptr};
    HWND closeToTrayHandle_{nullptr};
    HWND startWithWindowsHandle_{nullptr};
    HWND startAsAdministratorHandle_{nullptr};
    HWND useEtwHandle_{nullptr};
    HWND startInTrayHandle_{nullptr};
    HWND startMonitoringHandle_{nullptr};
    HWND checkForUpdatesHandle_{nullptr};
    HFONT titleFont_{nullptr};
    HFONT uiFont_{nullptr};
    HIMAGELIST programIconList_{nullptr};
    std::unordered_map<std::wstring, int> programIconIndexes_;
    int defaultProgramIconIndex_{-1};
    TrayIcon trayIcon_;
    std::vector<CatalogProgram> detectedPrograms_;
    struct RunningProcessEntry
    {
        std::wstring displayName;
        std::wstring processName;
        std::wstring executablePath;
        DWORD processId{};
        double cpuUsagePercent{};
        unsigned long long memoryUsageBytes{};
        bool hasCpuUsage{};
        bool hasMemoryUsage{};
    };
    std::vector<RunningProcessEntry> runningProcesses_;
    struct DetectedProcessEntry
    {
        std::wstring displayName;
        std::wstring processName;
        std::wstring executablePath;
        std::wstring effect;
        bool running{};
        bool verifiedRunningPath{};
        bool allowStop{};
    };
    std::vector<DetectedProcessEntry> detectedProcesses_;
    struct SourceResult
    {
        std::vector<CatalogProgram> programs;
        std::vector<RunningProcessEntry> running;
        std::vector<DetectedProcessEntry> detected;
    };
    static void CaptureRunningProcesses(SourceResult& result, const std::wstring& watchedProcessName, const std::atomic_bool& cancelled);
    static void CaptureDetectedProcesses(SourceResult& result, const std::wstring& watchedProcessName, const std::atomic_bool& cancelled);
    void StartSourceRefresh();
    void PollSourceRefresh();
    void SyncSourceRefreshUi();
    void InitializeProgramIcons();
    int ProgramIconIndex(const std::wstring& executablePath);
    std::array<BackgroundTask<SourceResult>, 3> sourceTasks_;
    static constexpr UINT_PTR kSourceRefreshTimer = 81;
    int sourceTabIndex_{0};
    bool exitRequested_{false};
    bool updateCheckInProgress_{false};
    bool updateInstallInProgress_{false};
    bool appliedStartWithWindows_{false};
    bool appliedStartAsAdministrator_{false};
};
