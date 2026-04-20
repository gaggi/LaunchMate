#include "MainWindow.h"

#include "CatalogPaths.h"
#include "StartupRegistration.h"
#include "resource.h"
#include "Utils.h"

#include <algorithm>
#include <commctrl.h>
#include <commdlg.h>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>

namespace
{
    constexpr COLORREF kBackground = RGB(16, 20, 24);
    constexpr COLORREF kPanel = RGB(21, 29, 36);
    constexpr COLORREF kText = RGB(236, 241, 246);
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

    HWND CreateEditControl(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h, HFONT font)
    {
        auto handle = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            text,
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
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

    struct MonitorPowerSetupDialogState
    {
        std::vector<MonitorPowerSetup>* setups{nullptr};
        std::vector<MonitorPowerController::DisplayInfo>* displays{nullptr};
        std::function<void()> saveCallback;
        std::function<bool(size_t)> applyCallback;
        int selectedIndex{0};
        bool syncingControls{false};
    };

    void PopulateMonitorPowerSetupList(HWND dialogHandle, MonitorPowerSetupDialogState& state)
    {
        state.syncingControls = true;
        const HWND setupListHandle = GetDlgItem(dialogHandle, IDC_MONITOR_SETUP_LIST);
        SendMessageW(setupListHandle, LB_RESETCONTENT, 0, 0);
        for (const auto& setup : *state.setups)
        {
            const auto label = setup.name.empty() ? std::wstring(L"(Unnamed setup)") : setup.name;
            SendMessageW(setupListHandle, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }

        if (state.setups->empty())
        {
            state.selectedIndex = -1;
            SetDlgItemTextW(dialogHandle, IDC_MONITOR_SETUP_NAME, L"");
            SendMessageW(GetDlgItem(dialogHandle, IDC_MONITOR_SETUP_HOTKEY), HKM_SETHOTKEY, 0, 0);
            SendMessageW(GetDlgItem(dialogHandle, IDC_MONITOR_SETUP_MONITOR_LIST), LB_SETSEL, FALSE, -1);
            state.syncingControls = false;
            return;
        }

        state.selectedIndex = std::clamp(state.selectedIndex, 0, static_cast<int>(state.setups->size()) - 1);
        SendMessageW(setupListHandle, LB_SETCURSEL, static_cast<WPARAM>(state.selectedIndex), 0);
        state.syncingControls = false;
    }

    void PopulateMonitorPowerDisplayList(HWND dialogHandle, MonitorPowerSetupDialogState& state)
    {
        const HWND monitorListHandle = GetDlgItem(dialogHandle, IDC_MONITOR_SETUP_MONITOR_LIST);
        SendMessageW(monitorListHandle, LB_RESETCONTENT, 0, 0);
        for (const auto& display : *state.displays)
        {
            SendMessageW(monitorListHandle, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.displayLabel.c_str()));
        }
    }

    void LoadSelectedMonitorPowerSetup(HWND dialogHandle, MonitorPowerSetupDialogState& state)
    {
        state.syncingControls = true;
        if (state.selectedIndex < 0 || state.selectedIndex >= static_cast<int>(state.setups->size()))
        {
            SetDlgItemTextW(dialogHandle, IDC_MONITOR_SETUP_NAME, L"");
            SendMessageW(GetDlgItem(dialogHandle, IDC_MONITOR_SETUP_HOTKEY), HKM_SETHOTKEY, 0, 0);
            SendMessageW(GetDlgItem(dialogHandle, IDC_MONITOR_SETUP_MONITOR_LIST), LB_SETSEL, FALSE, -1);
            state.syncingControls = false;
            return;
        }

        const auto& setup = (*state.setups)[static_cast<size_t>(state.selectedIndex)];
        SetDlgItemTextW(dialogHandle, IDC_MONITOR_SETUP_NAME, setup.name.c_str());
        BYTE hotkeyModifiers = 0;
        if ((setup.hotkeyModifiers & MOD_CONTROL) != 0) hotkeyModifiers |= HOTKEYF_CONTROL;
        if ((setup.hotkeyModifiers & MOD_ALT) != 0) hotkeyModifiers |= HOTKEYF_ALT;
        if ((setup.hotkeyModifiers & MOD_SHIFT) != 0) hotkeyModifiers |= HOTKEYF_SHIFT;
        if ((setup.hotkeyModifiers & MOD_WIN) != 0) hotkeyModifiers |= HOTKEYF_EXT;
        SendMessageW(
            GetDlgItem(dialogHandle, IDC_MONITOR_SETUP_HOTKEY),
            HKM_SETHOTKEY,
            MAKEWORD(setup.hotkeyVirtualKey, hotkeyModifiers),
            0);

        const HWND monitorListHandle = GetDlgItem(dialogHandle, IDC_MONITOR_SETUP_MONITOR_LIST);
        SendMessageW(monitorListHandle, LB_SETSEL, FALSE, -1);
        for (int index = 0; index < static_cast<int>(state.displays->size()); ++index)
        {
            const auto& display = (*state.displays)[static_cast<size_t>(index)];
            const bool selected = std::find_if(
                setup.enabledDisplays.begin(),
                setup.enabledDisplays.end(),
                [&display](const std::wstring& value)
                {
                    return _wcsicmp(value.c_str(), display.displayName.c_str()) == 0;
                }) != setup.enabledDisplays.end();
            if (selected)
            {
                SendMessageW(monitorListHandle, LB_SETSEL, TRUE, index);
            }
        }
        state.syncingControls = false;
    }

    void StoreSelectedMonitorPowerSetup(HWND dialogHandle, MonitorPowerSetupDialogState& state)
    {
        if (state.syncingControls || state.selectedIndex < 0 || state.selectedIndex >= static_cast<int>(state.setups->size()))
        {
            return;
        }

        auto& setup = (*state.setups)[static_cast<size_t>(state.selectedIndex)];
        wchar_t nameBuffer[256] = {};
        GetDlgItemTextW(dialogHandle, IDC_MONITOR_SETUP_NAME, nameBuffer, static_cast<int>(std::size(nameBuffer)));
        setup.name = nameBuffer;
        const DWORD hotkeyValue = static_cast<DWORD>(SendMessageW(GetDlgItem(dialogHandle, IDC_MONITOR_SETUP_HOTKEY), HKM_GETHOTKEY, 0, 0));
        setup.hotkeyVirtualKey = LOBYTE(hotkeyValue);
        setup.hotkeyModifiers = 0;
        const BYTE hotkeyModifiers = HIBYTE(hotkeyValue);
        if ((hotkeyModifiers & HOTKEYF_CONTROL) != 0) setup.hotkeyModifiers |= MOD_CONTROL;
        if ((hotkeyModifiers & HOTKEYF_ALT) != 0) setup.hotkeyModifiers |= MOD_ALT;
        if ((hotkeyModifiers & HOTKEYF_SHIFT) != 0) setup.hotkeyModifiers |= MOD_SHIFT;
        if ((hotkeyModifiers & HOTKEYF_EXT) != 0) setup.hotkeyModifiers |= MOD_WIN;

        setup.enabledDisplays.clear();
        const HWND monitorListHandle = GetDlgItem(dialogHandle, IDC_MONITOR_SETUP_MONITOR_LIST);
        for (int index = 0; index < static_cast<int>(state.displays->size()); ++index)
        {
            if (SendMessageW(monitorListHandle, LB_GETSEL, index, 0) > 0)
            {
                setup.enabledDisplays.push_back((*state.displays)[static_cast<size_t>(index)].displayName);
            }
        }
    }

    bool ValidateMonitorPowerSetups(HWND dialogHandle, const MonitorPowerSetupDialogState& state)
    {
        std::vector<DWORD> assignedHotkeys;
        for (const auto& setup : *state.setups)
        {
            if (setup.name.empty())
            {
                MessageBoxW(dialogHandle, L"Each monitor config needs a name.", L"LaunchMate", MB_OK | MB_ICONWARNING);
                return false;
            }
            if (setup.enabledDisplays.empty())
            {
                MessageBoxW(dialogHandle, L"Each monitor config must keep at least one monitor powered on.", L"LaunchMate", MB_OK | MB_ICONWARNING);
                return false;
            }
            if (setup.hotkeyVirtualKey != 0)
            {
                const DWORD hotkey = MAKELONG(setup.hotkeyModifiers, setup.hotkeyVirtualKey);
                if (std::find(assignedHotkeys.begin(), assignedHotkeys.end(), hotkey) != assignedHotkeys.end())
                {
                    MessageBoxW(dialogHandle, L"Each monitor config hotkey must be unique.", L"LaunchMate", MB_OK | MB_ICONWARNING);
                    return false;
                }
                assignedHotkeys.push_back(hotkey);
            }
        }
        return true;
    }

    INT_PTR CALLBACK MonitorPowerSetupsDialogProc(HWND dialogHandle, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* state = reinterpret_cast<MonitorPowerSetupDialogState*>(GetWindowLongPtrW(dialogHandle, GWLP_USERDATA));

        switch (message)
        {
        case WM_INITDIALOG:
        {
            state = reinterpret_cast<MonitorPowerSetupDialogState*>(lParam);
            SetWindowLongPtrW(dialogHandle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            if (state == nullptr)
            {
                return FALSE;
            }

            PopulateMonitorPowerDisplayList(dialogHandle, *state);
            PopulateMonitorPowerSetupList(dialogHandle, *state);
            LoadSelectedMonitorPowerSetup(dialogHandle, *state);
            return TRUE;
        }
        case WM_COMMAND:
            if (state == nullptr)
            {
                return FALSE;
            }

            if (state->syncingControls)
            {
                return TRUE;
            }

            switch (LOWORD(wParam))
            {
            case IDC_MONITOR_SETUP_LIST:
                if (HIWORD(wParam) == LBN_SELCHANGE)
                {
                    StoreSelectedMonitorPowerSetup(dialogHandle, *state);
                    state->selectedIndex = static_cast<int>(SendMessageW(GetDlgItem(dialogHandle, IDC_MONITOR_SETUP_LIST), LB_GETCURSEL, 0, 0));
                    LoadSelectedMonitorPowerSetup(dialogHandle, *state);
                }
                return TRUE;
            case IDC_MONITOR_SETUP_NAME:
                if (HIWORD(wParam) == EN_CHANGE)
                {
                    StoreSelectedMonitorPowerSetup(dialogHandle, *state);
                    PopulateMonitorPowerSetupList(dialogHandle, *state);
                }
                return TRUE;
            case IDC_MONITOR_SETUP_MONITOR_LIST:
                if (HIWORD(wParam) == LBN_SELCHANGE)
                {
                    StoreSelectedMonitorPowerSetup(dialogHandle, *state);
                }
                return TRUE;
            case IDC_MONITOR_SETUP_ADD:
            {
                StoreSelectedMonitorPowerSetup(dialogHandle, *state);
                MonitorPowerSetup setup;
                setup.name = L"New setup";
                if (!state->displays->empty())
                {
                    setup.enabledDisplays.push_back(state->displays->front().displayName);
                }
                state->setups->push_back(std::move(setup));
                state->selectedIndex = static_cast<int>(state->setups->size()) - 1;
                PopulateMonitorPowerSetupList(dialogHandle, *state);
                LoadSelectedMonitorPowerSetup(dialogHandle, *state);
                return TRUE;
            }
            case IDC_MONITOR_SETUP_REMOVE:
                if (state->selectedIndex >= 0 && state->selectedIndex < static_cast<int>(state->setups->size()))
                {
                    state->setups->erase(state->setups->begin() + state->selectedIndex);
                    if (state->selectedIndex >= static_cast<int>(state->setups->size()))
                    {
                        state->selectedIndex = static_cast<int>(state->setups->size()) - 1;
                    }
                    PopulateMonitorPowerSetupList(dialogHandle, *state);
                    LoadSelectedMonitorPowerSetup(dialogHandle, *state);
                }
                return TRUE;
            case IDC_MONITOR_SETUP_APPLY:
                StoreSelectedMonitorPowerSetup(dialogHandle, *state);
                if (!ValidateMonitorPowerSetups(dialogHandle, *state))
                {
                    return TRUE;
                }
                if (state->selectedIndex >= 0 && state->applyCallback)
                {
                    state->applyCallback(static_cast<size_t>(state->selectedIndex));
                }
                return TRUE;
            case IDC_MONITOR_SETUP_SAVE:
                StoreSelectedMonitorPowerSetup(dialogHandle, *state);
                if (!ValidateMonitorPowerSetups(dialogHandle, *state))
                {
                    return TRUE;
                }
                if (state->saveCallback)
                {
                    state->saveCallback();
                }
                return TRUE;
            case IDC_MONITOR_SETUP_CLOSE:
            case IDCANCEL:
                EndDialog(dialogHandle, IDC_MONITOR_SETUP_CLOSE);
                return TRUE;
            }
            break;
        }

        return FALSE;
    }

    void ShowMonitorPowerSetupsDialog(
        HINSTANCE instanceHandle,
        HWND owner,
        std::vector<MonitorPowerSetup>& setups,
        std::vector<MonitorPowerController::DisplayInfo>& displays,
        std::function<void()> saveCallback,
        std::function<bool(size_t)> applyCallback)
    {
        MonitorPowerSetupDialogState state;
        state.setups = &setups;
        state.displays = &displays;
        state.saveCallback = std::move(saveCallback);
        state.applyCallback = std::move(applyCallback);
        DialogBoxParamW(
            instanceHandle,
            MAKEINTRESOURCEW(IDD_MONITOR_POWER_SETUPS),
            owner,
            MonitorPowerSetupsDialogProc,
            reinterpret_cast<LPARAM>(&state));
    }

    template <typename T>
    void PostOwnedMessage(HWND windowHandle, UINT message, T* payload)
    {
        if (!PostMessageW(windowHandle, message, 0, reinterpret_cast<LPARAM>(payload)))
        {
            delete payload;
        }
    }

    struct PostedUpdateCheckResult
    {
        UpdateCheckResult result;
        bool interactive{false};
    };

    std::wstring ToLowerCopy(std::wstring text)
    {
        for (auto& character : text)
        {
            character = static_cast<wchar_t>(::towlower(character));
        }
        return text;
    }

    std::wstring ExpandEnvironmentPath(const std::wstring& path)
    {
        if (path.empty())
        {
            return {};
        }

        const DWORD requiredSize = ExpandEnvironmentStringsW(path.c_str(), nullptr, 0);
        if (requiredSize == 0)
        {
            return path;
        }

        std::wstring expanded(requiredSize, L'\0');
        const DWORD copiedSize = ExpandEnvironmentStringsW(path.c_str(), expanded.data(), requiredSize);
        if (copiedSize == 0 || copiedSize > expanded.size())
        {
            return path;
        }

        if (!expanded.empty() && expanded.back() == L'\0')
        {
            expanded.pop_back();
        }

        return expanded;
    }

    bool ContainsInsensitive(const std::wstring& haystack, const std::wstring& needle)
    {
        if (needle.empty())
        {
            return true;
        }

        const auto loweredHaystack = ToLowerCopy(haystack);
        const auto loweredNeedle = ToLowerCopy(needle);
        return loweredHaystack.find(loweredNeedle) != std::wstring::npos;
    }

    bool TryAppendCatalogProgram(
        std::vector<CatalogProgram>& programs,
        std::unordered_set<std::wstring>& seenPaths,
        const std::wstring& displayName,
        const std::wstring& filePath)
    {
        if (filePath.empty())
        {
            return false;
        }

        const auto normalizedPath = ToLowerCopy(filePath);
        if (!seenPaths.insert(normalizedPath).second)
        {
            return false;
        }

        programs.push_back({displayName, filePath});
        return true;
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
    SyncCatalogProgramsFromConfiguration();
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

    RegisterMonitorHotkeys();
    StartUpdateCheck(false);
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
        if (code == EN_CHANGE && controlId == IdCatalogSearch)
        {
            PopulateCatalogPrograms();
            return 0;
        }

        if (code == LBN_SELCHANGE && controlId == IdWatchedList)
        {
            PopulateRulePrograms();
            return 0;
        }

        if (code == LBN_DBLCLK && controlId == IdCatalogList)
        {
            AddSelectedCatalogProgram();
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
        case IdMonitorPowerSetups: ManageMonitorPowerSetups(); return 0;
        case IdSaveConfig: SaveConfiguration(); return 0;
        case IdCheckForUpdates: StartUpdateCheck(true); return 0;
        case IdDetectInstalledApps: DetectInstalledApps(); return 0;
        case IdTransferCatalogProgram: AddSelectedCatalogProgram(); return 0;
        case IdAddCatalogProgram: AddCustomCatalogProgram(); return 0;
        case IdRemoveCatalogProgram: RemoveSelectedCatalogProgram(); return 0;
        case IdAddWatchedProcess: AddWatchedProcess(); return 0;
        case IdRemoveWatchedProcess: RemoveWatchedProcess(); return 0;
        case IdRemoveRuleProgram: RemoveRuleProgram(); return 0;
        default:
            if (controlId >= IdSettingsMinimizeToTray && controlId <= IdSettingsCheckForUpdatesOnStartup)
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
    case WM_HOTKEY:
        if (wParam >= kMonitorSetupHotkeyBase)
        {
            const size_t setupIndex = static_cast<size_t>(wParam - kMonitorSetupHotkeyBase);
            ApplyMonitorPowerSetup(setupIndex, true);
            return 0;
        }
        break;
    case WM_DESTROY:
        UnregisterMonitorHotkeys();
        PostQuitMessage(0);
        return 0;
    default:
        if (message == kUpdateCheckResultMessage)
        {
            std::unique_ptr<PostedUpdateCheckResult> postedResult(reinterpret_cast<PostedUpdateCheckResult*>(lParam));
            updateCheckInProgress_ = false;
            if (!postedResult)
            {
                return 0;
            }

            const auto& result = postedResult->result;
            if (result.state == UpdateCheckState::Failed)
            {
                app_.Log(L"Update check failed: " + result.message);
                if (postedResult->interactive)
                {
                    MessageBoxW(windowHandle_, result.message.c_str(), L"LaunchMate Update", MB_OK | MB_ICONWARNING);
                }
                return 0;
            }

            if (result.state == UpdateCheckState::UpToDate)
            {
                app_.Log(L"Update check complete. LaunchMate is up to date.");
                if (postedResult->interactive)
                {
                    MessageBoxW(windowHandle_, L"LaunchMate is already up to date.", L"LaunchMate Update", MB_OK | MB_ICONINFORMATION);
                }
                return 0;
            }

            app_.Log(L"Update available: " + result.release.versionDisplay);

            std::wstring prompt = L"LaunchMate " + result.release.versionDisplay + L" is available.";
            if (!result.release.assetDownloadUrl.empty())
            {
                prompt += L"\n\nDo you want to download and install it now?";
                if (MessageBoxW(windowHandle_, prompt.c_str(), L"LaunchMate Update", MB_YESNO | MB_ICONINFORMATION) == IDYES)
                {
                    BeginUpdateInstall(result.release);
                }
            }
            else
            {
                prompt += L"\n\nThis release does not include a direct updater package yet. Open the GitHub release page instead?";
                if (MessageBoxW(windowHandle_, prompt.c_str(), L"LaunchMate Update", MB_YESNO | MB_ICONINFORMATION) == IDYES &&
                    !UpdateChecker::OpenReleasePage(result.release.releasePageUrl))
                {
                    MessageBoxW(windowHandle_, L"Could not open the GitHub release page.", L"LaunchMate Update", MB_OK | MB_ICONWARNING);
                }
            }

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
            updateInstallInProgress_ = false;
            if (errorText && !errorText->empty())
            {
                app_.Log(*errorText);
                MessageBoxW(windowHandle_, errorText->c_str(), L"LaunchMate Update", MB_OK | MB_ICONWARNING);
            }
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
    titleFont_ = CreateFontW(24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    uiFont_ = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

void MainWindow::CreateControls()
{
    constexpr int kGlobalListX = 24;
    constexpr int kCatalogSearchY = 106;
    constexpr int kGlobalListWidth = 530;
    constexpr int kCatalogSearchHeight = 28;
    constexpr int kGlobalListY = 144;
    constexpr int kGlobalListHeight = 392;
    constexpr int kWatchedListX = 620;
    constexpr int kWatchedListY = 106;
    constexpr int kWatchedListWidth = 560;
    constexpr int kWatchedListHeight = 200;
    constexpr int kRuleListX = 620;
    constexpr int kRuleListY = 362;
    constexpr int kRuleListWidth = 560;
    constexpr int kRuleListHeight = 174;
    constexpr int kTransferButtonWidth = 34;
    constexpr int kTransferButtonHeight = 38;
    constexpr int kActionButtonWidth = 42;
    constexpr int kActionButtonGap = 6;

    const int globalButtonsRight = kGlobalListX + kGlobalListWidth;
    const int watchedButtonsRight = kWatchedListX + kWatchedListWidth;
    const int ruleButtonsRight = kRuleListX + kRuleListWidth;
    const int transferButtonX = ((kGlobalListX + kGlobalListWidth) + kRuleListX - kTransferButtonWidth) / 2;
    const int transferButtonY = kRuleListY + ((kRuleListHeight - kTransferButtonHeight) / 2);

    CreateLabel(windowHandle_, L"LaunchMate", 22, 16, 220, 32, titleFont_);
    constexpr int kTopButtonGap = 12;
    constexpr int kMonitorSetupButtonWidth = 150;
    toggleButtonHandle_ = CreateButtonControl(windowHandle_, IdToggleMonitoring, L"Start monitoring", watchedButtonsRight - 220, 14, 220, 34, uiFont_);
    CreateButtonControl(
        windowHandle_,
        IdMonitorPowerSetups,
        L"Monitor configs",
        watchedButtonsRight - 220 - kTopButtonGap - kMonitorSetupButtonWidth,
        14,
        kMonitorSetupButtonWidth,
        34,
        uiFont_);

    CreateLabel(windowHandle_, L"Detected apps", 24, 72, 120, 22, uiFont_);
    CreateButtonControl(windowHandle_, IdDetectInstalledApps, L"Detect installed apps", globalButtonsRight - 396, 72, 150, 28, uiFont_);
    CreateButtonControl(windowHandle_, IdAddCatalogProgram, L"+", globalButtonsRight - (kActionButtonWidth * 2) - kActionButtonGap, 72, kActionButtonWidth, 28, uiFont_);
    CreateButtonControl(windowHandle_, IdRemoveCatalogProgram, L"-", globalButtonsRight - kActionButtonWidth, 72, kActionButtonWidth, 28, uiFont_);
    catalogSearchHandle_ = CreateEditControl(windowHandle_, IdCatalogSearch, L"", kGlobalListX, kCatalogSearchY, kGlobalListWidth, kCatalogSearchHeight, uiFont_);
    SendMessageW(catalogSearchHandle_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Search apps"));
    catalogListHandle_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        kGlobalListX, kGlobalListY, kGlobalListWidth, kGlobalListHeight, windowHandle_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdCatalogList)), nullptr, nullptr);
    SendMessageW(catalogListHandle_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);

    CreateLabel(windowHandle_, L"Watched processes", 620, 72, 240, 22, uiFont_);
    CreateButtonControl(windowHandle_, IdAddWatchedProcess, L"+", watchedButtonsRight - (kActionButtonWidth * 2) - kActionButtonGap, 72, kActionButtonWidth, 28, uiFont_);
    CreateButtonControl(windowHandle_, IdRemoveWatchedProcess, L"-", watchedButtonsRight - kActionButtonWidth, 72, kActionButtonWidth, 28, uiFont_);
    watchedListHandle_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        kWatchedListX, kWatchedListY, kWatchedListWidth, kWatchedListHeight, windowHandle_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdWatchedList)), nullptr, nullptr);
    SendMessageW(watchedListHandle_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);

    CreateLabel(windowHandle_, L"Linked programs", 620, 328, 240, 22, uiFont_);
    CreateButtonControl(windowHandle_, IdTransferCatalogProgram, L">", transferButtonX, transferButtonY, kTransferButtonWidth, kTransferButtonHeight, uiFont_);
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
    checkForUpdatesHandle_ = CreateCheckbox(windowHandle_, IdSettingsCheckForUpdatesOnStartup, L"Check for updates on startup", 360, 654, 360, 24, uiFont_);
    CreateButtonControl(windowHandle_, IdCheckForUpdates, L"Check updates", 840, 620, 190, 34, uiFont_);
    CreateButtonControl(windowHandle_, IdSaveConfig, L"Save", 1040, 620, 140, 34, uiFont_);
}

void MainWindow::PopulateLists()
{
    PopulateCatalogPrograms();

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

void MainWindow::SyncCatalogProgramsFromConfiguration()
{
    detectedPrograms_ = app_.Configuration().catalogPrograms;

    std::sort(
        detectedPrograms_.begin(),
        detectedPrograms_.end(),
        [](const CatalogProgram& left, const CatalogProgram& right)
        {
            return _wcsicmp(left.displayName.c_str(), right.displayName.c_str()) < 0;
        });
}

void MainWindow::DetectInstalledApps()
{
    std::vector<CatalogProgram> detectedPrograms;
    std::unordered_set<std::wstring> seenPaths;
    int removedPrograms = 0;
    int addedPrograms = 0;

    for (const auto& program : app_.Configuration().catalogPrograms)
    {
        if (!program.filePath.empty() && std::filesystem::exists(program.filePath))
        {
            TryAppendCatalogProgram(detectedPrograms, seenPaths, program.displayName, program.filePath);
        }
        else
        {
            ++removedPrograms;
        }
    }

    for (const auto& candidate : kCatalogPathCandidates)
    {
        const auto expandedPath = ExpandEnvironmentPath(candidate.path);
        if (expandedPath.empty() || !std::filesystem::exists(expandedPath))
        {
            continue;
        }

        if (TryAppendCatalogProgram(detectedPrograms, seenPaths, candidate.displayName, expandedPath))
        {
            ++addedPrograms;
        }
    }

    app_.Configuration().catalogPrograms = std::move(detectedPrograms);
    SyncCatalogProgramsFromConfiguration();
    PopulateCatalogPrograms();
    SaveConfiguration();

    std::wstring message = L"Detection finished.\n\nFound apps: " + std::to_wstring(app_.Configuration().catalogPrograms.size());
    if (addedPrograms > 0)
    {
        message += L"\nNew apps added: " + std::to_wstring(addedPrograms);
    }
    if (removedPrograms > 0)
    {
        message += L"\nMissing apps removed: " + std::to_wstring(removedPrograms);
    }

    MessageBoxW(windowHandle_, message.c_str(), L"LaunchMate", MB_OK | MB_ICONINFORMATION);
}

void MainWindow::PopulateCatalogPrograms()
{
    filteredDetectedProgramIndexes_.clear();
    SendMessageW(catalogListHandle_, LB_RESETCONTENT, 0, 0);

    wchar_t searchBuffer[256] = {};
    GetWindowTextW(catalogSearchHandle_, searchBuffer, static_cast<int>(std::size(searchBuffer)));
    const std::wstring searchText(searchBuffer);

    for (size_t index = 0; index < detectedPrograms_.size(); ++index)
    {
        const auto& program = detectedPrograms_[index];
        if (!ContainsInsensitive(program.displayName, searchText) && !ContainsInsensitive(program.filePath, searchText))
        {
            continue;
        }

        filteredDetectedProgramIndexes_.push_back(index);
        const std::wstring line = program.displayName + L"  |  " + program.filePath;
        SendMessageW(catalogListHandle_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
    }
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

void MainWindow::ManageMonitorPowerSetups()
{
    availableDisplays_ = MonitorPowerController::EnumerateDisplays();
    if (app_.LoggingEnabled())
    {
        app_.Log(L"[MonitorSetup] Detected displays for setup dialog: " + std::to_wstring(availableDisplays_.size()));
        for (const auto& display : availableDisplays_)
        {
            app_.Log(L"[MonitorSetup] Display option: " + display.displayLabel);
        }
    }
    if (availableDisplays_.empty())
    {
        MessageBoxW(windowHandle_, L"No monitors were detected.", L"LaunchMate", MB_OK | MB_ICONWARNING);
        return;
    }

    auto workingSetups = app_.Configuration().monitorPowerSetups;
    auto workingDisplays = availableDisplays_;
    ShowMonitorPowerSetupsDialog(
        app_.InstanceHandle(),
        windowHandle_,
        workingSetups,
        workingDisplays,
        [this, &workingSetups]()
        {
            app_.Configuration().monitorPowerSetups = workingSetups;
            SaveConfiguration();
        },
        [this, &workingSetups](size_t index)
        {
            if (index >= workingSetups.size())
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

            if (!MonitorPowerController::ApplySetup(workingSetups[index].enabledDisplays, logger, &errorMessage))
            {
                const auto label = workingSetups[index].name.empty() ? std::wstring(L"(Unnamed setup)") : workingSetups[index].name;
                app_.Log(L"Failed to apply monitor config " + label + L": " + errorMessage);
                MessageBoxW(windowHandle_, errorMessage.c_str(), L"LaunchMate", MB_OK | MB_ICONWARNING);
                return false;
            }

            const auto label = workingSetups[index].name.empty() ? std::wstring(L"(Unnamed setup)") : workingSetups[index].name;
            app_.Log(L"Applied monitor config: " + label);
            return true;
        });
}

void MainWindow::UnregisterMonitorHotkeys()
{
    for (size_t index = 0; index < app_.Configuration().monitorPowerSetups.size(); ++index)
    {
        UnregisterHotKey(windowHandle_, static_cast<int>(kMonitorSetupHotkeyBase + index));
    }
}

void MainWindow::RegisterMonitorHotkeys()
{
    UnregisterMonitorHotkeys();
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
    if (!MonitorPowerController::ApplySetup(setup.enabledDisplays, logger, &errorMessage))
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
    UpdateSettingsFromUi();
    CaptureWindowPlacement();
    app_.Config().Save(app_.Configuration());
    RegisterMonitorHotkeys();
    app_.Monitor().UpdateConfiguration(app_.Configuration());
    StartupRegistration::Apply(app_.Configuration().startWithWindows);
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

void MainWindow::AddSelectedCatalogProgram()
{
    const int watchedIndex = SelectedWatchedIndex();
    if (watchedIndex < 0)
    {
        MessageBoxW(windowHandle_, L"Select a watched process first.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const int catalogIndex = SelectedCatalogProgramIndex();
    if (catalogIndex < 0 || catalogIndex >= static_cast<int>(detectedPrograms_.size()))
    {
        MessageBoxW(windowHandle_, L"Select a detected app first.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const auto& detectedProgram = detectedPrograms_[static_cast<size_t>(catalogIndex)];
    auto& programs = app_.Configuration().watchedProcesses[static_cast<size_t>(watchedIndex)].programsToLaunch;
    const auto duplicate = std::find_if(
        programs.begin(),
        programs.end(),
        [&detectedProgram](const LaunchProgram& existingProgram)
        {
            return _wcsicmp(existingProgram.filePath.c_str(), detectedProgram.filePath.c_str()) == 0;
        });
    if (duplicate != programs.end())
    {
        MessageBoxW(windowHandle_, L"This app is already linked to the selected watched process.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
        return;
    }

    LaunchProgram program;
    program.displayName = detectedProgram.displayName;
    program.filePath = detectedProgram.filePath;
    programs.push_back(std::move(program));
    PopulateRulePrograms();
    SaveConfiguration();
}

void MainWindow::RemoveSelectedCatalogProgram()
{
    const int catalogIndex = SelectedCatalogProgramIndex();
    if (catalogIndex < 0 || catalogIndex >= static_cast<int>(detectedPrograms_.size()))
    {
        MessageBoxW(windowHandle_, L"Select a detected app first.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const auto selectedPath = detectedPrograms_[static_cast<size_t>(catalogIndex)].filePath;
    auto& catalogPrograms = app_.Configuration().catalogPrograms;
    catalogPrograms.erase(
        std::remove_if(
            catalogPrograms.begin(),
            catalogPrograms.end(),
            [&selectedPath](const CatalogProgram& program)
            {
                return _wcsicmp(program.filePath.c_str(), selectedPath.c_str()) == 0;
            }),
        catalogPrograms.end());

    SyncCatalogProgramsFromConfiguration();
    PopulateCatalogPrograms();
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

void MainWindow::AddCustomCatalogProgram()
{
    const auto program = SelectLaunchProgram();
    if (program.filePath.empty())
    {
        return;
    }

    const auto duplicate = std::find_if(
        app_.Configuration().catalogPrograms.begin(),
        app_.Configuration().catalogPrograms.end(),
        [&program](const CatalogProgram& existingProgram)
        {
            return _wcsicmp(existingProgram.filePath.c_str(), program.filePath.c_str()) == 0;
        });
    if (duplicate != app_.Configuration().catalogPrograms.end())
    {
        SyncCatalogProgramsFromConfiguration();
        PopulateCatalogPrograms();
        MessageBoxW(windowHandle_, L"This app is already in the detected apps list.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
        return;
    }

    app_.Configuration().catalogPrograms.push_back({program.displayName, program.filePath});
    SyncCatalogProgramsFromConfiguration();
    PopulateCatalogPrograms();
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
        SaveConfiguration();
        DestroyWindow(windowHandle_);
        break;
    }
}

void MainWindow::StartUpdateCheck(bool interactive)
{
    if (!interactive && !app_.Configuration().checkForUpdatesOnStartup)
    {
        return;
    }

    if (updateCheckInProgress_)
    {
        if (interactive)
        {
            MessageBoxW(windowHandle_, L"An update check is already running.", L"LaunchMate Update", MB_OK | MB_ICONINFORMATION);
        }
        return;
    }

    updateCheckInProgress_ = true;
    app_.Log(interactive
        ? L"Running manual GitHub release check for LaunchMate updates."
        : L"Checking GitHub releases for LaunchMate updates.");

    const HWND windowHandle = windowHandle_;
    std::thread([windowHandle, interactive]()
    {
        auto* result = new PostedUpdateCheckResult{};
        result->result = UpdateChecker::CheckForUpdate();
        result->interactive = interactive;
        PostOwnedMessage(windowHandle, MainWindow::kUpdateCheckResultMessage, result);
    }).detach();
}

void MainWindow::BeginUpdateInstall(UpdateReleaseInfo release)
{
    if (updateInstallInProgress_)
    {
        return;
    }

    updateInstallInProgress_ = true;
    app_.Log(L"Downloading LaunchMate " + release.versionDisplay + L" for self-update.");

    const HWND windowHandle = windowHandle_;
    std::thread([windowHandle, release = std::move(release)]() mutable
    {
        std::filesystem::path downloadedPath;
        std::wstring errorMessage;
        if (!UpdateChecker::DownloadReleaseAsset(release, downloadedPath, errorMessage))
        {
            PostOwnedMessage(windowHandle, MainWindow::kUpdateErrorMessage, new std::wstring(L"Failed to download the LaunchMate update.\n\n" + errorMessage));
            return;
        }

        if (!UpdateChecker::LaunchSelfUpdater(downloadedPath, GetCurrentProcessId(), errorMessage))
        {
            PostOwnedMessage(windowHandle, MainWindow::kUpdateErrorMessage, new std::wstring(L"Failed to prepare the LaunchMate update.\n\n" + errorMessage));
            return;
        }

        PostMessageW(windowHandle, MainWindow::kApplyDownloadedUpdateMessage, 0, 0);
    }).detach();
}

void MainWindow::UpdateSettingsFromUi()
{
    auto& config = app_.Configuration();
    config.minimizeToTray = SendMessageW(minimizeToTrayHandle_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    config.closeToTray = SendMessageW(closeToTrayHandle_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    config.startWithWindows = SendMessageW(startWithWindowsHandle_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    config.startInTray = SendMessageW(startInTrayHandle_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    config.startMonitoringOnLaunch = SendMessageW(startMonitoringHandle_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    config.checkForUpdatesOnStartup = SendMessageW(checkForUpdatesHandle_, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void MainWindow::UpdateSettingsUi()
{
    const auto& config = app_.Configuration();
    SendMessageW(minimizeToTrayHandle_, BM_SETCHECK, config.minimizeToTray ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(closeToTrayHandle_, BM_SETCHECK, config.closeToTray ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(startWithWindowsHandle_, BM_SETCHECK, config.startWithWindows ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(startInTrayHandle_, BM_SETCHECK, config.startInTray ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(startMonitoringHandle_, BM_SETCHECK, config.startMonitoringOnLaunch ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(checkForUpdatesHandle_, BM_SETCHECK, config.checkForUpdatesOnStartup ? BST_CHECKED : BST_UNCHECKED, 0);
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

int MainWindow::SelectedCatalogProgramIndex() const
{
    const int listIndex = static_cast<int>(SendMessageW(catalogListHandle_, LB_GETCURSEL, 0, 0));
    if (listIndex < 0 || listIndex >= static_cast<int>(filteredDetectedProgramIndexes_.size()))
    {
        return -1;
    }

    return static_cast<int>(filteredDetectedProgramIndexes_[static_cast<size_t>(listIndex)]);
}

int MainWindow::SelectedWatchedIndex() const
{
    return static_cast<int>(SendMessageW(watchedListHandle_, LB_GETCURSEL, 0, 0));
}
