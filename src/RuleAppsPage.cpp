#include "Pages.h"

#include "AppSources.h"
#include "ui/BackgroundTask.h"
#include "Utils.h"
#include "ui/PageWindow.h"
#include "ui/RowEditors.h"
#include "ui/RowList.h"
#include "ui/SegmentedControl.h"

#include <algorithm>
#include <commdlg.h>
#include <filesystem>
#include <optional>

namespace
{
    constexpr int kRuleListId = 100;
    constexpr int kCandidateListId = 101;
    constexpr int kSourceId = 102;
    constexpr int kSearchId = 103;
    constexpr int kBrowseId = 104;
    constexpr int kRefreshId = 105;
    constexpr UINT_PTR kPollTimer = 1;
    constexpr wchar_t kExpandGlyph = L'\uE70D';
    constexpr wchar_t kCollapseGlyph = L'\uE70E';
    constexpr wchar_t kRemoveGlyph = L'\uE711';
    // Controls in the expanded row.
    constexpr int kStartDelayId = 200;
    constexpr int kCloseDelayId = 201;
    constexpr int kArgumentsId = 202;
    constexpr int kNameId = 203;
    constexpr int kChangePathId = 204;
    constexpr int kGracefulId = 205;
    constexpr int kForceAfterId = 206;
    constexpr int kRestartDelayId = 207;
    constexpr int kExpandHeight = 160;

    enum class Source
    {
        BackgroundApps,
        RunningNow,
        Installed
    };

    // One program that can be added to the rule.
    struct Candidate
    {
        std::wstring displayName;
        std::wstring processName;
        std::wstring executablePath;
        std::wstring detail;
        std::wstring pill;
        RowList::Tone pillTone{RowList::Tone::Neutral};
        bool allowed{true};
    };

    std::wstring StemOf(const std::wstring& path)
    {
        return std::filesystem::path(path).stem().wstring();
    }

    std::wstring Seconds(int milliseconds)
    {
        const int tenths = (std::max(0, milliseconds) + 50) / 100;
        return tenths % 10 == 0 ? std::to_wstring(tenths / 10) + L" s"
            : std::to_wstring(tenths / 10) + L"." + std::to_wstring(tenths % 10) + L" s";
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
        info.lpstrTitle = L"Choose a program";
        info.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        return GetOpenFileNameW(&info) ? std::wstring(path) : std::wstring{};
    }

    class RuleAppsPage : public PageWindow
    {
    public:
        RuleAppsPage(const PageContext& context, WatchedProcessRule& rule, RuleSection section)
            : PageWindow(context.headingFont, context.textFont), context_(context), rule_(rule),
              starting_(section == RuleSection::StartPrograms)
        {
        }

    private:
        void OnCreate() override
        {
            ruleList_.Create(Instance(), Handle(), kRuleListId, HeadingFont(), TextFont());
            editors_.Attach(ruleList_, Instance(), TextFont());
            ruleList_.SetEmptyText(starting_
                ? L"Nothing starts yet. Pick apps on the right, for example SimHub or CrewChief."
                : L"Nothing is closed yet. Pick apps on the right that should not run while you race.");
            sources_ = starting_ ? std::vector<Source>{Source::Installed, Source::RunningNow}
                                 : std::vector<Source>{Source::BackgroundApps, Source::RunningNow, Source::Installed};
            std::vector<std::wstring> labels;
            for (const auto source : sources_)
                labels.push_back(source == Source::BackgroundApps ? L"Background apps" : source == Source::RunningNow ? L"Running now" : L"Installed");
            sourceControl_.Create(Instance(), Handle(), kSourceId, TextFont(), std::move(labels));
            AddButton(kRefreshId, L"Refresh");
            AddButton(kBrowseId, L"Browse...");
            search_ = AddEdit(kSearchId, L"Search");
            candidateList_.Create(Instance(), Handle(), kCandidateListId, HeadingFont(), TextFont());
            RefreshRuleList();
            ShowSource();
        }

