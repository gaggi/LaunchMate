#pragma once

#include <commctrl.h>
#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <initializer_list>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ListViewDetail
{
    struct SortValue
    {
        std::wstring text;
        double number{};
        bool numeric{};
    };

    struct SortContext
    {
        std::unordered_map<LPARAM, SortValue> values;
        bool numericColumn{};
        bool ascending{};
    };

    inline int CALLBACK CompareItems(LPARAM leftData, LPARAM rightData, LPARAM contextData)
    {
        const auto& context = *reinterpret_cast<const SortContext*>(contextData);
        const auto& left = context.values.at(leftData);
        const auto& right = context.values.at(rightData);
        int result = 0;
        if (context.numericColumn)
        {
            if (left.numeric != right.numeric) result = left.numeric ? -1 : 1;
            else if (left.numeric && left.number != right.number) result = left.number < right.number ? -1 : 1;
        }
        if (result == 0) result = _wcsicmp(left.text.c_str(), right.text.c_str());
        return context.ascending ? result : -result;
    }
}

inline void ConfigureListView(HWND list, std::initializer_list<std::pair<const wchar_t*, int>> columns)
{
    RemovePropW(list, L"LaunchMate.ListViewSort");
    ListView_DeleteAllItems(list);
    while (Header_GetItemCount(ListView_GetHeader(list)) > 0)
    {
        ListView_DeleteColumn(list, 0);
    }

    RECT rect{};
    GetClientRect(list, &rect);
    const int availableWidth = std::max(100, static_cast<int>(rect.right - rect.left) - GetSystemMetrics(SM_CXVSCROLL) - 4);
    int totalWeight = 0;
    for (const auto& column : columns) totalWeight += column.second;

    int index = 0;
    int usedWidth = 0;
    for (const auto& column : columns)
    {
        const int width = index + 1 == static_cast<int>(columns.size())
            ? availableWidth - usedWidth
            : (availableWidth * column.second) / totalWeight;
        LVCOLUMNW item{};
        item.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        item.pszText = const_cast<wchar_t*>(column.first);
        item.cx = width;
        item.fmt = LVCFMT_LEFT;
        ListView_InsertColumn(list, index++, &item);
        usedWidth += width;
    }
}

inline int AddListViewRow(HWND list, std::initializer_list<std::wstring> values, LPARAM itemData = -1)
{
    if (values.size() == 0) return -1;
    auto value = values.begin();
    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = ListView_GetItemCount(list);
    item.lParam = itemData >= 0 ? itemData : item.iItem;
    item.pszText = const_cast<wchar_t*>(value->c_str());
    const int row = ListView_InsertItem(list, &item);
    for (int column = 1; ++value != values.end(); ++column)
    {
        ListView_SetItemText(list, row, column, const_cast<wchar_t*>(value->c_str()));
    }
    return row;
}

inline int SelectedListViewRow(HWND list)
{
    const int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (row < 0) return -1;
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = row;
    return ListView_GetItem(list, &item) ? static_cast<int>(item.lParam) : -1;
}

inline void SortListViewByColumn(HWND list, int column)
{
    const auto previousState = reinterpret_cast<INT_PTR>(GetPropW(list, L"LaunchMate.ListViewSort"));
    const int previousColumn = previousState == 0 ? -1 : static_cast<int>((previousState >> 1) - 1);
    const bool previousAscending = (previousState & 1) != 0;
    const bool ascending = previousColumn == column ? !previousAscending : true;

    ListViewDetail::SortContext context;
    context.ascending = ascending;
    bool hasNumericValue = false;
    bool hasNonNumericValue = false;
    const int rowCount = ListView_GetItemCount(list);
    for (int row = 0; row < rowCount; ++row)
    {
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = row;
        ListView_GetItem(list, &item);

        wchar_t text[2048]{};
        ListView_GetItemText(list, row, column, text, static_cast<int>(std::size(text)));
        wchar_t* end = nullptr;
        const double number = std::wcstod(text, &end);
        const bool numeric = end != text;
        hasNumericValue = hasNumericValue || numeric;
        hasNonNumericValue = hasNonNumericValue || (!numeric && text[0] != L'-' && text[0] != L'\0');
        context.values.emplace(item.lParam, ListViewDetail::SortValue{text, number, numeric});
    }
    context.numericColumn = hasNumericValue && !hasNonNumericValue;

    ListView_SortItems(list, ListViewDetail::CompareItems, reinterpret_cast<LPARAM>(&context));
    SetPropW(list, L"LaunchMate.ListViewSort", reinterpret_cast<HANDLE>(
        static_cast<INT_PTR>(((column + 1) << 1) | (ascending ? 1 : 0))));

    HWND header = ListView_GetHeader(list);
    const int columnCount = Header_GetItemCount(header);
    for (int index = 0; index < columnCount; ++index)
    {
        HDITEMW item{};
        item.mask = HDI_FORMAT;
        Header_GetItem(header, index, &item);
        item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (index == column) item.fmt |= ascending ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(header, index, &item);
    }
}

inline void InitializeReportListView(HWND list)
{
    ListView_SetExtendedListViewStyle(list,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
}
