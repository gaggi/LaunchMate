#pragma once

#include <functional>
#include <string>
#include <vector>

class MonitorPowerController
{
public:
    struct DisplayInfo
    {
        std::wstring displayName;
        std::wstring displayLabel;
    };

    static std::vector<DisplayInfo> EnumerateDisplays();
    static bool SetPower(
        const std::wstring& displayName,
        bool powerOn,
        const std::function<void(const std::wstring&)>& logger = {},
        std::wstring* errorMessage = nullptr);
    static bool TogglePower(
        const std::wstring& displayName,
        const std::function<void(const std::wstring&)>& logger = {},
        std::wstring* errorMessage = nullptr);
    static bool ApplySetup(
        const std::vector<std::wstring>& enabledDisplays,
        const std::function<void(const std::wstring&)>& logger = {},
        std::wstring* errorMessage = nullptr);
};
