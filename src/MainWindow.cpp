#include "MainWindow.h"

#include "CatalogPaths.h"
#include "ListViewHelpers.h"
#include "RuleActionsDialog.h"
#include "StartupRegistration.h"
#include "TabHost.h"
#include "resource.h"
#include "Utils.h"

#include <algorithm>
#include <TlHelp32.h>
#include <commctrl.h>
#include <commdlg.h>
#include <psapi.h>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>

namespace
{
    constexpr UINT kMonitorSetupHotkeyBase = 5000;
    constexpr int kUpdateDialogInstall = 6101;
    constexpr int kUpdateDialogOpenGitHub = 6102;
    constexpr int kUpdateDialogLater = 6103;

    bool IsProtectedProcessName(const std::wstring& processName)
    {
        constexpr const wchar_t* protectedNames[] = {
            L"System", L"Registry", L"smss.exe", L"csrss.exe", L"wininit.exe",
            L"services.exe", L"lsass.exe", L"winlogon.exe", L"fontdrvhost.exe",
            L"svchost.exe", L"dwm.exe", L"Secure System", L"Memory Compression"
        };
        return std::any_of(std::begin(protectedNames), std::end(protectedNames), [&processName](const wchar_t* name)
        {
            return _wcsicmp(processName.c_str(), name) == 0;
        });
    }

    std::wstring QueryProcessPath(HANDLE process)
    {
        std::wstring path(1024, L'\0');
        DWORD length = static_cast<DWORD>(path.size());
        if (!QueryFullProcessImageNameW(process, 0, path.data(), &length)) length = 0;
        path.resize(length);
        return path;
    }

    unsigned long long FileTimeValue(const FILETIME& value)
    {
        ULARGE_INTEGER result{};
        result.LowPart = value.dwLowDateTime;
        result.HighPart = value.dwHighDateTime;
        return result.QuadPart;
    }

    std::wstring FormatCpuUsage(double cpuUsagePercent, bool available)
    {
        if (!available) return L"-";
        wchar_t value[32]{};
        swprintf_s(value, L"%.1f %%", cpuUsagePercent);
        return value;
    }

    std::wstring FormatMemoryUsage(unsigned long long memoryUsageBytes, bool available)
    {
        if (!available) return L"-";
        constexpr double bytesPerMegabyte = 1024.0 * 1024.0;
        wchar_t value[32]{};
        swprintf_s(value, L"%.1f MB", static_cast<double>(memoryUsageBytes) / bytesPerMegabyte);
        return value;
    }

    HRESULT CALLBACK UpdateDialogCallback(HWND, UINT notification, WPARAM, LPARAM lParam, LONG_PTR)
    {
        if (notification == TDN_HYPERLINK_CLICKED && lParam != 0)
        {
            UpdateChecker::OpenReleasePage(reinterpret_cast<const wchar_t*>(lParam));
        }
        return S_OK;
    }

    int ShowUpdateDetailsDialog(HWND owner, const UpdateCheckResult& result)
    {
        const std::wstring currentVersion = UpdateChecker::CurrentVersion();
        const std::wstring githubVersion = result.release.versionDisplay.empty() ? L"Unavailable" : result.release.versionDisplay;
        const bool updateAvailable = result.state == UpdateCheckState::UpdateAvailable;
        const std::wstring instruction = updateAvailable
            ? L"A newer LaunchMate version is available"
            : L"LaunchMate is up to date";

        std::wstring content = L"Current version:  " + currentVersion +
            L"\nGitHub version:  " + githubVersion;
        if (!result.release.releasePageUrl.empty())
        {
            content += L"\n\n<a href=\"" + result.release.releasePageUrl + L"\">Open this release on GitHub</a>";
        }

        TASKDIALOGCONFIG config{};
        config.cbSize = sizeof(config);
        config.hwndParent = owner;
        config.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        config.pszWindowTitle = L"LaunchMate Update";
        config.pszMainIcon = TD_INFORMATION_ICON;
        config.pszMainInstruction = instruction.c_str();
        config.pszContent = content.c_str();
        config.pfCallback = UpdateDialogCallback;

        TASKDIALOG_BUTTON buttons[2]{};
        if (updateAvailable)
        {
            if (!result.release.assetDownloadUrl.empty())
            {
                buttons[0] = {kUpdateDialogInstall, L"Download and install update"};
            }
            else
            {
                buttons[0] = {kUpdateDialogOpenGitHub, L"Open release on GitHub"};
            }
            buttons[1] = {kUpdateDialogLater, L"Later"};
            config.pButtons = buttons;
            config.cButtons = 2;
            config.nDefaultButton = buttons[0].nButtonID;
        }
        else
        {
            config.dwCommonButtons = TDCBF_OK_BUTTON;
        }

        int selectedButton = IDCANCEL;
        if (FAILED(TaskDialogIndirect(&config, &selectedButton, nullptr, nullptr)))
        {
            const UINT flags = updateAvailable ? MB_YESNO | MB_ICONINFORMATION : MB_OK | MB_ICONINFORMATION;
            selectedButton = MessageBoxW(owner, content.c_str(), L"LaunchMate Update", flags) == IDYES
                ? (result.release.assetDownloadUrl.empty() ? kUpdateDialogOpenGitHub : kUpdateDialogInstall)
                : IDCANCEL;
        }
        return selectedButton;
    }

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
                SetDlgItemInt(dialogHandle, IDC_PROGRAM_CLOSE_DELAY, static_cast<UINT>(state->program->closeDelayMilliseconds), FALSE);
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
                    BOOL closeDelayTranslated = FALSE;
                    const UINT closeDelayValue = GetDlgItemInt(dialogHandle, IDC_PROGRAM_CLOSE_DELAY, &closeDelayTranslated, FALSE);
                    state->program->arguments = argsBuffer;
                    state->program->waitTimeMilliseconds = translated ? static_cast<int>(delayValue) : 0;
                    state->program->closeDelayMilliseconds = closeDelayTranslated ? static_cast<int>(closeDelayValue) : 0;
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
        static constexpr int kEnabledControlBase = 3000;
        static constexpr int kPrimaryControlBase = 4000;

