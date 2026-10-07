#include "Pages.h"

#include "IRacingPerformance.h"
#include "ui/BackgroundTask.h"
#include "ui/PageWindow.h"
#include "ui/RowEditors.h"
#include "ui/RowList.h"

#include <algorithm>
#include <optional>

namespace
{
    constexpr int kListId = 100;
    constexpr int kSizeId = 200;
    constexpr UINT_PTR kPollTimer = 1;
    constexpr wchar_t kExpandGlyph = L'\uE70D';
    constexpr wchar_t kCollapseGlyph = L'\uE70E';

    enum class Row
    {
        None,
        CarPreload,
        TrackPreload,
        StreamingSize,
        SaveIni,
        Defender,
        CheckAgain
    };

    class IRacingPage : public PageWindow
    {
    public:
        IRacingPage(const PageContext& context, WatchedProcessRule& rule)
            : PageWindow(context.headingFont, context.textFont), rule_(rule)
        {
        }

    private:
        void OnCreate() override
        {
            list_.Create(Instance(), Handle(), kListId, HeadingFont(), TextFont());
            list_.SetEmptyText(L"Checking your iRacing setup...");
            editors_.Attach(list_, Instance(), TextFont());
            Check();
        }

        void OnSize(int width, int height) override
        {
            Place(list_.Handle(), 0, 0, std::min(width, 820), height);
        }

        void OnDestroy() override
        {
            KillTimer(Handle(), kPollTimer);
            task_.Cancel();
        }

        void Check()
        {
            if (task_.Running()) return;
            task_.Start([rule = rule_](const std::atomic_bool&) { return CheckIRacingSetup(rule); });
            SetTimer(Handle(), kPollTimer, 100, nullptr);
            Refresh();
        }

        void OnTimer(UINT_PTR id) override
        {
            if (id != kPollTimer) return;
            std::optional<IRacingCheck> result;
            if (!task_.Poll(result)) return;
            KillTimer(Handle(), kPollTimer);
            if (result)
            {
                // Keep values the user changed but has not saved yet.
                if (!check_ || !iniDirty_)
                {
                    carPreload_ = result->carPreload;
                    trackPreload_ = result->trackPreload;
                    streamingSize_ = result->streamingTextureSize;
                }
                check_ = std::move(*result);
            }
            Refresh();
        }

        bool IniDiffers() const
        {
            return check_ && (carPreload_ != check_->carPreload || trackPreload_ != check_->trackPreload ||
                streamingSize_ != check_->streamingTextureSize);
        }

