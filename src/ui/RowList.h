#pragma once

#include "UiTheme.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

// Settings-style list: rows grouped into white cards under small section headers.
// A row can carry a file icon, a title with an optional detail line, a status pill,
// a toggle switch, one text button and icon buttons. Clicks arrive at the parent as
// WM_COMMAND(MAKEWPARAM(id, notification), hwnd); NotifiedRow() and
// NotifiedIconButton() say which row and icon button were used.
//
// A row with expandHeight > 0 shows an extra area below its top line. The parent
// creates edit controls there as children of Handle(), places them with
// ExpansionRect() and moves them again on kLayoutChanged (scrolling, resizing).
// Their WM_COMMAND messages are passed on to the parent unchanged.
class RowList
{
public:
    enum Notification : WORD
    {
        kToggled = 1,
        kButton = 2,
        kIconButton = 3,
        kActivated = 4,
        kDeleteRequested = 5,
        // Rows moved (scrolled, resized or replaced); reposition controls in expansions.
        kLayoutChanged = 6
    };

    enum class Tone
    {
        Neutral,
        Active,
        Warning,
        Accent
    };

    struct Row
    {
        // A header starts a new card; the rows after it belong to that card.
        bool header{};
        std::wstring title;
        std::wstring detail;
        std::wstring pill;
        Tone pillTone{Tone::Neutral};
        bool showIcon{};
        std::wstring iconPath;
        // -1: no toggle, 0: off, 1: on.
        int toggle{-1};
        std::wstring toggleLabel;
        bool toggleEnabled{true};
        std::wstring button;
        bool buttonEnabled{true};
        // Glyphs from Segoe Fluent Icons / Segoe MDL2 Assets.
        std::vector<wchar_t> iconButtons;
        bool muted{};
        bool selected{};
        // Height in DIPs of the expanded area below the row; 0 when collapsed.
        int expandHeight{};
        bool operator==(const Row&) const = default;
    };