        std::vector<MonitorPowerSetup>* setups{nullptr};
        std::vector<MonitorPowerSetup::DisplayPath>* detectedDisplays{nullptr};
        std::function<void()> saveCallback;
        std::function<bool(size_t)> applyCallback;
        std::vector<HWND> rowControls;
        std::vector<HWND> enabledChecks;
        std::vector<HWND> primaryRadios;
        int selectedIndex{0};
        bool syncingControls{false};
    };

    bool IsSameDisplay(
        const MonitorPowerSetup::DisplayPath& left,
        const MonitorPowerSetup::DisplayPath& right)
    {
        return left.targetAdapterLowPart == right.targetAdapterLowPart &&
            left.targetAdapterHighPart == right.targetAdapterHighPart &&
            left.targetId == right.targetId;
    }

    void MergeDetectedDisplaysIntoSetup(
        MonitorPowerSetup& setup,
        const std::vector<MonitorPowerSetup::DisplayPath>& detectedDisplays)
    {
        if (detectedDisplays.empty()) return;

        std::vector<MonitorPowerSetup::DisplayPath> merged;
        merged.reserve(std::max(setup.displayPaths.size(), detectedDisplays.size()));
        for (const auto& detected : detectedDisplays)
        {
            const auto existing = std::find_if(
                setup.displayPaths.begin(),
                setup.displayPaths.end(),
                [&detected](const auto& path) { return IsSameDisplay(path, detected); });
            merged.push_back(existing == setup.displayPaths.end() ? detected : *existing);
        }
        for (const auto& existing : setup.displayPaths)
        {
            const auto present = std::any_of(
                merged.begin(),
                merged.end(),
                [&existing](const auto& path) { return IsSameDisplay(path, existing); });
            if (!present) merged.push_back(existing);
        }
        setup.displayPaths = std::move(merged);
    }

    RECT DialogUnitsToPixels(HWND dialogHandle, LONG x, LONG y, LONG width, LONG height)
    {
        RECT rectangle{x, y, x + width, y + height};
        MapDialogRect(dialogHandle, &rectangle);
        return rectangle;
    }

    HWND CreateMonitorSetupRowControl(
        HWND dialogHandle,
        MonitorPowerSetupDialogState& state,
        const wchar_t* className,
        const std::wstring& text,
        DWORD style,
        LONG x,
        LONG y,
        LONG width,
        LONG height,
        int controlId = 0)
    {
        const RECT rectangle = DialogUnitsToPixels(dialogHandle, x, y, width, height);
        HWND control = CreateWindowExW(
            0,
            className,
            text.c_str(),
            WS_CHILD | WS_VISIBLE | style,
            rectangle.left,
            rectangle.top,
            rectangle.right - rectangle.left,
            rectangle.bottom - rectangle.top,
            dialogHandle,
            controlId == 0 ? nullptr : reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
            reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dialogHandle, GWLP_HINSTANCE)),
            nullptr);
        SendMessageW(control, WM_SETFONT, SendMessageW(dialogHandle, WM_GETFONT, 0, 0), TRUE);
        state.rowControls.push_back(control);
        return control;
    }

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
            state.syncingControls = false;
            return;
        }

        state.selectedIndex = std::clamp(state.selectedIndex, 0, static_cast<int>(state.setups->size()) - 1);
        SendMessageW(setupListHandle, LB_SETCURSEL, static_cast<WPARAM>(state.selectedIndex), 0);
        state.syncingControls = false;
    }

    void DestroyMonitorSetupRows(MonitorPowerSetupDialogState& state)
    {
        for (HWND control : state.rowControls)
        {
            DestroyWindow(control);
        }
        state.rowControls.clear();
        state.enabledChecks.clear();
        state.primaryRadios.clear();
    }

    void PopulateMonitorSetupRows(HWND dialogHandle, MonitorPowerSetupDialogState& state, const MonitorPowerSetup& setup)
    {
        DestroyMonitorSetupRows(state);
        CreateMonitorSetupRowControl(dialogHandle, state, L"STATIC", L"Monitor", 0, 150, 52, 52, 10);
        CreateMonitorSetupRowControl(dialogHandle, state, L"STATIC", L"Name", 0, 207, 52, 180, 10);
        CreateMonitorSetupRowControl(dialogHandle, state, L"STATIC", L"Enabled", 0, 395, 52, 45, 10);
        CreateMonitorSetupRowControl(dialogHandle, state, L"STATIC", L"Primary", 0, 455, 52, 45, 10);

        if (setup.displayPaths.empty())
        {
            CreateMonitorSetupRowControl(
                dialogHandle,
                state,
                L"STATIC",
                L"No monitors detected yet. Activate the desired monitors in Windows and click Detect Current.",
                0,
                150,
                70,
                350,
                12);
            return;
        }

        for (size_t index = 0; index < setup.displayPaths.size(); ++index)
        {
            const auto& path = setup.displayPaths[index];
            const LONG y = 70 + static_cast<LONG>(index) * 24;
            CreateMonitorSetupRowControl(
                dialogHandle,
                state,
                L"STATIC",
                L"Monitor " + std::to_wstring(index + 1),
                0,
                150,
                y + 2,
                52,
                12);
            CreateMonitorSetupRowControl(
                dialogHandle,
                state,
                L"STATIC",
                path.monitorName.empty() ? path.displayName : path.monitorName,
                0,
                207,
                y + 2,
                180,
                12);
            HWND enabled = CreateMonitorSetupRowControl(
                dialogHandle,
                state,
                L"BUTTON",
                L"",
                BS_AUTOCHECKBOX,
                405,
                y,
                14,
                14,
                MonitorPowerSetupDialogState::kEnabledControlBase + static_cast<int>(index));
            HWND primary = CreateMonitorSetupRowControl(
                dialogHandle,
                state,
                L"BUTTON",
                L"",
                BS_RADIOBUTTON,
                466,
                y,
                14,
                14,
                MonitorPowerSetupDialogState::kPrimaryControlBase + static_cast<int>(index));
            SendMessageW(enabled, BM_SETCHECK, path.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(primary, BM_SETCHECK, path.isPrimary ? BST_CHECKED : BST_UNCHECKED, 0);
            state.enabledChecks.push_back(enabled);
            state.primaryRadios.push_back(primary);
        }
    }

    void LoadSelectedMonitorPowerSetup(HWND dialogHandle, MonitorPowerSetupDialogState& state)
    {
        state.syncingControls = true;
        if (state.selectedIndex < 0 || state.selectedIndex >= static_cast<int>(state.setups->size()))
        {
            SetDlgItemTextW(dialogHandle, IDC_MONITOR_SETUP_NAME, L"");
            SendMessageW(GetDlgItem(dialogHandle, IDC_MONITOR_SETUP_HOTKEY), HKM_SETHOTKEY, 0, 0);
            DestroyMonitorSetupRows(state);
            state.syncingControls = false;
            return;
        }

        auto& setup = (*state.setups)[static_cast<size_t>(state.selectedIndex)];
        if (state.detectedDisplays != nullptr)
        {
            MergeDetectedDisplaysIntoSetup(setup, *state.detectedDisplays);
        }
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

        PopulateMonitorSetupRows(dialogHandle, state, setup);
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

        for (size_t index = 0; index < setup.displayPaths.size() && index < state.enabledChecks.size(); ++index)
        {
            setup.displayPaths[index].enabled = SendMessageW(state.enabledChecks[index], BM_GETCHECK, 0, 0) == BST_CHECKED;
            setup.displayPaths[index].isPrimary = SendMessageW(state.primaryRadios[index], BM_GETCHECK, 0, 0) == BST_CHECKED;
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
            if (setup.displayPaths.empty())
            {
                MessageBoxW(dialogHandle, L"Capture the current Windows display state for each monitor config before saving.", L"LaunchMate", MB_OK | MB_ICONWARNING);
                return false;
            }
            const auto enabledCount = std::count_if(setup.displayPaths.begin(), setup.displayPaths.end(), [](const auto& display)
            {
                return display.enabled;
            });
            const auto primaryCount = std::count_if(setup.displayPaths.begin(), setup.displayPaths.end(), [](const auto& display)
            {
                return display.enabled && display.isPrimary;
            });
            if (enabledCount == 0 || primaryCount != 1)
            {
                MessageBoxW(dialogHandle, L"Each monitor config must enable at least one monitor and select exactly one enabled monitor as Primary.", L"LaunchMate", MB_OK | MB_ICONWARNING);
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

            if (LOWORD(wParam) >= MonitorPowerSetupDialogState::kEnabledControlBase &&
                LOWORD(wParam) < MonitorPowerSetupDialogState::kEnabledControlBase + static_cast<int>(state->enabledChecks.size()))
            {
                const size_t index = static_cast<size_t>(LOWORD(wParam) - MonitorPowerSetupDialogState::kEnabledControlBase);
                if (SendMessageW(state->enabledChecks[index], BM_GETCHECK, 0, 0) != BST_CHECKED &&
                    SendMessageW(state->primaryRadios[index], BM_GETCHECK, 0, 0) == BST_CHECKED)
                {
                    SendMessageW(state->primaryRadios[index], BM_SETCHECK, BST_UNCHECKED, 0);
                }
                StoreSelectedMonitorPowerSetup(dialogHandle, *state);
                return TRUE;
            }

            if (LOWORD(wParam) >= MonitorPowerSetupDialogState::kPrimaryControlBase &&
                LOWORD(wParam) < MonitorPowerSetupDialogState::kPrimaryControlBase + static_cast<int>(state->primaryRadios.size()))
            {
                const size_t selected = static_cast<size_t>(LOWORD(wParam) - MonitorPowerSetupDialogState::kPrimaryControlBase);
                SendMessageW(state->enabledChecks[selected], BM_SETCHECK, BST_CHECKED, 0);
                for (size_t index = 0; index < state->primaryRadios.size(); ++index)
                {
                    SendMessageW(state->primaryRadios[index], BM_SETCHECK, index == selected ? BST_CHECKED : BST_UNCHECKED, 0);
                }
                StoreSelectedMonitorPowerSetup(dialogHandle, *state);
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
            case IDC_MONITOR_SETUP_ADD:
            {
                StoreSelectedMonitorPowerSetup(dialogHandle, *state);
                MonitorPowerSetup setup;
                setup.name = L"New setup";
                if (state->detectedDisplays != nullptr && !state->detectedDisplays->empty())
                {
                    setup.displayPaths = *state->detectedDisplays;
                }
                else if (state->selectedIndex >= 0 && state->selectedIndex < static_cast<int>(state->setups->size()))
                {
                    setup.displayPaths = (*state->setups)[static_cast<size_t>(state->selectedIndex)].displayPaths;
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
            case IDC_MONITOR_SETUP_CAPTURE:
            {
                StoreSelectedMonitorPowerSetup(dialogHandle, *state);
                std::wstring errorMessage;
                MonitorPowerSetup detectedSetup;
                if (!MonitorPowerController::CaptureSetup(detectedSetup, &errorMessage))
                {
                    MessageBoxW(dialogHandle, errorMessage.c_str(), L"LaunchMate", MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
                if (state->detectedDisplays != nullptr)
                {
                    *state->detectedDisplays = detectedSetup.displayPaths;
                    for (auto& setup : *state->setups)
                    {
                        MergeDetectedDisplaysIntoSetup(setup, *state->detectedDisplays);
                    }
                }
                LoadSelectedMonitorPowerSetup(dialogHandle, *state);
                return TRUE;
            }
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
        std::vector<MonitorPowerSetup::DisplayPath>& detectedDisplays,
        std::function<void()> saveCallback,
        std::function<bool(size_t)> applyCallback)
    {
        MonitorPowerSetupDialogState state;
        state.setups = &setups;
        state.detectedDisplays = &detectedDisplays;
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
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = MainWindow::kWindowClassName;
    RegisterClassExW(&windowClass);

    CreateFonts();

    const auto& config = app_.Configuration();
    windowHandle_ = CreateWindowExW(
        0,
        windowClass.lpszClassName,
        (L"LaunchMate " + UpdateChecker::CurrentVersion()).c_str(),
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
        (L"LaunchMate " + UpdateChecker::CurrentVersion()).c_str(),
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

        switch (controlId)
        {
        case IdToggleMonitoring: ToggleMonitoring(); return 0;
        case IdMonitorPowerSetups: ManageMonitorPowerSetups(); return 0;
        case IdSaveConfig: SaveConfiguration(); return 0;
        case IdCheckForUpdates: StartUpdateCheck(true); return 0;
        case IdDetectInstalledApps:
            if (sourceTabIndex_ == 0) DetectInstalledApps(); else { CaptureRunningProcesses(); PopulateRunningProcesses(); }
            return 0;
        case IdTransferCatalogProgram: TransferSelectedSource(); return 0;
        case IdAddCatalogProgram: AddCustomCatalogProgram(); return 0;
        case IdRemoveCatalogProgram: RemoveSelectedCatalogProgram(); return 0;
        case IdAddWatchedProcess: AddWatchedProcess(); return 0;
        case IdRemoveWatchedProcess: RemoveWatchedProcess(); return 0;
        case IdRemoveRuleAction: RemoveSelectedRuleAction(); return 0;
        case IdEditRuleActions: EditRuleActions(); return 0;
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
    case WM_NOTIFY:
    {
        const auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header && header->idFrom == IdSourceTabs && header->code == TCN_SELCHANGE)
        {
            SwitchSourceTab();
            return 0;
        }
        if (header && header->code == LVN_COLUMNCLICK &&
            (header->idFrom == IdCatalogList || header->idFrom == IdWatchedList || header->idFrom == IdRuleProgramsList))
        {
            const auto* column = reinterpret_cast<NMLISTVIEW*>(lParam);
            SortListViewByColumn(header->hwndFrom, column->iSubItem);
            return 0;
        }
        if (header && header->idFrom == IdCatalogList && header->code == NM_DBLCLK)
        {
            TransferSelectedSource();
            return 0;
        }
        if (header && header->idFrom == IdRuleProgramsList && header->code == NM_DBLCLK)
        {
            EditRuleActions();
            return 0;
        }
        if (header && header->idFrom == IdWatchedList && header->code == LVN_ITEMCHANGED)
        {
            const auto* change = reinterpret_cast<NMLISTVIEW*>(lParam);
            if ((change->uNewState & LVIS_SELECTED) != 0 && (change->uOldState & LVIS_SELECTED) == 0)
            {
                PopulateRulePrograms();
                if (sourceTabIndex_ == 1) PopulateRunningProcesses();
            }
            return 0;
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
                    const std::wstring details = L"Current version:  " + UpdateChecker::CurrentVersion() +
                        L"\nGitHub version:  Unavailable\n\nUpdate check failed:\n" + result.message;
                    MessageBoxW(windowHandle_, details.c_str(), L"LaunchMate Update", MB_OK | MB_ICONWARNING);
                }
                return 0;
            }

            if (result.state == UpdateCheckState::UpToDate)
            {
                app_.Log(L"Update check complete. LaunchMate is up to date.");
                if (postedResult->interactive)
                {
                    ShowUpdateDetailsDialog(windowHandle_, result);
                }
                return 0;
            }

            app_.Log(L"Update available: " + result.release.versionDisplay);

            const int selectedButton = ShowUpdateDetailsDialog(windowHandle_, result);
            if (selectedButton == kUpdateDialogInstall)
            {
                BeginUpdateInstall(result.release);
            }
            else if (selectedButton == kUpdateDialogOpenGitHub &&
                !UpdateChecker::OpenReleasePage(result.release.releasePageUrl))
            {
                MessageBoxW(windowHandle_, L"Could not open the GitHub release page.", L"LaunchMate Update", MB_OK | MB_ICONWARNING);
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
    constexpr int kGlobalListX = 34;
    constexpr int kCatalogSearchY = 86;
    constexpr int kGlobalListWidth = 510;
    constexpr int kCatalogSearchHeight = 28;
    constexpr int kGlobalListY = 120;
    constexpr int kGlobalListHeight = 416;
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
    constexpr int kTransferButtonGap = 8;
    constexpr int kActionButtonWidth = 42;
    constexpr int kActionButtonGap = 6;

    const int globalButtonsRight = kGlobalListX + kGlobalListWidth;
    const int watchedButtonsRight = kWatchedListX + kWatchedListWidth;
    const int ruleButtonsRight = kRuleListX + kRuleListWidth;
    const int transferButtonX = ((kGlobalListX + kGlobalListWidth) + kRuleListX - kTransferButtonWidth) / 2;
    const int transferButtonsHeight = (kTransferButtonHeight * 2) + kTransferButtonGap;
    const int transferButtonY = kRuleListY + ((kRuleListHeight - transferButtonsHeight) / 2);

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

    sourceTabsHandle_ = CreateWindowExW(0, WC_TABCONTROLW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_FIXEDWIDTH,
        24, 14, 530, 536, windowHandle_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdSourceTabs)), nullptr, nullptr);
    SendMessageW(sourceTabsHandle_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
    TabCtrl_SetItemSize(sourceTabsHandle_, 180, 26);
    TCITEMW sourceTab{TCIF_TEXT};
    sourceTab.pszText = const_cast<wchar_t*>(L"Detected apps");
    TabCtrl_InsertItem(sourceTabsHandle_, 0, &sourceTab);
    sourceTab.pszText = const_cast<wchar_t*>(L"Running processes");
    TabCtrl_InsertItem(sourceTabsHandle_, 1, &sourceTab);
    detectSourceButtonHandle_ = CreateButtonControl(windowHandle_, IdDetectInstalledApps, L"Detect installed apps", kGlobalListX, 52, 150, 28, uiFont_);
    addCatalogButtonHandle_ = CreateButtonControl(windowHandle_, IdAddCatalogProgram, L"+", globalButtonsRight - (kActionButtonWidth * 2) - kActionButtonGap, 52, kActionButtonWidth, 28, uiFont_);
    removeCatalogButtonHandle_ = CreateButtonControl(windowHandle_, IdRemoveCatalogProgram, L"-", globalButtonsRight - kActionButtonWidth, 52, kActionButtonWidth, 28, uiFont_);
    catalogSearchHandle_ = CreateEditControl(windowHandle_, IdCatalogSearch, L"", kGlobalListX, kCatalogSearchY, kGlobalListWidth, kCatalogSearchHeight, uiFont_);
    SendMessageW(catalogSearchHandle_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Search apps"));
    catalogListHandle_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        kGlobalListX, kGlobalListY, kGlobalListWidth, kGlobalListHeight, windowHandle_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdCatalogList)), nullptr, nullptr);
    SendMessageW(catalogListHandle_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
    InitializeReportListView(catalogListHandle_);
    ConfigureListView(catalogListHandle_, {{L"Name", 2}, {L"Path", 5}});
    HostControlsInTab(windowHandle_, sourceTabsHandle_, {
        detectSourceButtonHandle_,
        addCatalogButtonHandle_,
        removeCatalogButtonHandle_,
        catalogSearchHandle_,
        catalogListHandle_});

    CreateLabel(windowHandle_, L"Watched processes", 620, 72, 240, 22, uiFont_);
    CreateButtonControl(windowHandle_, IdAddWatchedProcess, L"+", watchedButtonsRight - (kActionButtonWidth * 2) - kActionButtonGap, 72, kActionButtonWidth, 28, uiFont_);
    CreateButtonControl(windowHandle_, IdRemoveWatchedProcess, L"-", watchedButtonsRight - kActionButtonWidth, 72, kActionButtonWidth, 28, uiFont_);
    watchedListHandle_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        kWatchedListX, kWatchedListY, kWatchedListWidth, kWatchedListHeight, windowHandle_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdWatchedList)), nullptr, nullptr);
    SendMessageW(watchedListHandle_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
    InitializeReportListView(watchedListHandle_);
    ConfigureListView(watchedListHandle_, {{L"Name", 2}, {L"Path", 4}});

    CreateLabel(windowHandle_, L"Actions", 620, 328, 240, 22, uiFont_);
    CreateButtonControl(windowHandle_, IdTransferCatalogProgram, L">", transferButtonX, transferButtonY, kTransferButtonWidth, kTransferButtonHeight, uiFont_);
    CreateButtonControl(windowHandle_, IdRemoveRuleAction, L"<", transferButtonX,
        transferButtonY + kTransferButtonHeight + kTransferButtonGap,
        kTransferButtonWidth, kTransferButtonHeight, uiFont_);
    CreateButtonControl(windowHandle_, IdEditRuleActions, L"Edit actions...", ruleButtonsRight - 130, 328, 130, 28, uiFont_);
    ruleProgramsListHandle_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        kRuleListX, kRuleListY, kRuleListWidth, kRuleListHeight, windowHandle_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdRuleProgramsList)), nullptr, nullptr);
    SendMessageW(ruleProgramsListHandle_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
    InitializeReportListView(ruleProgramsListHandle_);
    ConfigureListView(ruleProgramsListHandle_, {{L"Type", 2}, {L"Name", 3}, {L"Details", 6}});

    CreateLabel(windowHandle_, L"Settings", 24, 560, 180, 22, uiFont_);
    minimizeToTrayHandle_ = CreateCheckbox(windowHandle_, IdSettingsMinimizeToTray, L"Minimize to tray", 24, 594, 320, 24, uiFont_);
    closeToTrayHandle_ = CreateCheckbox(windowHandle_, IdSettingsCloseToTray, L"Close to tray", 24, 624, 320, 24, uiFont_);
    startInTrayHandle_ = CreateCheckbox(windowHandle_, IdSettingsStartInTray, L"Start in tray", 24, 654, 320, 24, uiFont_);
    startWithWindowsHandle_ = CreateCheckbox(windowHandle_, IdSettingsStartWithWindows, L"Start with Windows", 360, 594, 320, 24, uiFont_);
    startMonitoringHandle_ = CreateCheckbox(windowHandle_, IdSettingsStartMonitoringOnLaunch, L"Start monitoring on launch", 360, 624, 320, 24, uiFont_);
    checkForUpdatesHandle_ = CreateCheckbox(windowHandle_, IdSettingsCheckForUpdatesOnStartup, L"Check for updates on startup", 360, 654, 360, 24, uiFont_);
    CreateButtonControl(windowHandle_, IdCheckForUpdates, L"Check for updates", 840, 620, 190, 34, uiFont_);
    CreateButtonControl(windowHandle_, IdSaveConfig, L"Save", 1040, 620, 140, 34, uiFont_);
}

void MainWindow::PopulateLists()
{
    PopulateCatalogPrograms();

    ListView_DeleteAllItems(watchedListHandle_);
    for (size_t index = 0; index < app_.Configuration().watchedProcesses.size(); ++index)
    {
        const auto& rule = app_.Configuration().watchedProcesses[index];
        AddListViewRow(watchedListHandle_, {
            rule.displayName,
            rule.executablePath.empty() ? L"Path unavailable" : rule.executablePath},
            static_cast<LPARAM>(index));
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
    if (sourceTabIndex_ == 1)
    {
        PopulateRunningProcesses();
        return;
    }

    ListView_DeleteAllItems(catalogListHandle_);

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

        AddListViewRow(catalogListHandle_, {program.displayName, program.filePath}, static_cast<LPARAM>(index));
    }
}

void MainWindow::PopulateRulePrograms()
{
    ListView_DeleteAllItems(ruleProgramsListHandle_);
    const int index = SelectedWatchedIndex();
    if (index < 0 || index >= static_cast<int>(app_.Configuration().watchedProcesses.size()))
    {
        return;
    }

    const auto& rule = app_.Configuration().watchedProcesses[static_cast<size_t>(index)];
    for (const auto& program : rule.programsToLaunch)
    {
        std::wstring details;
        if (!program.arguments.empty())
        {
            details = L"Arguments: " + program.arguments + L"; ";
        }
        details += L"Start delay: " + std::to_wstring(program.waitTimeMilliseconds) +
            L" ms; Stop delay: " + std::to_wstring(program.closeDelayMilliseconds) + L" ms";
        AddListViewRow(ruleProgramsListHandle_, {
            L"Start",
            program.displayName.empty() ? FileNameWithoutExtension(program.filePath) : program.displayName,
            details});
    }
    for (const auto& action : rule.processesToStop)
    {
        std::wstring details = action.processName;
        if (action.restartAfterWatchProcessEnds)
        {
            details += L"; Restart after exit: " + std::to_wstring(action.restartDelayMilliseconds) + L" ms";
        }
        AddListViewRow(ruleProgramsListHandle_, {
            L"Stop",
            action.displayName.empty() ? action.processName : action.displayName,
            details});
    }
    for (const auto& action : rule.homeAssistantActions)
    {
        AddListViewRow(ruleProgramsListHandle_, {
            L"Home Assistant",
            action.displayName,
            L"Delay: " + std::to_wstring(action.waitTimeMilliseconds) + L" ms"});
    }
    if (!rule.monitorPowerSetupName.empty())
    {
        std::wstring details = L"Apply delay: " + std::to_wstring(rule.monitorPowerSetupDelayMilliseconds) + L" ms";
        if (rule.restoreMonitorPowerSetupOnExit)
        {
            details += L"; Restore after exit: " +
                std::to_wstring(rule.restoreMonitorPowerSetupDelayMilliseconds) + L" ms";
        }
        AddListViewRow(ruleProgramsListHandle_, {L"Monitor config", rule.monitorPowerSetupName, details});
    }
}

void MainWindow::CaptureRunningProcesses()
{
    runningProcesses_.clear();
    std::unordered_set<std::wstring> seenProcesses;
    seenProcesses.reserve(128);
    if (runningProcesses_.capacity() < 128) runningProcesses_.reserve(128);

    struct CpuSample
    {
        HANDLE process{};
        size_t processIndex{};
        unsigned long long initialTime{};
        LARGE_INTEGER sampleStart{};
    };
    std::vector<CpuSample> cpuSamples;
    cpuSamples.reserve(128);

    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);

    std::wstring watchedProcessName;
    const int watchedIndex = SelectedWatchedIndex();
    if (watchedIndex >= 0 && watchedIndex < static_cast<int>(app_.Configuration().watchedProcesses.size()))
    {
        watchedProcessName = app_.Configuration().watchedProcesses[static_cast<size_t>(watchedIndex)].processName;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            const std::wstring processName = entry.szExeFile;
            if (entry.th32ProcessID == 0 || entry.th32ProcessID == GetCurrentProcessId() ||
                IsProtectedProcessName(processName) ||
                (!watchedProcessName.empty() && _wcsicmp(
                    std::filesystem::path(processName).stem().c_str(),
                    std::filesystem::path(watchedProcessName).stem().c_str()) == 0))
            {
                continue;
            }

            HANDLE process = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                FALSE,
                entry.th32ProcessID);
            if (!process)
            {
                process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            }

            const std::wstring path = process ? QueryProcessPath(process) : std::wstring{};
            std::wstring key = processName + L"|" + path;
            std::transform(key.begin(), key.end(), key.begin(), [](wchar_t character)
            {
                return static_cast<wchar_t>(std::towlower(character));
            });
            if (!seenProcesses.insert(key).second)
            {
                if (process) CloseHandle(process);
                continue;
            }

            RunningProcessEntry item;
            item.displayName = std::filesystem::path(processName).stem().wstring();
            item.processName = processName;
            item.executablePath = path;
            item.processId = entry.th32ProcessID;

            if (process)
            {
                PROCESS_MEMORY_COUNTERS memory{};
                if (K32GetProcessMemoryInfo(process, &memory, sizeof(memory)))
                {
                    item.memoryUsageBytes = memory.WorkingSetSize;
                    item.hasMemoryUsage = true;
                }
            }

            const size_t processIndex = runningProcesses_.size();
            runningProcesses_.push_back(std::move(item));

            if (process)
            {
                FILETIME creation{};
                FILETIME exit{};
                FILETIME kernel{};
                FILETIME user{};
                if (GetProcessTimes(process, &creation, &exit, &kernel, &user))
                {
                    LARGE_INTEGER sampleStart{};
                    QueryPerformanceCounter(&sampleStart);
                    cpuSamples.push_back({
                        process,
                        processIndex,
                        FileTimeValue(kernel) + FileTimeValue(user),
                        sampleStart});
                }
                else
                {
                    CloseHandle(process);
                }
            }
        }
        while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    if (!cpuSamples.empty()) Sleep(250);

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const double processorCount = std::max<DWORD>(1, systemInfo.dwNumberOfProcessors);

    for (const auto& sample : cpuSamples)
    {
        FILETIME creation{};
        FILETIME exit{};
        FILETIME kernel{};
        FILETIME user{};
        if (GetProcessTimes(sample.process, &creation, &exit, &kernel, &user))
        {
            LARGE_INTEGER sampleEnd{};
            QueryPerformanceCounter(&sampleEnd);
            const double elapsedSeconds = frequency.QuadPart > 0
                ? static_cast<double>(sampleEnd.QuadPart - sample.sampleStart.QuadPart) /
                    static_cast<double>(frequency.QuadPart)
                : 0.0;
            const unsigned long long finalTime = FileTimeValue(kernel) + FileTimeValue(user);
            if (elapsedSeconds > 0.0 && finalTime >= sample.initialTime)
            {
                auto& item = runningProcesses_[sample.processIndex];
                const double processSeconds = static_cast<double>(finalTime - sample.initialTime) / 10000000.0;
                item.cpuUsagePercent = std::clamp(
                    (processSeconds / elapsedSeconds / processorCount) * 100.0,
                    0.0,
                    100.0);
                item.hasCpuUsage = true;
            }
        }
        CloseHandle(sample.process);
    }

    std::sort(runningProcesses_.begin(), runningProcesses_.end(), [](const auto& left, const auto& right)
    {
        return _wcsicmp(left.displayName.c_str(), right.displayName.c_str()) < 0;
    });
}

void MainWindow::PopulateRunningProcesses()
{
    ListView_DeleteAllItems(catalogListHandle_);

    wchar_t searchBuffer[256]{};
    GetWindowTextW(catalogSearchHandle_, searchBuffer, static_cast<int>(std::size(searchBuffer)));
    const std::wstring searchText(searchBuffer);

    for (size_t index = 0; index < runningProcesses_.size(); ++index)
    {
        const auto& process = runningProcesses_[index];
        if (!ContainsInsensitive(process.displayName, searchText) &&
            !ContainsInsensitive(process.processName, searchText) &&
            !ContainsInsensitive(process.executablePath, searchText))
        {
            continue;
        }

        AddListViewRow(catalogListHandle_, {
            process.displayName,
            process.executablePath.empty() ? L"Path unavailable" : process.executablePath,
            FormatCpuUsage(process.cpuUsagePercent, process.hasCpuUsage),
            FormatMemoryUsage(process.memoryUsageBytes, process.hasMemoryUsage)},
            static_cast<LPARAM>(index));
    }
}

void MainWindow::SwitchSourceTab()
{
    sourceTabIndex_ = TabCtrl_GetCurSel(sourceTabsHandle_);
    const bool runningProcesses = sourceTabIndex_ == 1;
    SetWindowTextW(detectSourceButtonHandle_, runningProcesses ? L"Refresh processes" : L"Detect installed apps");
    EnableWindow(addCatalogButtonHandle_, !runningProcesses);
    EnableWindow(removeCatalogButtonHandle_, !runningProcesses);
    SendMessageW(catalogSearchHandle_, EM_SETCUEBANNER, FALSE,
        reinterpret_cast<LPARAM>(runningProcesses ? L"Search processes" : L"Search apps"));

    if (runningProcesses)
    {
        ConfigureListView(catalogListHandle_, {{L"Name", 2}, {L"Path", 5}, {L"CPU", 1}, {L"Memory", 2}});
        CaptureRunningProcesses();
        PopulateRunningProcesses();
    }
    else
    {
        ConfigureListView(catalogListHandle_, {{L"Name", 2}, {L"Path", 5}});
        PopulateCatalogPrograms();
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
    auto workingSetups = app_.Configuration().monitorPowerSetups;
    auto workingDetectedDisplays = app_.Configuration().detectedDisplays;
    ShowMonitorPowerSetupsDialog(
        app_.InstanceHandle(),
        windowHandle_,
        workingSetups,
        workingDetectedDisplays,
        [this, &workingSetups, &workingDetectedDisplays]()
        {
            app_.Configuration().monitorPowerSetups = workingSetups;
            app_.Configuration().detectedDisplays = workingDetectedDisplays;
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

            if (!MonitorPowerController::ApplySetup(workingSetups[index], logger, &errorMessage))
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
    const int programIndex = SelectedListViewRow(ruleProgramsListHandle_);
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
    ListView_SetItemState(ruleProgramsListHandle_, programIndex,
        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    SaveConfiguration();
}

void MainWindow::EditRuleActions()
{
    const int watchedIndex = SelectedWatchedIndex();
    if (watchedIndex < 0)
    {
        MessageBoxW(windowHandle_, L"Select a watched process first.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
        return;
    }

    auto& rule = app_.Configuration().watchedProcesses[static_cast<size_t>(watchedIndex)];
    if (!ShowRuleActionsDialog(app_.InstanceHandle(), windowHandle_, rule, app_.Configuration().monitorPowerSetups))
    {
        return;
    }

    PopulateRulePrograms();
    SaveConfiguration();
}

void MainWindow::TransferSelectedSource()
{
    if (sourceTabIndex_ == 1)
    {
        AddSelectedRunningProcess();
    }
    else
    {
        AddSelectedCatalogProgram();
    }
}

void MainWindow::AddSelectedRunningProcess()
{
    const int watchedIndex = SelectedWatchedIndex();
    if (watchedIndex < 0)
    {
        MessageBoxW(windowHandle_, L"Select a watched process first.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const int processIndex = SelectedRunningProcessIndex();
    if (processIndex < 0 || processIndex >= static_cast<int>(runningProcesses_.size()))
    {
        MessageBoxW(windowHandle_, L"Select a running process first.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const auto& selected = runningProcesses_[static_cast<size_t>(processIndex)];
    auto& rule = app_.Configuration().watchedProcesses[static_cast<size_t>(watchedIndex)];
    if (_wcsicmp(std::filesystem::path(selected.processName).stem().c_str(),
            std::filesystem::path(rule.processName).stem().c_str()) == 0)
    {
        MessageBoxW(windowHandle_, L"The watched process cannot stop itself.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const auto duplicate = std::find_if(rule.processesToStop.begin(), rule.processesToStop.end(), [&selected](const ProcessStopAction& action)
    {
        if (_wcsicmp(std::filesystem::path(action.processName).stem().c_str(),
                std::filesystem::path(selected.processName).stem().c_str()) != 0)
        {
            return false;
        }
        return action.executablePath.empty() || selected.executablePath.empty() ||
            _wcsicmp(action.executablePath.c_str(), selected.executablePath.c_str()) == 0;
    });
    if (duplicate != rule.processesToStop.end())
    {
        MessageBoxW(windowHandle_, L"This process is already in the Stop Processes actions.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
        return;
    }

    ProcessStopAction action;
    action.displayName = selected.displayName;
    action.processName = selected.processName;
    action.executablePath = selected.executablePath;
    rule.processesToStop.push_back(std::move(action));
    PopulateRulePrograms();
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

void MainWindow::RemoveSelectedRuleAction()
{
    const int watchedIndex = SelectedWatchedIndex();
    int actionIndex = SelectedListViewRow(ruleProgramsListHandle_);
    if (watchedIndex < 0 || actionIndex < 0) return;

    auto& rule = app_.Configuration().watchedProcesses[static_cast<size_t>(watchedIndex)];
    if (actionIndex < static_cast<int>(rule.programsToLaunch.size()))
    {
        rule.programsToLaunch.erase(rule.programsToLaunch.begin() + actionIndex);
    }
    else if ((actionIndex -= static_cast<int>(rule.programsToLaunch.size())) <
        static_cast<int>(rule.processesToStop.size()))
    {
        rule.processesToStop.erase(rule.processesToStop.begin() + actionIndex);
    }
    else if ((actionIndex -= static_cast<int>(rule.processesToStop.size())) <
        static_cast<int>(rule.homeAssistantActions.size()))
    {
        rule.homeAssistantActions.erase(rule.homeAssistantActions.begin() + actionIndex);
    }
    else if (!rule.monitorPowerSetupName.empty())
    {
        rule.monitorPowerSetupName.clear();
        rule.monitorPowerSetupDelayMilliseconds = 0;
        rule.restoreMonitorPowerSetupOnExit = false;
        rule.restoreMonitorPowerSetupDelayMilliseconds = 0;
    }
    else
    {
        return;
    }
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
    const int index = SelectedListViewRow(catalogListHandle_);
    if (index < 0 || index >= static_cast<int>(detectedPrograms_.size()))
    {
        return -1;
    }
    return index;
}

int MainWindow::SelectedRunningProcessIndex() const
{
    const int index = SelectedListViewRow(catalogListHandle_);
    if (index < 0 || index >= static_cast<int>(runningProcesses_.size()))
    {
        return -1;
    }
    return index;
}

int MainWindow::SelectedWatchedIndex() const
{
    return SelectedListViewRow(watchedListHandle_);
}