        void Refresh()
        {
            rows_.clear();
            std::vector<RowList::Row> rows;
            const auto header = [&](const wchar_t* title)
            {
                RowList::Row row;
                row.header = true;
                row.title = title;
                rows.push_back(std::move(row));
                rows_.push_back(Row::None);
            };
            const auto add = [&](RowList::Row row, Row kind)
            {
                rows.push_back(std::move(row));
                rows_.push_back(kind);
            };
            if (!check_) { list_.SetRows({}); return; }
            const bool iniAvailable = check_->iniReadable;

            header(L"Texture loading  \u00B7  Documents\\iRacing\\app.ini");
            {
                RowList::Row row;
                row.title = L"Preload all cars";
                row.detail = L"carPreloadAll  \u00B7  loads every car at session start instead of while driving";
                row.toggle = carPreload_ ? 1 : 0;
                row.toggleEnabled = iniAvailable;
                add(row, Row::CarPreload);
            }
            {
                RowList::Row row;
                row.title = L"Preload track textures";
                row.detail = L"trackTexturePreload";
                row.toggle = trackPreload_ ? 1 : 0;
                row.toggleEnabled = iniAvailable;
                add(row, Row::TrackPreload);
            }
            {
                RowList::Row row;
                row.title = L"Streaming texture size";
                row.detail = L"streamingTextureSize  \u00B7  " + std::to_wstring(streamingSize_) + (streamingSize_ == 256 ? L"" : L"  (suggested: 256)");
                const bool expanded = expandedSize_;
                row.iconButtons = {expanded ? kCollapseGlyph : kExpandGlyph};
                row.expandHeight = expanded ? 50 : 0;
                row.muted = !iniAvailable;
                add(row, Row::StreamingSize);
            }
            {
                RowList::Row row;
                const bool running = IsIRacingRunning();
                row.title = L"Save to app.ini";
                if (!iniAvailable) row.detail = L"app.ini was not found. Start iRacing once to create it.";
                else if (!iniMessage_.empty()) row.detail = iniMessage_;
                else if (running) row.detail = L"Close iRacing first, otherwise it overwrites the file.";
                else if (IniDiffers()) row.detail = L"Not saved yet. A backup of app.ini is made first.";
                else row.detail = L"app.ini matches these settings.";
                row.button = L"Save";
                row.buttonEnabled = iniAvailable && !running && IniDiffers();
                if (IniDiffers()) { row.pill = L"Unsaved"; row.pillTone = RowList::Tone::Warning; }
                add(row, Row::SaveIni);
            }

            header(L"Microsoft Defender");
            {
                RowList::Row row;
                row.title = L"Don't scan the iRacing folders";
                const bool bothExcluded = check_->defenderReadable ? check_->installExcluded && check_->documentsExcluded : defenderVerified_;
                if (check_->install.empty()) row.detail = L"The iRacing install folder is unknown for this rule.";
                else if (!defenderMessage_.empty()) row.detail = defenderMessage_;
                else if (check_->defenderReadable)
                    row.detail = std::wstring(L"Install folder: ") + (check_->installExcluded ? L"excluded" : L"scanned") +
                        L"  \u00B7  Documents\\iRacing: " + (check_->documentsExcluded ? L"excluded" : L"scanned");
                else row.detail = defenderVerified_ ? L"Added and verified this session."
                                                    : L"Windows shows the current exclusions only to administrators.";
                row.button = bothExcluded ? L"Remove exclusions" : L"Add exclusions";
                row.buttonEnabled = !check_->install.empty() && !check_->documents.empty();
                if (bothExcluded) { row.pill = L"Excluded"; row.pillTone = RowList::Tone::Active; }
                add(row, Row::Defender);
            }

            header(L"System check");
            {
                RowList::Row row;
                row.title = L"Active power plan";
                row.detail = check_->activePowerPlan.empty() ? L"Unknown" : check_->activePowerPlan;
                const bool balanced = check_->activePowerPlan.find(L"Balanced") != std::wstring::npos ||
                    check_->activePowerPlan.find(L"Ausbalanciert") != std::wstring::npos;
                if (check_->x3dProcessor && !balanced)
                {
                    row.pill = L"X3D: try Balanced";
                    row.pillTone = RowList::Tone::Warning;
                }
                add(row, Row::None);
            }
            for (const auto& display : check_->displays)
            {
                RowList::Row row;
                row.title = display.name;
                row.detail = std::to_wstring(display.current) + L" Hz";
                if (display.maximum > display.current)
                {
                    row.pill = std::to_wstring(display.maximum) + L" Hz available";
                    row.pillTone = RowList::Tone::Warning;
                }
                else
                {
                    row.pill = L"Highest refresh rate";
                    row.pillTone = RowList::Tone::Active;
                }
                add(row, Row::None);
            }
            for (const auto& [name, state] : {std::pair{L"RivaTuner Statistics Server", check_->rtss},
                     std::pair{L"MSI Afterburner", check_->afterburner}})
            {
                RowList::Row row;
                row.title = name;
                row.detail = state;
                if (state == L"Running")
                {
                    row.detail += L"  \u00B7  overlays can cost frames; close it under Close apps if you don't need it";
                    row.pill = L"Running";
                    row.pillTone = RowList::Tone::Warning;
                }
                add(row, Row::None);
            }
            {
                RowList::Row row;
                row.title = task_.Running() ? L"Checking..." : L"Check again";
                row.detail = L"Reads app.ini, Defender, displays and overlays again.";
                row.button = L"Check";
                row.buttonEnabled = !task_.Running();
                add(row, Row::CheckAgain);
            }
            list_.SetRows(std::move(rows));
        }

        int RowIndex(Row kind) const
        {
            for (size_t index = 0; index < rows_.size(); ++index)
                if (rows_[index] == kind) return static_cast<int>(index);
            return -1;
        }

