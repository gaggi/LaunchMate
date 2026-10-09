#pragma once

#include "UiTheme.h"

#include <algorithm>
#include <string>
#include <vector>
#include <windows.h>
#include <windowsx.h>

// A row of mutually exclusive choices drawn as one pill group. Selecting another
// segment notifies the parent with WM_COMMAND(MAKEWPARAM(id, kChanged), hwnd).
class SegmentedControl
{
public:
    static constexpr WORD kChanged = 1;

    SegmentedControl() = default;
    SegmentedControl(const SegmentedControl&) = delete;
    SegmentedControl& operator=(const SegmentedControl&) = delete;

    bool Create(HINSTANCE instance, HWND parent, int id, HFONT font, std::vector<std::wstring> items)
    {
        WNDCLASSW windowClass{};
        if (!GetClassInfoW(instance, kClassName, &windowClass))
        {
            windowClass.lpfnWndProc = WindowProc;
            windowClass.hInstance = instance;
            windowClass.hCursor = LoadCursorW(nullptr, IDC_HAND);
            windowClass.lpszClassName = kClassName;
            if (!RegisterClassW(&windowClass)) return false;
        }
        id_ = id;
        font_ = font;
        items_ = std::move(items);
        window_ = CreateWindowExW(0, kClassName, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            0, 0, 100, 30, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, this);
        return window_ != nullptr;
    }

    HWND Handle() const noexcept { return window_; }
    int Selected() const noexcept { return selected_; }

    void SetSelected(int index)
    {
        index = std::clamp(index, 0, std::max(0, static_cast<int>(items_.size()) - 1));
        if (index == selected_) return;
        selected_ = index;
        InvalidateRect(window_, nullptr, FALSE);
    }

    // Width in device pixels that fits every label.
    int IdealWidth() const
    {
        int width = 0;
        for (const int segment : SegmentWidths()) width += segment;
        return width;
    }

private:
    static constexpr wchar_t kClassName[] = L"LaunchMateSegmented";

    int Scale(int value) const { return MulDiv(value, GetDpiForWindow(window_), 96); }

    std::vector<int> SegmentWidths() const
    {
        std::vector<int> widths;
        const HDC dc = GetDC(window_);
        const auto old = SelectObject(dc, font_);
        for (const auto& item : items_)
        {
            SIZE size{};
            GetTextExtentPoint32W(dc, item.c_str(), static_cast<int>(item.size()), &size);
            widths.push_back(size.cx + Scale(28));
        }
        SelectObject(dc, old);
        ReleaseDC(window_, dc);
        return widths;
    }

    std::vector<RECT> SegmentRects() const
    {
        RECT client{};
        GetClientRect(window_, &client);
        std::vector<RECT> rects;
        int x = 0;
        for (const int width : SegmentWidths())
        {
            rects.push_back({x, 0, std::min<int>(x + width, client.right), client.bottom});
            x += width;
        }
        return rects;
    }

    void Select(int index)
    {
        if (index < 0 || index >= static_cast<int>(items_.size()) || index == selected_) return;
        selected_ = index;
        InvalidateRect(window_, nullptr, FALSE);
        SendMessageW(GetParent(window_), WM_COMMAND, MAKEWPARAM(id_, kChanged), reinterpret_cast<LPARAM>(window_));
    }

    static void Rounded(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border)
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

    void Paint(HDC target)
    {
        RECT client{};
        GetClientRect(window_, &client);
        const HDC dc = CreateCompatibleDC(target);
        const HBITMAP bitmap = CreateCompatibleBitmap(target, std::max<LONG>(1, client.right), std::max<LONG>(1, client.bottom));
        const auto oldBitmap = SelectObject(dc, bitmap);
        FillRect(dc, &client, UiTheme::BackgroundBrush());
        const auto rects = SegmentRects();
        RECT group{0, 0, rects.empty() ? 0 : rects.back().right, client.bottom};
        Rounded(dc, group, Scale(8), RGB(232, 234, 238), RGB(220, 222, 225));
        const auto oldFont = SelectObject(dc, font_);
        SetBkMode(dc, TRANSPARENT);
        const bool focused = GetFocus() == window_;
        for (size_t index = 0; index < rects.size(); ++index)
        {
            RECT rect = rects[index];
            const bool selected = static_cast<int>(index) == selected_;
            if (selected)
            {
                RECT inner = rect;
                InflateRect(&inner, -Scale(3), -Scale(3));
                Rounded(dc, inner, Scale(6), UiTheme::Surface, focused ? RGB(55, 138, 221) : RGB(210, 213, 218));
            }
            else if (static_cast<int>(index) == hover_)
            {
                RECT inner = rect;
                InflateRect(&inner, -Scale(3), -Scale(3));
                Rounded(dc, inner, Scale(6), RGB(224, 227, 231), RGB(224, 227, 231));
            }
            SetTextColor(dc, selected ? UiTheme::Text : RGB(80, 85, 95));
            DrawTextW(dc, items_[index].c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        SelectObject(dc, oldFont);
        BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }

    int HitTest(POINT point) const
    {
        const auto rects = SegmentRects();
        for (size_t index = 0; index < rects.size(); ++index)
            if (PtInRect(&rects[index], point)) return static_cast<int>(index);
        return -1;
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
        case WM_MOUSEMOVE:
        {
            const int hit = HitTest({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            if (hit != hover_) { hover_ = hit; InvalidateRect(window_, nullptr, FALSE); }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_};
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = -1;
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(window_);
            Select(HitTest({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}));
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        case WM_KEYDOWN:
            if (wParam == VK_LEFT) { Select(selected_ - 1); return 0; }
            if (wParam == VK_RIGHT) { Select(selected_ + 1); return 0; }
            break;
        // Like standard controls, so a parent can swap fonts after a DPI change.
        case WM_SETFONT:
            font_ = reinterpret_cast<HFONT>(wParam);
            if (LOWORD(lParam)) InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_GETFONT:
            return reinterpret_cast<LRESULT>(font_);
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<SegmentedControl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<SegmentedControl*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (message == WM_NCDESTROY && self)
        {
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            self->window_ = nullptr;
            return DefWindowProcW(window, message, wParam, lParam);
        }
        return self ? self->Handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
    }

    HWND window_{};
    int id_{};
    HFONT font_{};
    std::vector<std::wstring> items_;
    int selected_{};
    int hover_{-1};
};
