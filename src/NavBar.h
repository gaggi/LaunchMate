#pragma once

#include "UiTheme.h"

#include <algorithm>
#include <string>
#include <vector>
#include <windows.h>
#include <windowsx.h>

// Left navigation of the main window. Page items stay highlighted while their page
// is shown; other items (dialogs) only send their command. Clicks arrive at the
// parent as WM_COMMAND(MAKEWPARAM(id, BN_CLICKED), hwnd).
class NavBar
{
public:
    struct Item
    {
        // Glyph from Segoe Fluent Icons / Segoe MDL2 Assets.
        wchar_t glyph;
        std::wstring text;
        int id;
        bool page;
    };

    NavBar() = default;
    NavBar(const NavBar&) = delete;
    NavBar& operator=(const NavBar&) = delete;
    ~NavBar()
    {
        if (iconFont_) DeleteObject(iconFont_);
    }

    bool Create(HINSTANCE instance, HWND parent, std::wstring title, std::wstring footer, std::vector<Item> items,
        HFONT titleFont, HFONT textFont)
    {
        WNDCLASSW windowClass{};
        if (!GetClassInfoW(instance, kClassName, &windowClass))
        {
            windowClass.lpfnWndProc = WindowProc;
            windowClass.hInstance = instance;
            windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            windowClass.lpszClassName = kClassName;
            if (!RegisterClassW(&windowClass)) return false;
        }
        title_ = std::move(title);
        footer_ = std::move(footer);
        items_ = std::move(items);
        window_ = CreateWindowExW(0, kClassName, nullptr, WS_CHILD | WS_VISIBLE,
            0, 0, 100, 100, parent, nullptr, instance, this);
        if (!window_) return false;
        SetFonts(titleFont, textFont);
        return true;
    }

    HWND Handle() const noexcept { return window_; }

    void SetFonts(HFONT titleFont, HFONT textFont)
    {
        titleFont_ = titleFont;
        textFont_ = textFont;
        if (iconFont_) DeleteObject(iconFont_);
        iconFont_ = CreateFontW(-Scale(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, IconFontFace());
        InvalidateRect(window_, nullptr, FALSE);
    }

    void SetSelected(int id)
    {
        if (id == selected_) return;
        selected_ = id;
        InvalidateRect(window_, nullptr, FALSE);
    }

private:
    static constexpr wchar_t kClassName[] = L"LaunchMateNavBar";
    static constexpr COLORREF kBackground = RGB(236, 238, 241);

    // Windows 11 ships Segoe Fluent Icons; Windows 10 has the same code points in MDL2.
    static const wchar_t* IconFontFace()
    {
        static const wchar_t* face = []
        {
            const HDC dc = GetDC(nullptr);
            LOGFONTW query{};
            query.lfCharSet = DEFAULT_CHARSET;
            wcscpy_s(query.lfFaceName, L"Segoe Fluent Icons");
            bool found = false;
            EnumFontFamiliesExW(dc, &query, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found) -> int
            {
                *reinterpret_cast<bool*>(found) = true;
                return 0;
            }, reinterpret_cast<LPARAM>(&found), 0);
            ReleaseDC(nullptr, dc);
            return found ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
        }();
        return face;
    }

    int Scale(int value) const { return MulDiv(value, GetDpiForWindow(window_), 96); }

    // Items are laid out top-down; dialog items follow the pages after a gap.
    std::vector<RECT> ItemRects() const
    {
        RECT client{};
        GetClientRect(window_, &client);
        std::vector<RECT> rects;
        int y = Scale(72);
        bool previousPage = true;
        for (const auto& item : items_)
        {
            if (previousPage && !item.page) y += Scale(20);
            previousPage = item.page;
            rects.push_back({Scale(10), y, client.right - Scale(10), y + Scale(38)});
            y += Scale(40);
        }
        return rects;
    }

    int HitTest(POINT point) const
    {
        const auto rects = ItemRects();
        for (size_t index = 0; index < rects.size(); ++index)
            if (PtInRect(&rects[index], point)) return static_cast<int>(index);
        return -1;
    }

    void Paint(HDC target)
    {
        RECT client{};
        GetClientRect(window_, &client);
        const HDC dc = CreateCompatibleDC(target);
        const HBITMAP bitmap = CreateCompatibleBitmap(target, std::max<LONG>(1, client.right), std::max<LONG>(1, client.bottom));
        const auto oldBitmap = SelectObject(dc, bitmap);
        const HBRUSH background = CreateSolidBrush(kBackground);
        FillRect(dc, &client, background);
        DeleteObject(background);
        SetBkMode(dc, TRANSPARENT);

        const auto oldFont = SelectObject(dc, titleFont_);
        SetTextColor(dc, UiTheme::Text);
        RECT title{Scale(22), Scale(22), client.right - Scale(10), Scale(50)};
        DrawTextW(dc, title_.c_str(), -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        const auto rects = ItemRects();
        for (size_t index = 0; index < items_.size(); ++index)
        {
            const auto& item = items_[index];
            const RECT& rect = rects[index];
            const bool selected = item.page && item.id == selected_;
            if (selected || static_cast<int>(index) == hover_)
            {
                const HBRUSH fill = CreateSolidBrush(selected ? UiTheme::Surface : RGB(226, 229, 233));
                const HPEN pen = CreatePen(PS_SOLID, 1, selected ? RGB(220, 222, 225) : RGB(226, 229, 233));
                const auto oldBrush = SelectObject(dc, fill);
                const auto oldPen = SelectObject(dc, pen);
                RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, Scale(8), Scale(8));
                SelectObject(dc, oldPen);
                SelectObject(dc, oldBrush);
                DeleteObject(pen);
                DeleteObject(fill);
            }
            const COLORREF color = selected ? UiTheme::Text : RGB(80, 85, 95);
            SetTextColor(dc, color);
            SelectObject(dc, iconFont_);
            RECT icon{rect.left + Scale(10), rect.top, rect.left + Scale(36), rect.bottom};
            DrawTextW(dc, &item.glyph, 1, &icon, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, textFont_);
            RECT text{rect.left + Scale(44), rect.top, rect.right - Scale(6), rect.bottom};
            DrawTextW(dc, item.text.c_str(), -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }

        SelectObject(dc, textFont_);
        SetTextColor(dc, RGB(136, 135, 128));
        RECT footer{Scale(22), client.bottom - Scale(40), client.right - Scale(10), client.bottom - Scale(16)};
        DrawTextW(dc, footer_.c_str(), -1, &footer, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

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
            InvalidateRect(window_, nullptr, FALSE);
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
        case WM_LBUTTONUP:
        {
            const int hit = HitTest({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            if (hit >= 0)
                SendMessageW(GetParent(window_), WM_COMMAND, MAKEWPARAM(items_[hit].id, BN_CLICKED),
                    reinterpret_cast<LPARAM>(window_));
            return 0;
        }
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<NavBar*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<NavBar*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->Handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
    }

    HWND window_{};
    std::wstring title_;
    std::wstring footer_;
    std::vector<Item> items_;
    HFONT titleFont_{};
    HFONT textFont_{};
    HFONT iconFont_{};
    int selected_{-1};
    int hover_{-1};
};
