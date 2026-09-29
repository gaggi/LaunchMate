#pragma once

#include "Models.h"

#include <windows.h>

bool ShowRuleActionsDialog(
    HINSTANCE instanceHandle,
    HWND owner,
    WatchedProcessRule& rule,
    const std::vector<MonitorPowerSetup>& monitorSetups);

bool ShowStopProcessActionDialog(
    HINSTANCE instanceHandle,
    HWND owner,
    ProcessStopAction& action);
