#pragma once

#include "Models.h"

#include <windows.h>

bool ShowRuleActionsDialog(
    HINSTANCE instanceHandle,
    HWND owner,
    WatchedProcessRule& rule,
    const std::vector<MonitorPowerSetup>& monitorSetups,
    int initialTab = 0,
    int initialActionIndex = -1);

bool ShowStopProcessActionDialog(
    HINSTANCE instanceHandle,
    HWND owner,
    ProcessStopAction& action);

bool ShowHomeAssistantActionDialog(
    HINSTANCE instanceHandle,
    HWND owner,
    HomeAssistantAction& action);
