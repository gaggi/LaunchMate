#pragma once

#include "UiTheme.h"

#include <algorithm>
#include <string>
#include <vector>
#include <windows.h>
#include <windowsx.h>

// A vertical list of clickable cards (title, subtitle, status pill, chips), used for
// the rule overview and the start/exit columns of a rule. Notifies its parent with
// WM_COMMAND(MAKEWPARAM(id, notification), hwnd); FocusedIndex() names the card.
class CardList
{
public:
    enum Notification : WORD
    {
        kActivated = 1,
        kContextMenu = 2,
        kDeleteRequested = 3
    };

    enum class Tone
    {
        None,
        Neutral,
        Active,
        Warning
    };

    struct Item
    {
        std::wstring title;
        std::wstring subtitle;
        std::wstring pill;
        Tone pillTone{Tone::None};
        std::vector<std::wstring> chips;
        std::wstring trailing;
        // Rendered in gray, e.g. an action group that is not used yet.
        bool muted{};
        bool operator==(const Item&) const = default;
    };

    CardList() = default;
    CardList(const CardList&) = delete;
    CardList& operator=(const CardList&) = delete;
    ~CardList()
    {
        if (glyphFont_) DeleteObject(glyphFont_);
    }

    bool Create(HINSTANCE instance, HWND parent, int id, HFONT titleFont, HFONT textFont)
    {
        WNDCLASSW windowClass{};
        if (!GetClassInfoW(instance, kClassName, &windowClass))
        {
            windowClass.style = CS_DBLCLKS;
            windowClass.lpfnWndProc = WindowProc;
            windowClass.hInstance = instance;
            windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            windowClass.lpszClassName = kClassName;
            if (!RegisterClassW(&windowClass)) return false;
        }
        id_ = id;
        titleFont_ = titleFont;
        textFont_ = textFont;
        window_ = CreateWindowExW(0, kClassName, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL,
            0, 0, 100, 100, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, this);
        if (window_) CreateGlyphFont();
        return window_ != nullptr;
    }

    HWND Handle() const noexcept { return window_; }
    int FocusedIndex() const noexcept { return focused_; }
    size_t Count() const noexcept { return items_.size(); }

    void SetFonts(HFONT titleFont, HFONT textFont)
    {
        titleFont_ = titleFont;
        textFont_ = textFont;
        CreateGlyphFont();
        UpdateScrollRange();
        InvalidateRect(window_, nullptr, FALSE);
    }

