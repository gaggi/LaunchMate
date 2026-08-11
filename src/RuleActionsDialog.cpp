#include "RuleActionsDialog.h"

#include "ListViewHelpers.h"
#include "resource.h"
#include "TabHost.h"

#include <commctrl.h>
#include <commdlg.h>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <string>

namespace
{
    std::wstring GetText(HWND dialog, int id, int maximum = 4096)
    {
        std::wstring value(static_cast<size_t>(maximum), L'\0');
        const int length = GetDlgItemTextW(dialog, id, value.data(), maximum);
        value.resize(static_cast<size_t>(length));
        return value;
    }

    int GetNumber(HWND dialog, int id, int fallback = 0)
    {
        BOOL translated = FALSE;
        const UINT value = GetDlgItemInt(dialog, id, &translated, FALSE);
        return translated ? static_cast<int>(value) : fallback;
    }

    HWND FindActionControl(HWND dialog, int id)
    {
        if (HWND control = GetDlgItem(dialog, id)) return control;
        return GetDlgItem(GetDlgItem(dialog, IDC_ACTION_TAB), id);
    }

    bool IsActionCheckboxChecked(HWND dialog, int id)
    {
        return SendMessageW(FindActionControl(dialog, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    LRESULT SendActionMessage(HWND dialog, int id, UINT message, WPARAM wParam = 0, LPARAM lParam = 0)
    {
        return SendMessageW(FindActionControl(dialog, id), message, wParam, lParam);
    }

    int GetActionNumber(HWND dialog, int id, int fallback = 0)
    {
        wchar_t value[32]{};
        GetWindowTextW(FindActionControl(dialog, id), value, static_cast<int>(std::size(value)));
        wchar_t* end = nullptr;
        const unsigned long parsed = std::wcstoul(value, &end, 10);
        return end != value && *end == L'\0' ? static_cast<int>(parsed) : fallback;
    }

    void HostRuleActionControls(HWND dialog)
    {
        HWND tab = GetDlgItem(dialog, IDC_ACTION_TAB);
        HostControlsInTab(dialog, tab, {
            GetDlgItem(dialog, IDC_ACTION_LIST),
            GetDlgItem(dialog, IDC_ACTION_ADD),
            GetDlgItem(dialog, IDC_ACTION_EDIT),
            GetDlgItem(dialog, IDC_ACTION_REMOVE),
            GetDlgItem(dialog, IDC_ACTION_MONITOR_LABEL),
            GetDlgItem(dialog, IDC_ACTION_MONITOR_COMBO),
            GetDlgItem(dialog, IDC_ACTION_MONITOR_DELAY_LABEL),
            GetDlgItem(dialog, IDC_ACTION_MONITOR_DELAY),
            GetDlgItem(dialog, IDC_ACTION_MONITOR_RESTORE),
            GetDlgItem(dialog, IDC_ACTION_MONITOR_RESTORE_DELAY_LABEL),
            GetDlgItem(dialog, IDC_ACTION_MONITOR_RESTORE_DELAY)});
    }

    std::wstring PickExecutable(HWND owner)
    {
        wchar_t path[MAX_PATH]{};
        OPENFILENAMEW info{};
        info.lStructSize = sizeof(info);
        info.hwndOwner = owner;
        info.lpstrFilter = L"Programs (*.exe)\0*.exe\0All files (*.*)\0*.*\0";
        info.lpstrFile = path;
        info.nMaxFile = MAX_PATH;
        info.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        return GetOpenFileNameW(&info) ? std::wstring(path) : std::wstring{};
    }

    void SetDefaultName(HWND dialog, const std::wstring& path)
    {
        if (GetText(dialog, IDC_ACTION_NAME).empty())
        {
            SetDlgItemTextW(dialog, IDC_ACTION_NAME, std::filesystem::path(path).stem().c_str());
        }
    }

    template<typename T>
    struct ItemDialogState
    {
        T* item{};
        bool accepted{};
    };

    INT_PTR CALLBACK StartActionProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* state = reinterpret_cast<ItemDialogState<LaunchProgram>*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
        if (message == WM_INITDIALOG)
        {
            state = reinterpret_cast<ItemDialogState<LaunchProgram>*>(lParam);
            SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            const auto& item = *state->item;
            SetDlgItemTextW(dialog, IDC_ACTION_NAME, item.displayName.c_str());
            SetDlgItemTextW(dialog, IDC_ACTION_PATH, item.filePath.c_str());
            SetDlgItemTextW(dialog, IDC_ACTION_ARGUMENTS, item.arguments.c_str());
            SetDlgItemInt(dialog, IDC_ACTION_DELAY, item.waitTimeMilliseconds, FALSE);
            CheckDlgButton(dialog, IDC_ACTION_CLOSE, item.closeWhenGameStops ? BST_CHECKED : BST_UNCHECKED);
            SetDlgItemInt(dialog, IDC_ACTION_CLOSE_DELAY, item.closeDelayMilliseconds, FALSE);
            return TRUE;
        }
        if (message != WM_COMMAND) return FALSE;
        if (LOWORD(wParam) == IDC_ACTION_BROWSE)
        {
            const auto path = PickExecutable(dialog);
            if (!path.empty()) { SetDlgItemTextW(dialog, IDC_ACTION_PATH, path.c_str()); SetDefaultName(dialog, path); }
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK)
        {
            const auto path = GetText(dialog, IDC_ACTION_PATH);
            if (path.empty()) { MessageBoxW(dialog, L"Select a program first.", L"LaunchMate", MB_OK | MB_ICONINFORMATION); return TRUE; }
            auto& item = *state->item;
            item.displayName = GetText(dialog, IDC_ACTION_NAME);
            item.filePath = path;
            item.arguments = GetText(dialog, IDC_ACTION_ARGUMENTS);
            item.waitTimeMilliseconds = GetNumber(dialog, IDC_ACTION_DELAY);
            item.closeWhenGameStops = IsDlgButtonChecked(dialog, IDC_ACTION_CLOSE) == BST_CHECKED;
            item.closeDelayMilliseconds = GetNumber(dialog, IDC_ACTION_CLOSE_DELAY);
            if (item.displayName.empty()) item.displayName = std::filesystem::path(path).stem().wstring();
            state->accepted = true;
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
        return FALSE;
    }

    INT_PTR CALLBACK StopActionProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* state = reinterpret_cast<ItemDialogState<ProcessStopAction>*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
        if (message == WM_INITDIALOG)
        {
            state = reinterpret_cast<ItemDialogState<ProcessStopAction>*>(lParam);
            SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            const auto& item = *state->item;
            SetDlgItemTextW(dialog, IDC_ACTION_NAME, item.displayName.c_str());
            SetDlgItemTextW(dialog, IDC_ACTION_PROCESS, item.processName.c_str());
            SetDlgItemTextW(dialog, IDC_ACTION_PATH, item.executablePath.c_str());
            CheckDlgButton(dialog, IDC_ACTION_GRACEFUL, item.gracefulCloseFirst ? BST_CHECKED : BST_UNCHECKED);
            SetDlgItemInt(dialog, IDC_ACTION_FORCE_DELAY, item.forceAfterMilliseconds, FALSE);
            CheckDlgButton(dialog, IDC_ACTION_RESTART, item.restartAfterWatchProcessEnds ? BST_CHECKED : BST_UNCHECKED);
            SetDlgItemInt(dialog, IDC_ACTION_RESTART_DELAY, item.restartDelayMilliseconds, FALSE);
            EnableWindow(GetDlgItem(dialog, IDC_ACTION_RESTART_DELAY), item.restartAfterWatchProcessEnds);
            return TRUE;
        }
        if (message != WM_COMMAND) return FALSE;
        if (LOWORD(wParam) == IDC_ACTION_BROWSE)
        {
            const auto path = PickExecutable(dialog);
            if (!path.empty())
            {
                SetDlgItemTextW(dialog, IDC_ACTION_PATH, path.c_str());
                SetDlgItemTextW(dialog, IDC_ACTION_PROCESS, std::filesystem::path(path).filename().c_str());
                SetDefaultName(dialog, path);
            }
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_ACTION_RESTART)
        {
            EnableWindow(GetDlgItem(dialog, IDC_ACTION_RESTART_DELAY), IsDlgButtonChecked(dialog, IDC_ACTION_RESTART) == BST_CHECKED);
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK)
        {
            auto process = GetText(dialog, IDC_ACTION_PROCESS);
            const auto path = GetText(dialog, IDC_ACTION_PATH);
            if (process.empty() && !path.empty()) process = std::filesystem::path(path).filename().wstring();
            if (process.empty()) { MessageBoxW(dialog, L"Enter a process name or select a program.", L"LaunchMate", MB_OK | MB_ICONINFORMATION); return TRUE; }
            const bool restart = IsDlgButtonChecked(dialog, IDC_ACTION_RESTART) == BST_CHECKED;
            if (restart && path.empty())
            {
                MessageBoxW(dialog, L"Select the program path so LaunchMate can restart it later.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            auto& item = *state->item;
            item.displayName = GetText(dialog, IDC_ACTION_NAME);
            item.processName = process;
            item.executablePath = path;
            item.gracefulCloseFirst = IsDlgButtonChecked(dialog, IDC_ACTION_GRACEFUL) == BST_CHECKED;
            item.forceAfterMilliseconds = GetNumber(dialog, IDC_ACTION_FORCE_DELAY, 3000);
            item.restartAfterWatchProcessEnds = restart;
            item.restartDelayMilliseconds = GetNumber(dialog, IDC_ACTION_RESTART_DELAY);
            if (item.displayName.empty()) item.displayName = std::filesystem::path(process).stem().wstring();
            state->accepted = true;
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
        return FALSE;
    }

    INT_PTR CALLBACK HomeActionProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* state = reinterpret_cast<ItemDialogState<HomeAssistantAction>*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
        if (message == WM_INITDIALOG)
        {
            state = reinterpret_cast<ItemDialogState<HomeAssistantAction>*>(lParam);
            SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            const auto& item = *state->item;
            SetDlgItemTextW(dialog, IDC_ACTION_NAME, item.displayName.c_str());
            SetDlgItemTextW(dialog, IDC_ACTION_URL, item.webhookUrl.c_str());
            SetDlgItemTextW(dialog, IDC_ACTION_PAYLOAD, item.jsonPayload.c_str());
            SetDlgItemInt(dialog, IDC_ACTION_DELAY, item.waitTimeMilliseconds, FALSE);
            return TRUE;
        }
        if (message != WM_COMMAND) return FALSE;
        if (LOWORD(wParam) == IDOK)
        {
            const auto url = GetText(dialog, IDC_ACTION_URL);
            if (!url.starts_with(L"http://") && !url.starts_with(L"https://"))
            {
                MessageBoxW(dialog, L"Enter a complete HTTP or HTTPS webhook URL.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            auto& item = *state->item;
            item.displayName = GetText(dialog, IDC_ACTION_NAME);
            item.webhookUrl = url;
            item.jsonPayload = GetText(dialog, IDC_ACTION_PAYLOAD);
            if (item.jsonPayload.empty()) item.jsonPayload = L"{}";
            item.waitTimeMilliseconds = GetNumber(dialog, IDC_ACTION_DELAY);
            if (item.displayName.empty()) item.displayName = L"Home Assistant";
            state->accepted = true;
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
        return FALSE;
    }

    template<typename T>
    bool ShowItemDialog(HINSTANCE instance, HWND owner, int resource, DLGPROC procedure, T& item)
    {
        ItemDialogState<T> state{&item};
        DialogBoxParamW(instance, MAKEINTRESOURCEW(resource), owner, procedure, reinterpret_cast<LPARAM>(&state));
        return state.accepted;
    }

    struct ActionsState
    {
        HINSTANCE instance{};
        WatchedProcessRule workingRule;
        WatchedProcessRule* destination{};
        const std::vector<MonitorPowerSetup>* monitorSetups{};
        int tab{};
        bool accepted{};
    };

    int SelectedItem(HWND dialog)
    {
        return SelectedListViewRow(FindActionControl(dialog, IDC_ACTION_LIST));
    }

    void StoreMonitorSettings(HWND dialog, ActionsState& state)
    {
        const int selected = static_cast<int>(SendActionMessage(dialog, IDC_ACTION_MONITOR_COMBO, CB_GETCURSEL));
        state.workingRule.monitorPowerSetupName.clear();
        if (selected > 0 && state.monitorSetups && static_cast<size_t>(selected - 1) < state.monitorSetups->size())
        {
            state.workingRule.monitorPowerSetupName = (*state.monitorSetups)[static_cast<size_t>(selected - 1)].name;
        }
        state.workingRule.restoreMonitorPowerSetupOnExit =
            IsActionCheckboxChecked(dialog, IDC_ACTION_MONITOR_RESTORE);
        state.workingRule.monitorPowerSetupDelayMilliseconds = GetActionNumber(dialog, IDC_ACTION_MONITOR_DELAY);
        state.workingRule.restoreMonitorPowerSetupDelayMilliseconds = GetActionNumber(dialog, IDC_ACTION_MONITOR_RESTORE_DELAY);
    }

    void ShowTabControls(HWND dialog, const ActionsState& state)
    {
        const int listCommand = state.tab == 3 ? SW_HIDE : SW_SHOW;
        const int monitorCommand = state.tab == 3 ? SW_SHOW : SW_HIDE;
        for (const int id : {IDC_ACTION_LIST, IDC_ACTION_ADD, IDC_ACTION_EDIT, IDC_ACTION_REMOVE})
        {
            ShowWindow(FindActionControl(dialog, id), listCommand);
        }
        for (const int id : {
            IDC_ACTION_MONITOR_LABEL,
            IDC_ACTION_MONITOR_COMBO,
            IDC_ACTION_MONITOR_DELAY_LABEL,
            IDC_ACTION_MONITOR_DELAY,
            IDC_ACTION_MONITOR_RESTORE,
            IDC_ACTION_MONITOR_RESTORE_DELAY_LABEL,
            IDC_ACTION_MONITOR_RESTORE_DELAY})
        {
            ShowWindow(FindActionControl(dialog, id), monitorCommand);
        }
    }

    void RefreshActions(HWND dialog, ActionsState& state)
    {
        HWND list = FindActionControl(dialog, IDC_ACTION_LIST);
        if (state.tab == 0)
        {
            ConfigureListView(list, {{L"Name", 2}, {L"Program", 4}, {L"Arguments", 3}, {L"Start delay", 2}, {L"Stop delay", 2}});
            for (const auto& item : state.workingRule.programsToLaunch)
            {
                AddListViewRow(list, {
                    item.displayName,
                    item.filePath,
                    item.arguments,
                    std::to_wstring(item.waitTimeMilliseconds) + L" ms",
                    std::to_wstring(item.closeDelayMilliseconds) + L" ms"});
            }
        }
        else if (state.tab == 1)
        {
            ConfigureListView(list, {{L"Name", 2}, {L"Process", 3}, {L"Close mode", 3}, {L"Restart", 3}});
            for (const auto& item : state.workingRule.processesToStop)
            {
                const std::wstring restart = item.restartAfterWatchProcessEnds
                    ? std::to_wstring(item.restartDelayMilliseconds) + L" ms after exit"
                    : L"No";
                AddListViewRow(list, {
                    item.displayName,
                    item.processName,
                    item.gracefulCloseFirst ? L"Graceful, then force" : L"Force",
                    restart});
            }
        }
        else if (state.tab == 2)
        {
            ConfigureListView(list, {{L"Name", 2}, {L"Webhook URL", 6}, {L"Delay", 2}});
            for (const auto& item : state.workingRule.homeAssistantActions)
            {
                AddListViewRow(list, {
                    item.displayName,
                    item.webhookUrl,
                    std::to_wstring(item.waitTimeMilliseconds) + L" ms"});
            }
        }
        else
        {
            ListView_DeleteAllItems(list);
        }
        ShowTabControls(dialog, state);
    }

    bool EditAction(HWND dialog, ActionsState& state, bool add)
    {
        const int selected = SelectedItem(dialog);
        if (!add && selected < 0) return false;
        bool changed = false;
        if (state.tab == 0)
        {
            LaunchProgram item;
            if (!add) item = state.workingRule.programsToLaunch[static_cast<size_t>(selected)];
            changed = ShowItemDialog(state.instance, dialog, IDD_START_ACTION, StartActionProc, item);
            if (changed) { if (add) state.workingRule.programsToLaunch.push_back(std::move(item)); else state.workingRule.programsToLaunch[static_cast<size_t>(selected)] = std::move(item); }
        }
        else if (state.tab == 1)
        {
            ProcessStopAction item;
            if (!add) item = state.workingRule.processesToStop[static_cast<size_t>(selected)];
            changed = ShowItemDialog(state.instance, dialog, IDD_STOP_ACTION, StopActionProc, item);
            if (changed) { if (add) state.workingRule.processesToStop.push_back(std::move(item)); else state.workingRule.processesToStop[static_cast<size_t>(selected)] = std::move(item); }
        }
        else if (state.tab == 2)
        {
            HomeAssistantAction item;
            if (!add) item = state.workingRule.homeAssistantActions[static_cast<size_t>(selected)];
            changed = ShowItemDialog(state.instance, dialog, IDD_HOME_ACTION, HomeActionProc, item);
            if (changed) { if (add) state.workingRule.homeAssistantActions.push_back(std::move(item)); else state.workingRule.homeAssistantActions[static_cast<size_t>(selected)] = std::move(item); }
        }
        if (changed) RefreshActions(dialog, state);
        return changed;
    }

    void RemoveAction(HWND dialog, ActionsState& state)
    {
        const int selected = SelectedItem(dialog);
        if (selected < 0) return;
        if (state.tab == 0) state.workingRule.programsToLaunch.erase(state.workingRule.programsToLaunch.begin() + selected);
        else if (state.tab == 1) state.workingRule.processesToStop.erase(state.workingRule.processesToStop.begin() + selected);
        else if (state.tab == 2) state.workingRule.homeAssistantActions.erase(state.workingRule.homeAssistantActions.begin() + selected);
        RefreshActions(dialog, state);
    }

    INT_PTR CALLBACK ActionsProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* state = reinterpret_cast<ActionsState*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
        if (message == WM_INITDIALOG)
        {
            state = reinterpret_cast<ActionsState*>(lParam);
            SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            TCITEMW tab{TCIF_TEXT};
            tab.pszText = const_cast<wchar_t*>(L"Start programs"); TabCtrl_InsertItem(GetDlgItem(dialog, IDC_ACTION_TAB), 0, &tab);
            tab.pszText = const_cast<wchar_t*>(L"Stop processes"); TabCtrl_InsertItem(GetDlgItem(dialog, IDC_ACTION_TAB), 1, &tab);
            tab.pszText = const_cast<wchar_t*>(L"Home Assistant"); TabCtrl_InsertItem(GetDlgItem(dialog, IDC_ACTION_TAB), 2, &tab);
            tab.pszText = const_cast<wchar_t*>(L"Monitor config"); TabCtrl_InsertItem(GetDlgItem(dialog, IDC_ACTION_TAB), 3, &tab);
            SendDlgItemMessageW(dialog, IDC_ACTION_MONITOR_COMBO, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Do not change displays"));
            int monitorSelection = 0;
            if (state->monitorSetups)
            {
                for (size_t index = 0; index < state->monitorSetups->size(); ++index)
                {
                    const auto& setup = (*state->monitorSetups)[index];
                    SendDlgItemMessageW(dialog, IDC_ACTION_MONITOR_COMBO, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(setup.name.c_str()));
                    if (setup.name == state->workingRule.monitorPowerSetupName) monitorSelection = static_cast<int>(index) + 1;
                }
            }
            SendDlgItemMessageW(dialog, IDC_ACTION_MONITOR_COMBO, CB_SETCURSEL, monitorSelection, 0);
            CheckDlgButton(dialog, IDC_ACTION_MONITOR_RESTORE,
                state->workingRule.restoreMonitorPowerSetupOnExit ? BST_CHECKED : BST_UNCHECKED);
            SetDlgItemInt(dialog, IDC_ACTION_MONITOR_DELAY,
                static_cast<UINT>(state->workingRule.monitorPowerSetupDelayMilliseconds), FALSE);
            SetDlgItemInt(dialog, IDC_ACTION_MONITOR_RESTORE_DELAY,
                static_cast<UINT>(state->workingRule.restoreMonitorPowerSetupDelayMilliseconds), FALSE);
            EnableWindow(GetDlgItem(dialog, IDC_ACTION_MONITOR_RESTORE_DELAY), state->workingRule.restoreMonitorPowerSetupOnExit);
            EnableWindow(GetDlgItem(dialog, IDC_ACTION_MONITOR_RESTORE_DELAY_LABEL), state->workingRule.restoreMonitorPowerSetupOnExit);
            HostRuleActionControls(dialog);
            InitializeReportListView(FindActionControl(dialog, IDC_ACTION_LIST));
            RefreshActions(dialog, *state);
            return TRUE;
        }
        if (message == WM_NOTIFY)
        {
            const auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->idFrom == IDC_ACTION_TAB && header->code == TCN_SELCHANGE)
            {
                state->tab = TabCtrl_GetCurSel(GetDlgItem(dialog, IDC_ACTION_TAB));
                RefreshActions(dialog, *state);
                return TRUE;
            }
            if (header->idFrom == IDC_ACTION_LIST && header->code == LVN_COLUMNCLICK)
            {
                const auto* column = reinterpret_cast<NMLISTVIEW*>(lParam);
                SortListViewByColumn(header->hwndFrom, column->iSubItem);
                return TRUE;
            }
            if (header->idFrom == IDC_ACTION_LIST && header->code == NM_DBLCLK)
            {
                EditAction(dialog, *state, false);
                return TRUE;
            }
        }
        if (message != WM_COMMAND) return FALSE;
        if (LOWORD(wParam) == IDC_ACTION_ADD) { EditAction(dialog, *state, true); return TRUE; }
        if (LOWORD(wParam) == IDC_ACTION_EDIT) { EditAction(dialog, *state, false); return TRUE; }
        if (LOWORD(wParam) == IDC_ACTION_REMOVE) { RemoveAction(dialog, *state); return TRUE; }
        if (LOWORD(wParam) == IDC_ACTION_MONITOR_COMBO || LOWORD(wParam) == IDC_ACTION_MONITOR_RESTORE)
        {
            if (LOWORD(wParam) == IDC_ACTION_MONITOR_RESTORE)
            {
                const bool restore = IsActionCheckboxChecked(dialog, IDC_ACTION_MONITOR_RESTORE);
                EnableWindow(FindActionControl(dialog, IDC_ACTION_MONITOR_RESTORE_DELAY), restore);
                EnableWindow(FindActionControl(dialog, IDC_ACTION_MONITOR_RESTORE_DELAY_LABEL), restore);
            }
            StoreMonitorSettings(dialog, *state);
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK) { StoreMonitorSettings(dialog, *state); *state->destination = std::move(state->workingRule); state->accepted = true; EndDialog(dialog, IDOK); return TRUE; }
        if (LOWORD(wParam) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
        return FALSE;
    }
}

bool ShowRuleActionsDialog(
    HINSTANCE instanceHandle,
    HWND owner,
    WatchedProcessRule& rule,
    const std::vector<MonitorPowerSetup>& monitorSetups)
{
    ActionsState state{instanceHandle, rule, &rule, &monitorSetups};
    DialogBoxParamW(instanceHandle, MAKEINTRESOURCEW(IDD_RULE_ACTIONS), owner, ActionsProc, reinterpret_cast<LPARAM>(&state));
    return state.accepted;
}
