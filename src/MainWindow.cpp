#include "MainWindow.h"

#include "CatalogPaths.h"
#include "DetectedProcessCatalog.h"
#include "ListViewHelpers.h"
#include "RuleActionsDialog.h"
#include "IRacingPerformance.h"
#include "IRacingServices.h"
#include "StartupRegistration.h"
#include "TabHost.h"
#include "resource.h"
#include "Utils.h"
#include "UiTheme.h"

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

    struct MonitorPowerSetupDialogState
    {
        static constexpr int kEnabledControlBase = 3000;
        static constexpr int kPrimaryControlBase = 4000;

        // Edited copies; saved whenever they form a valid set.
        std::vector<MonitorPowerSetup> workingSetups;
        std::vector<MonitorPowerSetup::DisplayPath> workingDetectedDisplays;
        std::vector<MonitorPowerSetup>* setups{&workingSetups};
        std::vector<MonitorPowerSetup::DisplayPath>* detectedDisplays{&workingDetectedDisplays};
        std::function<void(const std::vector<MonitorPowerSetup>&, const std::vector<MonitorPowerSetup::DisplayPath>&)> saveCallback;
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
        CreateMonitorSetupRowControl(dialogHandle, state, L"STATIC", L"Monitor", 0, 112, 36, 50, 10);
        CreateMonitorSetupRowControl(dialogHandle, state, L"STATIC", L"Name", 0, 166, 36, 120, 10);
        CreateMonitorSetupRowControl(dialogHandle, state, L"STATIC", L"Enabled", 0, 292, 36, 40, 10);
        CreateMonitorSetupRowControl(dialogHandle, state, L"STATIC", L"Primary", 0, 338, 36, 42, 10);
        CreateMonitorSetupRowControl(dialogHandle, state, L"STATIC", L"", SS_ETCHEDHORZ, 112, 49, 268, 1);

        if (setup.displayPaths.empty())
        {
            CreateMonitorSetupRowControl(
                dialogHandle,
                state,
                L"STATIC",
                L"No monitors detected yet. Activate the desired monitors in Windows and click Detect current.",
                0,
                112,
                54,
                268,
                24);
            return;
        }

        for (size_t index = 0; index < setup.displayPaths.size(); ++index)
        {
            const auto& path = setup.displayPaths[index];
            const LONG y = 54 + static_cast<LONG>(index) * 20;
            CreateMonitorSetupRowControl(
                dialogHandle,
                state,
                L"STATIC",
                L"Monitor " + std::to_wstring(index + 1),
                0,
                112,
                y + 2,
                50,
                12);
            CreateMonitorSetupRowControl(
                dialogHandle,
                state,
                L"STATIC",
                path.monitorName.empty() ? path.displayName : path.monitorName,
                SS_CENTERIMAGE | SS_ENDELLIPSIS,
                166,
                y + 2,
                120,
                12);
            HWND enabled = CreateMonitorSetupRowControl(
                dialogHandle,
                state,
                L"BUTTON",
                L"",
                BS_AUTOCHECKBOX,
                302,
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
                350,
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

    // Empty when the configs can be saved; otherwise what still needs fixing.
    std::wstring ValidateMonitorPowerSetups(const MonitorPowerSetupDialogState& state)
    {
        std::vector<DWORD> assignedHotkeys;
        for (const auto& setup : *state.setups)
        {
            if (setup.name.empty()) return L"Each monitor config needs a name.";
            if (setup.displayPaths.empty()) return L"Click Detect current to capture the monitors for " + setup.name + L".";
            const auto enabledCount = std::count_if(setup.displayPaths.begin(), setup.displayPaths.end(), [](const auto& display)
            {
                return display.enabled;
            });
            const auto primaryCount = std::count_if(setup.displayPaths.begin(), setup.displayPaths.end(), [](const auto& display)
            {
                return display.enabled && display.isPrimary;
            });
            if (enabledCount == 0 || primaryCount != 1)
                return setup.name + L" needs at least one enabled monitor and exactly one enabled primary monitor.";
            if (setup.hotkeyVirtualKey != 0)
            {
                const DWORD hotkey = MAKELONG(setup.hotkeyModifiers, setup.hotkeyVirtualKey);
                if (std::find(assignedHotkeys.begin(), assignedHotkeys.end(), hotkey) != assignedHotkeys.end())
                    return L"Each monitor config hotkey must be unique.";
                assignedHotkeys.push_back(hotkey);
            }
        }
        return {};
    }

    // Saves the working copies once they are valid and says what is missing otherwise.
    void CommitMonitorPowerSetups(HWND dialogHandle, MonitorPowerSetupDialogState& state)
    {
        StoreSelectedMonitorPowerSetup(dialogHandle, state);
        const auto problem = ValidateMonitorPowerSetups(state);
        if (problem.empty() && state.saveCallback) state.saveCallback(*state.setups, *state.detectedDisplays);
        SetDlgItemTextW(dialogHandle, IDC_MONITOR_SETUP_STATUS,
            problem.empty() ? L"Changes are saved automatically." : (L"Not saved yet: " + problem).c_str());
    }

    INT_PTR CALLBACK MonitorPowerSetupsDialogProc(HWND dialogHandle, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* state = reinterpret_cast<MonitorPowerSetupDialogState*>(GetWindowLongPtrW(dialogHandle, GWLP_USERDATA));

        switch (message)
        {
        case WM_INITDIALOG:
        {
            UiTheme::Apply(dialogHandle);
            state = reinterpret_cast<MonitorPowerSetupDialogState*>(lParam);
            SetWindowLongPtrW(dialogHandle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            PopulateMonitorPowerSetupList(dialogHandle, *state);
            LoadSelectedMonitorPowerSetup(dialogHandle, *state);
            const auto problem = ValidateMonitorPowerSetups(*state);
            SetDlgItemTextW(dialogHandle, IDC_MONITOR_SETUP_STATUS,
                problem.empty() ? L"Changes are saved automatically." : (L"Not saved yet: " + problem).c_str());
            return FALSE;
        }
        case WM_DESTROY:
            // The hotkey control does not report edits, so capture them when the page closes.
            if (state) CommitMonitorPowerSetups(dialogHandle, *state);
            return FALSE;
        case WM_NCDESTROY:
            delete state;
            SetWindowLongPtrW(dialogHandle, GWLP_USERDATA, 0);
            return FALSE;
        case WM_COMMAND:
            if (state == nullptr || state->syncingControls)
            {
                return FALSE;
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
                CommitMonitorPowerSetups(dialogHandle, *state);
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
                CommitMonitorPowerSetups(dialogHandle, *state);
                return TRUE;
            }

            switch (LOWORD(wParam))
            {
            case IDC_MONITOR_SETUP_LIST:
                if (HIWORD(wParam) == LBN_SELCHANGE)
                {
                    CommitMonitorPowerSetups(dialogHandle, *state);
                    state->selectedIndex = static_cast<int>(SendMessageW(GetDlgItem(dialogHandle, IDC_MONITOR_SETUP_LIST), LB_GETCURSEL, 0, 0));
                    LoadSelectedMonitorPowerSetup(dialogHandle, *state);
                }
                return TRUE;
            case IDC_MONITOR_SETUP_NAME:
                if (HIWORD(wParam) == EN_CHANGE)
                {
                    CommitMonitorPowerSetups(dialogHandle, *state);
                    PopulateMonitorPowerSetupList(dialogHandle, *state);
                }
                return TRUE;
            case IDC_MONITOR_SETUP_ADD:
            {
                StoreSelectedMonitorPowerSetup(dialogHandle, *state);
                MonitorPowerSetup setup;
                setup.name = L"New setup";
                if (!state->detectedDisplays->empty())
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
                CommitMonitorPowerSetups(dialogHandle, *state);
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
                    CommitMonitorPowerSetups(dialogHandle, *state);
                }
                return TRUE;
            case IDC_MONITOR_SETUP_CAPTURE:
            {
                StoreSelectedMonitorPowerSetup(dialogHandle, *state);
                std::wstring errorMessage;
                MonitorPowerSetup detectedSetup;
                if (!MonitorPowerController::CaptureSetup(detectedSetup, &errorMessage))
                {
                    SetDlgItemTextW(dialogHandle, IDC_MONITOR_SETUP_STATUS, errorMessage.c_str());
                    return TRUE;
                }
                *state->detectedDisplays = detectedSetup.displayPaths;
                for (auto& setup : *state->setups)
                {
                    MergeDetectedDisplaysIntoSetup(setup, *state->detectedDisplays);
                }
                LoadSelectedMonitorPowerSetup(dialogHandle, *state);
                CommitMonitorPowerSetups(dialogHandle, *state);
                return TRUE;
            }
            case IDC_MONITOR_SETUP_APPLY:
            {
                // Apply uses the saved configuration, so save pending edits first.
                CommitMonitorPowerSetups(dialogHandle, *state);
                const auto problem = ValidateMonitorPowerSetups(*state);
                if (!problem.empty())
                {
                    SetDlgItemTextW(dialogHandle, IDC_MONITOR_SETUP_STATUS, (L"Cannot apply yet: " + problem).c_str());
                    return TRUE;
                }
                if (state->selectedIndex >= 0 && state->applyCallback)
                {
                    state->applyCallback(static_cast<size_t>(state->selectedIndex));
                }
                return TRUE;
            }
            }
            break;
        }

        return FALSE;
    }

    HWND CreateMonitorSetupsPane(
        HINSTANCE instanceHandle,
        HWND parent,
        const std::vector<MonitorPowerSetup>& setups,
        const std::vector<MonitorPowerSetup::DisplayPath>& detectedDisplays,
        std::function<void(const std::vector<MonitorPowerSetup>&, const std::vector<MonitorPowerSetup::DisplayPath>&)> saveCallback,
        std::function<bool(size_t)> applyCallback)
    {
        auto* state = new MonitorPowerSetupDialogState;
        state->workingSetups = setups;
        state->workingDetectedDisplays = detectedDisplays;
        state->saveCallback = std::move(saveCallback);
        state->applyCallback = std::move(applyCallback);
        const HWND pane = CreateDialogParamW(instanceHandle, MAKEINTRESOURCEW(IDD_MONITOR_POWER_SETUPS), parent,
            MonitorPowerSetupsDialogProc, reinterpret_cast<LPARAM>(state));
        if (!pane) delete state;
        return pane;
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

    struct InstalledAppRecord
    {
        std::wstring name;
        std::wstring location;
        std::wstring icon;
    };

    struct AppPathRecord
    {
        std::wstring executableName;
        std::wstring executablePath;
    };

    struct StartMenuProgram
    {
        std::wstring name;
        std::wstring path;
    };

    std::wstring NormalizeProgramName(const std::wstring& value)
    {
        std::wstring normalized;
        for (const wchar_t character : value)
            if (std::iswalnum(character)) normalized += std::towlower(character);
        return normalized;
    }

    int ProgramNameScore(const std::wstring& candidate, const std::wstring& displayName)
    {
        const auto left = NormalizeProgramName(candidate);
        const auto right = NormalizeProgramName(displayName);
        if (left.empty() || right.empty()) return 0;
        if (left == right) return 100;
        if (left.find(right) != std::wstring::npos || right.find(left) != std::wstring::npos) return 60;
        return 0;
    }

    std::wstring ResolveShortcutTarget(const std::filesystem::path& shortcut)
    {
        IShellLinkW* link = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&link)))) return {};
        IPersistFile* persist = nullptr;
        const HRESULT queried = link->QueryInterface(IID_PPV_ARGS(&persist));
        if (FAILED(queried)) { link->Release(); return {}; }
        const HRESULT loaded = persist->Load(shortcut.c_str(), STGM_READ);
        persist->Release();
        if (FAILED(loaded)) { link->Release(); return {}; }
        wchar_t target[32768]{};
        const HRESULT resolved = link->GetPath(target, static_cast<int>(std::size(target)), nullptr, SLGP_RAWPATH);
        link->Release();
        std::error_code error;
        return SUCCEEDED(resolved) && std::filesystem::is_regular_file(target, error) ? target : std::wstring{};
    }

    std::vector<StartMenuProgram> EnumerateStartMenuPrograms(const std::atomic_bool& cancelled)
    {
        std::vector<StartMenuProgram> programs;
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return programs;
        for (const KNOWNFOLDERID folder : {FOLDERID_Programs, FOLDERID_CommonPrograms})
        {
            PWSTR root = nullptr;
            if (FAILED(SHGetKnownFolderPath(folder, 0, nullptr, &root))) continue;
            const std::filesystem::path directory(root);
            CoTaskMemFree(root);
            std::error_code error;
            for (std::filesystem::recursive_directory_iterator it(directory,
                    std::filesystem::directory_options::skip_permission_denied, error), end;
                !error && it != end; it.increment(error))
            {
                if (cancelled) break;
                if (!it->is_regular_file(error) || _wcsicmp(it->path().extension().c_str(), L".lnk") != 0) continue;
                const auto target = ResolveShortcutTarget(it->path());
                if (!target.empty()) programs.push_back({it->path().stem().wstring(), target});
            }
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
        return programs;
    }

    std::wstring ReadRegistryText(HKEY key, const wchar_t* valueName)
    {
        wchar_t buffer[2048]{};
        DWORD size = sizeof(buffer);
        if (RegGetValueW(key, nullptr, valueName, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            nullptr, buffer, &size) != ERROR_SUCCESS) return {};
        return ExpandEnvironmentPath(buffer);
    }

    std::wstring ExtractExecutablePath(std::wstring value)
    {
        while (!value.empty() && std::iswspace(value.front())) value.erase(value.begin());
        while (!value.empty() && std::iswspace(value.back())) value.pop_back();
        if (value.empty()) return {};
        if (value.front() == L'\"')
        {
            const auto quote = value.find(L'\"', 1);
            value = quote == std::wstring::npos ? std::wstring{} : value.substr(1, quote - 1);
        }
        else if (const auto comma = value.find_last_of(L','); comma != std::wstring::npos)
        {
            const auto suffix = value.substr(comma + 1);
            const bool iconIndex = !suffix.empty() && std::all_of(suffix.begin(), suffix.end(), [](wchar_t ch)
            {
                return std::iswspace(ch) || ch == L'-' || std::iswdigit(ch);
            });
            if (iconIndex) value.resize(comma);
        }
        std::error_code error;
        const auto path = ExpandEnvironmentPath(value);
        return _wcsicmp(std::filesystem::path(path).extension().c_str(), L".exe") == 0 &&
            std::filesystem::is_regular_file(path, error) ? path : std::wstring{};
    }

    std::vector<InstalledAppRecord> EnumerateInstalledApps()
    {
        std::vector<InstalledAppRecord> apps;
        constexpr const wchar_t* uninstall = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
        for (const auto& [root, view] : {
            std::pair{HKEY_CURRENT_USER, REGSAM(0)},
            std::pair{HKEY_LOCAL_MACHINE, REGSAM(KEY_WOW64_64KEY)},
            std::pair{HKEY_LOCAL_MACHINE, REGSAM(KEY_WOW64_32KEY)}})
        {
            HKEY parent{};
            if (RegOpenKeyExW(root, uninstall, 0, KEY_ENUMERATE_SUB_KEYS | view, &parent) != ERROR_SUCCESS) continue;
            for (DWORD index = 0;; ++index)
            {
                wchar_t name[512]{};
                DWORD length = static_cast<DWORD>(std::size(name));
                const LONG result = RegEnumKeyExW(parent, index, name, &length, nullptr, nullptr, nullptr, nullptr);
                if (result == ERROR_NO_MORE_ITEMS) break;
                if (result != ERROR_SUCCESS) continue;
                HKEY item{};
                if (RegOpenKeyExW(parent, name, 0, KEY_QUERY_VALUE | view, &item) != ERROR_SUCCESS) continue;
                InstalledAppRecord app{ReadRegistryText(item, L"DisplayName"),
                    ReadRegistryText(item, L"InstallLocation"), ReadRegistryText(item, L"DisplayIcon")};
                RegCloseKey(item);
                if (!app.name.empty()) apps.push_back(std::move(app));
            }
            RegCloseKey(parent);
        }
        return apps;
    }

    std::vector<AppPathRecord> EnumerateAppPaths()
    {
        std::vector<AppPathRecord> paths;
        constexpr const wchar_t* appPaths = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths";
        for (const auto& [root, view] : {
            std::pair{HKEY_CURRENT_USER, REGSAM(0)},
            std::pair{HKEY_LOCAL_MACHINE, REGSAM(KEY_WOW64_64KEY)},
            std::pair{HKEY_LOCAL_MACHINE, REGSAM(KEY_WOW64_32KEY)}})
        {
            HKEY parent{};
            if (RegOpenKeyExW(root, appPaths, 0, KEY_ENUMERATE_SUB_KEYS | view, &parent) != ERROR_SUCCESS) continue;
            for (DWORD index = 0;; ++index)
            {
                wchar_t name[512]{};
                DWORD length = static_cast<DWORD>(std::size(name));
                const LONG result = RegEnumKeyExW(parent, index, name, &length, nullptr, nullptr, nullptr, nullptr);
                if (result == ERROR_NO_MORE_ITEMS) break;
                if (result != ERROR_SUCCESS) continue;
                HKEY item{};
                if (RegOpenKeyExW(parent, name, 0, KEY_QUERY_VALUE | view, &item) != ERROR_SUCCESS) continue;
                const auto path = ExtractExecutablePath(ReadRegistryText(item, nullptr));
                RegCloseKey(item);
                if (!path.empty()) paths.push_back({name, path});
            }
            RegCloseKey(parent);
        }
        return paths;
    }

    std::wstring InstalledExecutablePath(const std::vector<InstalledAppRecord>& apps,
        const DetectedProcessCandidate& candidate)
    {
        for (const auto& app : apps)
        {
            if (!ContainsInsensitive(app.name, candidate.name)) continue;
            std::error_code error;
            if (!app.location.empty())
            {
                const auto path = std::filesystem::path(app.location) / candidate.executable;
                if (std::filesystem::is_regular_file(path, error)) return path.wstring();
            }
            const auto icon = ExtractExecutablePath(app.icon);
            if (_wcsicmp(std::filesystem::path(icon).filename().c_str(), candidate.executable) == 0 &&
                std::filesystem::is_regular_file(icon, error)) return icon;
        }
        return {};
    }

    std::wstring InstalledAppExecutablePath(const InstalledAppRecord& app,
        const std::vector<StartMenuProgram>& startMenuPrograms, const std::vector<AppPathRecord>& appPaths)
    {
        const auto icon = ExtractExecutablePath(app.icon);
        std::error_code error;
        if (!icon.empty() && ProgramNameScore(std::filesystem::path(icon).stem().wstring(), app.name) >= 60)
            return icon;

        int bestScore = 0;
        std::wstring bestPath;
        for (const auto& program : startMenuPrograms)
        {
            int score = ProgramNameScore(program.name, app.name);
            if (!app.location.empty() && ToLowerCopy(program.path).starts_with(ToLowerCopy(app.location))) score += 50;
            if (score > bestScore) { bestScore = score; bestPath = program.path; }
        }
        if (bestScore >= 60) return bestPath;

        for (const auto& appPath : appPaths)
        {
            int score = ProgramNameScore(appPath.executableName, app.name);
            if (!app.location.empty() && ToLowerCopy(appPath.executablePath).starts_with(ToLowerCopy(app.location))) score += 50;
            if (score > bestScore) { bestScore = score; bestPath = appPath.executablePath; }
        }
        if (bestScore >= 60) return bestPath;

        if (app.location.empty() || !std::filesystem::is_directory(app.location, error)) return {};
        for (std::filesystem::recursive_directory_iterator it(app.location,
                std::filesystem::directory_options::skip_permission_denied, error), end;
            !error && it != end; it.increment(error))
        {
            if (it.depth() > 3) { it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file(error) || _wcsicmp(it->path().extension().c_str(), L".exe") != 0) continue;
            const auto stem = it->path().stem().wstring();
            int score = ProgramNameScore(stem, app.name);
            const auto lower = ToLowerCopy(stem);
            if (lower.find(L"unins") != std::wstring::npos || lower.find(L"uninstall") != std::wstring::npos ||
                lower.find(L"updat") != std::wstring::npos || lower.find(L"setup") != std::wstring::npos) continue;
            if (score > bestScore) { bestScore = score; bestPath = it->path().wstring(); }
        }
        return bestScore >= 60 ? bestPath : std::wstring{};
    }

    bool TryAppendCatalogProgram(
        std::vector<CatalogProgram>& programs,
        std::unordered_set<std::wstring>& seenPaths,
        const std::wstring& displayName,
        const std::wstring& filePath,
        bool manuallyAdded = false)
    {
        if (displayName.empty() || filePath.empty())
        {
            return false;
        }

        const auto key = filePath.empty()
            ? L"name:" + ToLowerCopy(displayName)
            : L"path:" + ToLowerCopy(filePath);
        if (!seenPaths.insert(key).second)
        {
            return false;
        }

        programs.push_back({displayName, filePath, manuallyAdded});
        return true;
    }

    std::vector<size_t> SelectedSourceIndices(HWND list)
    {
        std::vector<size_t> indices;
        for (int row = -1; (row = ListView_GetNextItem(list, row, LVNI_SELECTED)) != -1;)
        {
            LVITEMW item{};
            item.mask = LVIF_PARAM;
            item.iItem = row;
            if (ListView_GetItem(list, &item) && item.lParam >= 0)
                indices.push_back(static_cast<size_t>(item.lParam));
        }
        return indices;
    }
}

MainWindow::MainWindow(App& app)
    : app_(app)
{
}

MainWindow::~MainWindow()
{
    if (monitorStopThread_.joinable()) monitorStopThread_.join();
    if (programIconList_) ImageList_Destroy(programIconList_);
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
        (L"LaunchMate " + UpdateChecker::CurrentVersion()).c_str(),
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
    SyncCatalogProgramsFromConfiguration();
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
    if (monitorStopping_)
    {
        statusPanel_.SetState(StatusPanel::Tone::Busy, L"Stopping monitoring...",
            L"Closing started apps and restoring services, power plan and displays.", L"Stopping...", false);
        return;
    }
    if (!monitor.IsRunning())
    {
        statusPanel_.SetState(StatusPanel::Tone::Neutral, L"Monitoring is off",
            L"Start monitoring to run your rules automatically.", L"Start monitoring", true);
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
        if (code == EN_CHANGE && controlId == IdCatalogSearch)
        {
            PopulateCatalogPrograms();
            return 0;
        }

        if (controlId == IdRuleCards || controlId == IdStartCards || controlId == IdExitCards)
        {
            HandleCardCommand(controlId, code);
            return 0;
        }
        if (controlId == IdAppsRuleCombo)
        {
            if (code == CBN_SELCHANGE)
            {
                const auto selection = SendMessageW(appsRuleComboHandle_, CB_GETCURSEL, 0, 0);
                selectedRuleIndex_ = selection == CB_ERR ? -1 : static_cast<int>(selection);
                SetWindowTextW(appsFeedbackHandle_, L"");
                // The running-process list hides the selected rule's own process.
                if (sourceTabIndex_ == 1) PopulateRunningProcesses();
            }
            return 0;
        }

        switch (controlId)
        {
        case IdToggleMonitoring: ToggleMonitoring(); return 0;
        case IdMonitorPowerSetups: ShowPage(Page::Displays); return 0;
        case IdSettings: ShowPage(Page::Settings); return 0;
        case IdSectionBack: ShowPage(Page::RuleDetail); return 0;
        case IdNavRules: ShowPage(Page::Rules); return 0;
        case IdNavApps: ShowPage(Page::Apps); return 0;
        case IdRuleBack: ShowPage(Page::Rules); return 0;
        case IdRuleToggleEnabled: ToggleRuleEnabled(selectedRuleIndex_); return 0;
        case IdDetectInstalledApps:
            StartSourceRefresh();
            return 0;
        case IdTransferCatalogProgram: TransferSelectedSource(); return 0;
        case IdAddCatalogProgram: AddCustomCatalogProgram(); return 0;
        case IdRemoveCatalogProgram: RemoveSelectedCatalogProgram(); return 0;
        case IdAddWatchedProcess: AddWatchedProcess(); return 0;
        case IdRemoveWatchedProcess: RemoveWatchedProcess(selectedRuleIndex_); return 0;
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
        if (header && header->code == LVN_COLUMNCLICK && header->idFrom == IdCatalogList)
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
        if (header && header->code == LVN_KEYDOWN)
        {
            const auto* key = reinterpret_cast<NMLVKEYDOWN*>(lParam);
            if (key->wVKey == VK_DELETE)
            {
                if (header->idFrom == IdCatalogList && sourceTabIndex_ == 0) RemoveSelectedCatalogProgram();
                return 0;
            }
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
        if (wParam == kSourceRefreshTimer) { PollSourceRefresh(); return 0; }
        if (wParam == kSaveTimer) { FlushPendingSave(); return 0; }
        break;
    case WM_DESTROY:
        KillTimer(windowHandle_, kProcessStateTimer);
        KillTimer(windowHandle_, kSourceRefreshTimer);
        KillTimer(windowHandle_, kSaveTimer);
        for (auto& task : sourceTasks_) task.Cancel();
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
    DeleteObject(oldUi);
    DeleteObject(oldHeading);
}

void MainWindow::CreateControls()
{
    navBar_.Create(app_.InstanceHandle(), windowHandle_, L"LaunchMate", L"Version " + UpdateChecker::CurrentVersion(), {
        {L'\uE8FD', L"Rules", IdNavRules, true},
        {L'\uE71D', L"Apps", IdNavApps, true},
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

    // Apps page
    appsRuleLabelHandle_ = CreateLabel(windowHandle_, L"Add selected to rule", 0, 0, 140, 30, uiFont_);
    appsRuleComboHandle_ = CreateWindowExW(0, WC_COMBOBOXW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
        0, 0, 220, 300, windowHandle_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdAppsRuleCombo)), nullptr, nullptr);
    SendMessageW(appsRuleComboHandle_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
    CreateButtonControl(windowHandle_, IdTransferCatalogProgram, L"Start with rule", 0, 0, 130, 30, uiFont_);
    appsFeedbackHandle_ = CreateLabel(windowHandle_, L"", 0, 0, 240, 30, uiFont_);

    sourceTabsHandle_ = CreateWindowExW(WS_EX_CONTROLPARENT, WC_TABCONTROLW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN | TCS_FIXEDWIDTH,
        0, 0, 100, 100, windowHandle_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdSourceTabs)), nullptr, nullptr);
    SendMessageW(sourceTabsHandle_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
    TCITEMW sourceTab{TCIF_TEXT};
    sourceTab.pszText = const_cast<wchar_t*>(L"Detected apps");
    TabCtrl_InsertItem(sourceTabsHandle_, 0, &sourceTab);
    sourceTab.pszText = const_cast<wchar_t*>(L"Running processes");
    TabCtrl_InsertItem(sourceTabsHandle_, 1, &sourceTab);
    sourceTab.pszText = const_cast<wchar_t*>(L"Detected processes");
    TabCtrl_InsertItem(sourceTabsHandle_, 2, &sourceTab);
    detectSourceButtonHandle_ = CreateButtonControl(windowHandle_, IdDetectInstalledApps, L"Refresh installed apps", 0, 0, 164, 28, uiFont_);
    addCatalogButtonHandle_ = CreateButtonControl(windowHandle_, IdAddCatalogProgram, L"+", 0, 0, 36, 28, uiFont_);
    removeCatalogButtonHandle_ = CreateButtonControl(windowHandle_, IdRemoveCatalogProgram, L"-", 0, 0, 36, 28, uiFont_);
    catalogSearchHandle_ = CreateEditControl(windowHandle_, IdCatalogSearch, L"", 0, 0, 100, 28, uiFont_);
    SendMessageW(catalogSearchHandle_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Search apps"));
    const auto list = [this](int id, bool single)
    {
        HWND control = CreateWindowExW(0, WC_LISTVIEWW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL |
                LVS_REPORT | LVS_SHOWSELALWAYS | (single ? LVS_SINGLESEL : 0),
            0, 0, 100, 100, windowHandle_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
        InitializeReportListView(control);
        return control;
    };
    catalogListHandle_ = list(IdCatalogList, false);
    ConfigureListView(catalogListHandle_, {{L"", 0}, {L"Name", 2}, {L"Path", 5}});
    HostControlsInTab(windowHandle_, sourceTabsHandle_, {
        detectSourceButtonHandle_, addCatalogButtonHandle_, removeCatalogButtonHandle_,
        catalogSearchHandle_, catalogListHandle_});

    InitializeProgramIcons();

}

void MainWindow::LayoutControls(int width, int height)
{
    if (!sourceTabsHandle_) return;
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
    place(ruleTitleHandle_, left + 112, top - 4, content - 112 - 230, 24);
    place(ruleSubtitleHandle_, left + 112, top + 18, content - 112 - 230, 20);
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

    // Apps page
    place(appsRuleLabelHandle_, left, top, 140, 30);
    place(appsRuleComboHandle_, left + 144, top + 3, 220, 300);
    button(IdTransferCatalogProgram, left + 376, top, 130, 30);
    place(appsFeedbackHandle_, left + 520, top, content - 520, 30);
    const int tabsTop = top + 44;
    place(sourceTabsHandle_, left, tabsTop, content, bottom - tabsTop);
    TabCtrl_SetItemSize(sourceTabsHandle_, px(std::min(180, (content - 8) / 3)), px(UiTheme::TabHeight));
    place(detectSourceButtonHandle_, left + 12, tabsTop + 42, 164, 28);
    place(addCatalogButtonHandle_, right - 90, tabsTop + 42, 36, 28);
    place(removeCatalogButtonHandle_, right - 48, tabsTop + 42, 36, 28);
    place(catalogSearchHandle_, left + 12, tabsTop + 78, content - 24, 22);
    place(catalogListHandle_, left + 12, tabsTop + 110, content - 24, bottom - tabsTop - 122);

    if (sourceTabIndex_ == 0) ResizeListViewColumns(catalogListHandle_, {0, 2, 5}, px(24));
    else if (sourceTabIndex_ == 1) ResizeListViewColumns(catalogListHandle_, {0, 2, 5, 1, 2}, px(24));
    else ResizeListViewColumns(catalogListHandle_, {0, 2, 4, 2, 3}, px(24));
}

void MainWindow::InitializeProgramIcons()
{
    if (!programIconList_)
    {
        const int iconSize = MulDiv(16, dpi_, 96);
        programIconList_ = ImageList_Create(iconSize, iconSize, ILC_COLOR32 | ILC_MASK, 32, 32);
        if (!programIconList_) return;
        defaultProgramIconIndex_ = ProgramIconIndex(L"");
    }
    if (catalogListHandle_) ListView_SetImageList(catalogListHandle_, programIconList_, LVSIL_SMALL);
}

int MainWindow::ProgramIconIndex(const std::wstring& executablePath)
{
    if (!programIconList_) return -1;
    const std::wstring key = executablePath.empty() ? L"<default>" : ToLowerCopy(executablePath);
    if (const auto existing = programIconIndexes_.find(key); existing != programIconIndexes_.end()) return existing->second;

    SHFILEINFOW info{};
    const DWORD_PTR result = executablePath.empty()
        ? SHGetFileInfoW(L".exe", FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
            SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES)
        : SHGetFileInfoW(executablePath.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info), SHGFI_ICON | SHGFI_SMALLICON);
    int index = defaultProgramIconIndex_;
    if (result != 0 && info.hIcon)
    {
        index = ImageList_AddIcon(programIconList_, info.hIcon);
        DestroyIcon(info.hIcon);
    }
    if (index < 0) index = 0;
    programIconIndexes_.emplace(key, index);
    return index;
}

void MainWindow::PopulateLists()
{
    PopulateCatalogPrograms();
    PopulateRuleCombo();
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
    show({appsRuleLabelHandle_, appsRuleComboHandle_, item(IdTransferCatalogProgram), appsFeedbackHandle_,
        sourceTabsHandle_}, page == Page::Apps);
    const bool embedded = page == Page::RuleSection || page == Page::Displays || page == Page::Settings;
    show({pageTitleHandle_, pageHintHandle_, pageHost_.Handle()}, embedded);
    show({item(IdSectionBack)}, page == Page::RuleSection);
    navBar_.SetSelected(page == Page::Apps ? IdNavApps : page == Page::Displays ? IdMonitorPowerSetups
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
    else
    {
        PopulateRuleCombo();
        SetWindowTextW(appsFeedbackHandle_, L"");
    }
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
            [](const ProcessPerformanceAction& action)
            {
                return action.cpuPriorityClass != 0 || action.ioPriority >= 0 || action.memoryPriority >= 0 ||
                    action.affinityMask != 0;
            }));
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
        if (!rule.enabled) { item.pill = L"Disabled"; item.pillTone = CardList::Tone::Warning; }
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

    // Same order as the monitor runs them.
    std::vector<std::wstring> names;
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

    names.clear();
    int latestStart = 0;
    for (const auto& program : rule.programsToLaunch)
    {
        names.push_back(program.displayName.empty() ? FileNameWithoutExtension(program.filePath) : program.displayName);
        latestStart = std::max(latestStart, program.waitTimeMilliseconds);
    }
    std::wstring started = JoinNames(names);
    if (!started.empty() && latestStart > 0) started += L"  \u00B7  within " + Seconds(latestStart);
    add(start, startCardSections_, RuleSection::StartPrograms, L"Start apps", started, names.size(), L"Not used \u00B7 add tools such as SimHub or CrewChief");

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
    if (page == Page::Settings)
    {
        SetWindowTextW(pageTitleHandle_, L"Settings");
        SetWindowTextW(pageHintHandle_, L"Changes are saved immediately.");
        return CreateDialogParamW(app_.InstanceHandle(), MAKEINTRESOURCEW(IDD_SETTINGS), host, SettingsDialogProc,
            reinterpret_cast<LPARAM>(this));
    }
    if (page == Page::Displays)
    {
        SetWindowTextW(pageTitleHandle_, L"Displays");
        SetWindowTextW(pageHintHandle_, L"Saved monitor arrangements. Switch with a hotkey or the tray menu, or use one in a rule.");
        return CreateMonitorSetupsPane(app_.InstanceHandle(), host, config.monitorPowerSetups, config.detectedDisplays,
            [this](const auto& setups, const auto& detectedDisplays)
            {
                app_.Configuration().monitorPowerSetups = setups;
                app_.Configuration().detectedDisplays = detectedDisplays;
                ScheduleSave();
            },
            [this](size_t index) { return ApplyMonitorPowerSetup(index, true); });
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
    return CreateRuleSectionPane(app_.InstanceHandle(), host, rule, config.monitorPowerSetups, openSection_,
        [this] { ScheduleSave(); });
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

void MainWindow::PopulateRuleCombo()
{
    const auto& rules = app_.Configuration().watchedProcesses;
    SendMessageW(appsRuleComboHandle_, CB_RESETCONTENT, 0, 0);
    for (const auto& rule : rules)
        SendMessageW(appsRuleComboHandle_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(RuleName(rule).c_str()));
    if (SelectedWatchedIndex() < 0 && !rules.empty()) selectedRuleIndex_ = 0;
    SendMessageW(appsRuleComboHandle_, CB_SETCURSEL, SelectedWatchedIndex(), 0);
    EnableWindow(appsRuleComboHandle_, !rules.empty());
    EnableWindow(GetDlgItem(windowHandle_, IdTransferCatalogProgram), !rules.empty());
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

void MainWindow::ReportTransfer(size_t added, bool started)
{
    const int index = SelectedWatchedIndex();
    if (index < 0) return;
    const auto name = RuleName(app_.Configuration().watchedProcesses[static_cast<size_t>(index)]);
    const std::wstring text = added == 0 ? L"Already part of " + name + L"."
        : Plural(added, L"app", L"apps") + (started ? L" will start with " : L" will close when ") + name +
            (started ? L"." : L" starts.");
    SetWindowTextW(appsFeedbackHandle_, text.c_str());
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
    StartSourceRefresh();
}

void MainWindow::SyncSourceRefreshUi()
{
    const bool busy = sourceTasks_[sourceTabIndex_].Running();
    SetWindowTextW(detectSourceButtonHandle_, busy ? L"Loading..." :
        sourceTabIndex_ == 0 ? L"Refresh installed apps" : L"Refresh processes");
    EnableWindow(detectSourceButtonHandle_, !busy);
    EnableWindow(addCatalogButtonHandle_, sourceTabIndex_ == 0 && !sourceTasks_[0].Running());
    EnableWindow(removeCatalogButtonHandle_, sourceTabIndex_ == 0 && !sourceTasks_[0].Running());
}

void MainWindow::StartSourceRefresh()
{
    const int source = sourceTabIndex_;
    if (sourceTasks_[source].Running()) { SyncSourceRefreshUi(); return; }
    std::wstring watched;
    const int selected = SelectedWatchedIndex();
    if (selected >= 0 && selected < static_cast<int>(app_.Configuration().watchedProcesses.size()))
        watched = app_.Configuration().watchedProcesses[selected].processName;
    auto catalog = app_.Configuration().catalogPrograms;
    if (!SetTimer(windowHandle_, kSourceRefreshTimer, 100, nullptr))
    {
        SetWindowTextW(detectSourceButtonHandle_, L"Retry refresh");
        return;
    }
    const bool started = sourceTasks_[source].Start(
        [source, watched = std::move(watched), catalog = std::move(catalog)](const std::atomic_bool& cancelled)
    {
        SourceResult result;
        if (source == 1) CaptureRunningProcesses(result, watched, cancelled);
        else if (source == 2) CaptureDetectedProcesses(result, watched, cancelled);
        else
        {
            std::unordered_set<std::wstring> seen;
            const auto startMenuPrograms = EnumerateStartMenuPrograms(cancelled);
            const auto appPaths = EnumerateAppPaths();
            if (cancelled) return result;
            for (const auto& program : catalog)
            {
                if (cancelled) return result;
                std::error_code error;
                if (program.manuallyAdded && !program.filePath.empty() && std::filesystem::exists(program.filePath, error))
                    TryAppendCatalogProgram(result.programs, seen, program.displayName, program.filePath, true);
            }
            for (const auto& candidate : kCatalogPathCandidates)
            {
                if (cancelled) return result;
                const auto path = ExpandEnvironmentPath(candidate.path);
                std::error_code error;
                if (!path.empty() && std::filesystem::exists(path, error))
                    TryAppendCatalogProgram(result.programs, seen, candidate.displayName, path);
            }
            for (const auto& app : EnumerateInstalledApps())
            {
                if (cancelled) return result;
                TryAppendCatalogProgram(result.programs, seen, app.name,
                    InstalledAppExecutablePath(app, startMenuPrograms, appPaths));
            }
        }
        return result;
    });
    SyncSourceRefreshUi();
    if (!started) SetWindowTextW(detectSourceButtonHandle_, L"Retry refresh");
}

void MainWindow::PollSourceRefresh()
{
    for (int source = 0; source < 3; ++source)
    {
        std::optional<SourceResult> result;
        if (!sourceTasks_[source].Poll(result)) continue;
        if (result)
        {
            if (source == 0)
            {
                app_.Configuration().catalogPrograms = std::move(result->programs);
                SyncCatalogProgramsFromConfiguration();
                SaveConfiguration();
            }
            else if (source == 1) runningProcesses_ = std::move(result->running);
            else detectedProcesses_ = std::move(result->detected);
            if (sourceTabIndex_ == source) PopulateCatalogPrograms();
        }
        SyncSourceRefreshUi();
        if (!result && source == sourceTabIndex_)
            SetWindowTextW(detectSourceButtonHandle_, L"Retry refresh");
    }
    if (std::none_of(sourceTasks_.begin(), sourceTasks_.end(), [](const auto& task) { return task.Running(); }))
        KillTimer(windowHandle_, kSourceRefreshTimer);
}

void MainWindow::PopulateCatalogPrograms()
{
    if (sourceTabIndex_ == 2)
    {
        PopulateDetectedProcesses();
        return;
    }
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
        if (program.filePath.empty()) continue;
        if (!ContainsInsensitive(program.displayName, searchText) && !ContainsInsensitive(program.filePath, searchText))
        {
            continue;
        }

        AddListViewRow(catalogListHandle_, {L"", program.displayName, program.filePath},
            static_cast<LPARAM>(index), ProgramIconIndex(program.filePath));
    }
}

void MainWindow::CaptureRunningProcesses(SourceResult& result, const std::wstring& watchedProcessName, const std::atomic_bool& cancelled)
{
    auto& runningProcesses_ = result.running;
    runningProcesses_.clear();
    std::unordered_map<std::wstring, size_t> seenProcesses;
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

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (cancelled) break;
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

            const auto [existing, inserted] = seenProcesses.emplace(key, runningProcesses_.size());
            const size_t processIndex = existing->second;
            if (inserted) runningProcesses_.push_back(std::move(item));
            else
            {
                auto& combined = runningProcesses_[processIndex];
                combined.memoryUsageBytes += item.memoryUsageBytes;
                combined.hasMemoryUsage = combined.hasMemoryUsage || item.hasMemoryUsage;
            }

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
                item.cpuUsagePercent += std::clamp(
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
            L"",
            process.displayName,
            process.executablePath.empty() ? L"Path unavailable" : process.executablePath,
            FormatCpuUsage(process.cpuUsagePercent, process.hasCpuUsage),
            FormatMemoryUsage(process.memoryUsageBytes, process.hasMemoryUsage)},
            static_cast<LPARAM>(index), ProgramIconIndex(process.executablePath));
    }
}

void MainWindow::CaptureDetectedProcesses(SourceResult& result, const std::wstring& watchedProcessName, const std::atomic_bool& cancelled)
{
    auto& runningProcesses_ = result.running;
    auto& detectedProcesses_ = result.detected;
    struct Measurements { double cpuTotal{}; unsigned count{}; unsigned long long memory{}; };
    std::unordered_map<std::wstring, Measurements> measurements;
    // Three separate process-time intervals reduce the chance of rating a single idle instant.
    for (int sample = 0; sample < 3; ++sample)
    {
        if (cancelled) return;
        CaptureRunningProcesses(result, watchedProcessName, cancelled);
        for (const auto& process : runningProcesses_)
        {
            std::wstring key = process.processName + L"|" + process.executablePath;
            std::transform(key.begin(), key.end(), key.begin(), ::towlower);
            auto& value = measurements[key];
            if (process.hasCpuUsage) { value.cpuTotal += process.cpuUsagePercent; ++value.count; }
            if (process.hasMemoryUsage) value.memory = process.memoryUsageBytes;
        }
    }

    detectedProcesses_.clear();
    if (cancelled) return;
    const auto installedApps = EnumerateInstalledApps();
    std::unordered_set<std::wstring> foundNames;
    for (const auto& candidate : kDetectedProcessCandidates)
    {
        if (cancelled) return;
        const auto running = std::find_if(runningProcesses_.begin(), runningProcesses_.end(), [&candidate](const RunningProcessEntry& item)
        {
            return _wcsicmp(item.processName.c_str(), candidate.executable) == 0;
        });
        std::wstring path = running == runningProcesses_.end() ? L"" : running->executablePath;
        if (path.empty())
        {
            for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE})
            {
                const std::wstring key = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") + candidate.executable;
                wchar_t registryPath[2048]{};
                DWORD size = sizeof(registryPath);
                if (RegGetValueW(root, key.c_str(), nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                    nullptr, registryPath, &size) == ERROR_SUCCESS)
                {
                    std::error_code error;
                    const auto expanded = ExpandEnvironmentPath(registryPath);
                    if (std::filesystem::is_regular_file(expanded, error)) { path = expanded; break; }
                }
            }
        }
        if (path.empty() && candidate.fallbackPath[0] != L'\0')
        {
            const auto fallback = ExpandEnvironmentPath(candidate.fallbackPath);
            std::error_code error;
            if (std::filesystem::is_regular_file(fallback, error)) path = fallback;
        }
        if (path.empty()) path = InstalledExecutablePath(installedApps, candidate);
        if (running == runningProcesses_.end() && path.empty()) continue;
        std::wstring nameKey = candidate.name;
        std::transform(nameKey.begin(), nameKey.end(), nameKey.begin(), ::towlower);
        if (!foundNames.insert(nameKey).second) continue;

        DetectedProcessEntry item;
        item.displayName = candidate.name;
        item.processName = candidate.executable;
        item.executablePath = path;
        item.running = running != runningProcesses_.end();
        item.verifiedRunningPath = item.running && !running->executablePath.empty();
        item.allowStop = candidate.allowStop;
        item.effect = L"Unknown - no measurements";
        if (!item.allowStop) item.effect = L"Unknown - VPN connection";
        else if (item.running)
        {
            std::wstring processKey = running->processName + L"|" + running->executablePath;
            std::transform(processKey.begin(), processKey.end(), processKey.begin(), ::towlower);
            const auto measurement = measurements.find(processKey);
            if (measurement != measurements.end() && measurement->second.count >= 2)
            {
                const double cpu = measurement->second.cpuTotal / measurement->second.count;
                if (cpu >= 3.0) item.effect = L"High - CPU active";
                else if (cpu >= 0.5) item.effect = L"Medium - CPU active";
                else if (cpu >= 0.1 && std::wstring(candidate.category) == L"Overlay") item.effect = L"Medium - overlay active";
                else item.effect = L"Low - CPU currently quiet";
            }
        }
        detectedProcesses_.push_back(std::move(item));
    }
}

void MainWindow::PopulateDetectedProcesses()
{
    ListView_DeleteAllItems(catalogListHandle_);
    wchar_t searchBuffer[256]{};
    GetWindowTextW(catalogSearchHandle_, searchBuffer, static_cast<int>(std::size(searchBuffer)));
    const std::wstring searchText(searchBuffer);
    for (size_t index = 0; index < detectedProcesses_.size(); ++index)
    {
        const auto& process = detectedProcesses_[index];
        if (!ContainsInsensitive(process.displayName, searchText) &&
            !ContainsInsensitive(process.executablePath, searchText)) continue;
        AddListViewRow(catalogListHandle_, {L"",
            process.displayName,
            process.executablePath.empty() ? L"Path unavailable" : process.executablePath,
            process.running ? L"Running" : L"Installed, inactive", process.effect},
            static_cast<LPARAM>(index), ProgramIconIndex(process.executablePath));
    }
}

void MainWindow::SwitchSourceTab()
{
    sourceTabIndex_ = TabCtrl_GetCurSel(sourceTabsHandle_);
    const bool runningProcesses = sourceTabIndex_ == 1;
    SetWindowTextW(detectSourceButtonHandle_, sourceTabIndex_ == 0 ? L"Refresh installed apps" : L"Refresh processes");
    SetWindowTextW(GetDlgItem(windowHandle_, IdTransferCatalogProgram),
        sourceTabIndex_ == 0 ? L"Start with rule" : L"Close with rule");
    SetWindowTextW(appsFeedbackHandle_, L"");
    EnableWindow(addCatalogButtonHandle_, sourceTabIndex_ == 0);
    EnableWindow(removeCatalogButtonHandle_, sourceTabIndex_ == 0);
    SendMessageW(catalogSearchHandle_, EM_SETCUEBANNER, FALSE,
        reinterpret_cast<LPARAM>(sourceTabIndex_ == 0 ? L"Search apps" : L"Search processes"));

    if (runningProcesses)
    {
        ConfigureListView(catalogListHandle_, {{L"", 0}, {L"Name", 2}, {L"Path", 5}, {L"CPU", 1}, {L"Memory", 2}});
        PopulateRunningProcesses();
        StartSourceRefresh();
    }
    else if (sourceTabIndex_ == 2)
    {
        ConfigureListView(catalogListHandle_, {{L"", 0}, {L"Name", 2}, {L"Path", 4}, {L"Status", 2}, {L"Potential effect", 3}});
        PopulateDetectedProcesses();
        StartSourceRefresh();
    }
    else
    {
        ConfigureListView(catalogListHandle_, {{L"", 0}, {L"Name", 2}, {L"Path", 5}});
        PopulateCatalogPrograms();
    }
    SyncSourceRefreshUi();
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
        app_.Monitor().UpdateConfiguration(app_.Configuration());
        app_.Monitor().Start();
    }

    SyncMonitoringState();
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

void MainWindow::AddSelectedCatalogProgram()
{
    const int watchedIndex = SelectedWatchedIndex();
    if (watchedIndex < 0)
    {
        MessageBoxW(windowHandle_, L"Add a rule first.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const auto selectedIndices = SelectedSourceIndices(catalogListHandle_);
    if (selectedIndices.empty())
    {
        MessageBoxW(windowHandle_, L"Select one or more detected apps first.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
        return;
    }

    auto& programs = app_.Configuration().watchedProcesses[static_cast<size_t>(watchedIndex)].programsToLaunch;
    size_t added = 0;
    size_t unavailable = 0;
    for (const auto index : selectedIndices)
    {
        if (index >= detectedPrograms_.size()) continue;
        const auto& detectedProgram = detectedPrograms_[index];
        if (detectedProgram.filePath.empty()) { ++unavailable; continue; }
        const auto duplicate = std::find_if(programs.begin(), programs.end(), [&detectedProgram](const LaunchProgram& existingProgram)
        {
            return _wcsicmp(existingProgram.filePath.c_str(), detectedProgram.filePath.c_str()) == 0;
        });
        if (duplicate != programs.end()) continue;
        programs.push_back({detectedProgram.displayName, detectedProgram.filePath});
        ++added;
    }
    ReportTransfer(added, true);
    if (added != 0) SaveConfiguration();
    if (unavailable != 0)
        MessageBoxW(windowHandle_, L"Windows did not provide an executable path for one or more selected apps. Add those manually with the + button if you know their EXE files.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
}

void MainWindow::RemoveSelectedCatalogProgram()
{
    const auto selectedIndices = SelectedSourceIndices(catalogListHandle_);
    if (selectedIndices.empty())
    {
        MessageBoxW(windowHandle_, L"Select one or more detected apps first.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::unordered_set<std::wstring> selectedKeys;
    for (const auto index : selectedIndices)
        if (index < detectedPrograms_.size())
            selectedKeys.insert(ToLowerCopy(detectedPrograms_[index].displayName) + L"|" + ToLowerCopy(detectedPrograms_[index].filePath));
    auto& catalogPrograms = app_.Configuration().catalogPrograms;
    catalogPrograms.erase(
        std::remove_if(
            catalogPrograms.begin(),
            catalogPrograms.end(),
            [&selectedKeys](const CatalogProgram& program)
            {
                return selectedKeys.contains(ToLowerCopy(program.displayName) + L"|" + ToLowerCopy(program.filePath));
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
    auto& rules = app_.Configuration().watchedProcesses;
    rules.push_back(rule);
    SaveConfiguration();
    PopulateRuleCombo();
    OpenRule(static_cast<int>(rules.size()) - 1);
}

void MainWindow::TransferSelectedSource()
{
    if (sourceTabIndex_ == 2)
    {
        AddSelectedDetectedProcess();
    }
    else if (sourceTabIndex_ == 1)
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
        MessageBoxW(windowHandle_, L"Add a rule first.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const auto selectedIndices = SelectedSourceIndices(catalogListHandle_);
    if (selectedIndices.empty())
    {
        MessageBoxW(windowHandle_, L"Select one or more running processes first.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
        return;
    }

    auto& rule = app_.Configuration().watchedProcesses[static_cast<size_t>(watchedIndex)];
    size_t added = 0;
    for (const auto index : selectedIndices)
    {
        if (index >= runningProcesses_.size()) continue;
        const auto& selected = runningProcesses_[index];
        if (_wcsicmp(std::filesystem::path(selected.processName).stem().c_str(),
                std::filesystem::path(rule.processName).stem().c_str()) == 0)
        {
            continue;
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
        if (duplicate != rule.processesToStop.end()) continue;
        ProcessStopAction action;
        action.displayName = selected.displayName;
        action.processName = selected.processName;
        action.executablePath = selected.executablePath;
        rule.processesToStop.push_back(std::move(action));
        ++added;
    }
    ReportTransfer(added, false);
    if (added != 0) SaveConfiguration();
}

void MainWindow::AddSelectedDetectedProcess()
{
    const int watchedIndex = SelectedWatchedIndex();
    if (watchedIndex < 0) return;
    auto& rule = app_.Configuration().watchedProcesses[static_cast<size_t>(watchedIndex)];
    size_t added = 0;
    for (const auto index : SelectedSourceIndices(catalogListHandle_))
    {
        if (index >= detectedProcesses_.size()) continue;
        const auto& selected = detectedProcesses_[index];
        if (!selected.allowStop || _wcsicmp(std::filesystem::path(selected.processName).stem().c_str(),
                std::filesystem::path(rule.processName).stem().c_str()) == 0) continue;
        const auto duplicate = std::find_if(rule.processesToStop.begin(), rule.processesToStop.end(), [&selected](const ProcessStopAction& action)
        {
            return _wcsicmp(std::filesystem::path(action.processName).stem().c_str(),
                std::filesystem::path(selected.processName).stem().c_str()) == 0;
        });
        if (duplicate != rule.processesToStop.end()) continue;
        ProcessStopAction action;
        action.displayName = selected.displayName;
        action.processName = selected.processName;
        action.executablePath = selected.executablePath;
        action.gracefulCloseFirst = true;
        action.forceAfterMilliseconds = 3000;
        action.restartAfterWatchProcessEnds = false;
        rule.processesToStop.push_back(std::move(action));
        ++added;
    }
    ReportTransfer(added, false);
    if (added != 0) SaveConfiguration();
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

    app_.Configuration().catalogPrograms.push_back({program.displayName, program.filePath, true});
    SyncCatalogProgramsFromConfiguration();
    PopulateCatalogPrograms();
    SaveConfiguration();
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
    PopulateRuleCombo();
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

INT_PTR CALLBACK MainWindow::SettingsDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
    const auto setChecked = [dialog](int id, bool value)
    {
        CheckDlgButton(dialog, id, value ? BST_CHECKED : BST_UNCHECKED);
    };
    const auto isChecked = [dialog](int id) { return IsDlgButtonChecked(dialog, id) == BST_CHECKED; };
    const auto load = [&](const AppConfiguration& config)
    {
        setChecked(IDC_SETTINGS_MINIMIZE_TO_TRAY, config.minimizeToTray);
        setChecked(IDC_SETTINGS_CLOSE_TO_TRAY, config.closeToTray);
        setChecked(IDC_SETTINGS_START_IN_TRAY, config.startInTray);
        setChecked(IDC_SETTINGS_START_WITH_WINDOWS, config.startWithWindows);
        setChecked(IDC_SETTINGS_START_MONITORING, config.startMonitoringOnLaunch);
        setChecked(IDC_SETTINGS_CHECK_UPDATES, config.checkForUpdatesOnStartup);
        setChecked(IDC_SETTINGS_START_AS_ADMIN, config.startAsAdministrator);
        setChecked(IDC_SETTINGS_USE_ETW, config.useEtw);
    };
    if (message == WM_INITDIALOG)
    {
        SetWindowLongPtrW(dialog, GWLP_USERDATA, lParam);
        self = reinterpret_cast<MainWindow*>(lParam);
        load(self->app_.Configuration());
        UiTheme::Apply(dialog);
        InitializeMpoControls(dialog);
        return FALSE;
    }
    if (message != WM_COMMAND || !self) return FALSE;

    const int id = LOWORD(wParam);
    if (id == IDC_SETTINGS_CHECK_NOW)
    {
        self->StartUpdateCheck(true);
        return TRUE;
    }
    if (HandleMpoCommand(dialog, id)) return TRUE;
    if (id < IDC_SETTINGS_MINIMIZE_TO_TRAY || id > IDC_SETTINGS_USE_ETW || HIWORD(wParam) != BN_CLICKED) return FALSE;

    // ETW needs administrator rights, so the two options move together.
    if (id == IDC_SETTINGS_USE_ETW && isChecked(IDC_SETTINGS_USE_ETW)) setChecked(IDC_SETTINGS_START_AS_ADMIN, true);
    if (id == IDC_SETTINGS_START_AS_ADMIN && !isChecked(IDC_SETTINGS_START_AS_ADMIN)) setChecked(IDC_SETTINGS_USE_ETW, false);
    auto& config = self->app_.Configuration();
    config.minimizeToTray = isChecked(IDC_SETTINGS_MINIMIZE_TO_TRAY);
    config.closeToTray = isChecked(IDC_SETTINGS_CLOSE_TO_TRAY);
    config.startInTray = isChecked(IDC_SETTINGS_START_IN_TRAY);
    config.startWithWindows = isChecked(IDC_SETTINGS_START_WITH_WINDOWS);
    config.startMonitoringOnLaunch = isChecked(IDC_SETTINGS_START_MONITORING);
    config.checkForUpdatesOnStartup = isChecked(IDC_SETTINGS_CHECK_UPDATES);
    config.useEtw = isChecked(IDC_SETTINGS_USE_ETW);
    config.startAsAdministrator = config.useEtw || isChecked(IDC_SETTINGS_START_AS_ADMIN);
    self->SaveConfiguration();
    load(config); // A declined startup-task change is reverted by SaveConfiguration.
    return TRUE;
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
    const auto count = static_cast<int>(app_.Configuration().watchedProcesses.size());
    return selectedRuleIndex_ >= 0 && selectedRuleIndex_ < count ? selectedRuleIndex_ : -1;
}
