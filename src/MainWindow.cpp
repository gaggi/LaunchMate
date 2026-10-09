#include "MainWindow.h"

#include "IRacingPerformance.h"
#include "IRacingServices.h"
#include "Pages.h"
#include "StartupRegistration.h"
#include "resource.h"
#include "Utils.h"
#include "ui/UiTheme.h"

#include <algorithm>
#include <TlHelp32.h>
#include <commctrl.h>
#include <commdlg.h>
#include <psapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace
{
    constexpr UINT kMonitorSetupHotkeyBase = 5000;
    bool IsValidRect(const RECT& rect)
    {
        return rect.right > rect.left && rect.bottom > rect.top;
    }

    RECT EnsureVisibleRect(const RECT& rect)
    {
        RECT workArea{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);

        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (const auto monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
            monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo))
        {
            workArea = monitorInfo.rcWork;
        }

        const int workWidth = static_cast<int>(workArea.right - workArea.left);
        const int workHeight = static_cast<int>(workArea.bottom - workArea.top);
        const int maxWidth = std::max(320, workWidth);
        const int maxHeight = std::max(240, workHeight);
        const int rectWidth = static_cast<int>(rect.right - rect.left);
        const int rectHeight = static_cast<int>(rect.bottom - rect.top);
        const int width = std::clamp(rectWidth, 320, maxWidth);
        const int height = std::clamp(rectHeight, 240, maxHeight);

        int left = rect.left;
        int top = rect.top;

        if (left < workArea.left)
        {
            left = workArea.left;
        }
        if (top < workArea.top)
        {
            top = workArea.top;
        }
        if (left + width > workArea.right)
        {
            left = workArea.right - width;
        }
        if (top + height > workArea.bottom)
        {
            top = workArea.bottom - height;
        }

        left = std::max(left, static_cast<int>(workArea.left));
        top = std::max(top, static_cast<int>(workArea.top));

        return RECT{left, top, left + width, top + height};
    }

    RECT GetNormalWindowRect(HWND windowHandle)
    {
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (GetWindowPlacement(windowHandle, &placement) && IsValidRect(placement.rcNormalPosition))
        {
            return placement.rcNormalPosition;
        }

        RECT rect{};
        GetWindowRect(windowHandle, &rect);
        return rect;
    }

    HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, HFONT font)
    {
        auto handle = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, x, y, w, h, parent, nullptr, nullptr, nullptr);
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return handle;
    }

    HWND CreateButtonControl(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h, HFONT font, DWORD extraStyle = 0)
    {
        auto handle = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | extraStyle,
            x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return handle;
    }

    HWND CreateEditControl(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h, HFONT font)
    {
        auto handle = CreateWindowExW(
            0,
            L"EDIT",
            text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            x,
            y,
            w,
            h,
            parent,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            nullptr,
            nullptr);
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return handle;
    }

    std::wstring PickExecutablePath(HWND owner, const wchar_t* title)
    {
        wchar_t fileBuffer[MAX_PATH] = {};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = owner;
        dialog.lpstrTitle = title;
        dialog.lpstrFilter = L"Programs (*.exe)\0*.exe\0All files (*.*)\0*.*\0";
        dialog.lpstrFile = fileBuffer;
        dialog.nMaxFile = MAX_PATH;
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

        if (!GetOpenFileNameW(&dialog))
        {
            return {};
        }

        return dialog.lpstrFile;
    }

    template <typename T>
    void PostOwnedMessage(HWND windowHandle, UINT message, T* payload)
    {
        if (!PostMessageW(windowHandle, message, 0, reinterpret_cast<LPARAM>(payload)))
        {
            delete payload;
        }
    }

    // The watched program's file name; rules are matched by it, not by path.
    std::wstring ProgramFileName(const WatchedProcessRule& rule)
    {
        return std::filesystem::path(rule.processName.empty() ? rule.executablePath : rule.processName).filename().wstring();
    }

    struct PostedUpdateCheckResult
    {
        UpdateCheckResult result;
        bool startup{false};
    };

}

MainWindow::MainWindow(App& app)
    : app_(app)
{
}

MainWindow::~MainWindow()
{
    if (monitorStopThread_.joinable()) monitorStopThread_.join();
    if (headingFont_) DeleteObject(headingFont_);
    if (uiFont_) DeleteObject(uiFont_);
}

