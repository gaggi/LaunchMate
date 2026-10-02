#pragma once

#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <cwchar>

namespace UiTheme
{
    inline constexpr COLORREF Background = RGB(246, 247, 249);
    inline constexpr COLORREF Surface = RGB(255, 255, 255);
    inline constexpr COLORREF Text = RGB(40, 47, 58);
    inline constexpr int TabHeight = 28;

    class Brush
    {
    public:
        explicit Brush(COLORREF color) : handle_(CreateSolidBrush(color)) {}
        ~Brush() { DeleteObject(handle_); }
        HBRUSH Get() const { return handle_; }
        Brush(const Brush&) = delete;
        Brush& operator=(const Brush&) = delete;
    private:
        HBRUSH handle_{};
    };

    inline HBRUSH BackgroundBrush()
    {
        static const Brush brush(Background);
        return brush.Get();
    }

    inline HBRUSH SurfaceBrush()
    {
        static const Brush brush(Surface);
        return brush.Get();
    }

    inline bool IsClass(HWND window, const wchar_t* name)
    {
        wchar_t className[64]{};
        GetClassNameW(window, className, 64);
        return _wcsicmp(className, name) == 0;
    }

    inline constexpr wchar_t TabSurfaceProperty[] = L"LaunchMate.TabSurface";

    inline bool IsTabSurface(HWND window)
    {
        for (HWND current = window; current; current = GetParent(current))
        {
            if (IsClass(current, WC_TABCONTROLW) || GetPropW(current, TabSurfaceProperty)) return true;
            if (!(GetWindowLongPtrW(current, GWL_STYLE) & WS_CHILD)) break;
        }
        return false;
    }

    inline HBRUSH PanelBrush(HWND window)
    {
        return IsTabSurface(window) ? SurfaceBrush() : BackgroundBrush();
    }

