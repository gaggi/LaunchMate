#pragma once

#include "ui/UiTheme.h"

#include <algorithm>
#include <string>
#include <vector>
#include <windows.h>
#include <windowsx.h>

// Monitors drawn as tiles in their desktop arrangement. Clicking a tile switches it
// on or off; clicking its star makes it the main monitor. Notifies the parent with
// WM_COMMAND(MAKEWPARAM(id, kToggled or kPrimary), hwnd); NotifiedTile() says which.
class MonitorLayout
{
public:
    static constexpr WORD kToggled = 1;
    static constexpr WORD kPrimary = 2;

    struct Tile
    {
        std::wstring name;
        LONG x{};
        LONG y{};
        UINT width{};
        bool enabled{};
        bool primary{};
        bool operator==(const Tile&) const = default;
    };

    MonitorLayout() = default;
    MonitorLayout(const MonitorLayout&) = delete;
    MonitorLayout& operator=(const MonitorLayout&) = delete;
    ~MonitorLayout()
    {
        if (glyphFont_) DeleteObject(glyphFont_);
    }

    bool Create(HINSTANCE instance, HWND parent, int id, HFONT font)
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
        window_ = CreateWindowExW(0, kClassName, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 100, 100, parent,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, this);
        if (!window_) return false;
        glyphFont_ = CreateFontW(-MulDiv(16, GetDpiForWindow(window_), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, UiTheme::IconFontFace());
        return true;
    }

    HWND Handle() const noexcept { return window_; }
    int NotifiedTile() const noexcept { return notified_; }

    void SetTiles(std::vector<Tile> tiles, std::wstring emptyText)
    {
        if (tiles == tiles_ && emptyText == emptyText_) return;
        tiles_ = std::move(tiles);
        emptyText_ = std::move(emptyText);
        InvalidateRect(window_, nullptr, FALSE);
    }

private:
    static constexpr wchar_t kClassName[] = L"LaunchMateMonitorLayout";

    int Scale(int value) const { return MulDiv(value, GetDpiForWindow(window_), 96); }

    // Windows reports positions and widths only; assume 16:9 for the height.
    static LONG TileHeight(const Tile& tile) { return static_cast<LONG>(tile.width) * 9 / 16; }

    std::vector<RECT> TileRects() const
    {
        std::vector<RECT> rects;
        if (tiles_.empty()) return rects;
        LONG left = LONG_MAX, top = LONG_MAX, right = LONG_MIN, bottom = LONG_MIN;
        for (const auto& tile : tiles_)
        {
            const LONG width = std::max<LONG>(1, static_cast<LONG>(tile.width));
            left = std::min(left, tile.x);
            top = std::min(top, tile.y);
            right = std::max(right, tile.x + width);
            bottom = std::max(bottom, tile.y + std::max<LONG>(1, TileHeight(tile)));
        }
        RECT client{};
        GetClientRect(window_, &client);
        const int margin = Scale(16);
        const double scale = std::min(
            static_cast<double>(client.right - 2 * margin) / std::max<LONG>(1, right - left),
            static_cast<double>(client.bottom - 2 * margin) / std::max<LONG>(1, bottom - top));
        const int offsetX = (client.right - static_cast<int>((right - left) * scale)) / 2;
        const int offsetY = (client.bottom - static_cast<int>((bottom - top) * scale)) / 2;
        const int gap = Scale(3);
        for (const auto& tile : tiles_)
        {
            RECT rect{
                offsetX + static_cast<int>((tile.x - left) * scale) + gap,
                offsetY + static_cast<int>((tile.y - top) * scale) + gap,
                offsetX + static_cast<int>((tile.x - left + static_cast<LONG>(tile.width)) * scale) - gap,
                offsetY + static_cast<int>((tile.y - top + TileHeight(tile)) * scale) - gap};
            rects.push_back(rect);
        }
        return rects;
    }

    RECT StarRect(const RECT& tile) const
    {
        const int size = Scale(26);
        return {tile.right - size - Scale(4), tile.top + Scale(4), tile.right - Scale(4), tile.top + size + Scale(4)};
    }

    static void Rounded(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border, int width = 1)
    {
        const HBRUSH brush = CreateSolidBrush(fill);
        const HPEN pen = CreatePen(PS_SOLID, width, border);
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
        RECT stage = client;
        InflateRect(&stage, -1, -1);
        Rounded(dc, stage, Scale(12), RGB(238, 240, 243), RGB(225, 227, 231));
        SetBkMode(dc, TRANSPARENT);
        const auto oldFont = SelectObject(dc, font_);
        if (tiles_.empty())
        {
            SetTextColor(dc, RGB(110, 112, 118));
            DrawTextW(dc, emptyText_.c_str(), -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        const auto rects = TileRects();
        for (size_t index = 0; index < tiles_.size(); ++index)
        {
            const auto& tile = tiles_[index];
            const RECT& rect = rects[index];
            const bool hover = static_cast<int>(index) == hover_;
            if (tile.enabled)
                Rounded(dc, rect, Scale(8), RGB(230, 241, 251), RGB(55, 138, 221), Scale(2));
            else
                Rounded(dc, rect, Scale(8), hover ? RGB(248, 249, 251) : UiTheme::Surface, hover ? RGB(160, 165, 172) : RGB(200, 203, 208));
            SelectObject(dc, font_);
            SetTextColor(dc, tile.enabled ? RGB(12, 68, 124) : RGB(130, 132, 138));
            RECT text = rect;
            InflateRect(&text, -Scale(8), -Scale(8));
            const std::wstring label = tile.name + L"\n" + (tile.primary ? L"Main monitor" : tile.enabled ? L"On" : L"Off");
            RECT measure = text;
            DrawTextW(dc, label.c_str(), -1, &measure, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
            const int height = measure.bottom - measure.top;
            text.top = (rect.top + rect.bottom - height) / 2;
            text.bottom = text.top + height;
            DrawTextW(dc, label.c_str(), -1, &text, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(dc, glyphFont_);
            RECT star = StarRect(rect);
            SetTextColor(dc, tile.primary ? RGB(55, 138, 221) : RGB(160, 165, 172));
            DrawTextW(dc, tile.primary ? L"" : L"", 1, &star, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
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
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const auto rects = TileRects();
            int hit = -1;
            for (size_t index = 0; index < rects.size(); ++index)
                if (PtInRect(&rects[index], point)) hit = static_cast<int>(index);
            if (hit != hover_) { hover_ = hit; InvalidateRect(window_, nullptr, FALSE); }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_};
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = -1;
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP:
        {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const auto rects = TileRects();
            for (size_t index = 0; index < rects.size(); ++index)
            {
                if (!PtInRect(&rects[index], point)) continue;
                const RECT star = StarRect(rects[index]);
                notified_ = static_cast<int>(index);
                SendMessageW(GetParent(window_), WM_COMMAND, MAKEWPARAM(id_, PtInRect(&star, point) ? kPrimary : kToggled),
                    reinterpret_cast<LPARAM>(window_));
                return 0;
            }
            return 0;
        }
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<MonitorLayout*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<MonitorLayout*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
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
    HFONT glyphFont_{};
    std::vector<Tile> tiles_;
    std::wstring emptyText_;
    int hover_{-1};
    int notified_{-1};
};
