#include "Pages.h"

#include "ui/PageWindow.h"
#include "ui/RowEditors.h"
#include "ui/RowList.h"

#include <algorithm>
#include <filesystem>

namespace
{
    constexpr int kListId = 100;
    constexpr int kDelayId = 200;
    constexpr wchar_t kExpandGlyph = L'\uE70D';
    constexpr wchar_t kCollapseGlyph = L'\uE70E';

    enum class Row
    {
        StartHeader,
        Config,
        ApplyDelay,
        ExitHeader,
        Restore,
        RestoreDelay
    };

    class RuleDisplayPage : public PageWindow
    {
    public:
        RuleDisplayPage(const PageContext& context, WatchedProcessRule& rule)
            : PageWindow(context.headingFont, context.textFont), context_(context), rule_(rule)
        {
        }

    private:
        void OnCreate() override
        {
            list_.Create(Instance(), Handle(), kListId, HeadingFont(), TextFont());
            editors_.Attach(list_, Instance(), TextFont());
            Refresh();
        }

        void OnSize(int width, int height) override
        {
            Place(list_.Handle(), 0, 0, std::min(width, 820), height);
        }

        std::wstring RuleName() const
        {
            return rule_.displayName.empty() ? std::filesystem::path(rule_.processName).stem().wstring() : rule_.displayName;
        }

        static std::wstring DelayText(int milliseconds, const wchar_t* immediately)
        {
            return milliseconds > 0 ? L"After " + RowEditors::SecondsText(milliseconds) + L" s" : std::wstring(immediately);
        }

        void Refresh()
        {
            const bool configured = !rule_.monitorPowerSetupName.empty();
            std::vector<RowList::Row> rows(6);
            rows[static_cast<int>(Row::StartHeader)].header = true;
            rows[static_cast<int>(Row::StartHeader)].title = L"When " + RuleName() + L" starts";

            auto& config = rows[static_cast<int>(Row::Config)];
            config.title = L"Monitor config";
            config.detail = configured ? rule_.monitorPowerSetupName : L"Do not change displays";
            config.button = L"Choose...";

            auto& apply = rows[static_cast<int>(Row::ApplyDelay)];
            apply.title = L"Switch";
            apply.detail = DelayText(rule_.monitorPowerSetupDelayMilliseconds, L"Right away");
            apply.muted = !configured;

            rows[static_cast<int>(Row::ExitHeader)].header = true;
            rows[static_cast<int>(Row::ExitHeader)].title = L"When " + RuleName() + L" exits";

            auto& restore = rows[static_cast<int>(Row::Restore)];
            restore.title = L"Restore the previous arrangement";
            restore.detail = L"Goes back to the monitors that were active before.";
            restore.toggle = rule_.restoreMonitorPowerSetupOnExit ? 1 : 0;
            restore.toggleEnabled = configured;
            restore.muted = !configured;

            auto& restoreDelay = rows[static_cast<int>(Row::RestoreDelay)];
            restoreDelay.title = L"Restore";
            restoreDelay.detail = DelayText(rule_.restoreMonitorPowerSetupDelayMilliseconds, L"Right away");
            restoreDelay.muted = !configured || !rule_.restoreMonitorPowerSetupOnExit;

            for (const Row delayRow : {Row::ApplyDelay, Row::RestoreDelay})
            {
                auto& row = rows[static_cast<int>(delayRow)];
                const bool expanded = expanded_ == static_cast<int>(delayRow);
                row.iconButtons = {expanded ? kCollapseGlyph : kExpandGlyph};
                row.expandHeight = expanded ? 50 : 0;
            }
            list_.SetRows(std::move(rows));
        }

        void Expand(int row)
        {
            expanded_ = expanded_ == row ? -1 : row;
            Refresh();
            editors_.Clear();
            if (expanded_ < 0) return;
            syncing_ = true;
            editors_.Begin(expanded_);
            editors_.Label(L"Delay", 0, 12, 100);
            editors_.Seconds(kDelayId, expanded_ == static_cast<int>(Row::ApplyDelay)
                ? rule_.monitorPowerSetupDelayMilliseconds : rule_.restoreMonitorPowerSetupDelayMilliseconds, 104, 12);
            syncing_ = false;
            editors_.Position();
        }

        void ChooseConfig()
        {
            const auto& setups = context_.configuration->monitorPowerSetups;
            std::vector<std::wstring> items{L"Do not change displays"};
            int checked = 0;
            for (const auto& setup : setups)
            {
                items.push_back(setup.name);
                if (setup.name == rule_.monitorPowerSetupName) checked = static_cast<int>(items.size()) - 1;
            }
            int disabled = -1;
            if (setups.empty())
            {
                items.push_back(L"");
                items.push_back(L"No configs yet. Create one on the Displays page.");
                disabled = static_cast<int>(items.size()) - 1;
            }
            const int choice = ChooseFromMenu(items, checked, disabled);
            if (choice < 0 || choice > static_cast<int>(setups.size())) return;
            rule_.monitorPowerSetupName = choice == 0 ? std::wstring{} : setups[static_cast<size_t>(choice - 1)].name;
            context_.scheduleSave();
            Refresh();
        }

        bool OnCommand(int id, int code, HWND) override
        {
            if (id == kDelayId)
            {
                int milliseconds = 0;
                if (syncing_ || code != EN_CHANGE || !RowEditors::ParseSeconds(editors_.Text(id), milliseconds)) return true;
                if (expanded_ == static_cast<int>(Row::ApplyDelay)) rule_.monitorPowerSetupDelayMilliseconds = milliseconds;
                else rule_.restoreMonitorPowerSetupDelayMilliseconds = milliseconds;
                context_.scheduleSave();
                Refresh();
                return true;
            }
            if (id != kListId) return false;
            if (code == RowList::kLayoutChanged) { editors_.Position(); return true; }
            const auto row = static_cast<Row>(list_.NotifiedRow());
            if (list_.NotifiedRow() < 0) return true;
            if (row == Row::Config && (code == RowList::kButton || code == RowList::kActivated)) ChooseConfig();
            else if (row == Row::Restore && code == RowList::kToggled)
            {
                rule_.restoreMonitorPowerSetupOnExit = list_.Rows()[static_cast<size_t>(Row::Restore)].toggle == 1;
                context_.scheduleSave();
                Refresh();
            }
            else if ((row == Row::ApplyDelay || row == Row::RestoreDelay) &&
                (code == RowList::kActivated || code == RowList::kIconButton))
                Expand(static_cast<int>(row));
            return true;
        }

        PageContext context_;
        WatchedProcessRule& rule_;
        RowList list_;
        RowEditors editors_;
        int expanded_{-1};
        bool syncing_{};
    };
}

HWND CreateRuleDisplayPage(const PageContext& context, HWND parent, WatchedProcessRule& rule)
{
    return PageWindow::Show(std::make_unique<RuleDisplayPage>(context, rule), context.instance, parent, 520, 300);
}