        void ToggleSizeEditor()
        {
            expandedSize_ = !expandedSize_;
            Refresh();
            editors_.Clear();
            if (!expandedSize_) return;
            syncing_ = true;
            editors_.Begin(RowIndex(Row::StreamingSize));
            editors_.Label(L"Size", 0, 12, 100);
            editors_.Add(L"EDIT", std::to_wstring(streamingSize_).c_str(), WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, kSizeId, 104, 12, 80);
            syncing_ = false;
            editors_.Position();
        }

        void SaveIni()
        {
            std::wstring error;
            if (SaveIRacingIni(check_->documents, carPreload_, trackPreload_, streamingSize_, error))
            {
                iniMessage_ = L"Saved. The previous app.ini was kept as a backup next to it.";
                iniDirty_ = false;
                Check();
            }
            else iniMessage_ = error;
            Refresh();
        }

        void ChangeDefender()
        {
            const bool remove = check_->defenderReadable ? check_->installExcluded && check_->documentsExcluded : defenderVerified_;
            const std::wstring prompt = (remove ? std::wstring(L"Scan these folders again?\n\n") :
                std::wstring(L"Stop Microsoft Defender from scanning these folders?\n\n")) + check_->install.wstring() + L"\n" +
                check_->documents.wstring() + (remove ? L"" : L"\n\nFiles in them get less antivirus protection.") +
                L"\n\nWindows asks for administrator approval.";
            if (MessageBoxW(Handle(), prompt.c_str(), L"Defender exclusions", MB_YESNO | (remove ? MB_ICONQUESTION : MB_ICONWARNING)) != IDYES)
                return;
            std::wstring error;
            if (ChangeIRacingDefenderExclusions(check_->install, check_->documents, remove, error))
            {
                defenderVerified_ = !remove;
                defenderMessage_ = remove ? L"Removed. The folders are scanned again." : L"Added and verified.";
                Check();
            }
            else defenderMessage_ = error;
            Refresh();
        }

        bool OnCommand(int id, int code, HWND) override
        {
            if (id == kSizeId)
            {
                if (syncing_ || code != EN_CHANGE) return true;
                streamingSize_ = std::max(0, _wtoi(editors_.Text(kSizeId).c_str()));
                iniDirty_ = true;
                iniMessage_.clear();
                Refresh();
                return true;
            }
            if (id != kListId) return false;
            if (code == RowList::kLayoutChanged) { editors_.Position(); return true; }
            const int index = list_.NotifiedRow();
            if (index < 0 || static_cast<size_t>(index) >= rows_.size()) return true;
            switch (rows_[static_cast<size_t>(index)])
            {
            case Row::CarPreload:
            case Row::TrackPreload:
                if (code != RowList::kToggled) break;
                (rows_[static_cast<size_t>(index)] == Row::CarPreload ? carPreload_ : trackPreload_) =
                    list_.Rows()[static_cast<size_t>(index)].toggle == 1;
                iniDirty_ = true;
                iniMessage_.clear();
                Refresh();
                break;
            case Row::StreamingSize:
                if (code == RowList::kActivated || code == RowList::kIconButton) ToggleSizeEditor();
                break;
            case Row::SaveIni:
                if (code == RowList::kButton) SaveIni();
                break;
            case Row::Defender:
                if (code == RowList::kButton) ChangeDefender();
                break;
            case Row::CheckAgain:
                if (code == RowList::kButton) { iniMessage_.clear(); defenderMessage_.clear(); Check(); }
                break;
            default:
                break;
            }
            return true;
        }

        WatchedProcessRule& rule_;
        RowList list_;
        RowEditors editors_;
        BackgroundTask<IRacingCheck> task_;
        std::optional<IRacingCheck> check_;
        std::vector<Row> rows_;
        bool carPreload_{};
        bool trackPreload_{};
        int streamingSize_{256};
        bool iniDirty_{};
        bool expandedSize_{};
        bool defenderVerified_{};
        bool syncing_{};
        std::wstring iniMessage_;
        std::wstring defenderMessage_;
    };
}

HWND CreateIRacingPage(const PageContext& context, HWND parent, WatchedProcessRule& rule)
{
    return PageWindow::Show(std::make_unique<IRacingPage>(context, rule), context.instance, parent, 560, 360);
}
