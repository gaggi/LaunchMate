#include "MainWindow.h"

#include "StartupRegistration.h"
#include "resource.h"
#include "Utils.h"

#include <algorithm>
#include <commdlg.h>
#include <string>

namespace
{
    constexpr COLORREF kBackground = RGB(16, 20, 24);
    constexpr COLORREF kPanel = RGB(21, 29, 36);
    constexpr COLORREF kText = RGB(236, 241, 246);

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

    bool ShouldRestoreMaximized(HWND windowHandle)
    {
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (!GetWindowPlacement(windowHandle, &placement))
        {
            return false;
        }

        return placement.showCmd == SW_SHOWMAXIMIZED || (placement.flags & WPF_RESTORETOMAXIMIZED) != 0;
    }

    HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, HFONT font)
    {
        auto handle = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent, nullptr, nullptr, nullptr);
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return handle;
    }

    HWND CreateButtonControl(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h, HFONT font, DWORD extraStyle = 0)
    {
        auto handle = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | extraStyle,
            x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return handle;
    }

    HWND CreateCheckbox(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h, HFONT font)
    {
        auto handle = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
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

    struct ProgramOptionsDialogState
    {
        LaunchProgram* program{nullptr};
        bool accepted{false};
    };

    INT_PTR CALLBACK ProgramOptionsDialogProc(HWND dialogHandle, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* state = reinterpret_cast<ProgramOptionsDialogState*>(GetWindowLongPtrW(dialogHandle, GWLP_USERDATA));

        switch (message)
        {
        case WM_INITDIALOG:
        {
            state = reinterpret_cast<ProgramOptionsDialogState*>(lParam);
            SetWindowLongPtrW(dialogHandle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            if (state && state->program)
            {
                SetDlgItemTextW(dialogHandle, IDC_PROGRAM_ARGS, state->program->arguments.c_str());
                SetDlgItemInt(dialogHandle, IDC_PROGRAM_DELAY, static_cast<UINT>(state->program->waitTimeMilliseconds), FALSE);
            }
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case IDC_PROGRAM_OK:
                if (state && state->program)
                {
                    wchar_t argsBuffer[1024] = {};
                    GetDlgItemTextW(dialogHandle, IDC_PROGRAM_ARGS, argsBuffer, static_cast<int>(std::size(argsBuffer)));

                    BOOL translated = FALSE;
                    const UINT delayValue = GetDlgItemInt(dialogHandle, IDC_PROGRAM_DELAY, &translated, FALSE);
                    state->program->arguments = argsBuffer;
                    state->program->waitTimeMilliseconds = translated ? static_cast<int>(delayValue) : 0;
                    state->accepted = true;
                }
                EndDialog(dialogHandle, IDOK);
                return TRUE;
            case IDC_PROGRAM_CANCEL:
            case IDCANCEL:
                EndDialog(dialogHandle, IDCANCEL);
                return TRUE;
            }
            break;
        }

        return FALSE;
    }

    bool ShowProgramOptionsDialog(HINSTANCE instanceHandle, HWND owner, LaunchProgram& program)
    {
        ProgramOptionsDialogState state;
        state.program = &program;
        DialogBoxParamW(instanceHandle, MAKEINTRESOURCEW(IDD_PROGRAM_OPTIONS), owner, ProgramOptionsDialogProc, reinterpret_cast<LPARAM>(&state));
        return state.accepted;
    }
}

MainWindow::MainWindow(App& app)
    : app_(app)
{
}

MainWindow::~MainWindow()
{
    if (titleFont_) DeleteObject(titleFont_);
    if (uiFont_) DeleteObject(uiFont_);
    if (backgroundBrush_) DeleteObject(backgroundBrush_);
    if (panelBrush_) DeleteObject(panelBrush_);
}

bool MainWindow::Create(int showCommand)
{
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
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = MainWindow::kWindowClassName;
    RegisterClassExW(&windowClass);

    CreateFonts();
    backgroundBrush_ = CreateSolidBrush(kBackground);
    panelBrush_ = CreateSolidBrush(kPanel);

    const auto& config = app_.Configuration();
    windowHandle_ = CreateWindowExW(
        0,
        windowClass.lpszClassName,
        L"LaunchMate",
        WS_OVERLAPPEDWINDOW,
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
    PopulateLists();
    UpdateSettingsUi();
    RestoreWindowPlacement(app_.Configuration().startInTray ? SW_HIDE : showCommand);
    StartupRegistration::Apply(app_.Configuration().startWithWindows);

    trayIcon_.Create(
        windowHandle_,
        kTrayCallbackMessage,
        appSmallIcon ? appSmallIcon : windowClass.hIcon,
        L"LaunchMate",
        [this](UINT command)
    {
        HandleTrayCommand(command);
    });

    return true;
}

void MainWindow::SetStatus(const std::wstring& text)
{
    (void)text;
}

void MainWindow::SyncMonitoringState()
{
    SetWindowTextW(toggleButtonHandle_, app_.Monitor().IsRunning() ? L"Stop monitoring" : L"Start monitoring");
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
        if (code == LBN_SELCHANGE && controlId == IdWatchedList)
        {
            PopulateRulePrograms();
            return 0;
        }

        if (code == LBN_DBLCLK && controlId == IdGlobalList)
        {
            EditGlobalProgram();
            return 0;
        }

        if (code == LBN_DBLCLK && controlId == IdRuleProgramsList)
        {
            EditRuleProgram();
            return 0;
        }

        switch (controlId)
        {
        case IdToggleMonitoring: ToggleMonitoring(); return 0;
        case IdSaveConfig: SaveConfiguration(); return 0;
        case IdAddGlobalProgram: AddGlobalProgram(); return 0;
        case IdRemoveGlobalProgram: RemoveGlobalProgram(); return 0;
        case IdAddWatchedProcess: AddWatchedProcess(); return 0;
        case IdRemoveWatchedProcess: RemoveWatchedProcess(); return 0;
        case IdAddRuleProgram: AddRuleProgram(); return 0;
        case IdRemoveRuleProgram: RemoveRuleProgram(); return 0;
        default:
            if (controlId >= IdSettingsMinimizeToTray && controlId <= IdSettingsStartMonitoringOnLaunch)
            {
                UpdateSettingsFromUi();
                return 0;
            }
            break;
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kText);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(backgroundBrush_);
    }
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kText);
        SetBkColor(dc, kPanel);
        return reinterpret_cast<LRESULT>(panelBrush_);
    }
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(windowHandle_, &paint);
        RECT client{};
        GetClientRect(windowHandle_, &client);
        FillRect(dc, &client, backgroundBrush_);
        EndPaint(windowHandle_, &paint);
        return 0;
    }
    case WM_CLOSE:
        if (!exitRequested_ && app_.Configuration().closeToTray)
        {
            SaveConfiguration();
            HideToTray();
            return 0;
        }
        app_.Monitor().Stop();
        trayIcon_.Destroy();
        DestroyWindow(windowHandle_);
        return 0;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED && app_.Configuration().minimizeToTray)
        {
            HideToTray();
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
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
                trayIcon_.ShowContextMenu(app_.Monitor().IsRunning());
            }
            return 0;
        }
        break;
    }

    return DefWindowProcW(windowHandle_, message, wParam, lParam);
}