        void OnSize(int width, int height) override
        {
            const int gap = 16;
            const int left = std::max(260, (width - gap) / 2);
            const int rightX = left + gap;
            const int rightWidth = std::max(240, width - rightX);
            Place(ruleList_.Handle(), 0, 0, left, height);
            const int segmentWidth = MulDiv(sourceControl_.IdealWidth(), 96, GetDpiForWindow(Handle()));
            Place(sourceControl_.Handle(), rightX, 0, std::min(segmentWidth, rightWidth), 32);
            Place(GetDlgItem(Handle(), kBrowseId), rightX + rightWidth - 92, 40, 92, 28);
            Place(GetDlgItem(Handle(), kRefreshId), rightX + rightWidth - 184, 40, 86, 28);
            Place(search_, rightX, 42, rightWidth - 194, 24);
            Place(candidateList_.Handle(), rightX, 78, rightWidth, height - 78);
        }

        void OnDestroy() override
        {
            KillTimer(Handle(), kPollTimer);
            installedTask_.Cancel();
            runningTask_.Cancel();
            backgroundTask_.Cancel();
        }

        Source CurrentSource() const
        {
            const int index = sourceControl_.Selected();
            return sources_[static_cast<size_t>(std::clamp(index, 0, static_cast<int>(sources_.size()) - 1))];
        }

        // ---- loading -------------------------------------------------------------------
        void ShowSource()
        {
            const Source source = CurrentSource();
            const bool loaded = source == Source::Installed ? !context_.configuration->catalogPrograms.empty()
                : source == Source::RunningNow ? running_.has_value() : background_.has_value();
            if (!loaded) Load(source);
            RefreshCandidates();
        }

        void Load(Source source)
        {
            const auto watched = rule_.processName;
            if (source == Source::Installed && !installedTask_.Running())
            {
                installedTask_.Start([catalog = context_.configuration->catalogPrograms](const std::atomic_bool& cancelled)
                {
                    return FindInstalledApps(catalog, cancelled);
                });
            }
            else if (source == Source::RunningNow && !runningTask_.Running())
            {
                runningTask_.Start([watched](const std::atomic_bool& cancelled) { return CaptureRunningProcesses(watched, cancelled); });
            }
            else if (source == Source::BackgroundApps && !backgroundTask_.Running())
            {
                backgroundTask_.Start([watched](const std::atomic_bool& cancelled) { return CaptureBackgroundApps(watched, cancelled); });
            }
            SetTimer(Handle(), kPollTimer, 100, nullptr);
        }

        void OnTimer(UINT_PTR id) override
        {
            if (id != kPollTimer) return;
            std::optional<std::vector<CatalogProgram>> installed;
            if (installedTask_.Poll(installed) && installed)
            {
                context_.configuration->catalogPrograms = std::move(*installed);
                context_.scheduleSave();
            }
            std::optional<std::vector<RunningProcessEntry>> running;
            if (runningTask_.Poll(running) && running) running_ = std::move(*running);
            std::optional<std::vector<DetectedProcessEntry>> background;
            if (backgroundTask_.Poll(background) && background) background_ = std::move(*background);
            if (!installedTask_.Running() && !runningTask_.Running() && !backgroundTask_.Running())
                KillTimer(Handle(), kPollTimer);
            RefreshCandidates();
        }

        bool Loading(Source source) const
        {
            return source == Source::Installed ? installedTask_.Running()
                : source == Source::RunningNow ? runningTask_.Running() : backgroundTask_.Running();
        }

        // ---- rule contents -------------------------------------------------------------
        bool InRule(const Candidate& candidate) const
        {
            if (starting_)
            {
                return std::any_of(rule_.programsToLaunch.begin(), rule_.programsToLaunch.end(), [&](const LaunchProgram& program)
                {
                    return !candidate.executablePath.empty() && _wcsicmp(program.filePath.c_str(), candidate.executablePath.c_str()) == 0;
                });
            }
            const auto stem = StemOf(candidate.processName);
            return std::any_of(rule_.processesToStop.begin(), rule_.processesToStop.end(), [&](const ProcessStopAction& action)
            {
                return _wcsicmp(StemOf(action.processName).c_str(), stem.c_str()) == 0;
            });
        }

        bool IsWatchedProgram(const Candidate& candidate) const
        {
            return _wcsicmp(StemOf(candidate.processName).c_str(), StemOf(rule_.processName).c_str()) == 0;
        }

