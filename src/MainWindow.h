#pragma once

#include "App.h"
#include "MonitorPowerController.h"
#include "TrayIcon.h"
#include "UpdateChecker.h"
#include "BackgroundTask.h"
#include "CardList.h"
#include "NavBar.h"
#include "StatusPanel.h"
#include <array>
#include <thread>
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
        IdDetectInstalledApps,
        IdTransferCatalogProgram,
        IdAddCatalogProgram,
        IdRemoveCatalogProgram,
        IdAddWatchedProcess,
        IdRemoveWatchedProcess,
        IdCatalogSearch,
        IdCatalogList,
        IdSourceTabs,
        IdSettings,
        IdNavRules,
        IdNavApps,
        IdRuleCards,
        IdStartCards,
        IdExitCards,
        IdRuleBack,
        IdRuleToggleEnabled,
        IdAppsRuleCombo
    };

    enum class Page
    {
        Rules,
        RuleDetail,
        Apps
    };

    static constexpr UINT kTrayCallbackMessage = WM_APP + 1;
    static constexpr UINT kMonitorStoppedMessage = WM_APP + 6;

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void CreateFonts();
    void CreateControls();
    void LayoutControls(int width, int height);
    void PopulateLists();
    void RefreshProcessStates();
    static constexpr UINT_PTR kProcessStateTimer = 82;
    void PopulateCatalogPrograms();
    void PopulateRunningProcesses();
    void PopulateDetectedProcesses();
    void SwitchSourceTab();
    void SyncCatalogProgramsFromConfiguration();
    void DetectInstalledApps();
    void ShowPage(Page page);
    void PopulateRuleCards();
    void PopulateRuleDetail();
    void PopulateRuleCombo();
    void OpenRule(int index);
    void HandleCardCommand(int controlId, int code);
    void ShowRuleContextMenu(int index);
    void ToggleRuleEnabled(int index);
    void ReportTransfer(size_t added, bool started);
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
    void EditRuleActions(int initialTab = 0, int initialActionIndex = -1);
    void RemoveWatchedProcess(int index);
    void HandleTrayCommand(UINT command);
    void StartUpdateCheck(bool interactive);
    void BeginUpdateInstall(UpdateReleaseInfo release);
    void ShowSettingsDialog();
    static INT_PTR CALLBACK SettingsDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
    void RefreshStatusPanel();
    LaunchProgram SelectLaunchProgram();
    WatchedProcessRule SelectWatchedProcess();
    int SelectedWatchedIndex() const;

    App& app_;
    HWND windowHandle_{nullptr};
    StatusPanel statusPanel_;
    NavBar navBar_;
    CardList ruleCards_;
    CardList startCards_;
    CardList exitCards_;
    // Dialog tab of RuleActionsDialog that each start/exit card opens.
    std::vector<int> startCardTabs_;
    std::vector<int> exitCardTabs_;
    HWND rulesHeadingHandle_{nullptr};
    HWND ruleTitleHandle_{nullptr};
    HWND ruleSubtitleHandle_{nullptr};
    HWND startHeadingHandle_{nullptr};
    HWND exitHeadingHandle_{nullptr};
    HWND appsRuleLabelHandle_{nullptr};
    HWND appsRuleComboHandle_{nullptr};
    HWND appsFeedbackHandle_{nullptr};
    Page page_{Page::Rules};
    int selectedRuleIndex_{-1};
    HWND catalogSearchHandle_{nullptr};
    HWND catalogListHandle_{nullptr};
    HWND sourceTabsHandle_{nullptr};
    HWND detectSourceButtonHandle_{nullptr};
    HWND addCatalogButtonHandle_{nullptr};
    HWND removeCatalogButtonHandle_{nullptr};
    HFONT headingFont_{nullptr};
    HFONT uiFont_{nullptr};
    UINT dpi_{96};
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
    // Stopping restores sessions (closing programs, services, displays), which can
    // take seconds or wait for UAC, so it runs off the UI thread.
    std::thread monitorStopThread_;
    bool monitorStopping_{false};
    bool updateCheckInProgress_{false};
    bool updateInstallInProgress_{false};
    bool appliedStartWithWindows_{false};
    bool appliedStartAsAdministrator_{false};
};