    RowList() = default;
    RowList(const RowList&) = delete;
    RowList& operator=(const RowList&) = delete;
    ~RowList() { ReleaseResources(); }

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
        window_ = CreateWindowExW(WS_EX_CONTROLPARENT, kClassName, nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_CLIPCHILDREN,
            0, 0, 100, 100, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, this);
        if (!window_) return false;
        SetFonts(titleFont, textFont);
        return true;
    }

    HWND Handle() const noexcept { return window_; }
    int NotifiedRow() const noexcept { return notifiedRow_; }
    int NotifiedIconButton() const noexcept { return notifiedIconButton_; }
    const std::vector<Row>& Rows() const noexcept { return rows_; }

    void SetFonts(HFONT titleFont, HFONT textFont)
    {
        titleFont_ = titleFont;
        textFont_ = textFont;
        if (glyphFont_) DeleteObject(glyphFont_);
        glyphFont_ = CreateFontW(-Scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, UiTheme::IconFontFace());
        UpdateScrollRange();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void SetEmptyText(std::wstring text)
    {
        emptyText_ = std::move(text);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void SetRows(std::vector<Row> rows)
    {
        if (rows == rows_) return;
        rows_ = std::move(rows);
        if (focused_ >= static_cast<int>(rows_.size())) focused_ = -1;
        hover_ = {};
        UpdateScrollRange();
        InvalidateRect(window_, nullptr, FALSE);
        NotifyLayout();
    }

    // Client rectangle of a row's expanded area; empty when the row is collapsed.
    RECT ExpansionRect(int index) const
    {
        const auto layouts = Compute();
        if (index < 0 || index >= static_cast<int>(layouts.size())) return {};
        RECT rect = layouts[static_cast<size_t>(index)].expansion;
        OffsetRect(&rect, 0, -scroll_);
        return rect;
    }

    // Keeps the expanded area of a row in view, e.g. right after expanding it.
    void ScrollIntoView(int index)
    {
        EnsureVisible(index);
    }

private:
    static constexpr wchar_t kClassName[] = L"LaunchMateRowList";

    enum class PartKind { None, Body, Toggle, Button, IconButton };
    struct Part
    {
        int row{-1};
        PartKind kind{PartKind::None};
        int iconButton{-1};
        bool operator==(const Part&) const = default;
    };

    struct Layout
    {
        RECT bounds{};      // the row inside its card
        RECT toggle{};
        RECT button{};
        std::vector<RECT> iconButtons;
        RECT pill{};
        RECT text{};
        RECT icon{};
        RECT expansion{};
        bool firstInCard{};
        bool lastInCard{};
    };

    int Scale(int value) const { return MulDiv(value, GetDpiForWindow(window_), 96); }

    void ReleaseResources()
    {
        for (auto& [path, icon] : icons_)
            if (icon) DestroyIcon(icon);
        icons_.clear();
        if (glyphFont_) DeleteObject(glyphFont_);
        glyphFont_ = nullptr;
    }

    HICON IconFor(const std::wstring& path)
    {
        const auto found = icons_.find(path);
        if (found != icons_.end()) return found->second;
        SHFILEINFOW info{};
        const DWORD_PTR result = path.empty()
            ? SHGetFileInfoW(L".exe", FILE_ATTRIBUTE_NORMAL, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES)
            : SHGetFileInfoW(path.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON);
        const HICON icon = result != 0 ? info.hIcon : nullptr;
        icons_.emplace(path, icon);
        return icon;
    }

    int TextWidth(HDC dc, HFONT font, const std::wstring& text) const
    {
        const auto old = SelectObject(dc, font);
        SIZE size{};
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
        SelectObject(dc, old);
        return size.cx;
    }

    int RowHeight(const Row& row) const
    {
        if (row.header) return Scale(34);
        return LineHeight(row) + Scale(row.expandHeight);
    }

    // Height of the row's top line, without an expanded area.
    int LineHeight(const Row& row) const
    {
        return Scale(row.detail.empty() ? 44 : 58);
    }

    // Content coordinates (unscrolled) of every row.
    std::vector<Layout> Compute() const
    {
        RECT client{};
        GetClientRect(window_, &client);
        const int right = client.right - Scale(1);
        std::vector<Layout> layouts(rows_.size());
        const HDC dc = GetDC(window_);
        int y = 0;
        bool inCard = false;
        for (size_t index = 0; index < rows_.size(); ++index)
        {
            const auto& row = rows_[index];
            auto& layout = layouts[index];
            const int height = RowHeight(row);
            if (row.header)
            {
                if (inCard) y += Scale(14);
                layout.bounds = {0, y, right, y + height};
                layout.text = {Scale(4), y, right, y + height - Scale(8)};
                inCard = false;
                y += height;
                continue;
            }
            layout.firstInCard = !inCard;
            inCard = true;
            layout.bounds = {0, y, right, y + height};
            const int lineHeight = LineHeight(row);
            if (row.expandHeight > 0)
                layout.expansion = {Scale(14), y + lineHeight, right - Scale(14), y + height - Scale(10)};
            int x = right - Scale(12);
            const int middle = y + lineHeight / 2;
            for (size_t button = 0; button < row.iconButtons.size(); ++button)
            {
                layout.iconButtons.push_back({x - Scale(30), middle - Scale(15), x, middle + Scale(15)});
                x -= Scale(32);
            }
            if (!row.iconButtons.empty()) x -= Scale(4);
            if (!row.button.empty())
            {
                const int width = TextWidth(dc, textFont_, row.button) + Scale(28);
                layout.button = {x - width, middle - Scale(15), x, middle + Scale(15)};
                x -= width + Scale(10);
            }
            if (row.toggle >= 0)
            {
                layout.toggle = {x - Scale(40), middle - Scale(10), x, middle + Scale(10)};
                x -= Scale(40);
                if (!row.toggleLabel.empty())
                {
                    const int width = TextWidth(dc, textFont_, row.toggleLabel);
                    x -= width + Scale(10);
                }
                x -= Scale(12);
            }
            if (!row.pill.empty())
            {
                const int width = TextWidth(dc, textFont_, row.pill) + Scale(18);
                layout.pill = {x - width, middle - Scale(11), x, middle + Scale(11)};
                x -= width + Scale(12);
            }
            int left = Scale(14);
            if (row.showIcon)
            {
                layout.icon = {left, middle - Scale(10), left + Scale(20), middle + Scale(10)};
                left += Scale(32);
            }
            layout.text = {left, y, std::max<int>(left, x), y + lineHeight};
            y += height;
        }
        ReleaseDC(window_, dc);
        for (size_t index = 0; index < rows_.size(); ++index)
        {
            if (rows_[index].header) continue;
            layouts[index].lastInCard = index + 1 == rows_.size() || rows_[index + 1].header;
        }
        return layouts;
    }

    int ContentHeight() const
    {
        const auto layouts = Compute();
        return layouts.empty() ? 0 : layouts.back().bounds.bottom + Scale(2);
    }

    void UpdateScrollRange()
    {
        RECT client{};
        GetClientRect(window_, &client);
        const int content = ContentHeight();
        scroll_ = std::clamp(scroll_, 0, std::max(0, content - static_cast<int>(client.bottom)));
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
        info.nMax = std::max(0, content - 1);
        info.nPage = static_cast<UINT>(std::max<LONG>(0, client.bottom));
        info.nPos = scroll_;
        SetScrollInfo(window_, SB_VERT, &info, TRUE);
    }

    void ScrollTo(int position)
    {
        RECT client{};
        GetClientRect(window_, &client);
        position = std::clamp(position, 0, std::max(0, ContentHeight() - static_cast<int>(client.bottom)));
        if (position == scroll_) return;
        scroll_ = position;
        SetScrollPos(window_, SB_VERT, scroll_, TRUE);
        InvalidateRect(window_, nullptr, FALSE);
        NotifyLayout();
    }

    void NotifyLayout()
    {
        SendMessageW(GetParent(window_), WM_COMMAND, MAKEWPARAM(id_, kLayoutChanged), reinterpret_cast<LPARAM>(window_));
    }

    void EnsureVisible(int index)
    {
        const auto layouts = Compute();
        if (index < 0 || index >= static_cast<int>(layouts.size())) return;
        RECT client{};
        GetClientRect(window_, &client);
        const auto& bounds = layouts[static_cast<size_t>(index)].bounds;
        if (bounds.top < scroll_) ScrollTo(bounds.top);
        else if (bounds.bottom > scroll_ + client.bottom) ScrollTo(bounds.bottom - client.bottom);
    }

    Part HitTest(POINT point) const
    {
        point.y += scroll_;
        const auto layouts = Compute();
        for (size_t index = 0; index < layouts.size(); ++index)
        {
            const auto& row = rows_[index];
            const auto& layout = layouts[index];
            if (row.header || !PtInRect(&layout.bounds, point)) continue;
            // The expanded area belongs to the parent's controls.
            if (PtInRect(&layout.expansion, point)) return {};
            for (size_t button = 0; button < layout.iconButtons.size(); ++button)
                if (PtInRect(&layout.iconButtons[button], point)) return {static_cast<int>(index), PartKind::IconButton, static_cast<int>(button)};
            if (!row.button.empty() && PtInRect(&layout.button, point)) return {static_cast<int>(index), PartKind::Button};
            RECT toggle = layout.toggle;
            InflateRect(&toggle, Scale(6), Scale(8));
            if (row.toggle >= 0 && PtInRect(&toggle, point)) return {static_cast<int>(index), PartKind::Toggle};
            return {static_cast<int>(index), PartKind::Body};
        }
        return {};
    }

    void Notify(Notification notification, int row, int iconButton = -1)
    {
        notifiedRow_ = row;
        notifiedIconButton_ = iconButton;
        SendMessageW(GetParent(window_), WM_COMMAND, MAKEWPARAM(id_, notification), reinterpret_cast<LPARAM>(window_));
    }

    void Activate(const Part& part)
    {
        if (part.row < 0 || part.row >= static_cast<int>(rows_.size())) return;
        auto& row = rows_[static_cast<size_t>(part.row)];
        switch (part.kind)
        {
        case PartKind::Toggle:
            if (!row.toggleEnabled) return;
            row.toggle = row.toggle == 1 ? 0 : 1; // Show the change at once; the parent may refresh.
            InvalidateRect(window_, nullptr, FALSE);
            Notify(kToggled, part.row);
            return;
        case PartKind::Button:
            if (row.buttonEnabled) Notify(kButton, part.row);
            return;
        case PartKind::IconButton:
            Notify(kIconButton, part.row, part.iconButton);
            return;
        case PartKind::Body:
            Notify(kActivated, part.row);
            return;
        default:
            return;
        }
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

    static void FillRect(HDC dc, const RECT& rect, COLORREF color)
    {
        const HBRUSH brush = CreateSolidBrush(color);
        ::FillRect(dc, &rect, brush);
        DeleteObject(brush);
    }

    void PaintToggle(HDC dc, RECT rect, bool on, bool enabled, bool hover)
    {
        COLORREF track = on ? RGB(55, 138, 221) : hover ? RGB(140, 144, 152) : RGB(165, 169, 176);
        if (!enabled) track = on ? RGB(160, 196, 234) : RGB(214, 216, 220);
        FillRounded(dc, rect, rect.bottom - rect.top, track, track);
        const int inset = Scale(3);
        const int knob = rect.bottom - rect.top - 2 * inset;
        const int left = on ? rect.right - inset - knob : rect.left + inset;
        const RECT circle{left, rect.top + inset, left + knob, rect.top + inset + knob};
        const HBRUSH brush = CreateSolidBrush(UiTheme::Surface);
        const auto oldBrush = SelectObject(dc, brush);
        const auto oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, circle.left, circle.top, circle.right + 1, circle.bottom + 1);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(brush);
    }

    void PaintRow(HDC dc, size_t index, const Layout& base, bool focused)
    {
        const auto& row = rows_[index];
        auto layout = base;
        const auto shift = [this](RECT& rect) { OffsetRect(&rect, 0, -scroll_); };
        shift(layout.bounds); shift(layout.text); shift(layout.toggle); shift(layout.button); shift(layout.pill); shift(layout.icon);
        shift(layout.expansion);
        for (auto& rect : layout.iconButtons) shift(rect);
        SetBkMode(dc, TRANSPARENT);

        if (row.header)
        {
            SelectObject(dc, textFont_);
            SetTextColor(dc, RGB(95, 99, 108));
            DrawTextW(dc, row.title.c_str(), -1, &layout.text, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            return;
        }

        const COLORREF border = RGB(220, 222, 225);
        const bool hovered = hover_.row == static_cast<int>(index);
        // Expanded rows stay white: the controls in them are drawn on white.
        COLORREF fill = row.selected ? RGB(232, 240, 252)
            : hovered && hover_.kind == PartKind::Body && row.expandHeight == 0 ? RGB(248, 249, 251) : UiTheme::Surface;
        // Rows of a card share one rounded outline; inner edges are hairlines.
        RECT outline = layout.bounds;
        const int radius = Scale(10);
        if (!layout.firstInCard) outline.top -= radius;
        if (!layout.lastInCard) outline.bottom += radius;
        HRGN clip = CreateRectRgn(layout.bounds.left, layout.bounds.top, layout.bounds.right + 1, layout.bounds.bottom);
        SelectClipRgn(dc, clip);
        FillRounded(dc, outline, radius, fill, border);
        if (!layout.firstInCard)
        {
            const RECT line{layout.bounds.left + Scale(14), layout.bounds.top, layout.bounds.right - Scale(14), layout.bounds.top + 1};
            FillRect(dc, line, RGB(234, 236, 239));
        }
        if (row.expandHeight > 0)
        {
            const RECT line{layout.expansion.left, layout.expansion.top, layout.expansion.right, layout.expansion.top + 1};
            FillRect(dc, line, RGB(234, 236, 239));
        }
        if (focused)
        {
            RECT focus = layout.bounds;
            InflateRect(&focus, -Scale(3), -Scale(3));
            const HPEN pen = CreatePen(PS_SOLID, Scale(2), RGB(55, 138, 221));
            const auto oldPen = SelectObject(dc, pen);
            const auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            RoundRect(dc, focus.left, focus.top, focus.right, focus.bottom, Scale(8), Scale(8));
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }
        SelectClipRgn(dc, nullptr);
        DeleteObject(clip);

        if (row.showIcon)
        {
            if (const HICON icon = IconFor(row.iconPath))
                DrawIconEx(dc, layout.icon.left, layout.icon.top, icon, layout.icon.right - layout.icon.left,
                    layout.icon.bottom - layout.icon.top, 0, nullptr, DI_NORMAL);
        }

        const COLORREF titleColor = row.muted ? RGB(136, 135, 128) : UiTheme::Text;
        RECT title = layout.text;
        RECT detail = layout.text;
        if (!row.detail.empty())
        {
            const int middle = (layout.text.top + layout.text.bottom) / 2;
            title.bottom = middle + Scale(1);
            detail.top = middle + Scale(1);
        }
        SelectObject(dc, textFont_);
        SetTextColor(dc, titleColor);
        DrawTextW(dc, row.title.c_str(), -1, &title,
            (row.detail.empty() ? DT_VCENTER : DT_BOTTOM) | DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (!row.detail.empty())
        {
            SetTextColor(dc, RGB(110, 112, 118));
            DrawTextW(dc, row.detail.c_str(), -1, &detail, DT_TOP | DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }

        if (!row.pill.empty())
        {
            COLORREF pillFill = RGB(241, 239, 232), pillText = RGB(95, 94, 90);
            if (row.pillTone == Tone::Active) { pillFill = RGB(234, 243, 222); pillText = RGB(39, 80, 10); }
            else if (row.pillTone == Tone::Warning) { pillFill = RGB(250, 238, 218); pillText = RGB(133, 79, 11); }
            else if (row.pillTone == Tone::Accent) { pillFill = RGB(230, 241, 251); pillText = RGB(12, 68, 124); }
            FillRounded(dc, layout.pill, layout.pill.bottom - layout.pill.top, pillFill, pillFill);
            SetTextColor(dc, pillText);
            DrawTextW(dc, row.pill.c_str(), -1, &layout.pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        if (row.toggle >= 0)
        {
            if (!row.toggleLabel.empty())
            {
                RECT label{layout.text.right, layout.bounds.top, layout.toggle.left - Scale(10), layout.bounds.bottom};
                SetTextColor(dc, row.toggleEnabled ? RGB(80, 85, 95) : RGB(160, 162, 168));
                DrawTextW(dc, row.toggleLabel.c_str(), -1, &label, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            PaintToggle(dc, layout.toggle, row.toggle == 1, row.toggleEnabled,
                hovered && hover_.kind == PartKind::Toggle);
        }

        if (!row.button.empty())
        {
            const bool buttonHover = hovered && hover_.kind == PartKind::Button && row.buttonEnabled;
            FillRounded(dc, layout.button, Scale(8), buttonHover ? RGB(243, 244, 246) : UiTheme::Surface,
                row.buttonEnabled ? RGB(190, 194, 201) : RGB(226, 228, 231));
            SetTextColor(dc, row.buttonEnabled ? UiTheme::Text : RGB(160, 162, 168));
            DrawTextW(dc, row.button.c_str(), -1, &layout.button, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        SelectObject(dc, glyphFont_);
        for (size_t button = 0; button < row.iconButtons.size(); ++button)
        {
            const bool buttonHover = hovered && hover_.kind == PartKind::IconButton && hover_.iconButton == static_cast<int>(button);
            if (buttonHover) FillRounded(dc, layout.iconButtons[button], Scale(6), RGB(238, 240, 243), RGB(238, 240, 243));
            SetTextColor(dc, buttonHover ? UiTheme::Text : RGB(110, 112, 118));
            DrawTextW(dc, &row.iconButtons[button], 1, &layout.iconButtons[button], DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }

    void Paint(HDC target)
    {
        RECT client{};
        GetClientRect(window_, &client);
        const HDC dc = CreateCompatibleDC(target);
        const HBITMAP bitmap = CreateCompatibleBitmap(target, std::max<LONG>(1, client.right), std::max<LONG>(1, client.bottom));
        const auto oldBitmap = SelectObject(dc, bitmap);
        ::FillRect(dc, &client, UiTheme::BackgroundBrush());
        const auto oldFont = SelectObject(dc, textFont_);
        SetBkMode(dc, TRANSPARENT);
        if (rows_.empty() && !emptyText_.empty())
        {
            SetTextColor(dc, RGB(110, 112, 118));
            RECT text{Scale(4), Scale(8), client.right - Scale(4), client.bottom};
            DrawTextW(dc, emptyText_.c_str(), -1, &text, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        }
        const bool hasFocus = GetFocus() == window_;
        const auto layouts = Compute();
        for (size_t index = 0; index < rows_.size(); ++index)
        {
            const auto& bounds = layouts[index].bounds;
            if (bounds.bottom - scroll_ < 0 || bounds.top - scroll_ > client.bottom) continue;
            PaintRow(dc, index, layouts[index], hasFocus && static_cast<int>(index) == focused_);
        }
        SelectObject(dc, oldFont);
        BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }

    void MoveFocus(int direction)
    {
        int index = focused_;
        for (size_t step = 0; step < rows_.size(); ++step)
        {
            index += direction;
            if (index < 0 || index >= static_cast<int>(rows_.size())) return;
            if (!rows_[static_cast<size_t>(index)].header) break;
        }
        if (index < 0 || index >= static_cast<int>(rows_.size()) || rows_[static_cast<size_t>(index)].header) return;
        focused_ = index;
        EnsureVisible(focused_);
        InvalidateRect(window_, nullptr, FALSE);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_ERASEBKGND:
        case WM_PRINTCLIENT:
        {
            // Themed child controls (checkboxes) ask their parent to paint behind them;
            // they always sit on a white card.
            const HDC dc = reinterpret_cast<HDC>(wParam);
            if (message == WM_PRINTCLIENT || WindowFromDC(dc) != window_)
            {
                RECT client{};
                GetClientRect(window_, &client);
                ::FillRect(dc, &client, UiTheme::SurfaceBrush());
            }
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            const HWND control = reinterpret_cast<HWND>(lParam);
            SetBkMode(dc, TRANSPARENT);
            SetBkColor(dc, UiTheme::Surface);
            SetTextColor(dc, IsWindowEnabled(control) ? UiTheme::Text : RGB(160, 162, 168));
            return reinterpret_cast<LRESULT>(UiTheme::SurfaceBrush());
        }
        case WM_COMMAND:
            // Controls in expanded rows report to the page that created them.
            return SendMessageW(GetParent(window_), WM_COMMAND, wParam, lParam);
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
            NotifyLayout();
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
            const auto part = HitTest({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            if (!(part == hover_))
            {
                hover_ = part;
                InvalidateRect(window_, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_};
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = {};
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT && hover_.kind != PartKind::None && hover_.kind != PartKind::Body)
            {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        {
            SetFocus(window_);
            pressed_ = HitTest({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            if (pressed_.row >= 0) focused_ = pressed_.row;
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP:
        {
            const auto part = HitTest({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            if (part == pressed_) Activate(part);
            pressed_ = {};
            return 0;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        case WM_KEYDOWN:
            switch (wParam)
            {
            case VK_UP: MoveFocus(-1); return 0;
            case VK_DOWN: MoveFocus(1); return 0;
            case VK_SPACE:
            case VK_RETURN:
            {
                if (focused_ < 0) return 0;
                const auto& row = rows_[static_cast<size_t>(focused_)];
                const PartKind kind = row.toggle >= 0 ? PartKind::Toggle : !row.button.empty() ? PartKind::Button : PartKind::Body;
                Activate({focused_, kind});
                return 0;
            }
            case VK_DELETE:
                if (focused_ >= 0) Notify(kDeleteRequested, focused_);
                return 0;
            }
            break;
        case WM_SETFOCUS:
            if (focused_ < 0) MoveFocus(1);
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_KILLFOCUS:
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_NCDESTROY:
        {
            ReleaseResources();
            const HWND window = window_;
            window_ = nullptr;
            return DefWindowProcW(window, message, wParam, lParam);
        }
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<RowList*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<RowList*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wParam, lParam);
        const HWND handle = window;
        const LRESULT result = self->Handle(message, wParam, lParam);
        if (message == WM_NCDESTROY) SetWindowLongPtrW(handle, GWLP_USERDATA, 0);
        return result;
    }

    HWND window_{};
    int id_{};
    HFONT titleFont_{};
    HFONT textFont_{};
    HFONT glyphFont_{};
    std::vector<Row> rows_;
    std::wstring emptyText_;
    std::map<std::wstring, HICON> icons_;
    int scroll_{};
    int focused_{-1};
    Part hover_{};
    Part pressed_{};
    int notifiedRow_{-1};
    int notifiedIconButton_{-1};
};