    inline void StyleListView(HWND list)
    {
        SetWindowTheme(list, L"Explorer", nullptr);
        ListView_SetBkColor(list, Surface);
        ListView_SetTextBkColor(list, Surface);
        ListView_SetTextColor(list, Text);
        SetWindowLongPtrW(list, GWL_EXSTYLE, GetWindowLongPtrW(list, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
        SetWindowLongPtrW(list, GWL_STYLE, GetWindowLongPtrW(list, GWL_STYLE) | WS_BORDER);
        SetWindowPos(list, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    inline void StyleTextInput(HWND control)
    {
        RECT rect{};
        GetWindowRect(control, &rect);
        // Use the same native, themed input frame for dialog controls and dynamically created fields.
        SetWindowTheme(control, L"Explorer", nullptr);
        SetWindowLongPtrW(control, GWL_EXSTYLE, GetWindowLongPtrW(control, GWL_EXSTYLE) | WS_EX_CLIENTEDGE);
        SetWindowLongPtrW(control, GWL_STYLE, GetWindowLongPtrW(control, GWL_STYLE) & ~WS_BORDER);
        SetWindowPos(control, nullptr, 0, 0, rect.right - rect.left,
            MulDiv(22, GetDpiForWindow(control), 96),
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    inline LRESULT CALLBACK TabPaintProc(HWND tab, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR subclassId, DWORD_PTR)
    {
        if (message == WM_ERASEBKGND || message == WM_PRINTCLIENT)
        {
            // Themed checkboxes ask their parent to paint behind them, independently of WM_PAINT.
            RECT client{};
            GetClientRect(tab, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client, SurfaceBrush());
            return 1;
        }
        if (message == WM_PAINT)
        {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(tab, &paint);
            RECT client{};
            GetClientRect(tab, &client);
            FillRect(dc, &client, BackgroundBrush());
            const HPEN pen = CreatePen(PS_SOLID, 1, RGB(220, 222, 225));
            const auto oldPen = SelectObject(dc, pen);
            const auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            const auto oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(tab, WM_GETFONT, 0, 0)));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, Text);
            RECT first{};
            TabCtrl_GetItemRect(tab, 0, &first);
            RECT body{client.left, first.bottom, client.right, client.bottom};
            FillRect(dc, &body, SurfaceBrush());
            Rectangle(dc, client.left, first.bottom, client.right, client.bottom);
            const int selected = TabCtrl_GetCurSel(tab);
            for (int index = 0; index < TabCtrl_GetItemCount(tab); ++index)
            {
                RECT rect{};
                TabCtrl_GetItemRect(tab, index, &rect);
                // Keep the native hit rectangles and keyboard navigation; only replace the painting.
                rect.top = first.top;
                rect.bottom = first.bottom + 1;
                FillRect(dc, &rect, index == selected ? SurfaceBrush() : BackgroundBrush());
                Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
                if (index == selected)
                {
                    RECT seam{rect.left + 1, rect.bottom - 1, rect.right - 1, rect.bottom + 1};
                    FillRect(dc, &seam, SurfaceBrush());
                }
                wchar_t label[256]{};
                TCITEMW item{};
                item.mask = TCIF_TEXT;
                item.pszText = label;
                item.cchTextMax = 256;
                TabCtrl_GetItem(tab, index, &item);
                DrawTextW(dc, label, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                if (GetFocus() == tab && index == TabCtrl_GetCurFocus(tab) &&
                    !(SendMessageW(tab, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
                {
                    InflateRect(&rect, -4, -4);
                    DrawFocusRect(dc, &rect);
                }
            }
            SelectObject(dc, oldFont);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
            EndPaint(tab, &paint);
            return 0;
        }
        if (message == WM_NCDESTROY) RemoveWindowSubclass(tab, TabPaintProc, subclassId);
        return DefSubclassProc(tab, message, wParam, lParam);
    }

    inline LRESULT CALLBACK GroupPaintProc(HWND group, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR subclassId, DWORD_PTR headingFont)
    {
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_PAINT)
        {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(group, &paint);
            // Resource dialogs can place labels inside a group without making them its children.
            // Preserve those controls regardless of their sibling paint order.
            for (HWND sibling = GetWindow(GetParent(group), GW_CHILD); sibling;
                sibling = GetWindow(sibling, GW_HWNDNEXT))
            {
                if (sibling == group || !IsWindowVisible(sibling)) continue;
                RECT bounds{};
                GetWindowRect(sibling, &bounds);
                MapWindowPoints(nullptr, group, reinterpret_cast<POINT*>(&bounds), 2);
                ExcludeClipRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
            }
            RECT rect{};
            GetClientRect(group, &rect);
            FillRect(dc, &rect, PanelBrush(group));
            const auto oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(group, WM_GETFONT, 0, 0)));
            wchar_t caption[256]{};
            const int length = GetWindowTextW(group, caption, 256);
            SIZE size{};
            GetTextExtentPoint32W(dc, caption, length, &size);
            const int padding = MulDiv(7, GetDpiForWindow(group), 96);
            const HPEN pen = CreatePen(PS_SOLID, 1, RGB(220, 222, 225));
            const auto oldPen = SelectObject(dc, pen);
            const auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, rect.left, size.cy / 2, rect.right, rect.bottom);
            RECT label{padding, 0, padding + size.cx + 2, size.cy};
            FillRect(dc, &label, PanelBrush(group));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, Text);
            DrawTextW(dc, caption, length, &label, DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldFont);
            DeleteObject(pen);
            EndPaint(group, &paint);
            return 0;
        }
        if (message == WM_NCDESTROY)
        {
            RemoveWindowSubclass(group, GroupPaintProc, subclassId);
            DeleteObject(reinterpret_cast<HFONT>(headingFont));
        }
        return DefSubclassProc(group, message, wParam, lParam);
    }

    inline LRESULT CALLBACK ColorProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR subclassId, DWORD_PTR)
    {
        if (message == WM_CTLCOLORDLG) return reinterpret_cast<LRESULT>(PanelBrush(window));
        if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN ||
            message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX)
        {
            const HWND control = reinterpret_cast<HWND>(lParam);
            const bool white = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX ||
                IsClass(control, L"Edit") || IsTabSurface(control);
            const HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, IsWindowEnabled(control) ? Text : GetSysColor(COLOR_GRAYTEXT));
            SetBkColor(dc, white ? Surface : Background);
            return reinterpret_cast<LRESULT>(white ? SurfaceBrush() : BackgroundBrush());
        }
        if (message == WM_ERASEBKGND)
        {
            RECT rect{};
            GetClientRect(window, &rect);
            FillRect(reinterpret_cast<HDC>(wParam), &rect, PanelBrush(window));
            return 1;
        }
        if (message == WM_NCDESTROY) RemoveWindowSubclass(window, ColorProc, subclassId);
        return DefSubclassProc(window, message, wParam, lParam);
    }

    inline void Apply(HWND window)
    {
        SetWindowSubclass(window, ColorProc, 1, 0);
        EnumChildWindows(window, [](HWND control, LPARAM) -> BOOL
        {
            if (IsClass(control, WC_LISTVIEWW)) StyleListView(control);
            if ((IsClass(control, L"Edit") && !(GetWindowLongPtrW(control, GWL_STYLE) & ES_MULTILINE)) ||
                IsClass(control, HOTKEY_CLASSW))
                StyleTextInput(control);
            if (IsClass(control, WC_TABCONTROLW))
            {
                SetWindowLongPtrW(control, GWL_STYLE,
                    GetWindowLongPtrW(control, GWL_STYLE) | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
                SetWindowSubclass(control, TabPaintProc, 1, 0);
            }
            if (IsClass(control, L"Button") &&
                (GetWindowLongPtrW(control, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX)
            {
                // Native group-box painting can restore a white background when shown from the tray.
                SetWindowLongPtrW(control, GWL_STYLE, GetWindowLongPtrW(control, GWL_STYLE) | WS_CLIPSIBLINGS);
                DWORD_PTR heading{};
                if (!GetWindowSubclass(control, GroupPaintProc, 1, &heading))
                {
                    LOGFONTW font{};
                    GetObjectW(reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0)), sizeof(font), &font);
                    font.lfHeight = -MulDiv(10, GetDpiForWindow(control), 72);
                    font.lfWeight = FW_SEMIBOLD;
                    const HFONT handle = CreateFontIndirectW(&font);
                    heading = reinterpret_cast<DWORD_PTR>(handle);
                    if (handle) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(handle), FALSE);
                }
                SetWindowSubclass(control, GroupPaintProc, 1, heading);
            }
            return TRUE;
        }, 0);
    }
}
