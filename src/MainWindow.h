#pragma once

#include "App.h"
#include "MonitorPowerController.h"
#include "TrayIcon.h"
#include "UpdateChecker.h"
#include "ui/CardList.h"
#include "ui/NavBar.h"
#include "Pages.h"
#include "ui/ScrollHost.h"
#include "ui/StatusPanel.h"
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
        IdAddWatchedProcess,
        IdRemoveWatchedProcess,
        IdSettings,
        IdNavRules,
        IdRuleCards,
        IdStartCards,
        IdExitCards,
        IdRuleBack,
        IdRuleToggleEnabled,
        IdSectionBack,
        IdRuleAppSettings
    };

    enum class Page
    {
        Rules,
        RuleDetail,
        RuleSection,
        Displays,
        Settings
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
    void ShowPage(Page page);
    void PopulateRuleCards();
    void PopulateRuleDetail();
    void OpenRule(int index);
    void HandleCardCommand(int controlId, int code);
    void ShowRuleContextMenu(int index);
    void OpenRuleSection(RuleSection section);
    HWND CreatePagePane(Page page);
    void ScheduleSave();
    void FlushPendingSave();
    void ToggleRuleEnabled(int index);
    void SyncProcessStateTimer(bool visible);
    void ToggleMonitoring();
    bool ApplyMonitorPowerSetup(size_t index, bool interactive);
    void SaveConfiguration();
    void RegisterMonitorHotkeys();
    void UnregisterMonitorHotkeys();
    void CaptureWindowPlacement();
    void RestoreWindowPlacement(int showCommand);
    void HideToTray();
    void ShowFromTray();
    void AddWatchedProcess();
    void RemoveWatchedProcess(int index);
    void HandleTrayCommand(UINT command);
    // `startup` checks only when enabled in the settings and stays quiet on failure.
    void StartUpdateCheck(bool startup);
    void BeginUpdateInstall();
    // Lets the shown page redraw state it reads from the window (the update check).
    void RefreshPage();
    void RefreshStatusPanel();
    WatchedProcessRule SelectWatchedProcess();
    int SelectedWatchedIndex() const;

    App& app_;
    HWND windowHandle_{nullptr};
    StatusPanel statusPanel_;
    NavBar navBar_;
    CardList ruleCards_;
    CardList startCards_;
    CardList exitCards_;
    // Rule section that each start/exit card opens.
    std::vector<RuleSection> startCardSections_;
    std::vector<RuleSection> exitCardSections_;
    // Embedded page for Settings, Displays and rule sections.
    ScrollHost pageHost_;
    HWND pageTitleHandle_{nullptr};
    HWND pageHintHandle_{nullptr};
    RuleSection openSection_{RuleSection::StartPrograms};
    bool savePending_{false};
    static constexpr UINT_PTR kSaveTimer = 83;
    HWND rulesHeadingHandle_{nullptr};
    HWND ruleTitleHandle_{nullptr};
    HWND ruleSubtitleHandle_{nullptr};
    HWND startHeadingHandle_{nullptr};
    HWND exitHeadingHandle_{nullptr};
    Page page_{Page::Rules};
    int selectedRuleIndex_{-1};
    HFONT headingFont_{nullptr};
    HFONT uiFont_{nullptr};
    UINT dpi_{96};
    TrayIcon trayIcon_;
    bool exitRequested_{false};
    // Stopping restores sessions (closing programs, services, displays), which can
    // take seconds or wait for UAC, so it runs off the UI thread.
    std::thread monitorStopThread_;
    bool monitorStopping_{false};
    // Hotkey ids registered last time; configs may have been removed since.
    size_t registeredHotkeys_{0};
    UpdateState update_;
    bool appliedStartWithWindows_{false};
    bool appliedStartAsAdministrator_{false};
};
