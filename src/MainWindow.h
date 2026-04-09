#pragma once

#include "App.h"
#include "TrayIcon.h"
#include "UpdateChecker.h"

#include <windows.h>

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
        IdSaveConfig,
        IdCheckForUpdates,
        IdAddGlobalProgram,
        IdRemoveGlobalProgram,
        IdAddWatchedProcess,
        IdRemoveWatchedProcess,
        IdAddRuleProgram,
        IdRemoveRuleProgram,
        IdSettingsMinimizeToTray,
        IdSettingsCloseToTray,
        IdSettingsStartWithWindows,
        IdSettingsStartInTray,
        IdSettingsStartMonitoringOnLaunch,
        IdSettingsCheckForUpdatesOnStartup,
        IdGlobalList,
        IdWatchedList,
        IdRuleProgramsList
    };

    static constexpr UINT kTrayCallbackMessage = WM_APP + 1;

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void CreateFonts();
    void CreateControls();
    void PopulateLists();
    void PopulateRulePrograms();
    void ToggleMonitoring();
    void SaveConfiguration();
    void LoadConfiguration();
    void CaptureWindowPlacement();
    void RestoreWindowPlacement(int showCommand);
    void HideToTray();
    void ShowFromTray();
    void AddGlobalProgram();
    void AddWatchedProcess();
    void AddRuleProgram();
    void EditGlobalProgram();
    void EditRuleProgram();
    void RemoveGlobalProgram();
    void RemoveWatchedProcess();
    void RemoveRuleProgram();
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
    HWND globalListHandle_{nullptr};
    HWND watchedListHandle_{nullptr};
    HWND ruleProgramsListHandle_{nullptr};
    HWND minimizeToTrayHandle_{nullptr};
    HWND closeToTrayHandle_{nullptr};
    HWND startWithWindowsHandle_{nullptr};
    HWND startInTrayHandle_{nullptr};
    HWND startMonitoringHandle_{nullptr};
    HWND checkForUpdatesHandle_{nullptr};
    HFONT titleFont_{nullptr};
    HFONT uiFont_{nullptr};
    HBRUSH backgroundBrush_{nullptr};
    HBRUSH panelBrush_{nullptr};
    TrayIcon trayIcon_;
    bool exitRequested_{false};
    bool updateCheckInProgress_{false};
    bool updateInstallInProgress_{false};
};