    // Less vertical padding, for lists that should fit without scrolling.
    void SetCompact(bool compact)
    {
        compact_ = compact;
        UpdateScrollRange();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void SetEmptyText(std::wstring text)
    {
        emptyText_ = std::move(text);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void SetItems(std::vector<Item> items)
    {
        if (items == items_) return;
        items_ = std::move(items);
        focused_ = items_.empty() ? -1 : std::clamp(focused_, 0, static_cast<int>(items_.size()) - 1);
        hover_ = -1;
        UpdateScrollRange();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void SetFocusedIndex(int index)
    {
        focused_ = items_.empty() ? -1 : std::clamp(index, 0, static_cast<int>(items_.size()) - 1);
        EnsureVisible(focused_);
        InvalidateRect(window_, nullptr, FALSE);
    }

private:
    static constexpr wchar_t kClassName[] = L"LaunchMateCardList";

    int Scale(int value) const { return MulDiv(value, GetDpiForWindow(window_), 96); }

    int Padding() const { return Scale(compact_ ? 8 : 12); }

    int ItemHeight(const Item& item) const
    {
        int height = 2 * Padding() + Scale(20);
        if (!item.subtitle.empty()) height += Scale(18);
        if (!item.chips.empty()) height += Scale(32);
        return height;
    }

    // Content coordinates (before scrolling) of each card.
    std::vector<RECT> ItemRects() const
    {
        RECT client{};
        GetClientRect(window_, &client);
        std::vector<RECT> rects;
        rects.reserve(items_.size());
        int y = 0;
        for (const auto& item : items_)
        {
            const int height = ItemHeight(item);
            rects.push_back({0, y, client.right - Scale(1), y + height});
            y += height + Scale(compact_ ? 6 : 8);
        }
        return rects;
    }

    int ContentHeight() const
    {
        const auto rects = ItemRects();
        return rects.empty() ? 0 : rects.back().bottom + Scale(1);
    }

    void UpdateScrollRange()
    {
        RECT client{};
        GetClientRect(window_, &client);
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
        info.nMin = 0;
        info.nMax = std::max(0, ContentHeight() - 1);
        info.nPage = static_cast<UINT>(std::max<LONG>(0, client.bottom));
        scroll_ = std::clamp(scroll_, 0, std::max(0, ContentHeight() - static_cast<int>(client.bottom)));
        info.nPos = scroll_;
        SetScrollInfo(window_, SB_VERT, &info, TRUE);
    }

    void ScrollTo(int position)
    {
        RECT client{};
        GetClientRect(window_, &client);
        const int limit = std::max(0, ContentHeight() - static_cast<int>(client.bottom));
        position = std::clamp(position, 0, limit);
        if (position == scroll_) return;
        scroll_ = position;
        SetScrollPos(window_, SB_VERT, scroll_, TRUE);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void EnsureVisible(int index)
    {
        if (index < 0) return;
        const auto rects = ItemRects();
        if (static_cast<size_t>(index) >= rects.size()) return;
        RECT client{};
        GetClientRect(window_, &client);
        if (rects[index].top < scroll_) ScrollTo(rects[index].top);
        else if (rects[index].bottom > scroll_ + client.bottom) ScrollTo(rects[index].bottom - client.bottom);
    }

    int HitTest(POINT point) const
    {
        const auto rects = ItemRects();
        for (size_t index = 0; index < rects.size(); ++index)
        {
            RECT rect = rects[index];
            OffsetRect(&rect, 0, -scroll_);
            if (PtInRect(&rect, point)) return static_cast<int>(index);
        }
        return -1;
    }

    void Notify(Notification notification) const
    {
        SendMessageW(GetParent(window_), WM_COMMAND, MAKEWPARAM(id_, notification), reinterpret_cast<LPARAM>(window_));
    }

    void CreateGlyphFont()
    {
        if (glyphFont_) DeleteObject(glyphFont_);
        glyphFont_ = CreateFontW(-Scale(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, UiTheme::IconFontFace());
    }

    static void FillRounded(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border)
    {
        const HBRUSH brush = CreateSolidBrush(fill);
        const HPEN pen = CreatePen(PS_SOLID, 1, border);
        const auto oldBrush = SelectObject(dc, brush);
        const auto oldPen = SelectObject(dc, pen);
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);
    }

    void PaintItem(HDC dc, const Item& item, RECT rect, bool hover, bool focused) const
    {
        const COLORREF border = focused ? RGB(55, 138, 221) : hover ? RGB(176, 181, 189) : RGB(220, 222, 225);
        FillRounded(dc, rect, Scale(10), UiTheme::Surface, border);
        const COLORREF titleColor = item.muted ? RGB(136, 135, 128) : UiTheme::Text;
        const COLORREF detailColor = RGB(110, 112, 118);
        const int padding = Scale(14);
        int right = rect.right - padding;

        // Chevron: the whole card is a button.
        SelectObject(dc, glyphFont_ ? glyphFont_ : titleFont_);
        SetTextColor(dc, RGB(110, 112, 118));
        RECT chevron{right - Scale(14), rect.top, right, rect.bottom};
        DrawTextW(dc, glyphFont_ ? L"\uE76C" : L"\u203A", -1, &chevron, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        right -= Scale(24);

        // The count sits next to the chevron as a neutral pill, centered on the card.
        if (!item.trailing.empty())
        {
            SelectObject(dc, textFont_);
            SIZE size{};
            GetTextExtentPoint32W(dc, item.trailing.c_str(), static_cast<int>(item.trailing.size()), &size);
            const int width = std::max<int>(size.cx + Scale(16), Scale(26));
            const int height = Scale(20);
            const int top = (rect.top + rect.bottom - height) / 2;
            RECT count{right - width, top, right, top + height};
            FillRounded(dc, count, height, RGB(241, 239, 232), RGB(241, 239, 232));
            SetTextColor(dc, RGB(95, 94, 90));
            DrawTextW(dc, item.trailing.c_str(), -1, &count, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            right = count.left - Scale(10);
        }

        const int titleTop = rect.top + Padding() - Scale(1);
        if (!item.pill.empty())
        {
            SelectObject(dc, textFont_);
            SIZE size{};
            GetTextExtentPoint32W(dc, item.pill.c_str(), static_cast<int>(item.pill.size()), &size);
            RECT pill{right - size.cx - Scale(20), titleTop, right, titleTop + Scale(22)};
            COLORREF fill = RGB(241, 239, 232), text = RGB(95, 94, 90);
            if (item.pillTone == Tone::Active) { fill = RGB(234, 243, 222); text = RGB(39, 80, 10); }
            else if (item.pillTone == Tone::Warning) { fill = RGB(250, 238, 218); text = RGB(133, 79, 11); }
            FillRounded(dc, pill, Scale(22), fill, fill);
            SetTextColor(dc, text);
            DrawTextW(dc, item.pill.c_str(), -1, &pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            right = pill.left - Scale(10);
        }

        SelectObject(dc, titleFont_);
        SetTextColor(dc, titleColor);
        RECT title{rect.left + padding, titleTop, right, titleTop + Scale(22)};
        DrawTextW(dc, item.title.c_str(), -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        int y = titleTop + Scale(22);
        if (!item.subtitle.empty())
        {
            SelectObject(dc, textFont_);
            SetTextColor(dc, detailColor);
            RECT subtitle{rect.left + padding, y, rect.right - Scale(36), y + Scale(18)};
            DrawTextW(dc, item.subtitle.c_str(), -1, &subtitle,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            y += Scale(18);
        }
        if (!item.chips.empty())
        {
            SelectObject(dc, textFont_);
            SetTextColor(dc, RGB(75, 80, 90));
            int x = rect.left + padding;
            const int top = y + Scale(8);
            for (const auto& chipText : item.chips)
            {
                SIZE size{};
                GetTextExtentPoint32W(dc, chipText.c_str(), static_cast<int>(chipText.size()), &size);
                RECT chip{x, top, x + size.cx + Scale(16), top + Scale(22)};
                if (chip.right > rect.right - Scale(36)) break;
                FillRounded(dc, chip, Scale(8), RGB(241, 242, 244), RGB(241, 242, 244));
                DrawTextW(dc, chipText.c_str(), -1, &chip, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                x = chip.right + Scale(6);
            }
        }
    }

    void Paint(HDC target)
    {
        RECT client{};
        GetClientRect(window_, &client);
        const HDC dc = CreateCompatibleDC(target);
        const HBITMAP bitmap = CreateCompatibleBitmap(target, std::max<LONG>(1, client.right), std::max<LONG>(1, client.bottom));
        const auto oldBitmap = SelectObject(dc, bitmap);
        FillRect(dc, &client, UiTheme::BackgroundBrush());
        SetBkMode(dc, TRANSPARENT);
        const auto oldFont = SelectObject(dc, textFont_);
        if (items_.empty() && !emptyText_.empty())
        {
            SetTextColor(dc, RGB(110, 112, 118));
            RECT text{client.left + Scale(4), client.top + Scale(8), client.right - Scale(4), client.bottom};
            DrawTextW(dc, emptyText_.c_str(), -1, &text, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        }
        const bool hasFocus = GetFocus() == window_;
        const auto rects = ItemRects();
        for (size_t index = 0; index < items_.size(); ++index)
        {
            RECT rect = rects[index];
            OffsetRect(&rect, 0, -scroll_);
            if (rect.bottom < 0 || rect.top > client.bottom) continue;
            PaintItem(dc, items_[index], rect, static_cast<int>(index) == hover_,
                hasFocus && static_cast<int>(index) == focused_);
        }
        SelectObject(dc, oldFont);
        BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window_, &paint);
            Paint(dc);
            EndPaint(window_, &paint);
            return 0;
        }
        case WM_SIZE:
            UpdateScrollRange();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_VSCROLL:
        {
            SCROLLINFO info{sizeof(info), SIF_ALL};
            GetScrollInfo(window_, SB_VERT, &info);
            int position = scroll_;
            switch (LOWORD(wParam))
            {
            case SB_LINEUP: position -= Scale(40); break;
            case SB_LINEDOWN: position += Scale(40); break;
            case SB_PAGEUP: position -= static_cast<int>(info.nPage); break;
            case SB_PAGEDOWN: position += static_cast<int>(info.nPage); break;
            case SB_THUMBTRACK: case SB_THUMBPOSITION: position = info.nTrackPos; break;
            case SB_TOP: position = 0; break;
            case SB_BOTTOM: position = info.nMax; break;
            }
            ScrollTo(position);
            return 0;
        }
        case WM_MOUSEWHEEL:
            ScrollTo(scroll_ - GET_WHEEL_DELTA_WPARAM(wParam) * Scale(48) / WHEEL_DELTA);
            return 0;
        case WM_MOUSEMOVE:
        {
            const int hit = HitTest({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            if (hit != hover_)
            {
                hover_ = hit;
                InvalidateRect(window_, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_};
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = -1;
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT && hover_ >= 0)
            {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        {
            SetFocus(window_);
            pressed_ = HitTest({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            if (pressed_ >= 0) focused_ = pressed_;
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP:
        {
            const int hit = HitTest({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            if (hit >= 0 && hit == pressed_) Notify(kActivated);
            pressed_ = -1;
            return 0;
        }
        case WM_RBUTTONUP:
        {
            const int hit = HitTest({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            if (hit >= 0 && hit == pressed_) Notify(kContextMenu);
            pressed_ = -1;
            return 0;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        case WM_KEYDOWN:
            if (items_.empty()) break;
            switch (wParam)
            {
            case VK_UP: SetFocusedIndex(std::max(0, focused_ - 1)); return 0;
            case VK_DOWN: SetFocusedIndex(focused_ + 1); return 0;
            case VK_HOME: SetFocusedIndex(0); return 0;
            case VK_END: SetFocusedIndex(static_cast<int>(items_.size()) - 1); return 0;
            case VK_RETURN: case VK_SPACE: if (focused_ >= 0) Notify(kActivated); return 0;
            case VK_DELETE: if (focused_ >= 0) Notify(kDeleteRequested); return 0;
            case VK_APPS: if (focused_ >= 0) Notify(kContextMenu); return 0;
            }
            break;
        case WM_SETFOCUS:
            if (focused_ < 0 && !items_.empty()) focused_ = 0;
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_KILLFOCUS:
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<CardList*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<CardList*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->Handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
    }

    HWND window_{};
    int id_{};
    HFONT titleFont_{};
    HFONT textFont_{};
    HFONT glyphFont_{};
    std::vector<Item> items_;
    std::wstring emptyText_;
    int scroll_{};
    int hover_{-1};
    int focused_{-1};
    int pressed_{-1};
    bool compact_{};
};
