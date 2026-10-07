#pragma once

#include "UiTheme.h"

#include <algorithm>
#include <windows.h>

// Hosts an embedded page (a child dialog) inside the main window. The page keeps
// at least its template size; when the window is smaller it scrolls vertically,
// when larger the page is stretched so it can grow lists and other wide controls.
class ScrollHost
{
public:
    ScrollHost() = default;
    ScrollHost(const ScrollHost&) = delete;
    ScrollHost& operator=(const ScrollHost&) = delete;

    bool Create(HINSTANCE instance, HWND parent)
    {
        WNDCLASSW windowClass{};
        if (!GetClassInfoW(instance, kClassName, &windowClass))
        {
            windowClass.lpfnWndProc = WindowProc;
            windowClass.hInstance = instance;
            windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            windowClass.hbrBackground = UiTheme::BackgroundBrush();
            windowClass.lpszClassName = kClassName;
            if (!RegisterClassW(&windowClass)) return false;
        }
        window_ = CreateWindowExW(WS_EX_CONTROLPARENT, kClassName, nullptr, WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN,
            0, 0, 100, 100, parent, nullptr, instance, this);
        return window_ != nullptr;
    }

    HWND Handle() const noexcept { return window_; }
    HWND Content() const noexcept { return content_; }

    // Takes ownership of a page created as a child of Handle(); its current size is its minimum.
    void SetContent(HWND content)
    {
        Clear();
        content_ = content;
        if (!content_) return;
        RECT rect{};
        GetWindowRect(content_, &rect);
        minimumWidth_ = rect.right - rect.left;
        minimumHeight_ = rect.bottom - rect.top;
        scroll_ = 0;
        ShowWindow(content_, SW_SHOW);
        Layout();
    }

    void Clear()
    {
        if (content_) DestroyWindow(content_);
        content_ = nullptr;
    }

private:
    static constexpr wchar_t kClassName[] = L"LaunchMateScrollHost";

    void Layout()
    {
        if (!content_) return;
        RECT client{};
        GetClientRect(window_, &client);
        const int height = std::max<int>(minimumHeight_, client.bottom);
        scroll_ = std::clamp(scroll_, 0, std::max(0, height - static_cast<int>(client.bottom)));
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
        info.nMax = height - 1;
        info.nPage = static_cast<UINT>(client.bottom);
        info.nPos = scroll_;
        SetScrollInfo(window_, SB_VERT, &info, TRUE);
        GetClientRect(window_, &client); // The scroll bar may have changed the width.
        SetWindowPos(content_, nullptr, 0, -scroll_, std::max<int>(minimumWidth_, client.right), height,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void ScrollTo(int position)
    {
        RECT client{};
        GetClientRect(window_, &client);
        RECT content{};
        GetWindowRect(content_, &content);
        const int limit = std::max(0, static_cast<int>(content.bottom - content.top) - static_cast<int>(client.bottom));
        position = std::clamp(position, 0, limit);
        if (position == scroll_) return;
        scroll_ = position;
        SetScrollPos(window_, SB_VERT, scroll_, TRUE);
        SetWindowPos(content_, nullptr, 0, -scroll_, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_SIZE:
            Layout();
            return 0;
        case WM_VSCROLL:
        {
            if (!content_) return 0;
            SCROLLINFO info{sizeof(info), SIF_ALL};
            GetScrollInfo(window_, SB_VERT, &info);
            const int line = MulDiv(40, GetDpiForWindow(window_), 96);
            int position = scroll_;
            switch (LOWORD(wParam))
            {
            case SB_LINEUP: position -= line; break;
            case SB_LINEDOWN: position += line; break;
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
            // Child controls that do not scroll themselves pass the wheel up to here.
            if (content_)
                ScrollTo(scroll_ - GET_WHEEL_DELTA_WPARAM(wParam) * MulDiv(48, GetDpiForWindow(window_), 96) / WHEEL_DELTA);
            return 0;
        case WM_NCDESTROY:
            content_ = nullptr;
            break;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<ScrollHost*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<ScrollHost*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->Handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
    }

    HWND window_{};
    HWND content_{};
    int minimumWidth_{};
    int minimumHeight_{};
    int scroll_{};
};