        void Add(const Candidate& candidate)
        {
            if (InRule(candidate) || !candidate.allowed || IsWatchedProgram(candidate)) return;
            if (starting_)
            {
                LaunchProgram program;
                program.displayName = candidate.displayName;
                program.filePath = candidate.executablePath;
                rule_.programsToLaunch.push_back(std::move(program));
            }
            else
            {
                ProcessStopAction action;
                action.displayName = candidate.displayName;
                action.processName = candidate.processName;
                action.executablePath = candidate.executablePath;
                rule_.processesToStop.push_back(std::move(action));
            }
            Changed();
        }

        void Browse()
        {
            const auto path = PickExecutable(Handle());
            if (path.empty()) return;
            Candidate candidate;
            candidate.displayName = StemOf(path);
            candidate.processName = std::filesystem::path(path).filename().wstring();
            candidate.executablePath = path;
            // Remember it, so it also shows up under Installed next time.
            auto& catalog = context_.configuration->catalogPrograms;
            const bool known = std::any_of(catalog.begin(), catalog.end(), [&path](const CatalogProgram& program)
            {
                return _wcsicmp(program.filePath.c_str(), path.c_str()) == 0;
            });
            if (!known) catalog.push_back({candidate.displayName, path, true});
            Add(candidate);
        }

        // ---- expanded row ---------------------------------------------------------------
        void BuildEditors()
        {
            editors_.Clear();
            if (expanded_ < 0) return;
            syncing_ = true;
            editors_.Begin(expanded_ + kFirstItemRow);
            const auto index = static_cast<size_t>(expanded_);
            std::wstring name;
            std::wstring path;
            if (starting_)
            {
                const auto& program = rule_.programsToLaunch[index];
                editors_.Label(L"Start after", 0, 12, 100);
                editors_.Seconds(kStartDelayId, program.waitTimeMilliseconds, 104, 12);
                editors_.Label(L"Close after exit", 210, 12, 120);
                editors_.Seconds(kCloseDelayId, program.closeDelayMilliseconds, 334, 12);
                editors_.Label(L"Arguments", 0, 48, 100);
                editors_.Edit(kArgumentsId, program.arguments, 104, 48, 0);
                name = program.displayName;
                path = program.filePath;
            }
            else
            {
                const auto& action = rule_.processesToStop[index];
                editors_.Check(kGracefulId, L"Ask to close first", action.gracefulCloseFirst, 0, 12, 190);
                editors_.Label(L"End it after", 210, 12, 120);
                editors_.Seconds(kForceAfterId, action.forceAfterMilliseconds, 334, 12);
                editors_.Label(L"Reopen after", 0, 48, 100);
                editors_.Seconds(kRestartDelayId, action.restartDelayMilliseconds, 104, 48);
                name = action.displayName;
                path = action.executablePath.empty() ? action.processName : action.executablePath;
            }
            editors_.Label(L"Name", 0, 84, 100);
            editors_.Edit(kNameId, name, 104, 84, 240);
            editors_.Label(L"Program", 0, 120, 100);
            editors_.Button(kChangePathId, L"Change...", 104, 118, 100);
            editors_.Add(L"STATIC", path.c_str(), SS_CENTERIMAGE | SS_PATHELLIPSIS, 0, 214, 120, 0);
            syncing_ = false;
            UpdateEditorStates();
            editors_.Position();
        }

        // Delays only matter when their switch is on.
        void UpdateEditorStates()
        {
            if (expanded_ < 0) return;
            const auto index = static_cast<size_t>(expanded_);
            if (starting_)
                EnableWindow(editors_.Get(kCloseDelayId), rule_.programsToLaunch[index].closeWhenGameStops);
            else
            {
                EnableWindow(editors_.Get(kForceAfterId), rule_.processesToStop[index].gracefulCloseFirst);
                EnableWindow(editors_.Get(kRestartDelayId), rule_.processesToStop[index].restartAfterWatchProcessEnds);
            }
        }

        void ToggleExpanded(int index)
        {
            expanded_ = expanded_ == index ? -1 : index;
            RefreshRuleList();
            BuildEditors();
            if (expanded_ >= 0) ruleList_.ScrollIntoView(expanded_ + kFirstItemRow);
        }

