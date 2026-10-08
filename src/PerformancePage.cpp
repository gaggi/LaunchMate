#include "Pages.h"

#include "AppSources.h"
#include "IRacingPerformance.h"
#include "ui/PageWindow.h"
#include "ui/RowEditors.h"
#include "ui/RowList.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>

namespace
{
    constexpr int kListId = 100;
    constexpr int kCpuPriorityId = 200;
    constexpr int kIoPriorityId = 201;
    constexpr int kMemoryPriorityId = 202;
    constexpr int kAllCpusId = 203;
    constexpr int kCpuFirstId = 300;
    constexpr wchar_t kExpandGlyph = L'\uE70D';
    constexpr wchar_t kCollapseGlyph = L'\uE70E';
    constexpr wchar_t kResetGlyph = L'\uE711';

    struct Choice
    {
        const wchar_t* label;
        int value;
    };

    constexpr Choice kCpuChoices[] = {
        {L"Do not change", 0}, {L"Low", IDLE_PRIORITY_CLASS}, {L"Below normal", BELOW_NORMAL_PRIORITY_CLASS},
        {L"Normal", NORMAL_PRIORITY_CLASS}, {L"Above normal", ABOVE_NORMAL_PRIORITY_CLASS}, {L"High", HIGH_PRIORITY_CLASS},
        {L"Real time", REALTIME_PRIORITY_CLASS}};
    constexpr Choice kIoChoices[] = {{L"Do not change", -1}, {L"Very low", 0}, {L"Low", 1}, {L"Normal", 2}, {L"High", 3}};
    constexpr Choice kMemoryChoices[] = {
        {L"Do not change", -1}, {L"Very low", 1}, {L"Low", 2}, {L"Medium", 3}, {L"Below normal", 4}, {L"Normal", 5}};

    template<size_t Count>
    const wchar_t* LabelOf(const Choice (&choices)[Count], int value)
    {
        for (const auto& choice : choices)
            if (choice.value == value) return choice.label;
        return L"Custom";
    }

    template<size_t Count>
    int IndexOf(const Choice (&choices)[Count], int value)
    {
        for (size_t index = 0; index < Count; ++index)
            if (choices[index].value == value) return static_cast<int>(index);
        return 0;
    }

    template<size_t Count>
    std::vector<std::wstring> Labels(const Choice (&choices)[Count])
    {
        std::vector<std::wstring> labels;
        for (const auto& choice : choices) labels.emplace_back(choice.label);
        return labels;
    }

