#include "Pages.h"

#include "IRacingServices.h"
#include "ui/PageWindow.h"
#include "ui/RowList.h"

#include <algorithm>

namespace
{
    constexpr int kListId = 100;
    constexpr int kRecommendedId = 101;

    class ServicesPage : public PageWindow
    {
    public:
        ServicesPage(const PageContext& context, WatchedProcessRule& rule)
            : PageWindow(context.headingFont, context.textFont), context_(context), rule_(rule)
        {
        }

    private:
        struct Service
        {
            const IRacingServiceOption* option{};
            std::wstring status;
        };

        void OnCreate() override
        {
            hint_ = AddLabel(L"Windows asks for administrator approval only when these actions run.");
            AddButton(kRecommendedId, L"Add recommended");
            list_.Create(Instance(), Handle(), kListId, HeadingFont(), TextFont());
            // Reading the service states takes a few milliseconds; once per page is enough.
            for (const auto& option : IRacingServiceOptions())
                services_.push_back({&option, DescribeIRacingService(option.name)});
            Refresh();
        }

        void OnSize(int width, int height) override
        {
            const int contentWidth = std::min(width, 760);
            Place(hint_, 0, 0, contentWidth - 150, 30);
            Place(GetDlgItem(Handle(), kRecommendedId), contentWidth - 140, 0, 140, 30);
            Place(list_.Handle(), 0, 40, contentWidth, height - 40);
        }

        bool Selected(const wchar_t* name) const
        {
            return std::any_of(rule_.servicesToStop.begin(), rule_.servicesToStop.end(),
                [name](const std::wstring& selected) { return _wcsicmp(selected.c_str(), name) == 0; });
        }

        void SetSelected(const wchar_t* name, bool selected)
        {
            std::vector<std::wstring> updated;
            // Keep the order of the option list.
            for (const auto& service : services_)
            {
                const bool keep = _wcsicmp(service.option->name, name) == 0 ? selected : Selected(service.option->name);
                if (keep) updated.push_back(service.option->name);
            }
            rule_.servicesToStop = std::move(updated);
            context_.scheduleSave();
        }

        bool OnCommand(int id, int code, HWND) override
        {
            if (id == kRecommendedId)
            {
                for (const auto& service : services_)
                    if (service.option->disableWhileRacing && service.status != L"not installed") SetSelected(service.option->name, true);
                Refresh();
                return true;
            }
            if (id != kListId || code != RowList::kToggled) return false;
            const int index = list_.NotifiedRow();
            if (index >= 0 && static_cast<size_t>(index) < rowServices_.size() && rowServices_[static_cast<size_t>(index)])
                SetSelected(rowServices_[static_cast<size_t>(index)]->option->name, list_.Rows()[static_cast<size_t>(index)].toggle == 1);
            Refresh();
            return true;
        }

        void Refresh()
        {
            std::vector<RowList::Row> rows;
            rowServices_.clear();
            for (const bool disable : {true, false})
            {
                RowList::Row header;
                header.header = true;
                header.title = disable ? L"Disable while racing  \u00B7  their start type is restored afterwards"
                                       : L"Stop while racing  \u00B7  restarted afterwards if they were running";
                rows.push_back(std::move(header));
                rowServices_.push_back(nullptr);
                for (const auto& service : services_)
                {
                    if (service.option->disableWhileRacing != disable) continue;
                    const bool selected = Selected(service.option->name);
                    const bool installed = service.status != L"not installed";
                    RowList::Row row;
                    row.title = service.option->label;
                    row.detail = service.option->description;
                    row.toggle = selected ? 1 : 0;
                    // A missing service can still be switched off if an old rule selected it.
                    row.toggleEnabled = installed || selected;
                    row.muted = !installed;
                    if (!installed) row.pill = L"Not installed";
                    else if (service.status.find(L"disabled") != std::wstring::npos) { row.pill = L"Disabled"; row.pillTone = RowList::Tone::Warning; }
                    else if (service.status.starts_with(L"running")) { row.pill = L"Running"; row.pillTone = RowList::Tone::Active; }
                    else if (service.status.starts_with(L"stopped")) row.pill = L"Stopped";
                    else row.pill = L"Unknown";
                    rows.push_back(std::move(row));
                    rowServices_.push_back(&service);
                }
            }
            list_.SetRows(std::move(rows));
        }

        PageContext context_;
        WatchedProcessRule& rule_;
        RowList list_;
        HWND hint_{};
        std::vector<Service> services_;
        // Service shown in each row; null for headers.
        std::vector<const Service*> rowServices_;
    };
}

HWND CreateServicesPage(const PageContext& context, HWND parent, WatchedProcessRule& rule)
{
    return PageWindow::Show(std::make_unique<ServicesPage>(context, rule), context.instance, parent, 520, 360);
}