        // Applies an edit in the expanded row; returns false if the id is not an editor.
        bool OnEditorCommand(int id, int code)
        {
            const bool editor = id >= kStartDelayId && id <= kRestartDelayId;
            if (!editor) return false;
            if (expanded_ < 0 || syncing_) return true;
            const auto index = static_cast<size_t>(expanded_);
            int milliseconds = 0;
            if (code == EN_CHANGE && (id == kStartDelayId || id == kCloseDelayId || id == kForceAfterId || id == kRestartDelayId))
            {
                if (!RowEditors::ParseSeconds(editors_.Text(id), milliseconds)) return true; // Keep the last valid value while typing.
                if (id == kStartDelayId) rule_.programsToLaunch[index].waitTimeMilliseconds = milliseconds;
                else if (id == kCloseDelayId) rule_.programsToLaunch[index].closeDelayMilliseconds = milliseconds;
                else if (id == kForceAfterId) rule_.processesToStop[index].forceAfterMilliseconds = milliseconds;
                else rule_.processesToStop[index].restartDelayMilliseconds = milliseconds;
            }
            else if (code == EN_CHANGE && id == kArgumentsId && starting_)
            {
                rule_.programsToLaunch[index].arguments = editors_.Text(id);
            }
            else if (code == EN_CHANGE && id == kNameId)
            {
                if (starting_) rule_.programsToLaunch[index].displayName = editors_.Text(id);
                else rule_.processesToStop[index].displayName = editors_.Text(id);
            }
            else if (code == BN_CLICKED && id == kGracefulId && !starting_)
            {
                rule_.processesToStop[index].gracefulCloseFirst = editors_.Checked(kGracefulId);
                UpdateEditorStates();
            }
            else if (code == BN_CLICKED && id == kChangePathId)
            {
                const auto path = PickExecutable(Handle());
                if (path.empty()) return true;
                if (starting_) rule_.programsToLaunch[index].filePath = path;
                else
                {
                    auto& action = rule_.processesToStop[index];
                    action.executablePath = path;
                    action.processName = std::filesystem::path(path).filename().wstring();
                }
                context_.scheduleSave();
                RefreshRuleList();
                BuildEditors(); // Shows the new path.
                RefreshCandidates();
                return true;
            }
            else return true;
            context_.scheduleSave();
            RefreshRuleList();
            return true;
        }

        void RemoveItem(size_t index)
        {
            if (starting_ && index < rule_.programsToLaunch.size()) rule_.programsToLaunch.erase(rule_.programsToLaunch.begin() + static_cast<std::ptrdiff_t>(index));
            else if (!starting_ && index < rule_.processesToStop.size()) rule_.processesToStop.erase(rule_.processesToStop.begin() + static_cast<std::ptrdiff_t>(index));
            else return;
            const int removed = static_cast<int>(index);
            if (removed == expanded_) { expanded_ = -1; editors_.Clear(); }
            else if (removed < expanded_) --expanded_;
            Changed();
        }

        void Changed()
        {
            context_.scheduleSave();
            RefreshRuleList();
            RefreshCandidates();
            UpdateEditorStates();
        }

        // Row 0 is the header; item rows follow in rule order.
        static constexpr int kFirstItemRow = 1;

        bool OnCommand(int id, int code, HWND) override
        {
            if (id == kSourceId && code == SegmentedControl::kChanged) { ShowSource(); return true; }
            if (id == kSearchId && code == EN_CHANGE) { RefreshCandidates(); return true; }
            if (id == kBrowseId) { Browse(); return true; }
            if (id == kRefreshId) { Load(CurrentSource()); RefreshCandidates(); return true; }
            if (id == kCandidateListId)
            {
                const int row = candidateList_.NotifiedRow();
                if ((code == RowList::kButton || code == RowList::kActivated) && row >= 0 && static_cast<size_t>(row) < shown_.size())
                    Add(shown_[static_cast<size_t>(row)]);
                return true;
            }
            if (OnEditorCommand(id, code)) return true;
            if (id == kRuleListId && code == RowList::kLayoutChanged)
            {
                editors_.Position();
                return true;
            }
            if (id == kRuleListId)
            {
                const int row = ruleList_.NotifiedRow() - kFirstItemRow;
                if (row < 0) return true;
                const auto index = static_cast<size_t>(row);
                if (code == RowList::kToggled)
                {
                    const bool on = ruleList_.Rows()[static_cast<size_t>(row + kFirstItemRow)].toggle == 1;
                    if (starting_ && index < rule_.programsToLaunch.size()) rule_.programsToLaunch[index].closeWhenGameStops = on;
                    else if (!starting_ && index < rule_.processesToStop.size()) rule_.processesToStop[index].restartAfterWatchProcessEnds = on;
                    Changed();
                }
                else if (code == RowList::kIconButton && ruleList_.NotifiedIconButton() == 0) ToggleExpanded(row);
                else if (code == RowList::kIconButton && ruleList_.NotifiedIconButton() == 1) RemoveItem(index);
                else if (code == RowList::kActivated) ToggleExpanded(row);
                else if (code == RowList::kDeleteRequested) RemoveItem(index);
                return true;
            }
            return false;
        }

