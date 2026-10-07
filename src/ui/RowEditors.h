#pragma once

#include "RowList.h"
#include "UiTheme.h"

#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>
#include <windows.h>
#include <commctrl.h>

// Native controls inside the expanded area of one RowList row. Positions are in
// DIPs relative to that area; call Position() on RowList::kLayoutChanged. The
// controls are children of the list, which passes their WM_COMMAND on unchanged.
class RowEditors
{
public:
    void Attach(RowList& list, HINSTANCE instance, HFONT font)
    {
        list_ = &list;
        instance_ = instance;
        font_ = font;
    }

    bool Empty() const noexcept { return editors_.empty(); }
    int Row() const noexcept { return row_; }

    // Starts a new set of controls for `row`, removing the previous ones.
    void Begin(int row)
    {
        Clear();
        row_ = row;
    }

    void Clear()
    {
        for (const auto& editor : editors_) DestroyWindow(editor.control);
        editors_.clear();
        row_ = -1;
    }

    // width <= 0 stretches the control to the right edge, keeping -width DIPs free.
    HWND Add(const wchar_t* className, const wchar_t* text, DWORD style, int id, int x, int y, int width, int height = 24)
    {
        const HWND control = CreateWindowExW(0, className, text, WS_CHILD | style, 0, 0, 10, 10, list_->Handle(),
            id == 0 ? nullptr : reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        if (_wcsicmp(className, L"EDIT") == 0 && !(style & ES_MULTILINE)) UiTheme::StyleTextInput(control);
        if (_wcsicmp(className, L"EDIT") == 0 && (style & ES_MULTILINE))
            SetWindowLongPtrW(control, GWL_EXSTYLE, GetWindowLongPtrW(control, GWL_EXSTYLE) | WS_EX_CLIENTEDGE);
        editors_.push_back({control, x, y, width, height});
        return control;
    }

    HWND Label(const wchar_t* text, int x, int y, int width)
    {
        return Add(L"STATIC", text, SS_CENTERIMAGE | SS_ENDELLIPSIS, 0, x, y, width);
    }

    HWND Edit(int id, const std::wstring& text, int x, int y, int width)
    {
        return Add(L"EDIT", text.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, id, x, y, width);
    }

    // A small edit for a duration in seconds, followed by "s".
    HWND Seconds(int id, int milliseconds, int x, int y)
    {
        const HWND edit = Edit(id, SecondsText(milliseconds), x, y, 64);
        Label(L"s", x + 70, y, 20);
        return edit;
    }

    HWND Check(int id, const wchar_t* text, bool checked, int x, int y, int width)
    {
        const HWND box = Add(L"BUTTON", text, WS_TABSTOP | BS_AUTOCHECKBOX, id, x, y, width);
        SendMessageW(box, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
        return box;
    }

    HWND Combo(int id, const std::vector<std::wstring>& items, int selected, int x, int y, int width)
    {
        const HWND combo = Add(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, id, x, y, width, 300);
        for (const auto& item : items) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(std::max(0, selected)), 0);
        return combo;
    }

    HWND Button(int id, const wchar_t* text, int x, int y, int width)
    {
        return Add(L"BUTTON", text, WS_TABSTOP | BS_PUSHBUTTON, id, x, y, width, 28);
    }

    HWND Get(int id) const { return GetDlgItem(list_->Handle(), id); }

    std::wstring Text(int id) const
    {
        wchar_t text[4096]{};
        GetDlgItemTextW(list_->Handle(), id, text, static_cast<int>(std::size(text)));
        return text;
    }

    bool Checked(int id) const { return SendMessageW(Get(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
    int Selection(int id) const { return static_cast<int>(SendMessageW(Get(id), CB_GETCURSEL, 0, 0)); }

    // Moves the controls to the row's current place and shows them.
    void Position()
    {
        if (editors_.empty()) return;
        const RECT area = list_->ExpansionRect(row_);
        const UINT dpi = GetDpiForWindow(list_->Handle());
        const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
        const int width = MulDiv(area.right - area.left, 96, static_cast<int>(dpi));
        for (const auto& editor : editors_)
        {
            const int controlWidth = editor.width > 0 ? editor.width : std::max(40, width - editor.x + editor.width);
            MoveWindow(editor.control, area.left + scale(editor.x), area.top + scale(editor.y), scale(controlWidth),
                scale(editor.height), TRUE);
            ShowWindow(editor.control, SW_SHOW);
        }
    }

    static std::wstring SecondsText(int milliseconds)
    {
        wchar_t text[32]{};
        swprintf_s(text, L"%g", std::max(0, milliseconds) / 1000.0);
        return text;
    }

    // Accepts "0.5" and "0,5"; empty means 0.
    static bool ParseSeconds(std::wstring text, int& milliseconds)
    {
        std::replace(text.begin(), text.end(), L',', L'.');
        const auto first = text.find_first_not_of(L" \t");
        if (first == std::wstring::npos) { milliseconds = 0; return true; }
        text = text.substr(first, text.find_last_not_of(L" \t") - first + 1);
        wchar_t* end = nullptr;
        const double value = std::wcstod(text.c_str(), &end);
        if (end != text.c_str() + text.size() || value < 0 || value > 3600) return false;
        milliseconds = static_cast<int>(value * 1000 + 0.5);
        return true;
    }

private:
    struct Editor
    {
        HWND control{};
        int x{};
        int y{};
        int width{};
        int height{};
    };

    RowList* list_{};
    HINSTANCE instance_{};
    HFONT font_{};
    std::vector<Editor> editors_;
    int row_{-1};
};
