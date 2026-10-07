#include "RuleActionsDialog.h"

#include "ListViewHelpers.h"
#include "IRacingPerformance.h"
#include "resource.h"
#include "ui/UiTheme.h"

#include <commctrl.h>
#include <commdlg.h>
#include <algorithm>
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

    int GetActionNumber(HWND dialog, int id, int fallback = 0)
    {
        wchar_t value[32]{};
        GetDlgItemTextW(dialog, id, value, static_cast<int>(std::size(value)));
        wchar_t* end = nullptr;
        const unsigned long parsed = std::wcstoul(value, &end, 10);
        return end != value && *end == L'\0' ? static_cast<int>(parsed) : fallback;
    }

    template<typename T>
    struct ItemDialogState
    {
        T* item{};
        bool accepted{};
    };

    INT_PTR CALLBACK HomeActionProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* state = reinterpret_cast<ItemDialogState<HomeAssistantAction>*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
        if (message == WM_INITDIALOG)
        {
            UiTheme::Apply(dialog);
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

    struct SectionState
    {
        HINSTANCE instance{};
        WatchedProcessRule* rule{};
        const std::vector<MonitorPowerSetup>* monitorSetups{};
        RuleSection section{};
        std::function<void()> changed;
        // Programmatic control updates during setup must not count as edits.
        bool initializing{true};
    };

    bool IsListSection(const SectionState& state)
    {
        return state.section == RuleSection::HomeAssistant;
    }

    int SelectedItem(HWND dialog)
    {
        return SelectedListViewRow(GetDlgItem(dialog, IDC_ACTION_LIST));
    }

    void StoreMonitorSettings(HWND dialog, SectionState& state)
    {
        auto& rule = *state.rule;
        const int selected = static_cast<int>(SendDlgItemMessageW(dialog, IDC_ACTION_MONITOR_COMBO, CB_GETCURSEL, 0, 0));
        rule.monitorPowerSetupName.clear();
        if (selected > 0 && static_cast<size_t>(selected - 1) < state.monitorSetups->size())
            rule.monitorPowerSetupName = (*state.monitorSetups)[static_cast<size_t>(selected - 1)].name;
        rule.restoreMonitorPowerSetupOnExit = IsDlgButtonChecked(dialog, IDC_ACTION_MONITOR_RESTORE) == BST_CHECKED;
        rule.monitorPowerSetupDelayMilliseconds = GetActionNumber(dialog, IDC_ACTION_MONITOR_DELAY);
        rule.restoreMonitorPowerSetupDelayMilliseconds = GetActionNumber(dialog, IDC_ACTION_MONITOR_RESTORE_DELAY);
    }

    void ShowSectionControls(HWND dialog, const SectionState& state)
    {
        const int listCommand = IsListSection(state) ? SW_SHOW : SW_HIDE;
        const int monitorCommand = state.section == RuleSection::MonitorConfig ? SW_SHOW : SW_HIDE;
        for (const int id : {IDC_ACTION_LIST, IDC_ACTION_ADD, IDC_ACTION_EDIT, IDC_ACTION_REMOVE})
            ShowWindow(GetDlgItem(dialog, id), listCommand);
        for (const int id : {
            IDC_ACTION_MONITOR_LABEL,
            IDC_ACTION_MONITOR_COMBO,
            IDC_ACTION_MONITOR_DELAY_LABEL,
            IDC_ACTION_MONITOR_DELAY,
            IDC_ACTION_MONITOR_RESTORE,
            IDC_ACTION_MONITOR_RESTORE_DELAY_LABEL,
            IDC_ACTION_MONITOR_RESTORE_DELAY,
            IDC_ACTION_MONITOR_APPLY_GROUP,
            IDC_ACTION_MONITOR_RESTORE_GROUP,
            IDC_ACTION_MONITOR_APPLY_HINT,
            IDC_ACTION_MONITOR_RESTORE_HINT})
        {
            ShowWindow(GetDlgItem(dialog, id), monitorCommand);
        }
    }

    void RefreshActions(HWND dialog, SectionState& state)
    {
        HWND list = GetDlgItem(dialog, IDC_ACTION_LIST);
        const auto& rule = *state.rule;
        if (state.section == RuleSection::HomeAssistant)
        {
            ConfigureListView(list, {{L"Name", 2}, {L"Webhook URL", 6}, {L"Delay", 2}});
            for (const auto& item : rule.homeAssistantActions)
            {
                AddListViewRow(list, {
                    item.displayName,
                    item.webhookUrl,
                    std::to_wstring(item.waitTimeMilliseconds) + L" ms"});
            }
        }
        UpdateListActionButtons(list, GetDlgItem(dialog, IDC_ACTION_EDIT), GetDlgItem(dialog, IDC_ACTION_REMOVE));
    }

    void EditAction(HWND dialog, SectionState& state, bool add)
    {
        const int selected = SelectedItem(dialog);
        if (!add && selected < 0) return;
        auto& rule = *state.rule;
        bool changed = false;
        if (state.section == RuleSection::HomeAssistant)
        {
            HomeAssistantAction item;
            if (!add) item = rule.homeAssistantActions[static_cast<size_t>(selected)];
            changed = ShowItemDialog(state.instance, dialog, IDD_HOME_ACTION, HomeActionProc, item);
            if (changed) { if (add) rule.homeAssistantActions.push_back(std::move(item)); else rule.homeAssistantActions[static_cast<size_t>(selected)] = std::move(item); }
        }
        if (!changed) return;
        RefreshActions(dialog, state);
        state.changed();
    }

    void RemoveAction(HWND dialog, SectionState& state)
    {
        const int selected = SelectedItem(dialog);
        if (selected < 0) return;
        auto& rule = *state.rule;
        if (state.section == RuleSection::HomeAssistant) rule.homeAssistantActions.erase(rule.homeAssistantActions.begin() + selected);
        RefreshActions(dialog, state);
        state.changed();
    }

    void InitializeMonitorControls(HWND dialog, const SectionState& state)
    {
        const auto& rule = *state.rule;
        SendDlgItemMessageW(dialog, IDC_ACTION_MONITOR_COMBO, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Do not change displays"));
        int selection = 0;
        for (size_t index = 0; index < state.monitorSetups->size(); ++index)
        {
            const auto& setup = (*state.monitorSetups)[index];
            SendDlgItemMessageW(dialog, IDC_ACTION_MONITOR_COMBO, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(setup.name.c_str()));
            if (setup.name == rule.monitorPowerSetupName) selection = static_cast<int>(index) + 1;
        }
        SendDlgItemMessageW(dialog, IDC_ACTION_MONITOR_COMBO, CB_SETCURSEL, selection, 0);
        CheckDlgButton(dialog, IDC_ACTION_MONITOR_RESTORE, rule.restoreMonitorPowerSetupOnExit ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(dialog, IDC_ACTION_MONITOR_DELAY, static_cast<UINT>(rule.monitorPowerSetupDelayMilliseconds), FALSE);
        SetDlgItemInt(dialog, IDC_ACTION_MONITOR_RESTORE_DELAY, static_cast<UINT>(rule.restoreMonitorPowerSetupDelayMilliseconds), FALSE);
        EnableWindow(GetDlgItem(dialog, IDC_ACTION_MONITOR_RESTORE_DELAY), rule.restoreMonitorPowerSetupOnExit);
        EnableWindow(GetDlgItem(dialog, IDC_ACTION_MONITOR_RESTORE_DELAY_LABEL), rule.restoreMonitorPowerSetupOnExit);
    }

    INT_PTR CALLBACK SectionProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* state = reinterpret_cast<SectionState*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
        if (message == WM_INITDIALOG)
        {
            state = reinterpret_cast<SectionState*>(lParam);
            SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            UiTheme::Apply(dialog);
            InitializeReportListView(GetDlgItem(dialog, IDC_ACTION_LIST));
            InitializeMonitorControls(dialog, *state);
            ShowSectionControls(dialog, *state);
            if (IsListSection(*state)) RefreshActions(dialog, *state);
            state->initializing = false;
            return FALSE;
        }
        if (!state) return FALSE;
        if (message == WM_SIZE)
        {
            // The list grows with the page; the buttons above it stay where they are.
            const HWND list = GetDlgItem(dialog, IDC_ACTION_LIST);
            RECT bounds{};
            GetWindowRect(list, &bounds);
            MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&bounds), 2);
            MoveWindow(list, bounds.left, bounds.top, LOWORD(lParam) - bounds.left, HIWORD(lParam) - bounds.top, TRUE);
            if (IsListSection(*state)) RefreshActions(dialog, *state); // Re-fits the columns.
            return TRUE;
        }
        if (message == WM_NCDESTROY)
        {
            delete state;
            SetWindowLongPtrW(dialog, GWLP_USERDATA, 0);
            return FALSE;
        }
        if (message == WM_NOTIFY)
        {
            const auto* header = reinterpret_cast<NMHDR*>(lParam);
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
            if (header->idFrom == IDC_ACTION_LIST && header->code == LVN_ITEMCHANGED)
            {
                UpdateListActionButtons(header->hwndFrom, GetDlgItem(dialog, IDC_ACTION_EDIT), GetDlgItem(dialog, IDC_ACTION_REMOVE));
                return TRUE;
            }
            if (header->idFrom == IDC_ACTION_LIST && header->code == LVN_KEYDOWN &&
                reinterpret_cast<const NMLVKEYDOWN*>(lParam)->wVKey == VK_DELETE)
            {
                RemoveAction(dialog, *state);
                return TRUE;
            }
            return FALSE;
        }
        if (message != WM_COMMAND || state->initializing) return FALSE;
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if (id == IDC_ACTION_ADD) { EditAction(dialog, *state, true); return TRUE; }
        if (id == IDC_ACTION_EDIT) { EditAction(dialog, *state, false); return TRUE; }
        if (id == IDC_ACTION_REMOVE) { RemoveAction(dialog, *state); return TRUE; }
        const bool monitorEdit = (id == IDC_ACTION_MONITOR_COMBO && code == CBN_SELCHANGE) ||
            (id == IDC_ACTION_MONITOR_RESTORE && code == BN_CLICKED) ||
            ((id == IDC_ACTION_MONITOR_DELAY || id == IDC_ACTION_MONITOR_RESTORE_DELAY) && code == EN_CHANGE);
        if (monitorEdit)
        {
            const bool restore = IsDlgButtonChecked(dialog, IDC_ACTION_MONITOR_RESTORE) == BST_CHECKED;
            EnableWindow(GetDlgItem(dialog, IDC_ACTION_MONITOR_RESTORE_DELAY), restore);
            EnableWindow(GetDlgItem(dialog, IDC_ACTION_MONITOR_RESTORE_DELAY_LABEL), restore);
            StoreMonitorSettings(dialog, *state);
            state->changed();
            return TRUE;
        }
        return FALSE;
    }

    // The performance pane keeps the power plan choice in its combo box until saved;
    // copy it into the rule after every interaction and report the change.
    struct PaneObserver
    {
        std::function<void()> changed;
    };

    LRESULT CALLBACK PaneChangeProc(HWND pane, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR data)
    {
        auto* observer = reinterpret_cast<PaneObserver*>(data);
        if (message == WM_NCDESTROY)
        {
            RemoveWindowSubclass(pane, PaneChangeProc, subclassId);
            delete observer;
            return DefSubclassProc(pane, message, wParam, lParam);
        }
        const LRESULT result = DefSubclassProc(pane, message, wParam, lParam);
        const bool interaction = message == WM_COMMAND ||
            (message == WM_NOTIFY && reinterpret_cast<NMHDR*>(lParam)->code == NM_DBLCLK);
        if (interaction)
        {
            SaveIRacingPerformancePane(pane);
            observer->changed();
        }
        return result;
    }
}

HWND CreateRuleSectionPane(
    HINSTANCE instanceHandle,
    HWND parent,
    WatchedProcessRule& rule,
    const std::vector<MonitorPowerSetup>& monitorSetups,
    RuleSection section,
    std::function<void()> changed)
{
    if (section == RuleSection::Performance)
    {
        const HWND pane = CreateIRacingPerformancePane(instanceHandle, parent, rule);
        if (pane) SetWindowSubclass(pane, PaneChangeProc, 1, reinterpret_cast<DWORD_PTR>(new PaneObserver{std::move(changed)}));
        return pane;
    }
    auto* state = new SectionState{instanceHandle, &rule, &monitorSetups, section, std::move(changed)};
    const HWND pane = CreateDialogParamW(instanceHandle, MAKEINTRESOURCEW(IDD_RULE_SECTION), parent, SectionProc,
        reinterpret_cast<LPARAM>(state));
    if (!pane) delete state;
    return pane;
}