        // ---- lists ---------------------------------------------------------------------
        void RefreshRuleList()
        {
            std::vector<RowList::Row> rows;
            const size_t count = starting_ ? rule_.programsToLaunch.size() : rule_.processesToStop.size();
            if (count != 0)
            {
                RowList::Row header;
                header.header = true;
                header.title = L"In this rule  \u00B7  " + std::to_wstring(count);
                rows.push_back(std::move(header));
            }
            if (starting_)
            {
                for (const auto& program : rule_.programsToLaunch)
                {
                    RowList::Row row;
                    row.showIcon = true;
                    row.iconPath = program.filePath;
                    row.title = program.displayName.empty() ? StemOf(program.filePath) : program.displayName;
                    row.detail = program.waitTimeMilliseconds > 0 ? L"Starts after " + Seconds(program.waitTimeMilliseconds) : L"Starts right away";
                    if (!program.arguments.empty()) row.detail += L"  \u00B7  " + program.arguments;
                    row.toggleLabel = L"Close on exit";
                    row.toggle = program.closeWhenGameStops ? 1 : 0;
                    rows.push_back(std::move(row));
                }
            }
            else
            {
                for (const auto& action : rule_.processesToStop)
                {
                    RowList::Row row;
                    row.showIcon = true;
                    row.iconPath = action.executablePath;
                    row.title = action.displayName.empty() ? StemOf(action.processName) : action.displayName;
                    row.detail = action.gracefulCloseFirst
                        ? L"Asks to close, ends it after " + Seconds(action.forceAfterMilliseconds)
                        : L"Ends it right away";
                    row.toggleLabel = L"Reopen";
                    row.toggle = action.restartAfterWatchProcessEnds ? 1 : 0;
                    rows.push_back(std::move(row));
                }
            }
            // The arrow (rightmost) expands a row's settings; the cross removes it.
            for (size_t index = 0; index < count; ++index)
            {
                auto& row = rows[index + kFirstItemRow];
                const bool expanded = static_cast<int>(index) == expanded_;
                row.iconButtons = {expanded ? kCollapseGlyph : kExpandGlyph, kRemoveGlyph};
                row.expandHeight = expanded ? kExpandHeight : 0;
            }
            ruleList_.SetRows(std::move(rows));
        }

