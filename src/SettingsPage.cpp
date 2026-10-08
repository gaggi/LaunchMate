#include "Pages.h"

#include "IRacingPerformance.h"
#include "UpdateChecker.h"
#include "ui/BackgroundTask.h"
#include "ui/PageWindow.h"
#include "ui/RowList.h"

#include <algorithm>
#include <objbase.h>
#include <optional>
#include <utility>

namespace
{
    constexpr int kListId = 100;
    constexpr UINT_PTR kMpoTimer = 1;

    // Rows of the page; headers have no key.
    enum class Setting
    {
        None,
        MinimizeToTray,
        CloseToTray,
        StartInTray,
        StartWithWindows,
        StartMonitoring,
        StartAsAdministrator,
        UseEtw,
        CheckForUpdatesOnStartup,
        CheckNow,
        Mpo
    };

    class SettingsPage : public PageWindow
    {
    public:
        SettingsPage(const PageContext& context, std::function<void()> checkForUpdates)
            : PageWindow(context.headingFont, context.textFont), context_(context), checkForUpdates_(std::move(checkForUpdates))
        {
        }

    private:
        void OnCreate() override
        {
            list_.Create(Instance(), Handle(), kListId, HeadingFont(), TextFont());
            Refresh();
        }

        void OnSize(int width, int height) override
        {
            Place(list_.Handle(), 0, 0, std::min(width, 760), height);
        }

        bool OnCommand(int id, int code, HWND) override
        {
            if (id != kListId) return false;
            const int index = list_.NotifiedRow();
            if (index < 0 || static_cast<size_t>(index) >= keys_.size()) return true;
            const Setting setting = keys_[static_cast<size_t>(index)];
            if (code == RowList::kToggled) Toggle(setting, list_.Rows()[static_cast<size_t>(index)].toggle == 1);
            else if (code == RowList::kButton && setting == Setting::CheckNow) checkForUpdates_();
            else if (code == RowList::kButton && setting == Setting::Mpo) ChangeMpoSetting();
            return true;
        }

        void Toggle(Setting setting, bool on)
        {
            auto& config = *context_.configuration;
            switch (setting)
            {
            case Setting::MinimizeToTray: config.minimizeToTray = on; break;
            case Setting::CloseToTray: config.closeToTray = on; break;
            case Setting::StartInTray: config.startInTray = on; break;
            case Setting::StartWithWindows: config.startWithWindows = on; break;
            case Setting::StartMonitoring: config.startMonitoringOnLaunch = on; break;
            case Setting::CheckForUpdatesOnStartup: config.checkForUpdatesOnStartup = on; break;
            case Setting::StartAsAdministrator:
                // Instant detection needs administrator rights, so the two move together.
                config.startAsAdministrator = on;
                if (!on) config.useEtw = false;
                break;
            case Setting::UseEtw:
                config.useEtw = on;
                if (on) config.startAsAdministrator = true;
                break;
            default:
                return;
            }
            // The startup task changes immediately (and may ask for approval); a declined
            // change is reverted by the save, so read everything back afterwards.
            context_.saveNow();
            Refresh();
        }

        // The approval prompt can take a while; keep the window responsive meanwhile.
        void ChangeMpoSetting()
        {
            if (mpoTask_.Running()) return;
            const bool disable = !ReadMpoState().customized;
            mpoTask_.Start([disable](const std::atomic_bool&)
            {
                // ShellExecuteEx with "runas" expects COM on the calling thread.
                const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                std::wstring error;
                const bool ok = ChangeMpo(disable, error);
                if (SUCCEEDED(com)) CoUninitialize();
                return std::make_pair(ok, error);
            });
            mpoError_.clear();
            SetTimer(Handle(), kMpoTimer, 200, nullptr);
            Refresh();
        }

        void OnTimer(UINT_PTR id) override
        {
            if (id != kMpoTimer) return;
            std::optional<std::pair<bool, std::wstring>> result;
            if (!mpoTask_.Poll(result)) return;
            KillTimer(Handle(), kMpoTimer);
            mpoError_ = !result ? L"The setting could not be changed." : result->first ? std::wstring{} : result->second;
            Refresh();
        }