void MainWindow::CreateFonts()
{
    titleFont_ = CreateFontW(24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    uiFont_ = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

void MainWindow::CreateControls()
{
    constexpr int kGlobalListX = 24;
    constexpr int kGlobalListY = 106;
    constexpr int kGlobalListWidth = 530;
    constexpr int kGlobalListHeight = 430;
    constexpr int kWatchedListX = 620;
    constexpr int kWatchedListY = 106;
    constexpr int kWatchedListWidth = 560;
    constexpr int kWatchedListHeight = 200;
    constexpr int kRuleListX = 620;
    constexpr int kRuleListY = 362;
    constexpr int kRuleListWidth = 560;
    constexpr int kRuleListHeight = 174;
    constexpr int kActionButtonWidth = 42;
    constexpr int kActionButtonGap = 6;

    const int globalButtonsRight = kGlobalListX + kGlobalListWidth;
    const int watchedButtonsRight = kWatchedListX + kWatchedListWidth;
    const int ruleButtonsRight = kRuleListX + kRuleListWidth;

    CreateLabel(windowHandle_, L"LaunchMate", 22, 16, 220, 32, titleFont_);
    toggleButtonHandle_ = CreateButtonControl(windowHandle_, IdToggleMonitoring, L"Start monitoring", watchedButtonsRight - 220, 14, 220, 34, uiFont_);

    CreateLabel(windowHandle_, L"Global programs", 24, 72, 340, 22, uiFont_);
    CreateButtonControl(windowHandle_, IdAddGlobalProgram, L"+", globalButtonsRight - (kActionButtonWidth * 2) - kActionButtonGap, 72, kActionButtonWidth, 28, uiFont_);
    CreateButtonControl(windowHandle_, IdRemoveGlobalProgram, L"-", globalButtonsRight - kActionButtonWidth, 72, kActionButtonWidth, 28, uiFont_);
    globalListHandle_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        kGlobalListX, kGlobalListY, kGlobalListWidth, kGlobalListHeight, windowHandle_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdGlobalList)), nullptr, nullptr);
    SendMessageW(globalListHandle_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);

    CreateLabel(windowHandle_, L"Watched processes", 620, 72, 240, 22, uiFont_);
    CreateButtonControl(windowHandle_, IdAddWatchedProcess, L"+", watchedButtonsRight - (kActionButtonWidth * 2) - kActionButtonGap, 72, kActionButtonWidth, 28, uiFont_);
    CreateButtonControl(windowHandle_, IdRemoveWatchedProcess, L"-", watchedButtonsRight - kActionButtonWidth, 72, kActionButtonWidth, 28, uiFont_);
    watchedListHandle_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        kWatchedListX, kWatchedListY, kWatchedListWidth, kWatchedListHeight, windowHandle_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdWatchedList)), nullptr, nullptr);
    SendMessageW(watchedListHandle_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);

    CreateLabel(windowHandle_, L"Linked programs", 620, 328, 240, 22, uiFont_);
    CreateButtonControl(windowHandle_, IdAddRuleProgram, L"+", ruleButtonsRight - (kActionButtonWidth * 2) - kActionButtonGap, 328, kActionButtonWidth, 28, uiFont_);
    CreateButtonControl(windowHandle_, IdRemoveRuleProgram, L"-", ruleButtonsRight - kActionButtonWidth, 328, kActionButtonWidth, 28, uiFont_);
    ruleProgramsListHandle_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        kRuleListX, kRuleListY, kRuleListWidth, kRuleListHeight, windowHandle_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdRuleProgramsList)), nullptr, nullptr);
    SendMessageW(ruleProgramsListHandle_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);

    CreateLabel(windowHandle_, L"Settings", 24, 560, 180, 22, uiFont_);
    minimizeToTrayHandle_ = CreateCheckbox(windowHandle_, IdSettingsMinimizeToTray, L"Minimize to tray", 24, 594, 320, 24, uiFont_);
    closeToTrayHandle_ = CreateCheckbox(windowHandle_, IdSettingsCloseToTray, L"Close to tray", 24, 624, 320, 24, uiFont_);
    startInTrayHandle_ = CreateCheckbox(windowHandle_, IdSettingsStartInTray, L"Start in tray", 24, 654, 320, 24, uiFont_);
    startWithWindowsHandle_ = CreateCheckbox(windowHandle_, IdSettingsStartWithWindows, L"Start with Windows", 360, 594, 320, 24, uiFont_);
    startMonitoringHandle_ = CreateCheckbox(windowHandle_, IdSettingsStartMonitoringOnLaunch, L"Start monitoring on launch", 360, 624, 320, 24, uiFont_);
    CreateButtonControl(windowHandle_, IdSaveConfig, L"Save", 1040, 620, 140, 34, uiFont_);
}