        std::vector<Candidate> CandidatesFor(Source source) const
        {
            std::vector<Candidate> candidates;
            if (source == Source::Installed)
            {
                for (const auto& program : context_.configuration->catalogPrograms)
                {
                    Candidate candidate;
                    candidate.displayName = program.displayName;
                    candidate.processName = std::filesystem::path(program.filePath).filename().wstring();
                    candidate.executablePath = program.filePath;
                    candidate.detail = program.filePath;
                    candidates.push_back(std::move(candidate));
                }
            }
            else if (source == Source::RunningNow && running_)
            {
                for (const auto& process : *running_)
                {
                    Candidate candidate;
                    candidate.displayName = process.displayName;
                    candidate.processName = process.processName;
                    candidate.executablePath = process.executablePath;
                    wchar_t usage[64]{};
                    swprintf_s(usage, L"CPU %.1f %%  \u00B7  %.0f MB", process.hasCpuUsage ? process.cpuUsagePercent : 0.0,
                        static_cast<double>(process.memoryUsageBytes) / (1024.0 * 1024.0));
                    candidate.detail = usage;
                    // Without a path a launch is impossible; closing by name still works.
                    candidate.allowed = !starting_ || !process.executablePath.empty();
                    candidates.push_back(std::move(candidate));
                }
            }
            else if (source == Source::BackgroundApps && background_)
            {
                for (const auto& app : *background_)
                {
                    Candidate candidate;
                    candidate.displayName = app.displayName;
                    candidate.processName = app.processName;
                    candidate.executablePath = app.executablePath;
                    // "High - CPU active" becomes the pill "High impact" and the detail "CPU active".
                    const auto separator = app.effect.find(L" - ");
                    const auto level = app.effect.substr(0, separator);
                    const auto reason = separator == std::wstring::npos ? std::wstring{} : app.effect.substr(separator + 3);
                    candidate.detail = app.category + L"  \u00B7  " + (app.running ? L"running" : L"installed, not running");
                    if (!reason.empty()) candidate.detail += L"  \u00B7  " + reason;
                    if (level == L"High") { candidate.pill = L"High impact"; candidate.pillTone = RowList::Tone::Warning; }
                    else if (level == L"Medium") candidate.pill = L"Medium impact";
                    else if (level == L"Low") candidate.pill = L"Low impact";
                    candidate.allowed = app.allowStop;
                    candidates.push_back(std::move(candidate));
                }
            }
            return candidates;
        }

        void RefreshCandidates()
        {
            const Source source = CurrentSource();
            wchar_t buffer[256]{};
            GetWindowTextW(search_, buffer, static_cast<int>(std::size(buffer)));
            const std::wstring filter = buffer;
            EnableWindow(GetDlgItem(Handle(), kRefreshId), !Loading(source));
            SetDlgItemTextW(Handle(), kRefreshId, Loading(source) ? L"Loading..." : L"Refresh");

            shown_.clear();
            std::vector<RowList::Row> rows;
            for (auto& candidate : CandidatesFor(source))
            {
                if (IsWatchedProgram(candidate)) continue;
                if (!ContainsInsensitive(candidate.displayName, filter) && !ContainsInsensitive(candidate.executablePath, filter)) continue;
                RowList::Row row;
                row.showIcon = true;
                row.iconPath = candidate.executablePath;
                row.title = candidate.displayName;
                row.detail = candidate.detail;
                row.pill = candidate.pill;
                row.pillTone = candidate.pillTone;
                const bool added = InRule(candidate);
                row.button = added ? L"Added" : candidate.allowed ? L"Add" : L"Not supported";
                row.buttonEnabled = !added && candidate.allowed;
                row.muted = added || !candidate.allowed;
                rows.push_back(std::move(row));
                shown_.push_back(std::move(candidate));
            }
            candidateList_.SetEmptyText(Loading(source)
                ? (source == Source::BackgroundApps ? L"Measuring background apps..." : L"Loading...")
                : filter.empty() ? L"Nothing found. Use Browse to choose a program." : L"No match for this search.");
            candidateList_.SetRows(std::move(rows));
        }

        PageContext context_;
        WatchedProcessRule& rule_;
        bool starting_{};
        std::vector<Source> sources_;
        RowList ruleList_;
        RowList candidateList_;
        SegmentedControl sourceControl_;
        HWND search_{};
        std::vector<Candidate> shown_;
        // Item whose settings are expanded, or -1.
        int expanded_{-1};
        RowEditors editors_;
        // Set while the editors are filled, so their change notifications are ignored.
        bool syncing_{};
        std::optional<std::vector<RunningProcessEntry>> running_;
        std::optional<std::vector<DetectedProcessEntry>> background_;
        BackgroundTask<std::vector<CatalogProgram>> installedTask_;
        BackgroundTask<std::vector<RunningProcessEntry>> runningTask_;
        BackgroundTask<std::vector<DetectedProcessEntry>> backgroundTask_;
    };
}

HWND CreateRuleAppsPage(const PageContext& context, HWND parent, WatchedProcessRule& rule, RuleSection section)
{
    return PageWindow::Show(std::make_unique<RuleAppsPage>(context, rule, section), context.instance, parent, 600, 360);
}