bool MainWindow::Create(int showCommand)
{
    dpi_ = GetDpiForSystem();
    const auto appIcon = static_cast<HICON>(LoadImageW(
        app_.InstanceHandle(),
        MAKEINTRESOURCEW(IDI_APPICON),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXICON),
        GetSystemMetrics(SM_CYICON),
        LR_DEFAULTCOLOR));
    const auto appSmallIcon = static_cast<HICON>(LoadImageW(
        app_.InstanceHandle(),
        MAKEINTRESOURCEW(IDI_APPICON),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = MainWindow::WindowProc;
    windowClass.hInstance = app_.InstanceHandle();
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = appIcon ? appIcon : LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = appSmallIcon ? appSmallIcon : windowClass.hIcon;
    windowClass.hbrBackground = UiTheme::BackgroundBrush();
    windowClass.lpszClassName = MainWindow::kWindowClassName;
    RegisterClassExW(&windowClass);

    CreateFonts();

    const auto& config = app_.Configuration();
    windowHandle_ = CreateWindowExW(
        0,
        windowClass.lpszClassName,
        L"LaunchMate",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        config.windowWidth,
        config.windowHeight,
        nullptr,
        nullptr,
        app_.InstanceHandle(),
        this);

    if (!windowHandle_)
    {
        return false;
    }

    CreateControls();
    UiTheme::Apply(windowHandle_);
    RECT client{};
    GetClientRect(windowHandle_, &client);
    LayoutControls(client.right, client.bottom);
    PopulateLists();
    RestoreWindowPlacement(app_.Configuration().startInTray ? SW_HIDE : showCommand);
    // Refresh an older normal logon task once this instance has already been
    // elevated.  This is the one-time migration to a highest-privilege task.
    if (app_.Configuration().startWithWindows && app_.Configuration().startAsAdministrator)
        StartupRegistration::Apply(true, true);
    appliedStartWithWindows_ = app_.Configuration().startWithWindows;
    appliedStartAsAdministrator_ = app_.Configuration().startAsAdministrator;

    trayIcon_.Create(
        windowHandle_,
        kTrayCallbackMessage,
        appSmallIcon ? appSmallIcon : windowClass.hIcon,
        L"LaunchMate",
        [this](UINT command)
    {
        HandleTrayCommand(command);
    });

    RegisterMonitorHotkeys();
    StartUpdateCheck(true);
    return true;
}

void MainWindow::SetStatus(const std::wstring& text)
{
    // Progress messages go to the log (App); failures arrive through ReportProblem.
    (void)text;
}

void MainWindow::ReportProblem(const std::wstring& problem)
{
    PostOwnedMessage(windowHandle_, kMonitorProblemMessage, new std::wstring(problem));
}

void MainWindow::SyncMonitoringState()
{
    RefreshStatusPanel();
}

namespace
{
    std::wstring DescribeRunningTime(ULONGLONG milliseconds)
    {
        const auto minutes = milliseconds / 60000;
        if (minutes == 0) return L"just started";
        if (minutes < 60) return L"running for " + std::to_wstring(minutes) + L" min";
        return L"running for " + std::to_wstring(minutes / 60) + L" h " + std::to_wstring(minutes % 60) + L" min";
    }
}

void MainWindow::RefreshStatusPanel()
{
    if (!statusPanel_.Handle()) return;
    auto& monitor = app_.Monitor();
    // Problems come first: the banner is the one place that is always visible.
    std::wstring warning = app_.Config().LoadProblem();
    if (warning.empty() && !problems_.empty())
    {
        warning = problems_.back();
        if (problems_.size() > 1) warning = L"(" + std::to_wstring(problems_.size()) + L" problems) " + warning;
    }
    if (monitorStopping_)
    {
        statusPanel_.SetState(StatusPanel::Tone::Busy, L"Stopping monitoring...",
            L"Closing started apps and restoring services, power plan and displays.", L"Stopping...", false);
        return;
    }
    if (!monitor.IsRunning())
    {
        statusPanel_.SetState(warning.empty() ? StatusPanel::Tone::Neutral : StatusPanel::Tone::Busy, L"Monitoring is off",
            warning.empty() ? L"Start monitoring to run your rules automatically." : warning, L"Start monitoring", true);
        return;
    }

    const auto& rules = app_.Configuration().watchedProcesses;
    const auto runningSince = monitor.GetRunningSince(rules);
    const auto now = GetTickCount64();
    const auto ruleName = [](const WatchedProcessRule& rule)
    {
        return rule.displayName.empty() ? rule.processName : rule.displayName;
    };
    std::wstring detail = monitor.IsUsingEtw() ? L"ETW" : L"Process polling";
    std::vector<size_t> enabledRules;
    bool anyRunning = false;
    for (size_t index = 0; index < rules.size(); ++index)
    {
        if (rules[index].enabled) enabledRules.push_back(index);
        if (index >= runningSince.size() || runningSince[index] == 0) continue;
        anyRunning = true;
        detail += L"  \u00B7  " + ruleName(rules[index]) + L" " + DescribeRunningTime(now - runningSince[index]);
    }
    if (!anyRunning)
    {
        detail += enabledRules.empty() ? L"  \u00B7  No enabled rules"
            : enabledRules.size() == 1 ? L"  \u00B7  Waiting for " + ruleName(rules[enabledRules.front()])
            : L"  \u00B7  Waiting for " + std::to_wstring(enabledRules.size()) + L" watched programs";
    }
    if (!warning.empty())
        statusPanel_.SetState(StatusPanel::Tone::Busy, L"Monitoring active", warning, L"Stop monitoring", true);
    else
        statusPanel_.SetState(StatusPanel::Tone::Active, L"Monitoring active", detail, L"Stop monitoring", true);
}

LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    MainWindow* self = nullptr;
    if (message == WM_NCCREATE)
    {
        auto createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->windowHandle_ = hwnd;
    }
    else
    {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_COMMAND:
    {
        const int controlId = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if (controlId == IdRuleCards || controlId == IdStartCards || controlId == IdExitCards)
        {
            HandleCardCommand(controlId, code);
            return 0;
        }
        switch (controlId)
        {
        case IdToggleMonitoring: ToggleMonitoring(); return 0;
        case IdMonitorPowerSetups: ShowPage(Page::Displays); return 0;
        case IdSettings: ShowPage(Page::Settings); return 0;
        case IdSectionBack: ShowPage(Page::RuleDetail); return 0;
        case IdNavRules: ShowPage(Page::Rules); return 0;
        case IdRuleBack: ShowPage(Page::Rules); return 0;
        case IdRuleToggleEnabled: ToggleRuleEnabled(selectedRuleIndex_); return 0;
        case IdRuleAppSettings: OpenRuleSection(RuleSection::AppSpecific); return 0;
        case IdAddWatchedProcess: AddWatchedProcess(); return 0;
        case IdRemoveWatchedProcess: RemoveWatchedProcess(selectedRuleIndex_); return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (!exitRequested_ && app_.Configuration().closeToTray)
        {
            SaveConfiguration();
            HideToTray();
            return 0;
        }
        // Closing the page saves what it still holds (e.g. an edited hotkey).
        pageHost_.Clear();
        FlushPendingSave();
        // App's destructor stops monitoring once the window is gone, so restoring an
        // active session never leaves a frozen window on screen.
        trayIcon_.Destroy();
        DestroyWindow(windowHandle_);
        return 0;
    case WM_SHOWWINDOW:
        SyncProcessStateTimer(wParam != FALSE);
        break;
    case WM_SIZE:
        SyncProcessStateTimer(wParam != SIZE_MINIMIZED && IsWindowVisible(windowHandle_));
        if (wParam == SIZE_MINIMIZED && app_.Configuration().minimizeToTray)
        {
            HideToTray();
            return 0;
        }
        if (wParam != SIZE_MINIMIZED) LayoutControls(LOWORD(lParam), HIWORD(lParam));
        break;
    case WM_GETMINMAXINFO:
    {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize = {MulDiv(900, dpi_, 96), MulDiv(600, dpi_, 96)};
        return 0;
    }
    case WM_DPICHANGED:
    {
        dpi_ = HIWORD(wParam);
        CreateFonts();
        const auto* rect = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(windowHandle_, nullptr, rect->left, rect->top,
            rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_HOTKEY:
        if (wParam >= kMonitorSetupHotkeyBase)
        {
            const size_t setupIndex = static_cast<size_t>(wParam - kMonitorSetupHotkeyBase);
            ApplyMonitorPowerSetup(setupIndex, true);
            return 0;
        }
        break;
    case WM_TIMER:
        if (wParam == kProcessStateTimer)
        {
            if (IsWindowVisible(windowHandle_) && !IsIconic(windowHandle_)) RefreshProcessStates();
            return 0;
        }
        if (wParam == kSaveTimer) { FlushPendingSave(); return 0; }
        break;
    case WM_DESTROY:
        KillTimer(windowHandle_, kProcessStateTimer);
        KillTimer(windowHandle_, kSaveTimer);
        UnregisterMonitorHotkeys();
        PostQuitMessage(0);
        return 0;
    default:
        if (message == kUpdateCheckResultMessage)
        {
            std::unique_ptr<PostedUpdateCheckResult> posted(reinterpret_cast<PostedUpdateCheckResult*>(lParam));
            if (!posted) return 0;
            const auto& result = posted->result;
            update_.release = result.release;
            update_.message = result.message;
            switch (result.state)
            {
            case UpdateCheckState::Failed:
                app_.Log(L"Update check failed: " + result.message);
                // A failed check at startup (no network yet) is not worth a warning.
                update_.phase = posted->startup ? UpdateState::Phase::Idle : UpdateState::Phase::Failed;
                break;
            case UpdateCheckState::UpToDate:
                app_.Log(L"Update check complete. LaunchMate is up to date.");
                update_.phase = UpdateState::Phase::UpToDate;
                if (!result.release.versionDisplay.empty())
                    update_.message = L"Latest release on GitHub: " + result.release.versionDisplay + L".";
                break;
            case UpdateCheckState::UpdateAvailable:
                app_.Log(L"Update available: " + result.release.versionDisplay);
                update_.phase = UpdateState::Phase::Available;
                navBar_.SetFooter(L"Update available: " + result.release.versionDisplay, true);
                break;
            }
            RefreshPage();
            return 0;
        }

        if (message == kApplyDownloadedUpdateMessage)
        {
            app_.Log(L"Update downloaded. Restarting LaunchMate to finish installation.");
            SaveConfiguration();
            exitRequested_ = true;
            PostMessageW(windowHandle_, WM_CLOSE, 0, 0);
            return 0;
        }

        if (message == kUpdateErrorMessage)
        {
            std::unique_ptr<std::wstring> errorText(reinterpret_cast<std::wstring*>(lParam));
            update_.phase = UpdateState::Phase::Failed;
            update_.message = errorText && !errorText->empty() ? *errorText : L"The update could not be installed.";
            app_.Log(update_.message);
            RefreshPage();
            return 0;
        }

        if (message == kMonitorProblemMessage)
        {
            std::unique_ptr<std::wstring> problem(reinterpret_cast<std::wstring*>(lParam));
            if (!problem) return 0;
            if (problem->empty()) problems_.clear();
            else
            {
                problems_.push_back(*problem);
                if (problems_.size() > 20) problems_.erase(problems_.begin());
            }
            RefreshStatusPanel();
            return 0;
        }

        if (message == kMonitorStoppedMessage)
        {
            if (monitorStopThread_.joinable()) monitorStopThread_.join();
            monitorStopping_ = false;
            SyncMonitoringState();
            return 0;
        }

        if (message == kRestoreRequestMessage)
        {
            ShowFromTray();
            return 0;
        }

        if (message == kTrayCallbackMessage)
        {
            if (lParam == WM_LBUTTONUP)
            {
                ShowFromTray();
            }
            else if (lParam == WM_RBUTTONUP)
            {
                std::vector<std::wstring> monitorSetupNames;
                monitorSetupNames.reserve(app_.Configuration().monitorPowerSetups.size());
                for (const auto& setup : app_.Configuration().monitorPowerSetups)
                {
                    monitorSetupNames.push_back(setup.name);
                }
                trayIcon_.ShowContextMenu(app_.Monitor().IsRunning(), monitorSetupNames);
            }
            return 0;
        }
        break;
    }

    return DefWindowProcW(windowHandle_, message, wParam, lParam);
}

void MainWindow::CreateFonts()
{
    const HFONT oldUi = uiFont_, oldHeading = headingFont_;
    const auto font = [this](int points, int weight)
    {
        return CreateFontW(-MulDiv(points, dpi_, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
    };
    uiFont_ = font(9, FW_NORMAL);
    headingFont_ = font(10, FW_SEMIBOLD);
    if (windowHandle_)
        EnumChildWindows(windowHandle_, [](HWND control, LPARAM value) -> BOOL
        {
            SendMessageW(control, WM_SETFONT, static_cast<WPARAM>(value), TRUE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(uiFont_));
    for (HWND control : {rulesHeadingHandle_, ruleTitleHandle_, startHeadingHandle_, exitHeadingHandle_, pageTitleHandle_})
        if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont_), TRUE);
    if (statusPanel_.Handle()) statusPanel_.SetFonts(headingFont_, uiFont_);
    if (navBar_.Handle()) navBar_.SetFonts(headingFont_, uiFont_);
    for (CardList* cards : {&ruleCards_, &startCards_, &exitCards_})
        if (cards->Handle()) cards->SetFonts(headingFont_, uiFont_);
    // Embedded pages and their lists keep the font handles they were created with.
    if (pageHost_.Content()) ShowPage(page_);
    DeleteObject(oldUi);
    DeleteObject(oldHeading);
}

void MainWindow::CreateControls()
{
    navBar_.Create(app_.InstanceHandle(), windowHandle_, L"LaunchMate", L"Version " + UpdateChecker::CurrentVersion(), {
        {L'\uE8FD', L"Rules", IdNavRules, true},
        {L'\uE7F4', L"Displays", IdMonitorPowerSetups, true},
        {L'\uE713', L"Settings", IdSettings, true}}, headingFont_, uiFont_);
    statusPanel_.Create(app_.InstanceHandle(), windowHandle_, IdToggleMonitoring, headingFont_, uiFont_);

    // Rules page
    rulesHeadingHandle_ = CreateLabel(windowHandle_, L"Rules", 0, 0, 240, 24, headingFont_);
    CreateButtonControl(windowHandle_, IdAddWatchedProcess, L"Add rule", 0, 0, 110, 30, uiFont_);
    ruleCards_.Create(app_.InstanceHandle(), windowHandle_, IdRuleCards, headingFont_, uiFont_);
    ruleCards_.SetEmptyText(L"Add a rule for a program LaunchMate should watch, for example iRacing. "
        L"Its actions run when the program starts and are undone when it exits.");

    // Rule page
    CreateButtonControl(windowHandle_, IdRuleBack, L"\u2190  Rules", 0, 0, 96, 30, uiFont_);
    ruleTitleHandle_ = CreateLabel(windowHandle_, L"", 0, 0, 240, 24, headingFont_);
    ruleSubtitleHandle_ = CreateLabel(windowHandle_, L"", 0, 0, 240, 20, uiFont_);
    SetWindowLongPtrW(ruleSubtitleHandle_, GWL_STYLE, GetWindowLongPtrW(ruleSubtitleHandle_, GWL_STYLE) | SS_PATHELLIPSIS);
    CreateButtonControl(windowHandle_, IdRuleAppSettings, L"iRacing settings", 0, 0, 130, 30, uiFont_);
    CreateButtonControl(windowHandle_, IdRuleToggleEnabled, L"Disable", 0, 0, 100, 30, uiFont_);
    CreateButtonControl(windowHandle_, IdRemoveWatchedProcess, L"Remove rule", 0, 0, 110, 30, uiFont_);
    startHeadingHandle_ = CreateLabel(windowHandle_, L"", 0, 0, 240, 24, headingFont_);
    exitHeadingHandle_ = CreateLabel(windowHandle_, L"", 0, 0, 240, 24, headingFont_);
    startCards_.Create(app_.InstanceHandle(), windowHandle_, IdStartCards, headingFont_, uiFont_);
    exitCards_.Create(app_.InstanceHandle(), windowHandle_, IdExitCards, headingFont_, uiFont_);
    startCards_.SetCompact(true);
    exitCards_.SetCompact(true);

    // Embedded pages: rule sections, Displays and Settings
    CreateButtonControl(windowHandle_, IdSectionBack, L"", 0, 0, 150, 30, uiFont_);
    pageTitleHandle_ = CreateLabel(windowHandle_, L"", 0, 0, 240, 24, headingFont_);
    pageHintHandle_ = CreateLabel(windowHandle_, L"", 0, 0, 240, 20, uiFont_);
    SetWindowLongPtrW(pageHintHandle_, GWL_STYLE, GetWindowLongPtrW(pageHintHandle_, GWL_STYLE) | SS_ENDELLIPSIS);
    pageHost_.Create(app_.InstanceHandle(), windowHandle_);


}

void MainWindow::LayoutControls(int width, int height)
{
    if (!statusPanel_.Handle()) return;
    const auto px = [this](int value) { return MulDiv(value, dpi_, 96); };
    const int w = MulDiv(width, 96, dpi_);
    const int h = MulDiv(height, 96, dpi_);
    constexpr int navWidth = 184;
    const int left = navWidth + 24;
    const int content = w - left - 24;
    const int right = left + content;
    const int top = 92; // Below the status banner.
    const int bottom = h - 20;
    const auto place = [&](HWND control, int x, int y, int cw, int ch)
    {
        if (!control) return;
        POINT position{px(x), px(y)};
        MapWindowPoints(windowHandle_, GetParent(control), &position, 1);
        MoveWindow(control, position.x, position.y, px(cw), px(ch), TRUE);
    };
    const auto button = [&](int id, int x, int y, int cw, int ch)
    { place(GetDlgItem(windowHandle_, id), x, y, cw, ch); };

    place(navBar_.Handle(), 0, 0, navWidth, h);
    place(statusPanel_.Handle(), left, 20, content, 56);

    // Rules page
    place(rulesHeadingHandle_, left, top + 3, content - 130, 24);
    button(IdAddWatchedProcess, right - 110, top, 110, 30);
    place(ruleCards_.Handle(), left, top + 44, content, bottom - top - 44);

    // Rule page
    button(IdRuleBack, left, top, 96, 30);
    // The app settings button only shows for iRacing rules; keep its space either way.
    place(ruleTitleHandle_, left + 112, top - 4, content - 112 - 370, 24);
    place(ruleSubtitleHandle_, left + 112, top + 18, content - 112 - 370, 20);
    button(IdRuleAppSettings, right - 360, top, 130, 30);
    button(IdRuleToggleEnabled, right - 220, top, 100, 30);
    button(IdRemoveWatchedProcess, right - 110, top, 110, 30);
    const int columnWidth = (content - 20) / 2;
    place(startHeadingHandle_, left, top + 52, columnWidth, 24);
    place(exitHeadingHandle_, left + columnWidth + 20, top + 52, columnWidth, 24);
    place(startCards_.Handle(), left, top + 84, columnWidth, bottom - top - 84);
    place(exitCards_.Handle(), left + columnWidth + 20, top + 84, columnWidth, bottom - top - 84);

    // Embedded pages
    const bool sectionPage = page_ == Page::RuleSection;
    button(IdSectionBack, left, top, 150, 30);
    const int titleLeft = sectionPage ? left + 166 : left;
    place(pageTitleHandle_, titleLeft, top - 1, right - titleLeft, 24);
    place(pageHintHandle_, titleLeft, top + 21, right - titleLeft, 20);
    place(pageHost_.Handle(), left, top + 56, content, bottom - top - 56);

}

void MainWindow::PopulateLists()
{
    if (page_ == Page::RuleDetail) PopulateRuleDetail();
    RefreshProcessStates();
    ShowPage(page_);
}

void MainWindow::ShowPage(Page page)
{
    if ((page == Page::RuleDetail || page == Page::RuleSection) && SelectedWatchedIndex() < 0) page = Page::Rules;
    // The embedded page edits configuration objects directly; close it (which saves
    // anything it still holds) before the page changes.
    pageHost_.Clear();
    FlushPendingSave();
    page_ = page;
    const auto show = [](std::initializer_list<HWND> controls, bool visible)
    {
        for (HWND control : controls)
            if (control) ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
    };
    const auto item = [this](int id) { return GetDlgItem(windowHandle_, id); };
    show({rulesHeadingHandle_, item(IdAddWatchedProcess), ruleCards_.Handle()}, page == Page::Rules);
    show({item(IdRuleBack), ruleTitleHandle_, ruleSubtitleHandle_, item(IdRuleToggleEnabled), item(IdRemoveWatchedProcess),
        startHeadingHandle_, exitHeadingHandle_, startCards_.Handle(), exitCards_.Handle()}, page == Page::RuleDetail);
    const bool embedded = page == Page::RuleSection || page == Page::Displays || page == Page::Settings;
    show({pageTitleHandle_, pageHintHandle_, pageHost_.Handle()}, embedded);
    show({item(IdSectionBack)}, page == Page::RuleSection);
    const int ruleIndex = SelectedWatchedIndex();
    show({item(IdRuleAppSettings)}, page == Page::RuleDetail && ruleIndex >= 0 &&
        IsIRacingRule(app_.Configuration().watchedProcesses[static_cast<size_t>(ruleIndex)]));
    navBar_.SetSelected(page == Page::Displays ? IdMonitorPowerSetups
        : page == Page::Settings ? IdSettings : IdNavRules);
    if (embedded)
    {
        RECT client{};
        GetClientRect(windowHandle_, &client);
        LayoutControls(client.right, client.bottom); // The title moves next to the back button.
        pageHost_.SetContent(CreatePagePane(page));
    }

    if (page == Page::Rules) PopulateRuleCards();
    else if (page == Page::RuleDetail) PopulateRuleDetail();
}

namespace
{
    std::wstring RuleName(const WatchedProcessRule& rule)
    {
        return rule.displayName.empty() ? FileNameWithoutExtension(rule.processName) : rule.displayName;
    }

    std::wstring Plural(size_t count, const wchar_t* singular, const wchar_t* plural)
    {
        return std::to_wstring(count) + L" " + (count == 1 ? singular : plural);
    }

    // "SimHub, CrewChief, Garage61, +2"
    std::wstring JoinNames(const std::vector<std::wstring>& names)
    {
        std::wstring text;
        for (size_t index = 0; index < names.size() && index < 3; ++index)
        {
            if (index != 0) text += L", ";
            text += names[index];
        }
        if (names.size() > 3) text += L", +" + std::to_wstring(names.size() - 3);
        return text;
    }

    std::wstring PowerSchemeName(const std::wstring& guidText)
    {
        for (const auto& scheme : EnumeratePowerSchemes())
        {
            wchar_t guid[40]{};
            StringFromGUID2(scheme.id, guid, static_cast<int>(std::size(guid)));
            if (_wcsicmp(guid, guidText.c_str()) == 0) return scheme.name;
        }
        return L"Unavailable power plan";
    }

    std::wstring ServiceLabel(const std::wstring& name)
    {
        for (const auto& option : IRacingServiceOptions())
            if (_wcsicmp(option.name, name.c_str()) == 0) return option.label;
        return name;
    }

    std::wstring Seconds(int milliseconds)
    {
        const int tenths = (std::max(0, milliseconds) + 50) / 100;
        return tenths % 10 == 0 ? std::to_wstring(tenths / 10) + L" s"
            : std::to_wstring(tenths / 10) + L"." + std::to_wstring(tenths % 10) + L" s";
    }

    size_t ConfiguredPerformanceActions(const WatchedProcessRule& rule)
    {
        return static_cast<size_t>(std::count_if(rule.processPerformanceActions.begin(), rule.processPerformanceActions.end(),
            [](const ProcessPerformanceAction& action) { return !action.ChangesNothing(); }));
    }
}

void MainWindow::PopulateRuleCards()
{
    const auto& rules = app_.Configuration().watchedProcesses;
    const auto states = app_.Monitor().GetProcessStates(rules);
    std::vector<CardList::Item> items;
    items.reserve(rules.size());
    for (size_t index = 0; index < rules.size(); ++index)
    {
        const auto& rule = rules[index];
        CardList::Item item;
        item.title = RuleName(rule);
        item.subtitle = rule.executablePath.empty() ? rule.processName : rule.executablePath;
        const auto& state = index < states.size() ? states[index] : std::wstring(L"Unknown");
        const bool duplicate = std::any_of(rules.begin(), rules.begin() + static_cast<std::ptrdiff_t>(index), [&rule](const auto& earlier)
        {
            return earlier.enabled && _wcsicmp(ProgramFileName(earlier).c_str(), ProgramFileName(rule).c_str()) == 0;
        });
        if (!rule.enabled) { item.pill = L"Disabled"; item.pillTone = CardList::Tone::Warning; }
        else if (duplicate) { item.pill = L"Not watched: duplicate"; item.pillTone = CardList::Tone::Warning; }
        else if (state == L"Running") { item.pill = L"Running"; item.pillTone = CardList::Tone::Active; }
        else { item.pill = state; item.pillTone = CardList::Tone::Neutral; }
        item.muted = !rule.enabled;
        if (!rule.programsToLaunch.empty())
            item.chips.push_back(L"Starts " + Plural(rule.programsToLaunch.size(), L"app", L"apps"));
        if (!rule.processesToStop.empty())
            item.chips.push_back(L"Closes " + Plural(rule.processesToStop.size(), L"app", L"apps"));
        if (!rule.servicesToStop.empty())
            item.chips.push_back(Plural(rule.servicesToStop.size(), L"service", L"services"));
        if (!rule.powerSchemeGuid.empty()) item.chips.push_back(L"Power plan");
        if (const auto performance = ConfiguredPerformanceActions(rule); performance != 0)
            item.chips.push_back(Plural(performance, L"priority setting", L"priority settings"));
        if (!rule.monitorPowerSetupName.empty()) item.chips.push_back(L"Displays");
        if (!rule.homeAssistantActions.empty())
            item.chips.push_back(Plural(rule.homeAssistantActions.size(), L"webhook", L"webhooks"));
        items.push_back(std::move(item));
    }
    ruleCards_.SetItems(std::move(items));
}

void MainWindow::PopulateRuleDetail()
{
    const int index = SelectedWatchedIndex();
    if (index < 0) return;
    const auto& rule = app_.Configuration().watchedProcesses[static_cast<size_t>(index)];
    const auto name = RuleName(rule);
    SetWindowTextW(ruleTitleHandle_, name.c_str());
    const std::wstring subtitle = (rule.enabled ? L"" : L"Disabled  \u00B7  ") +
        (rule.executablePath.empty() ? rule.processName : rule.executablePath);
    SetWindowTextW(ruleSubtitleHandle_, subtitle.c_str());
    SetDlgItemTextW(windowHandle_, IdRuleToggleEnabled, rule.enabled ? L"Disable" : L"Enable");
    SetWindowTextW(startHeadingHandle_, (L"When " + name + L" starts").c_str());
    SetWindowTextW(exitHeadingHandle_, (L"When " + name + L" exits").c_str());

    std::vector<CardList::Item> start;
    startCardSections_.clear();
    const auto add = [](std::vector<CardList::Item>& items, std::vector<RuleSection>& sections, RuleSection section,
        std::wstring title, std::wstring subtitle, size_t count, const wchar_t* unused)
    {
        CardList::Item item;
        item.title = std::move(title);
        item.muted = subtitle.empty();
        item.subtitle = subtitle.empty() ? unused : std::move(subtitle);
        if (count != 0) item.trailing = std::to_wstring(count);
        items.push_back(std::move(item));
        sections.push_back(section);
    };

    // Start apps first, as the main thing a rule does; the rest in the order the
    // monitor runs them.
    std::vector<std::wstring> names;
    int latestStart = 0;
    for (const auto& program : rule.programsToLaunch)
    {
        names.push_back(program.displayName.empty() ? FileNameWithoutExtension(program.filePath) : program.displayName);
        latestStart = std::max(latestStart, program.waitTimeMilliseconds);
    }
    std::wstring started = JoinNames(names);
    if (!started.empty() && latestStart > 0) started += L"  \u00B7  within " + Seconds(latestStart);
    add(start, startCardSections_, RuleSection::StartPrograms, L"Start apps", started, names.size(), L"Not used \u00B7 add tools such as SimHub or CrewChief");

    names.clear();
    for (const auto& action : rule.processesToStop)
        names.push_back(action.displayName.empty() ? action.processName : action.displayName);
    add(start, startCardSections_, RuleSection::StopProcesses, L"Close apps", JoinNames(names), names.size(), L"Not used \u00B7 add apps that should not run in the background");

    names.clear();
    for (const auto& service : rule.servicesToStop) names.push_back(ServiceLabel(service));
    add(start, startCardSections_, RuleSection::WindowsServices, L"Stop Windows services", JoinNames(names), names.size(), L"Not used");

    std::wstring performance;
    if (!rule.powerSchemeGuid.empty()) performance = PowerSchemeName(rule.powerSchemeGuid);
    if (const auto count = ConfiguredPerformanceActions(rule); count != 0)
        performance += (performance.empty() ? L"" : L"  \u00B7  ") + Plural(count, L"priority setting", L"priority settings");
    add(start, startCardSections_, RuleSection::Performance, L"Power plan and priorities", performance, 0, L"Not changed");

    std::wstring display;
    if (!rule.monitorPowerSetupName.empty())
    {
        display = rule.monitorPowerSetupName;
        if (rule.monitorPowerSetupDelayMilliseconds > 0) display += L" after " + Seconds(rule.monitorPowerSetupDelayMilliseconds);
    }
    add(start, startCardSections_, RuleSection::MonitorConfig, L"Display configuration", display, 0, L"Not changed");

    names.clear();
    for (const auto& action : rule.homeAssistantActions) names.push_back(action.displayName);
    add(start, startCardSections_, RuleSection::HomeAssistant, L"Home Assistant webhooks", JoinNames(names), names.size(), L"Not used");

    std::vector<CardList::Item> exit;
    exitCardSections_.clear();
    names.clear();
    for (const auto& program : rule.programsToLaunch)
        if (program.closeWhenGameStops)
            names.push_back(program.displayName.empty() ? FileNameWithoutExtension(program.filePath) : program.displayName);
    add(exit, exitCardSections_, RuleSection::StartPrograms, L"Close started apps", JoinNames(names), names.size(), L"Nothing to close");

    std::vector<std::wstring> restored;
    if (!rule.servicesToStop.empty()) restored.push_back(L"services");
    if (!rule.powerSchemeGuid.empty()) restored.push_back(L"power plan");
    if (!rule.monitorPowerSetupName.empty() && rule.restoreMonitorPowerSetupOnExit) restored.push_back(L"displays");
    std::wstring restoredText;
    for (size_t position = 0; position < restored.size(); ++position)
        restoredText += (position == 0 ? L"" : position + 1 == restored.size() ? L" and " : L", ") + restored[position];
    if (!restoredText.empty()) restoredText[0] = static_cast<wchar_t>(towupper(restoredText[0]));
    add(exit, exitCardSections_, !rule.servicesToStop.empty() ? RuleSection::WindowsServices
        : !rule.powerSchemeGuid.empty() ? RuleSection::Performance : RuleSection::MonitorConfig,
        L"Restore system settings", restoredText, 0, L"Nothing to restore");

    names.clear();
    for (const auto& action : rule.processesToStop)
        if (action.restartAfterWatchProcessEnds)
            names.push_back(action.displayName.empty() ? action.processName : action.displayName);
    add(exit, exitCardSections_, RuleSection::StopProcesses, L"Reopen closed apps", JoinNames(names), names.size(), L"Nothing to reopen");

    startCards_.SetItems(std::move(start));
    exitCards_.SetItems(std::move(exit));
}

namespace
{
    struct SectionText
    {
        const wchar_t* title;
        const wchar_t* hint;
    };

    SectionText DescribeSection(RuleSection section)
    {
        switch (section)
        {
        case RuleSection::StartPrograms: return {L"Start apps", L"Started when %s starts and closed again when it exits, unless set to keep running."};
        case RuleSection::StopProcesses: return {L"Close apps", L"Closed when %s starts: asked to close first, then ended after the configured time."};
        case RuleSection::HomeAssistant: return {L"Home Assistant webhooks", L"Called when %s starts."};
        case RuleSection::MonitorConfig: return {L"Display configuration", L"A saved monitor arrangement used while %s runs."};
        case RuleSection::Performance: return {L"Power plan and priorities", L"Power plan and process priorities while %s runs."};
        case RuleSection::WindowsServices: return {L"Stop Windows services", L"Stopped while %s runs; their original state is restored afterwards."};
        case RuleSection::AppSpecific: return {L"iRacing settings", L"Texture loading, Defender exclusions and a system check for %s."};
        }
        return {L"", L""};
    }
}

void MainWindow::OpenRuleSection(RuleSection section)
{
    if (SelectedWatchedIndex() < 0) return;
    openSection_ = section;
    ShowPage(Page::RuleSection);
}

HWND MainWindow::CreatePagePane(Page page)
{
    auto& config = app_.Configuration();
    const HWND host = pageHost_.Handle();
    PageContext context;
    context.instance = app_.InstanceHandle();
    context.headingFont = headingFont_;
    context.textFont = uiFont_;
    context.configuration = &config;
    context.scheduleSave = [this] { ScheduleSave(); };
    context.saveNow = [this] { SaveConfiguration(); };
    context.update = &update_;
    context.checkForUpdates = [this] { StartUpdateCheck(false); };
    context.installUpdate = [this] { BeginUpdateInstall(); };
    if (page == Page::Settings)
    {
        SetWindowTextW(pageTitleHandle_, L"Settings");
        SetWindowTextW(pageHintHandle_, L"Changes are saved immediately.");
        return CreateSettingsPage(context, host);
    }
    if (page == Page::Displays)
    {
        SetWindowTextW(pageTitleHandle_, L"Displays");
        SetWindowTextW(pageHintHandle_, L"Saved monitor arrangements. Switch with a hotkey or the tray menu, or use one in a rule.");
        return CreateDisplaysPage(context, host, [this](size_t index) { return ApplyMonitorPowerSetup(index, true); });
    }

    const int index = SelectedWatchedIndex();
    if (index < 0) return nullptr;
    auto& rule = config.watchedProcesses[static_cast<size_t>(index)];
    const auto name = RuleName(rule);
    const auto text = DescribeSection(openSection_);
    std::wstring hint = text.hint;
    hint.replace(hint.find(L"%s"), 2, name);
    SetWindowTextW(pageTitleHandle_, text.title);
    SetWindowTextW(pageHintHandle_, hint.c_str());
    SetDlgItemTextW(windowHandle_, IdSectionBack, (L"\u2190  " + name).c_str());
    switch (openSection_)
    {
    case RuleSection::StartPrograms:
    case RuleSection::StopProcesses:
        return CreateRuleAppsPage(context, host, rule, openSection_);
    case RuleSection::WindowsServices:
        return CreateServicesPage(context, host, rule);
    case RuleSection::Performance:
        return CreatePerformancePage(context, host, rule);
    case RuleSection::HomeAssistant:
        return CreateWebhooksPage(context, host, rule);
    case RuleSection::MonitorConfig:
        return CreateRuleDisplayPage(context, host, rule);
    case RuleSection::AppSpecific:
        return CreateIRacingPage(context, host, rule);
    }
    return nullptr;
}

void MainWindow::ScheduleSave()
{
    // Typing in a delay field reports every keystroke; save once the edits pause.
    savePending_ = true;
    SetTimer(windowHandle_, kSaveTimer, 500, nullptr);
}

void MainWindow::FlushPendingSave()
{
    if (savePending_) SaveConfiguration();
}

void MainWindow::OpenRule(int index)
{
    if (index < 0 || index >= static_cast<int>(app_.Configuration().watchedProcesses.size())) return;
    selectedRuleIndex_ = index;
    ShowPage(Page::RuleDetail);
}

void MainWindow::HandleCardCommand(int controlId, int code)
{
    if (controlId == IdRuleCards)
    {
        const int index = ruleCards_.FocusedIndex();
        if (code == CardList::kActivated) OpenRule(index);
        else if (code == CardList::kContextMenu) ShowRuleContextMenu(index);
        else if (code == CardList::kDeleteRequested) RemoveWatchedProcess(index);
        return;
    }
    if (code != CardList::kActivated) return;
    const bool startColumn = controlId == IdStartCards;
    const auto& sections = startColumn ? startCardSections_ : exitCardSections_;
    const int index = (startColumn ? startCards_ : exitCards_).FocusedIndex();
    if (index >= 0 && static_cast<size_t>(index) < sections.size()) OpenRuleSection(sections[static_cast<size_t>(index)]);
}

void MainWindow::ShowRuleContextMenu(int index)
{
    const auto& rules = app_.Configuration().watchedProcesses;
    if (index < 0 || index >= static_cast<int>(rules.size())) return;
    enum : UINT { kOpen = 1, kToggle, kRemove };
    const HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kOpen, L"Open");
    AppendMenuW(menu, MF_STRING, kToggle, rules[static_cast<size_t>(index)].enabled ? L"Disable" : L"Enable");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kRemove, L"Remove...");
    SetMenuDefaultItem(menu, kOpen, FALSE);
    POINT cursor{};
    GetCursorPos(&cursor);
    const UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, windowHandle_, nullptr);
    DestroyMenu(menu);
    if (choice == kOpen) OpenRule(index);
    else if (choice == kToggle) ToggleRuleEnabled(index);
    else if (choice == kRemove) RemoveWatchedProcess(index);
}

void MainWindow::ToggleRuleEnabled(int index)
{
    auto& rules = app_.Configuration().watchedProcesses;
    if (index < 0 || index >= static_cast<int>(rules.size())) return;
    auto& rule = rules[static_cast<size_t>(index)];
    rule.enabled = !rule.enabled;
    SaveConfiguration();
    PopulateRuleCards();
    if (page_ == Page::RuleDetail) PopulateRuleDetail();
    RefreshStatusPanel();
}

void MainWindow::SyncProcessStateTimer(bool visible)
{
    KillTimer(windowHandle_, kProcessStateTimer);
    if (visible && !IsIconic(windowHandle_) && ruleCards_.Handle())
    {
        RefreshProcessStates();
        SetTimer(windowHandle_, kProcessStateTimer, 1000, nullptr);
    }
}

void MainWindow::RefreshProcessStates()
{
    RefreshStatusPanel();
    if (page_ == Page::Rules) PopulateRuleCards();
}

void MainWindow::ToggleMonitoring()
{
    if (monitorStopping_) return;
    if (app_.Monitor().IsRunning())
    {
        monitorStopping_ = true;
        const HWND window = windowHandle_;
        auto& monitor = app_.Monitor();
        monitorStopThread_ = std::thread([&monitor, window]
        {
            monitor.Stop();
            PostMessageW(window, kMonitorStoppedMessage, 0, 0);
        });
    }
    else
    {
        problems_.clear();
        app_.Monitor().UpdateConfiguration(app_.Configuration());
        app_.Monitor().Start();
    }

    SyncMonitoringState();
}

void MainWindow::UnregisterMonitorHotkeys()
{
    for (size_t index = 0; index < registeredHotkeys_; ++index)
    {
        UnregisterHotKey(windowHandle_, static_cast<int>(kMonitorSetupHotkeyBase + index));
    }
    registeredHotkeys_ = 0;
}

void MainWindow::RegisterMonitorHotkeys()
{
    UnregisterMonitorHotkeys();
    registeredHotkeys_ = app_.Configuration().monitorPowerSetups.size();
    for (size_t index = 0; index < app_.Configuration().monitorPowerSetups.size(); ++index)
    {
        const auto& setup = app_.Configuration().monitorPowerSetups[index];
        if (setup.hotkeyVirtualKey == 0)
        {
            continue;
        }

        if (!RegisterHotKey(
                windowHandle_,
                static_cast<int>(kMonitorSetupHotkeyBase + index),
                setup.hotkeyModifiers,
                setup.hotkeyVirtualKey) &&
            app_.LoggingEnabled())
        {
            const auto label = setup.name.empty() ? std::wstring(L"(Unnamed setup)") : setup.name;
            app_.Log(L"[MonitorSetup] Failed to register hotkey for " + label + L".");
        }
    }
}

bool MainWindow::ApplyMonitorPowerSetup(size_t index, bool interactive)
{
    if (index >= app_.Configuration().monitorPowerSetups.size())
    {
        return false;
    }

    std::wstring errorMessage;
    std::function<void(const std::wstring&)> logger;
    if (app_.LoggingEnabled())
    {
        logger = [this](const std::wstring& line)
        {
            app_.Log(L"[MonitorSetup] " + line);
        };
    }

    const auto& setup = app_.Configuration().monitorPowerSetups[index];
    if (!MonitorPowerController::ApplySetup(setup, logger, &errorMessage))
    {
        const auto label = setup.name.empty() ? std::wstring(L"(Unnamed setup)") : setup.name;
        app_.Log(L"Failed to apply monitor config " + label + L": " + errorMessage);
        if (interactive)
        {
            MessageBoxW(windowHandle_, errorMessage.c_str(), L"LaunchMate", MB_OK | MB_ICONWARNING);
        }
        return false;
    }

    const auto label = setup.name.empty() ? std::wstring(L"(Unnamed setup)") : setup.name;
    app_.Log(L"Applied monitor config: " + label);
    return true;
}

void MainWindow::SaveConfiguration()
{
    savePending_ = false;
    KillTimer(windowHandle_, kSaveTimer);
    auto& config = app_.Configuration();
    // Changing administrator mode only affects the task when the task exists.
    // For a manually launched app it is just a saved application preference.
    const bool startupTaskChanged = config.startWithWindows != appliedStartWithWindows_ ||
        ((config.startWithWindows || appliedStartWithWindows_) &&
            config.startAsAdministrator != appliedStartAsAdministrator_);
    if (startupTaskChanged)
    {
        if (!StartupRegistration::Apply(config.startWithWindows, config.startAsAdministrator))
        {
            MessageBoxW(windowHandle_, L"Windows startup settings could not be applied. The previous settings were kept.", L"LaunchMate", MB_OK | MB_ICONERROR);
            config.startWithWindows = appliedStartWithWindows_;
            config.startAsAdministrator = appliedStartAsAdministrator_;
            return;
        }
    }
    appliedStartWithWindows_ = config.startWithWindows;
    appliedStartAsAdministrator_ = config.startAsAdministrator;
    CaptureWindowPlacement();
    if (!app_.Config().Save(config))
    {
        MessageBoxW(windowHandle_, L"The settings could not be saved. The previous configuration file has been preserved.",
            L"LaunchMate", MB_OK | MB_ICONERROR);
        return;
    }
    RegisterMonitorHotkeys();
    app_.Monitor().UpdateConfiguration(config);
}

void MainWindow::CaptureWindowPlacement()
{
    const RECT rect = GetNormalWindowRect(windowHandle_);
    auto& config = app_.Configuration();
    config.windowLeft = rect.left;
    config.windowTop = rect.top;
    config.windowWidth = rect.right - rect.left;
    config.windowHeight = rect.bottom - rect.top;
    config.hasWindowPlacement = true;
    config.startMaximized = IsZoomed(windowHandle_) != FALSE;
}

void MainWindow::RestoreWindowPlacement(int showCommand)
{
    const auto& config = app_.Configuration();
    if (config.hasWindowPlacement)
    {
        const RECT visibleRect = EnsureVisibleRect(RECT{
            config.windowLeft,
            config.windowTop,
            config.windowLeft + config.windowWidth,
            config.windowTop + config.windowHeight});
        SetWindowPos(
            windowHandle_,
            nullptr,
            visibleRect.left,
            visibleRect.top,
            visibleRect.right - visibleRect.left,
            visibleRect.bottom - visibleRect.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }

    const bool restoreMaximized = config.startMaximized && showCommand != SW_HIDE && showCommand != SW_SHOWMINIMIZED &&
        showCommand != SW_MINIMIZE && showCommand != SW_SHOWMINNOACTIVE;
    ShowWindow(windowHandle_, restoreMaximized ? SW_SHOWMAXIMIZED : showCommand);
    UpdateWindow(windowHandle_);
}

void MainWindow::HideToTray()
{
    ShowWindow(windowHandle_, SW_HIDE);
}

void MainWindow::ShowFromTray()
{
    if (IsZoomed(windowHandle_))
    {
        // Moving a maximized window would break its maximized layout.
        ShowWindow(windowHandle_, SW_SHOW);
        SetForegroundWindow(windowHandle_);
        return;
    }
    const RECT visibleRect = EnsureVisibleRect(GetNormalWindowRect(windowHandle_));
    SetWindowPos(
        windowHandle_,
        nullptr,
        visibleRect.left,
        visibleRect.top,
        visibleRect.right - visibleRect.left,
        visibleRect.bottom - visibleRect.top,
        SWP_NOZORDER | SWP_NOACTIVATE);

    ShowWindow(windowHandle_, SW_SHOW);
    ShowWindow(windowHandle_, SW_RESTORE);
    RedrawWindow(windowHandle_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    SetForegroundWindow(windowHandle_);
}

void MainWindow::AddWatchedProcess()
{
    const auto rule = SelectWatchedProcess();
    if (rule.processName.empty()) return;
    auto& rules = app_.Configuration().watchedProcesses;
    // Programs are recognized by file name, so a second rule would never run; open the existing one.
    for (size_t index = 0; index < rules.size(); ++index)
    {
        if (_wcsicmp(ProgramFileName(rules[index]).c_str(), ProgramFileName(rule).c_str()) == 0)
        {
            OpenRule(static_cast<int>(index));
            return;
        }
    }
    rules.push_back(rule);
    SaveConfiguration();
    OpenRule(static_cast<int>(rules.size()) - 1);
}

void MainWindow::RemoveWatchedProcess(int index)
{
    auto& watched = app_.Configuration().watchedProcesses;
    if (index < 0 || index >= static_cast<int>(watched.size())) return;
    const std::wstring question = L"Remove the rule for " + RuleName(watched[static_cast<size_t>(index)]) +
        L"? Its actions are removed too.";
    if (MessageBoxW(windowHandle_, question.c_str(), L"LaunchMate", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;

    watched.erase(watched.begin() + index);
    selectedRuleIndex_ = -1;
    SaveConfiguration();
    ShowPage(Page::Rules);
}

void MainWindow::HandleTrayCommand(UINT command)
{
    if (command >= TrayIcon::kMonitorSetupCommandBase)
    {
        const size_t setupIndex = static_cast<size_t>(command - TrayIcon::kMonitorSetupCommandBase);
        ApplyMonitorPowerSetup(setupIndex, true);
        return;
    }

    switch (command)
    {
    case 1001: ShowFromTray(); break;
    case 1002: ToggleMonitoring(); break;
    case 1003:
        exitRequested_ = true;
        pageHost_.Clear();
        SaveConfiguration();
        DestroyWindow(windowHandle_);
        break;
    }
}

void MainWindow::StartUpdateCheck(bool startup)
{
    if (startup && !app_.Configuration().checkForUpdatesOnStartup) return;
    if (update_.phase == UpdateState::Phase::Checking || update_.phase == UpdateState::Phase::Downloading) return;
    update_.phase = UpdateState::Phase::Checking;
    RefreshPage();
    app_.Log(startup ? L"Checking GitHub releases for LaunchMate updates." : L"Running manual GitHub release check for LaunchMate updates.");

    const HWND windowHandle = windowHandle_;
    std::thread([windowHandle, startup]()
    {
        auto* result = new PostedUpdateCheckResult{};
        try { result->result = UpdateChecker::CheckForUpdate(); }
        catch (...) { result->result = {}; result->result.message = L"The update check failed unexpectedly."; }
        result->startup = startup;
        PostOwnedMessage(windowHandle, MainWindow::kUpdateCheckResultMessage, result);
    }).detach();
}

void MainWindow::BeginUpdateInstall()
{
    if (update_.phase != UpdateState::Phase::Available || update_.release.assetDownloadUrl.empty()) return;
    update_.phase = UpdateState::Phase::Downloading;
    RefreshPage();
    app_.Log(L"Downloading LaunchMate " + update_.release.versionDisplay + L" for self-update.");

    const HWND windowHandle = windowHandle_;
    std::thread([windowHandle, release = update_.release]() mutable
    {
        std::filesystem::path downloadedPath;
        std::wstring errorMessage;
        if (!UpdateChecker::DownloadReleaseAsset(release, downloadedPath, errorMessage))
        {
            PostOwnedMessage(windowHandle, MainWindow::kUpdateErrorMessage, new std::wstring(L"Could not download the update. " + errorMessage));
            return;
        }

        if (!UpdateChecker::LaunchSelfUpdater(downloadedPath, GetCurrentProcessId(), errorMessage))
        {
            PostOwnedMessage(windowHandle, MainWindow::kUpdateErrorMessage, new std::wstring(L"Could not prepare the update. " + errorMessage));
            return;
        }

        PostMessageW(windowHandle, MainWindow::kApplyDownloadedUpdateMessage, 0, 0);
    }).detach();
}

void MainWindow::RefreshPage()
{
    if (page_ != Page::Settings) return;
    if (const HWND page = pageHost_.Content()) SendMessageW(page, WM_TIMER, kPageRefreshTimer, 0);
}

WatchedProcessRule MainWindow::SelectWatchedProcess()
{
    WatchedProcessRule rule;
    rule.executablePath = PickExecutablePath(windowHandle_, L"Select watched process");
    if (!rule.executablePath.empty())
    {
        rule.displayName = FileNameWithoutExtension(rule.executablePath);
        rule.processName = rule.displayName;
    }
    return rule;
}

int MainWindow::SelectedWatchedIndex() const
{
    const auto count = static_cast<int>(app_.Configuration().watchedProcesses.size());
    return selectedRuleIndex_ >= 0 && selectedRuleIndex_ < count ? selectedRuleIndex_ : -1;
}
