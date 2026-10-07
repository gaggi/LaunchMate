#pragma once

#include "UiTheme.h"

#include <string>
#include <windows.h>

// The colored monitoring banner at the top of the main window. It owns the
// start/stop button so the button can be painted on the banner's color, and
// forwards the button's WM_COMMAND to the main window.
class StatusPanel
{
public:
    enum class Tone
    {
        Neutral,
        Active,
        Busy
    };

    StatusPanel() = default;
    StatusPanel(const StatusPanel&) = delete;
    StatusPanel& operator=(const StatusPanel&) = delete;
    ~StatusPanel()
    {
        if (brush_) DeleteObject(brush_);
    }

    bool Create(HINSTANCE instance, HWND parent, int buttonId, HFONT titleFont, HFONT textFont)
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
        titleFont_ = titleFont;
        textFont_ = textFont;
        window_ = CreateWindowExW(WS_EX_CONTROLPARENT, kClassName, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
            0, 0, 100, 56, parent, nullptr, instance, this);
        if (!window_) return false;
        button_ = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 100, 30, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(buttonId)), instance, nullptr);
        SendMessageW(button_, WM_SETFONT, reinterpret_cast<WPARAM>(textFont), TRUE);
        SetState(Tone::Neutral, L"", L"", L"", true);
        return button_ != nullptr;
    }

    HWND Handle() const noexcept { return window_; }

    void SetFonts(HFONT titleFont, HFONT textFont)
    {
        titleFont_ = titleFont;
        textFont_ = textFont;
        SendMessageW(button_, WM_SETFONT, reinterpret_cast<WPARAM>(textFont), TRUE);
        InvalidateRect(window_, nullptr, TRUE);
    }

    void SetState(Tone tone, const std::wstring& title, const std::wstring& detail,
        const std::wstring& buttonText, bool buttonEnabled)
    {
        const bool changed = tone != tone_ || title != title_ || detail != detail_;
        if (tone != tone_ || !brush_)
        {
            tone_ = tone;
            if (brush_) DeleteObject(brush_);
            brush_ = CreateSolidBrush(Colors().background);
        }
        title_ = title;
        detail_ = detail;
        wchar_t current[64]{};
        GetWindowTextW(button_, current, static_cast<int>(std::size(current)));
        if (buttonText != current) SetWindowTextW(button_, buttonText.c_str());
        EnableWindow(button_, buttonEnabled);
        if (changed)
        {
            InvalidateRect(window_, nullptr, TRUE);
            InvalidateRect(button_, nullptr, TRUE);
        }
    }

private:
    static constexpr wchar_t kClassName[] = L"LaunchMateStatusPanel";

    struct Palette
    {
        COLORREF background;
        COLORREF border;
        COLORREF dot;
        COLORREF title;
        COLORREF detail;
    };

    Palette Colors() const
    {
        switch (tone_)
        {
        case Tone::Active: return {RGB(234, 243, 222), RGB(192, 221, 151), RGB(99, 153, 34), RGB(39, 80, 10), RGB(59, 109, 17)};
        case Tone::Busy: return {RGB(250, 238, 218), RGB(250, 199, 117), RGB(186, 117, 23), RGB(99, 56, 6), RGB(133, 79, 11)};
        default: return {UiTheme::Surface, RGB(220, 222, 225), RGB(180, 178, 169), UiTheme::Text, RGB(95, 94, 90)};
        }
    }

    int Scale(int value) const { return MulDiv(value, GetDpiForWindow(window_), 96); }

    void Layout()
    {
        RECT client{};
        GetClientRect(window_, &client);
        const int width = Scale(150);
        const int height = Scale(30);
        MoveWindow(button_, client.right - Scale(12) - width, (client.bottom - height) / 2, width, height, TRUE);
    }

    void Paint(HDC dc)
    {
        const auto colors = Colors();
        RECT client{};
        GetClientRect(window_, &client);
        FillRect(dc, &client, UiTheme::BackgroundBrush());
        const HPEN pen = CreatePen(PS_SOLID, 1, colors.border);
        const auto oldPen = SelectObject(dc, pen);
        const auto oldBrush = SelectObject(dc, brush_);
        const int radius = Scale(10);
        RoundRect(dc, client.left, client.top, client.right, client.bottom, radius, radius);

        const HBRUSH dotBrush = CreateSolidBrush(colors.dot);
        SelectObject(dc, dotBrush);
        SelectObject(dc, GetStockObject(NULL_PEN));
        const int dot = Scale(10);
        const int dotX = Scale(16);
        const int dotY = (client.bottom - dot) / 2;
        Ellipse(dc, dotX, dotY, dotX + dot, dotY + dot);

        RECT button{};
        GetWindowRect(button_, &button);
        MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&button), 2);
        SetBkMode(dc, TRANSPARENT);
        const int textLeft = dotX + dot + Scale(12);
        const int middle = client.bottom / 2;
        RECT titleRect{textLeft, client.top, button.left - Scale(12), detail_.empty() ? client.bottom : middle + Scale(1)};
        RECT detailRect{textLeft, middle + Scale(1), button.left - Scale(12), client.bottom};
        const auto oldFont = SelectObject(dc, titleFont_);
        SetTextColor(dc, colors.title);
        DrawTextW(dc, title_.c_str(), -1, &titleRect,
            (detail_.empty() ? DT_VCENTER : DT_BOTTOM) | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (!detail_.empty())
        {
            SelectObject(dc, textFont_);
            SetTextColor(dc, colors.detail);
            DrawTextW(dc, detail_.c_str(), -1, &detailRect, DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        SelectObject(dc, oldFont);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(dotBrush);
        DeleteObject(pen);
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<StatusPanel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<StatusPanel*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wParam, lParam);
        switch (message)
        {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window, &paint);
            self->Paint(dc);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_SIZE:
            self->Layout();
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        case WM_CTLCOLORBTN:
            // The button's rounded corners show the banner color, not the window background.
            return reinterpret_cast<LRESULT>(self->brush_);
        case WM_COMMAND:
            return SendMessageW(GetParent(window), WM_COMMAND, wParam, lParam);
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    HWND window_{};
    HWND button_{};
    HFONT titleFont_{};
    HFONT textFont_{};
    HBRUSH brush_{};
    Tone tone_{Tone::Neutral};
    std::wstring title_;
    std::wstring detail_;
};