    int CpuCount() { return static_cast<int>(std::min<DWORD>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), 64)); }

    bool IsDefault(const ProcessPerformanceAction& action)
    {
        return action.cpuPriorityClass == 0 && action.ioPriority < 0 && action.memoryPriority < 0 && action.affinityMask == 0;
    }

    std::wstring Summary(const ProcessPerformanceAction& action)
    {
        if (IsDefault(action)) return L"Not changed";
        std::vector<std::wstring> parts;
        if (action.cpuPriorityClass != 0) parts.push_back(std::wstring(L"CPU ") + LabelOf(kCpuChoices, action.cpuPriorityClass));
        if (action.ioPriority >= 0) parts.push_back(std::wstring(L"I/O ") + LabelOf(kIoChoices, action.ioPriority));
        if (action.memoryPriority >= 0) parts.push_back(std::wstring(L"memory ") + LabelOf(kMemoryChoices, action.memoryPriority));
        if (action.affinityMask != 0)
        {
            int count = 0;
            for (int cpu = 0; cpu < 64; ++cpu) count += (action.affinityMask >> cpu) & 1;
            parts.push_back(std::to_wstring(count) + L" of " + std::to_wstring(CpuCount()) + L" CPUs");
        }
        std::wstring text;
        for (const auto& part : parts) text += (text.empty() ? L"" : L"  \u00B7  ") + part;
        return text;
    }

    std::wstring ExeName(const std::wstring& nameOrPath)
    {
        auto name = std::filesystem::path(nameOrPath).filename().wstring();
        if (!name.empty() && _wcsicmp(std::filesystem::path(name).extension().c_str(), L".exe") != 0) name += L".exe";
        return name;
    }

    class PerformancePage : public PageWindow
    {
    public:
        PerformancePage(const PageContext& context, WatchedProcessRule& rule)
            : PageWindow(context.headingFont, context.textFont), context_(context), rule_(rule)
        {
        }

    private:
        enum class RowKind { Header, PowerPlan, Process, AddProcess };

        void OnCreate() override
        {
            list_.Create(Instance(), Handle(), kListId, HeadingFont(), TextFont());
            editors_.Attach(list_, Instance(), TextFont());
            schemes_ = EnumeratePowerSchemes();
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

        // Processes shown: the watched program, its started apps, then everything configured.
        std::vector<std::wstring> ProcessNames() const
        {
            std::vector<std::wstring> names;
            const auto add = [&names](const std::wstring& name)
            {
                if (name.empty()) return;
                if (std::none_of(names.begin(), names.end(), [&](const auto& existing) { return _wcsicmp(existing.c_str(), name.c_str()) == 0; }))
                    names.push_back(name);
            };
            add(ExeName(rule_.processName.empty() ? rule_.executablePath : rule_.processName));
            for (const auto& program : rule_.programsToLaunch) add(ExeName(program.filePath));
            for (const auto& action : rule_.processPerformanceActions) add(ExeName(action.processName));
            for (const auto& extra : extraProcesses_) add(extra);
            return names;
        }

        ProcessPerformanceAction* Find(const std::wstring& name)
        {
            for (auto& action : rule_.processPerformanceActions)
                if (_wcsicmp(ExeName(action.processName).c_str(), name.c_str()) == 0) return &action;
            return nullptr;
        }

        // The configured settings for a process, or defaults if it has none yet.
        ProcessPerformanceAction Settings(const std::wstring& name)
        {
            if (const auto* action = Find(name)) return *action;
            ProcessPerformanceAction action;
            action.processName = name;
            return action;
        }

        // Stores settings; a process without any change is removed from the rule.
        void Store(const ProcessPerformanceAction& settings)
        {
            auto& actions = rule_.processPerformanceActions;
            auto* existing = Find(settings.processName);
            if (IsDefault(settings))
            {
                if (existing) actions.erase(actions.begin() + (existing - actions.data()));
            }
            else if (existing) *existing = settings;
            else actions.push_back(settings);
            context_.scheduleSave();
        }

        std::wstring PowerPlanName() const
        {
            if (rule_.powerSchemeGuid.empty()) return L"Not changed";
            for (const auto& scheme : schemes_)
                if (_wcsicmp(PowerSchemeGuidText(scheme.id).c_str(), rule_.powerSchemeGuid.c_str()) == 0) return scheme.name;
            return L"Unavailable power plan";
        }

        void Refresh()
        {
            std::vector<RowList::Row> rows;
            kinds_.clear();
            names_.clear();
            const auto push = [&](RowList::Row row, RowKind kind, std::wstring name = {})
            {
                rows.push_back(std::move(row));
                kinds_.push_back(kind);
                names_.push_back(std::move(name));
            };

            RowList::Row header;
            header.header = true;
            header.title = L"Power plan";
            push(header, RowKind::Header);
            RowList::Row plan;
            plan.title = L"While " + RuleName() + L" runs";
            plan.detail = PowerPlanName() + (rule_.powerSchemeGuid.empty() ? L"" : L"  \u00B7  the previous plan returns afterwards");
            plan.button = L"Change...";
            push(plan, RowKind::PowerPlan);

            header.title = L"Process priorities  \u00B7  applied while " + RuleName() + L" runs";
            push(header, RowKind::Header);
            for (const auto& name : ProcessNames())
            {
                const auto settings = Settings(name);
                const bool expanded = _wcsicmp(name.c_str(), expandedName_.c_str()) == 0;
                RowList::Row row;
                row.title = name;
                row.detail = Summary(settings);
                row.muted = IsDefault(settings);
                row.iconButtons = {expanded ? kCollapseGlyph : kExpandGlyph};
                if (!IsDefault(settings)) row.iconButtons.push_back(kResetGlyph);
                row.expandHeight = expanded ? ExpandHeight(settings) : 0;
                push(row, RowKind::Process, name);
            }
            RowList::Row add;
            add.title = L"Another process";
            add.detail = L"Pick from the programs running right now.";
            add.button = L"Add...";
            push(add, RowKind::AddProcess);
            list_.SetRows(std::move(rows));
        }

        static int ExpandHeight(const ProcessPerformanceAction& settings)
        {
            if (settings.affinityMask == 0) return 166;
            const int rows = (CpuCount() + 7) / 8;
            return 166 + rows * 26 + 4;
        }

        int ExpandedRow() const
        {
            for (size_t index = 0; index < names_.size(); ++index)
                if (kinds_[index] == RowKind::Process && _wcsicmp(names_[index].c_str(), expandedName_.c_str()) == 0)
                    return static_cast<int>(index);
            return -1;
        }

        void BuildEditors()
        {
            editors_.Clear();
            const int row = ExpandedRow();
            if (row < 0) return;
            syncing_ = true;
            editors_.Begin(row);
            const auto settings = Settings(expandedName_);
            editors_.Label(L"CPU priority", 0, 12, 120);
            editors_.Combo(kCpuPriorityId, Labels(kCpuChoices), IndexOf(kCpuChoices, settings.cpuPriorityClass), 126, 12, 180);
            editors_.Label(L"I/O priority", 0, 48, 120);
            editors_.Combo(kIoPriorityId, Labels(kIoChoices), IndexOf(kIoChoices, settings.ioPriority), 126, 48, 180);
            editors_.Label(L"Memory priority", 0, 84, 120);
            editors_.Combo(kMemoryPriorityId, Labels(kMemoryChoices), IndexOf(kMemoryChoices, settings.memoryPriority), 126, 84, 180);
            editors_.Check(kAllCpusId, L"Use all CPUs", settings.affinityMask == 0, 0, 122, 200);
            if (settings.affinityMask != 0)
            {
                for (int cpu = 0; cpu < CpuCount(); ++cpu)
                {
                    const std::wstring label = L"CPU " + std::to_wstring(cpu);
                    editors_.Check(kCpuFirstId + cpu, label.c_str(), (settings.affinityMask >> cpu) & 1, (cpu % 8) * 72, 156 + (cpu / 8) * 26, 70);
                }
            }
            syncing_ = false;
            editors_.Position();
        }

        void Expand(const std::wstring& name)
        {
            expandedName_ = _wcsicmp(expandedName_.c_str(), name.c_str()) == 0 ? std::wstring{} : name;
            // Keep the row while it is open, even if its last setting is reset.
            if (!expandedName_.empty()) extraProcesses_.push_back(expandedName_);
            Refresh();
            BuildEditors();
            if (const int row = ExpandedRow(); row >= 0) list_.ScrollIntoView(row);
        }

        void ChoosePowerPlan()
        {
            std::vector<std::wstring> items{L"Do not change"};
            int checked = 0;
            for (const auto& scheme : schemes_)
            {
                items.push_back(scheme.name);
                if (_wcsicmp(PowerSchemeGuidText(scheme.id).c_str(), rule_.powerSchemeGuid.c_str()) == 0)
                    checked = static_cast<int>(items.size()) - 1;
            }
            const int choice = ChooseFromMenu(items, checked);
            if (choice < 0) return;
            rule_.powerSchemeGuid = choice == 0 ? std::wstring{} : PowerSchemeGuidText(schemes_[static_cast<size_t>(choice - 1)].id);
            context_.scheduleSave();
            Refresh();
        }

        void AddProcess()
        {
            const auto shown = ProcessNames();
            std::vector<std::wstring> items;
            for (const auto& name : RunningProcessNames())
            {
                if (std::none_of(shown.begin(), shown.end(), [&](const auto& existing) { return _wcsicmp(existing.c_str(), name.c_str()) == 0; }))
                    items.push_back(name);
            }
            if (items.empty()) return;
            const int choice = ChooseFromMenu(items);
            if (choice < 0) return;
            extraProcesses_.push_back(items[static_cast<size_t>(choice)]);
            Expand(items[static_cast<size_t>(choice)]);
        }

        void ApplyEditors(int id)
        {
            auto settings = Settings(expandedName_);
            if (id == kCpuPriorityId) settings.cpuPriorityClass = kCpuChoices[std::max(0, editors_.Selection(id))].value;
            else if (id == kIoPriorityId) settings.ioPriority = kIoChoices[std::max(0, editors_.Selection(id))].value;
            else if (id == kMemoryPriorityId) settings.memoryPriority = kMemoryChoices[std::max(0, editors_.Selection(id))].value;
            else if (id == kAllCpusId)
            {
                // Start a custom selection from all CPUs, so nothing changes until a box is cleared.
                const std::uint64_t all = CpuCount() == 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << CpuCount()) - 1);
                settings.affinityMask = editors_.Checked(kAllCpusId) ? 0 : all;
            }
            else if (id >= kCpuFirstId && id < kCpuFirstId + CpuCount())
            {
                std::uint64_t mask = 0;
                for (int cpu = 0; cpu < CpuCount(); ++cpu)
                    if (editors_.Checked(kCpuFirstId + cpu)) mask |= std::uint64_t{1} << cpu;
                if (mask == 0) { BuildEditors(); return; } // At least one CPU must stay.
                settings.affinityMask = mask;
            }
            const bool layoutChanged = id == kAllCpusId;
            Store(settings);
            Refresh();
            if (layoutChanged) BuildEditors();
        }

        bool OnCommand(int id, int code, HWND) override
        {
            if (id >= kCpuPriorityId && id < kCpuFirstId + 64)
            {
                const bool relevant = (code == CBN_SELCHANGE && id <= kMemoryPriorityId) || (code == BN_CLICKED && id >= kAllCpusId);
                if (!syncing_ && relevant) ApplyEditors(id);
                return true;
            }
            if (id != kListId) return false;
            if (code == RowList::kLayoutChanged) { editors_.Position(); return true; }
            const int row = list_.NotifiedRow();
            if (row < 0 || static_cast<size_t>(row) >= kinds_.size()) return true;
            const auto kind = kinds_[static_cast<size_t>(row)];
            const auto name = names_[static_cast<size_t>(row)];
            if (kind == RowKind::PowerPlan && (code == RowList::kButton || code == RowList::kActivated)) ChoosePowerPlan();
            else if (kind == RowKind::AddProcess && (code == RowList::kButton || code == RowList::kActivated)) AddProcess();
            else if (kind == RowKind::Process)
            {
                if (code == RowList::kActivated || (code == RowList::kIconButton && list_.NotifiedIconButton() == 0)) Expand(name);
                else if ((code == RowList::kIconButton && list_.NotifiedIconButton() == 1) || code == RowList::kDeleteRequested)
                {
                    ProcessPerformanceAction reset;
                    reset.processName = name;
                    Store(reset);
                    Refresh();
                    BuildEditors();
                }
            }
            return true;
        }

        PageContext context_;
        WatchedProcessRule& rule_;
        RowList list_;
        RowEditors editors_;
        std::vector<PowerSchemeInfo> schemes_;
        std::vector<RowKind> kinds_;
        std::vector<std::wstring> names_;
        // Added from the running processes but not configured yet.
        std::vector<std::wstring> extraProcesses_;
        std::wstring expandedName_;
        bool syncing_{};
    };
}

HWND CreatePerformancePage(const PageContext& context, HWND parent, WatchedProcessRule& rule)
{
    return PageWindow::Show(std::make_unique<PerformancePage>(context, rule), context.instance, parent, 560, 360);
}
