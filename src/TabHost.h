#pragma once

#include <commctrl.h>
#include <windows.h>

#include <initializer_list>

inline LRESULT CALLBACK HostedPanelSubclassProc(
    HWND tab,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR subclassId,
    DWORD_PTR referenceData)
{
    if (message == WM_COMMAND || message == WM_NOTIFY)
    {
        return SendMessageW(reinterpret_cast<HWND>(referenceData), message, wParam, lParam);
    }
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN ||
        message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX)
    {
        return SendMessageW(reinterpret_cast<HWND>(referenceData), message, wParam, lParam);
    }
    if (message == WM_NCDESTROY)
    {
        RemoveWindowSubclass(tab, HostedPanelSubclassProc, subclassId);
    }
    return DefSubclassProc(tab, message, wParam, lParam);
}

inline void HostControlsInPanel(HWND owner, HWND tab, std::initializer_list<HWND> controls)
{
    SetWindowSubclass(tab, HostedPanelSubclassProc, 1, reinterpret_cast<DWORD_PTR>(owner));
    for (HWND control : controls)
    {
        if (!control) continue;
        RECT rect{};
        GetWindowRect(control, &rect);
        POINT position{rect.left, rect.top};
        ScreenToClient(tab, &position);
        SetParent(control, tab);
        SetWindowPos(control, nullptr, position.x, position.y, rect.right - rect.left, rect.bottom - rect.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

inline void HostControlsInTab(HWND owner, HWND tab, std::initializer_list<HWND> controls)
{
    HostControlsInPanel(owner, tab, controls);
}
