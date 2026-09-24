#pragma once

#include "Models.h"

#include <string>
#include <vector>

struct PowerSchemeInfo
{
    GUID id{};
    std::wstring name;
};

bool IsIRacingRule(const WatchedProcessRule& rule);
std::vector<PowerSchemeInfo> EnumeratePowerSchemes();
bool ActivatePowerScheme(const std::wstring& schemeGuid, GUID& previousScheme);
bool RestorePowerScheme(const GUID& scheme);
HWND CreateIRacingPerformancePane(HINSTANCE instance, HWND parent, WatchedProcessRule& rule);
void SaveIRacingPerformancePane(HWND pane);
void RefreshIRacingPerformancePane(HWND pane);
HWND CreateIRacingServicesPane(HINSTANCE instance, HWND parent, WatchedProcessRule& rule);
void SaveIRacingServicesPane(HWND pane);
void ShowMpoSettingsDialog(HINSTANCE instance, HWND parent);
