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
// MPO controls (IDC_MPO_*) embedded in another dialog, e.g. the Settings page.
void InitializeMpoControls(HWND dialog);
bool HandleMpoCommand(HWND dialog, int controlId);
