#pragma once

#include "UiTheme.h"

#include <memory>
#include <string>
#include <vector>
#include <windows.h>
#include <commctrl.h>

// Base for a page embedded in the main window's content area. The page is a child
// window that owns its controls and handles their notifications itself. Show()
// hands ownership to the window: the object is deleted when the window is destroyed.
class PageWindow
{
public:
    PageWindow(const PageWindow&) = delete;
    PageWindow& operator=(const PageWindow&) = delete;
    virtual ~PageWindow() = default;

    // minimumWidth and minimumHeight are in DIPs; a ScrollHost scrolls below them.
    static HWND Show(std::unique_ptr<PageWindow> page, HINSTANCE instance, HWND parent, int minimumWidth, int minimumHeight)
    {
        WNDCLASSW windowClass{};
        if (!GetClassInfoW(instance, kClassName, &windowClass))
        {
            windowClass.lpfnWndProc = WindowProc;
            windowClass.hInstance = instance;
            windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            windowClass.hbrBackground = UiTheme::BackgroundBrush();
            windowClass.lpszClassName = kClassName;
            if (!RegisterClassW(&windowClass)) return nullptr;
        }
        page->instance_ = instance;
        const UINT dpi = GetDpiForWindow(parent);
        PageWindow* raw = page.release();
        const HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, kClassName, nullptr, WS_CHILD | WS_CLIPCHILDREN,
            0, 0, MulDiv(minimumWidth, dpi, 96), MulDiv(minimumHeight, dpi, 96), parent, nullptr, instance, raw);
        if (!window) delete raw;
        return window;
    }

protected:
    PageWindow(HFONT headingFont, HFONT textFont) : headingFont_(headingFont), textFont_(textFont) {}

    HWND Handle() const noexcept { return window_; }
    HINSTANCE Instance() const noexcept { return instance_; }
    HFONT HeadingFont() const noexcept { return headingFont_; }
    HFONT TextFont() const noexcept { return textFont_; }
    int Scale(int value) const { return MulDiv(value, GetDpiForWindow(window_), 96); }

    HWND AddButton(int id, const wchar_t* text)
    {
        return AddControl(L"BUTTON", text, WS_TABSTOP | BS_PUSHBUTTON, id, textFont_);
    }

    HWND AddLabel(const wchar_t* text, bool heading = false, DWORD style = 0)
    {
        return AddControl(L"STATIC", text, SS_CENTERIMAGE | SS_ENDELLIPSIS | style, 0, heading ? headingFont_ : textFont_);
    }

    HWND AddEdit(int id, const wchar_t* cueBanner = nullptr)
    {
        const HWND edit = AddControl(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, id, textFont_);
        if (cueBanner) SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cueBanner));
        return edit;
    }

    HWND AddControl(const wchar_t* className, const wchar_t* text, DWORD style, int id, HFONT font)
    {
        const HWND control = CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, window_,
            id == 0 ? nullptr : reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return control;
    }

    // Shows a menu at the cursor and returns the chosen index, or -1. Items that are
    // empty become separators; `checked` gets a check mark; `disabled` greys one out.
    int ChooseFromMenu(const std::vector<std::wstring>& items, int checked = -1, int disabled = -1) const
    {
        const HMENU menu = CreatePopupMenu();
        for (size_t index = 0; index < items.size(); ++index)
        {
            if (items[index].empty()) { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); continue; }
            UINT flags = MF_STRING;
            if (static_cast<int>(index) == checked) flags |= MF_CHECKED;
            if (static_cast<int>(index) == disabled) flags |= MF_GRAYED;
            AppendMenuW(menu, flags, index + 1, items[index].c_str());
        }
        POINT cursor{};
        GetCursorPos(&cursor);
        const UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window_, nullptr);
        DestroyMenu(menu);
        return static_cast<int>(choice) - 1;
    }

    // Positions in DIPs, relative to the page.
    void Place(HWND control, int x, int y, int width, int height) const
    {
        if (control) MoveWindow(control, Scale(x), Scale(y), Scale(width), Scale(height), TRUE);
    }

    virtual void OnCreate() = 0;
    // Page size in DIPs.
    virtual void OnSize(int width, int height) { (void)width; (void)height; }
    virtual bool OnCommand(int id, int code, HWND control) { (void)id; (void)code; (void)control; return false; }
    virtual bool OnNotify(const NMHDR& header, LRESULT& result) { (void)header; (void)result; return false; }
    virtual void OnTimer(UINT_PTR id) { (void)id; }
    // The window still exists; save anything the controls hold.
    virtual void OnDestroy() {}

private:
    static constexpr wchar_t kClassName[] = L"LaunchMatePage";

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<PageWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        switch (message)
        {
        case WM_NCCREATE:
            self = static_cast<PageWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            break;
        case WM_CREATE:
            self->OnCreate();
            UiTheme::Apply(window);
            return 0;
        case WM_SIZE:
            if (self)
            {
                const UINT dpi = GetDpiForWindow(window);
                self->OnSize(MulDiv(LOWORD(lParam), 96, dpi), MulDiv(HIWORD(lParam), 96, dpi));
            }
            return 0;
        case WM_COMMAND:
            if (self && self->OnCommand(LOWORD(wParam), HIWORD(wParam), reinterpret_cast<HWND>(lParam))) return 0;
            break;
        case WM_NOTIFY:
        {
            LRESULT result = 0;
            if (self && self->OnNotify(*reinterpret_cast<NMHDR*>(lParam), result)) return result;
            break;
        }
        case WM_TIMER:
            if (self) self->OnTimer(wParam);
            return 0;
        case WM_DESTROY:
            if (self) self->OnDestroy();
            break;
        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            delete self;
            return DefWindowProcW(window, message, wParam, lParam);
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    HWND window_{};
    HINSTANCE instance_{};
    HFONT headingFont_{};
    HFONT textFont_{};
};
