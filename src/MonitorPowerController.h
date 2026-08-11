#pragma once

#include "Models.h"

#include <functional>
#include <string>
#include <vector>

class MonitorPowerController
{
public:
    static bool CaptureSetup(
        MonitorPowerSetup& setup,
        std::wstring* errorMessage = nullptr);
    static bool ApplySetup(
        const MonitorPowerSetup& setup,
        const std::function<void(const std::wstring&)>& logger = {},
        std::wstring* errorMessage = nullptr);
};
