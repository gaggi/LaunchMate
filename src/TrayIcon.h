#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

class TrayIcon
{
public:
    using Callback = std::function<void(UINT)>;
    static constexpr UINT kMonitorSetupCommandBase = 4000;

    TrayIcon();
    ~TrayIcon();

    bool Create(HWND windowHandle, UINT callbackMessage, HICON icon, std::wstring tooltip, Callback callback);
    void Destroy();
    void UpdateTooltip(const std::wstring& tooltip);
    void ShowContextMenu(bool isMonitoring, const std::vector<std::wstring>& monitorSetupNames = {});

private:
    struct Data;

    HWND windowHandle_{nullptr};
    UINT callbackMessage_{0};
    std::unique_ptr<Data> data_;
    HMENU menu_{nullptr};
    Callback callback_;
};
