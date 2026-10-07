#include "Pages.h"

#include "ui/PageWindow.h"
#include "ui/RowEditors.h"
#include "ui/RowList.h"

#include <algorithm>

namespace
{
    constexpr int kListId = 100;
    constexpr int kAddId = 101;
    constexpr int kNameId = 200;
    constexpr int kUrlId = 201;
    constexpr int kDelayId = 202;
    constexpr int kPayloadId = 203;
    constexpr int kExpandHeight = 250;
    constexpr wchar_t kExpandGlyph = L'\uE70D';
    constexpr wchar_t kCollapseGlyph = L'\uE70E';
    constexpr wchar_t kRemoveGlyph = L'\uE711';

    bool LooksLikeUrl(const std::wstring& url)
    {
        return _wcsnicmp(url.c_str(), L"http://", 7) == 0 || _wcsnicmp(url.c_str(), L"https://", 8) == 0;
    }

    // Payloads are stored with \n; the edit control needs \r\n.
    std::wstring ToEditText(const std::wstring& text)
    {
        std::wstring result;
        for (const wchar_t character : text)
        {
            if (character == L'\n' && (result.empty() || result.back() != L'\r')) result += L'\r';
            result += character;
        }
        return result;
    }

    std::wstring FromEditText(const std::wstring& text)
    {
        std::wstring result;
        for (const wchar_t character : text)
            if (character != L'\r') result += character;
        return result;
    }

    class WebhooksPage : public PageWindow
    {
    public:
        WebhooksPage(const PageContext& context, WatchedProcessRule& rule)
            : PageWindow(context.headingFont, context.textFont), context_(context), rule_(rule)
        {
        }

    private:
        void OnCreate() override
        {
            AddButton(kAddId, L"Add webhook");
            list_.Create(Instance(), Handle(), kListId, HeadingFont(), TextFont());
            list_.SetEmptyText(L"No webhooks yet. Add one to switch lights or other Home Assistant automations when the game starts.");
            editors_.Attach(list_, Instance(), TextFont());
            Refresh();
        }

        void OnSize(int width, int height) override
        {
            const int contentWidth = std::min(width, 820);
            Place(GetDlgItem(Handle(), kAddId), 0, 0, 130, 30);
            Place(list_.Handle(), 0, 40, contentWidth, height - 40);
        }

        void Refresh()
        {
            std::vector<RowList::Row> rows;
            for (size_t index = 0; index < rule_.homeAssistantActions.size(); ++index)
            {
                const auto& action = rule_.homeAssistantActions[index];
                const bool expanded = static_cast<int>(index) == expanded_;
                RowList::Row row;
                row.title = action.displayName.empty() ? L"Webhook" : action.displayName;
                row.detail = action.webhookUrl.empty() ? L"No URL yet" : action.webhookUrl;
                if (action.waitTimeMilliseconds > 0) row.detail += L"  \u00B7  after " + RowEditors::SecondsText(action.waitTimeMilliseconds) + L" s";
                if (!action.webhookUrl.empty() && !LooksLikeUrl(action.webhookUrl))
                {
                    row.pill = L"Check URL";
                    row.pillTone = RowList::Tone::Warning;
                }
                row.iconButtons = {expanded ? kCollapseGlyph : kExpandGlyph, kRemoveGlyph};
                row.expandHeight = expanded ? kExpandHeight : 0;
                rows.push_back(std::move(row));
            }
            list_.SetRows(std::move(rows));
        }

        void BuildEditors()
        {
            editors_.Clear();
            if (expanded_ < 0) return;
            syncing_ = true;
            editors_.Begin(expanded_);
            const auto& action = rule_.homeAssistantActions[static_cast<size_t>(expanded_)];
            editors_.Label(L"Name", 0, 12, 100);
            editors_.Edit(kNameId, action.displayName, 104, 12, 240);
            editors_.Label(L"Webhook URL", 0, 48, 100);
            editors_.Edit(kUrlId, action.webhookUrl, 104, 48, 0);
            editors_.Label(L"Call after", 0, 84, 100);
            editors_.Seconds(kDelayId, action.waitTimeMilliseconds, 104, 84);
            editors_.Label(L"JSON payload", 0, 120, 100);
            editors_.Add(L"EDIT", ToEditText(action.jsonPayload).c_str(),
                WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL, kPayloadId, 104, 120, 0, 110);
            syncing_ = false;
            editors_.Position();
        }

        void Expand(int index)
        {
            expanded_ = expanded_ == index ? -1 : index;
            Refresh();
            BuildEditors();
            if (expanded_ >= 0) list_.ScrollIntoView(expanded_);
        }

        void Remove(int index)
        {
            auto& actions = rule_.homeAssistantActions;
            if (index < 0 || index >= static_cast<int>(actions.size())) return;
            actions.erase(actions.begin() + index);
            if (index == expanded_) { expanded_ = -1; editors_.Clear(); }
            else if (index < expanded_) --expanded_;
            context_.scheduleSave();
            Refresh();
            editors_.Position();
        }

        bool OnCommand(int id, int code, HWND) override
        {
            if (id == kAddId)
            {
                HomeAssistantAction action;
                action.displayName = L"Home Assistant";
                rule_.homeAssistantActions.push_back(std::move(action));
                context_.scheduleSave();
                expanded_ = -1;
                Expand(static_cast<int>(rule_.homeAssistantActions.size()) - 1);
                SetFocus(editors_.Get(kUrlId));
                return true;
            }
            if (id >= kNameId && id <= kPayloadId)
            {
                if (syncing_ || code != EN_CHANGE || expanded_ < 0) return true;
                auto& action = rule_.homeAssistantActions[static_cast<size_t>(expanded_)];
                if (id == kNameId) action.displayName = editors_.Text(id);
                else if (id == kUrlId) action.webhookUrl = editors_.Text(id);
                else if (id == kPayloadId) action.jsonPayload = FromEditText(editors_.Text(id));
                else
                {
                    int milliseconds = 0;
                    if (!RowEditors::ParseSeconds(editors_.Text(id), milliseconds)) return true;
                    action.waitTimeMilliseconds = milliseconds;
                }
                context_.scheduleSave();
                Refresh();
                return true;
            }
            if (id != kListId) return false;
            if (code == RowList::kLayoutChanged) { editors_.Position(); return true; }
            const int row = list_.NotifiedRow();
            if (row < 0) return true;
            if (code == RowList::kActivated || (code == RowList::kIconButton && list_.NotifiedIconButton() == 0)) Expand(row);
            else if ((code == RowList::kIconButton && list_.NotifiedIconButton() == 1) || code == RowList::kDeleteRequested) Remove(row);
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

HWND CreateWebhooksPage(const PageContext& context, HWND parent, WatchedProcessRule& rule)
{
    return PageWindow::Show(std::make_unique<WebhooksPage>(context, rule), context.instance, parent, 560, 360);
}