void MainWindow::PopulateLists()
{
    SendMessageW(globalListHandle_, LB_RESETCONTENT, 0, 0);
    for (const auto& program : app_.Configuration().globalPrograms)
    {
        std::wstring line = program.displayName.empty() ? FileNameWithoutExtension(program.filePath) : program.displayName;
        if (!program.arguments.empty())
        {
            line += L"  |  Arguments: " + program.arguments;
        }
        line += L"  |  Delay: " + std::to_wstring(program.waitTimeMilliseconds) + L" ms";
        SendMessageW(globalListHandle_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
    }

    SendMessageW(watchedListHandle_, LB_RESETCONTENT, 0, 0);
    for (const auto& rule : app_.Configuration().watchedProcesses)
    {
        std::wstring line = rule.displayName;
        if (!rule.processName.empty() && rule.processName != rule.displayName)
        {
            line += L"  |  " + rule.processName;
        }
        SendMessageW(watchedListHandle_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
    }

    PopulateRulePrograms();
}

void MainWindow::PopulateRulePrograms()
{
    SendMessageW(ruleProgramsListHandle_, LB_RESETCONTENT, 0, 0);
    const int index = SelectedWatchedIndex();
    if (index < 0 || index >= static_cast<int>(app_.Configuration().watchedProcesses.size()))
    {
        return;
    }

    const auto& rule = app_.Configuration().watchedProcesses[static_cast<size_t>(index)];
    for (const auto& program : rule.programsToLaunch)
    {
        std::wstring line = program.displayName.empty() ? FileNameWithoutExtension(program.filePath) : program.displayName;
        if (!program.arguments.empty())
        {
            line += L"  |  Arguments: " + program.arguments;
        }
        line += L"  |  Delay: " + std::to_wstring(program.waitTimeMilliseconds) + L" ms";
        SendMessageW(ruleProgramsListHandle_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
    }
}

void MainWindow::ToggleMonitoring()
{
    if (app_.Monitor().IsRunning())
    {
        app_.Monitor().Stop();
    }
    else
    {
        UpdateSettingsFromUi();
        app_.Monitor().UpdateConfiguration(app_.Configuration());
        app_.Monitor().Start();
    }

    SyncMonitoringState();
}

void MainWindow::SaveConfiguration()
{
    UpdateSettingsFromUi();
    CaptureWindowPlacement();
    app_.Config().Save(app_.Configuration());
    app_.Monitor().UpdateConfiguration(app_.Configuration());
    StartupRegistration::Apply(app_.Configuration().startWithWindows);
}

void MainWindow::LoadConfiguration()
{
    if (app_.Monitor().IsRunning())
    {
        MessageBoxW(windowHandle_, L"Please stop monitoring before reloading the configuration.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
        return;
    }

    app_.Configuration() = app_.Config().Load();
    app_.Monitor().UpdateConfiguration(app_.Configuration());
    UpdateSettingsUi();
    PopulateLists();
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
    config.startMaximized = ShouldRestoreMaximized(windowHandle_);
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

    ShowWindow(windowHandle_, config.startMaximized ? SW_MAXIMIZE : showCommand);
    UpdateWindow(windowHandle_);
}

void MainWindow::HideToTray()
{
    ShowWindow(windowHandle_, SW_HIDE);
}

void MainWindow::ShowFromTray()
{
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
    ShowWindow(windowHandle_, ShouldRestoreMaximized(windowHandle_) ? SW_MAXIMIZE : SW_RESTORE);
    SetForegroundWindow(windowHandle_);
}

void MainWindow::AddGlobalProgram()
{
    const auto program = SelectLaunchProgram();
    if (program.filePath.empty()) return;
    app_.Configuration().globalPrograms.push_back(program);
    PopulateLists();
    SaveConfiguration();
}

void MainWindow::AddWatchedProcess()
{
    const auto rule = SelectWatchedProcess();
    if (rule.processName.empty()) return;
    app_.Configuration().watchedProcesses.push_back(rule);
    PopulateLists();
    SaveConfiguration();
}

void MainWindow::AddRuleProgram()
{
    const int watchedIndex = SelectedWatchedIndex();
    if (watchedIndex < 0) return;

    const auto program = SelectLaunchProgram();
    if (program.filePath.empty()) return;

    app_.Configuration().watchedProcesses[static_cast<size_t>(watchedIndex)].programsToLaunch.push_back(program);
    PopulateRulePrograms();
    SaveConfiguration();
}

void MainWindow::EditGlobalProgram()
{
    const int index = static_cast<int>(SendMessageW(globalListHandle_, LB_GETCURSEL, 0, 0));
    if (index < 0)
    {
        return;
    }

    auto& program = app_.Configuration().globalPrograms[static_cast<size_t>(index)];
    if (!ShowProgramOptionsDialog(app_.InstanceHandle(), windowHandle_, program))
    {
        return;
    }

    PopulateLists();
    SendMessageW(globalListHandle_, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
    SaveConfiguration();
}

void MainWindow::EditRuleProgram()
{
    const int watchedIndex = SelectedWatchedIndex();
    const int programIndex = static_cast<int>(SendMessageW(ruleProgramsListHandle_, LB_GETCURSEL, 0, 0));
    if (watchedIndex < 0 || programIndex < 0)
    {
        return;
    }

    auto& program = app_.Configuration().watchedProcesses[static_cast<size_t>(watchedIndex)].programsToLaunch[static_cast<size_t>(programIndex)];
    if (!ShowProgramOptionsDialog(app_.InstanceHandle(), windowHandle_, program))
    {
        return;
    }

    PopulateRulePrograms();
    SendMessageW(ruleProgramsListHandle_, LB_SETCURSEL, static_cast<WPARAM>(programIndex), 0);
    SaveConfiguration();
}

void MainWindow::RemoveGlobalProgram()
{
    const int index = static_cast<int>(SendMessageW(globalListHandle_, LB_GETCURSEL, 0, 0));
    if (index < 0) return;

    auto& programs = app_.Configuration().globalPrograms;
    programs.erase(programs.begin() + index);
    PopulateLists();
    SaveConfiguration();
}

void MainWindow::RemoveWatchedProcess()
{
    const int index = SelectedWatchedIndex();
    if (index < 0) return;

    auto& watched = app_.Configuration().watchedProcesses;
    watched.erase(watched.begin() + index);
    PopulateLists();
    SaveConfiguration();
}

void MainWindow::RemoveRuleProgram()
{
    const int watchedIndex = SelectedWatchedIndex();
    const int programIndex = static_cast<int>(SendMessageW(ruleProgramsListHandle_, LB_GETCURSEL, 0, 0));
    if (watchedIndex < 0 || programIndex < 0) return;

    auto& programs = app_.Configuration().watchedProcesses[static_cast<size_t>(watchedIndex)].programsToLaunch;
    programs.erase(programs.begin() + programIndex);
    PopulateRulePrograms();
    SaveConfiguration();
}

void MainWindow::HandleTrayCommand(UINT command)
{
    switch (command)
    {
    case 1001: ShowFromTray(); break;
    case 1002: ToggleMonitoring(); break;
    case 1003:
        exitRequested_ = true;
        SaveConfiguration();
        DestroyWindow(windowHandle_);
        break;
    }
}

void MainWindow::UpdateSettingsFromUi()
{
    auto& config = app_.Configuration();
    config.minimizeToTray = SendMessageW(minimizeToTrayHandle_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    config.closeToTray = SendMessageW(closeToTrayHandle_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    config.startWithWindows = SendMessageW(startWithWindowsHandle_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    config.startInTray = SendMessageW(startInTrayHandle_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    config.startMonitoringOnLaunch = SendMessageW(startMonitoringHandle_, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void MainWindow::UpdateSettingsUi()
{
    const auto& config = app_.Configuration();
    SendMessageW(minimizeToTrayHandle_, BM_SETCHECK, config.minimizeToTray ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(closeToTrayHandle_, BM_SETCHECK, config.closeToTray ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(startWithWindowsHandle_, BM_SETCHECK, config.startWithWindows ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(startInTrayHandle_, BM_SETCHECK, config.startInTray ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(startMonitoringHandle_, BM_SETCHECK, config.startMonitoringOnLaunch ? BST_CHECKED : BST_UNCHECKED, 0);
}

LaunchProgram MainWindow::SelectLaunchProgram()
{
    LaunchProgram program;
    program.filePath = PickExecutablePath(windowHandle_, L"Select program");
    if (!program.filePath.empty())
    {
        program.displayName = FileNameWithoutExtension(program.filePath);
    }
    return program;
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
    return static_cast<int>(SendMessageW(watchedListHandle_, LB_GETCURSEL, 0, 0));
}
