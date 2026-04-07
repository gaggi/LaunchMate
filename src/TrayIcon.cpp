#include "TrayIcon.h"

#include <memory>
#include <windows.h>
#include <shellapi.h>

namespace
{
    constexpr UINT kMenuOpen = 1001;
    constexpr UINT kMenuToggleMonitoring = 1002;
    constexpr UINT kMenuExit = 1003;
}

struct TrayIcon::Data
{
    NOTIFYICONDATAW notifyIcon{};
};

TrayIcon::TrayIcon() = default;

TrayIcon::~TrayIcon()
{
    Destroy();
}

bool TrayIcon::Create(HWND windowHandle, UINT callbackMessage, HICON icon, std::wstring tooltip, Callback callback)
{
    windowHandle_ = windowHandle;
    callbackMessage_ = callbackMessage;
    callback_ = std::move(callback);

    data_ = std::make_unique<Data>();
    auto& notifyIcon = data_->notifyIcon;
    notifyIcon = {};
    notifyIcon.cbSize = sizeof(notifyIcon);
    notifyIcon.hWnd = windowHandle_;
    notifyIcon.uID = 1;
    notifyIcon.uCallbackMessage = callbackMessage_;
    notifyIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    notifyIcon.hIcon = icon;
    wcsncpy_s(notifyIcon.szTip, tooltip.c_str(), _TRUNCATE);

    menu_ = CreatePopupMenu();
    AppendMenuW(menu_, MF_STRING, kMenuOpen, L"Open");
    AppendMenuW(menu_, MF_STRING, kMenuToggleMonitoring, L"Start monitoring");
    AppendMenuW(menu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu_, MF_STRING, kMenuExit, L"Exit");

    return Shell_NotifyIconW(NIM_ADD, &notifyIcon) == TRUE;
}

void TrayIcon::Destroy()
{
    if (data_ && data_->notifyIcon.hWnd != nullptr)
    {
        Shell_NotifyIconW(NIM_DELETE, &data_->notifyIcon);
        data_->notifyIcon.hWnd = nullptr;
    }

    if (menu_ != nullptr)
    {
        DestroyMenu(menu_);
        menu_ = nullptr;
    }

    data_.reset();
}

void TrayIcon::UpdateTooltip(const std::wstring& tooltip)
{
    if (!data_ || data_->notifyIcon.hWnd == nullptr)
    {
        return;
    }

    data_->notifyIcon.uFlags = NIF_TIP;
    wcsncpy_s(data_->notifyIcon.szTip, tooltip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data_->notifyIcon);
}

void TrayIcon::ShowContextMenu(bool isMonitoring)
{
    if (menu_ == nullptr)
    {
        return;
    }

    ModifyMenuW(menu_, kMenuToggleMonitoring, MF_BYCOMMAND | MF_STRING, kMenuToggleMonitoring,
        isMonitoring ? L"Stop monitoring" : L"Start monitoring");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(windowHandle_);
    const auto command = TrackPopupMenu(menu_, TPM_RIGHTBUTTON | TPM_RETURNCMD, cursor.x, cursor.y, 0, windowHandle_, nullptr);
    if (command != 0 && callback_)
    {
        callback_(command);
    }
}