        void OnDestroy() override
        {
            KillTimer(Handle(), kMpoTimer);
            mpoTask_.Cancel();
        }

        void Refresh()
        {
            const auto& config = *context_.configuration;
            std::vector<RowList::Row> rows;
            keys_.clear();
            const auto header = [&](const wchar_t* title)
            {
                RowList::Row row;
                row.header = true;
                row.title = title;
                rows.push_back(std::move(row));
                keys_.push_back(Setting::None);
            };
            const auto toggle = [&](Setting key, const wchar_t* title, const wchar_t* detail, bool on)
            {
                RowList::Row row;
                row.title = title;
                row.detail = detail;
                row.toggle = on ? 1 : 0;
                rows.push_back(std::move(row));
                keys_.push_back(key);
            };

            header(L"Window");
            toggle(Setting::MinimizeToTray, L"Minimize to tray", L"The minimize button hides LaunchMate in the notification area.", config.minimizeToTray);
            toggle(Setting::CloseToTray, L"Close to tray", L"Closing the window keeps LaunchMate running in the notification area.", config.closeToTray);
            toggle(Setting::StartInTray, L"Start in tray", L"Opens hidden in the notification area.", config.startInTray);

            header(L"Startup");
            toggle(Setting::StartWithWindows, L"Start with Windows", L"Starts LaunchMate when you sign in.", config.startWithWindows);
            toggle(Setting::StartMonitoring, L"Start monitoring when LaunchMate starts", L"", config.startMonitoringOnLaunch);

            header(L"Monitoring");
            toggle(Setting::StartAsAdministrator, L"Start as administrator",
                L"Needed for Windows services and instant detection. Takes effect at the next start.", config.startAsAdministrator);
            toggle(Setting::UseEtw, L"Instant detection (ETW)",
                L"Reacts the moment a program starts instead of checking every second.", config.useEtw);
            rows.back().pill = L"Recommended";
            rows.back().pillTone = RowList::Tone::Accent;

            header(L"Updates");
            toggle(Setting::CheckForUpdatesOnStartup, L"Check for updates on startup", L"", config.checkForUpdatesOnStartup);
            {
                RowList::Row row;
                row.title = L"Version " + UpdateChecker::CurrentVersion();
                row.detail = L"Look for a newer release on GitHub.";
                row.button = L"Check now";
                rows.push_back(std::move(row));
                keys_.push_back(Setting::CheckNow);
            }

            header(L"System tweaks");
            {
                const auto state = ReadMpoState();
                RowList::Row row;
                row.title = L"Multiplane overlay (MPO)";
                if (mpoTask_.Running()) row.detail = L"Waiting for administrator approval...";
                else if (!mpoError_.empty()) row.detail = mpoError_;
                else if (!state.readable) row.detail = L"The current setting cannot be read.";
                else row.detail = std::wstring(state.disabled ? L"Disabled" : state.customized ? L"Partly changed" : L"Windows default") +
                    L"  \u00B7  May help with flickering or stutter from overlays. Needs a Windows restart.";
                row.button = state.customized ? L"Restore default" : L"Disable MPO";
                row.buttonEnabled = state.readable && !mpoTask_.Running();
                if (state.disabled) { row.pill = L"Disabled"; row.pillTone = RowList::Tone::Warning; }
                rows.push_back(std::move(row));
                keys_.push_back(Setting::Mpo);
            }
            list_.SetRows(std::move(rows));
        }

        PageContext context_;
        std::function<void()> checkForUpdates_;
        RowList list_;
        std::vector<Setting> keys_;
        std::wstring mpoError_;
        BackgroundTask<std::pair<bool, std::wstring>> mpoTask_;
    };
}

HWND CreateSettingsPage(const PageContext& context, HWND parent, std::function<void()> checkForUpdates)
{
    return PageWindow::Show(std::make_unique<SettingsPage>(context, std::move(checkForUpdates)), context.instance, parent, 520, 360);
}
