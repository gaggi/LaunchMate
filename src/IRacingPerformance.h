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
// Multiplane overlay: Windows default, or disabled through the two community
// registry values. Changes need administrator rights and a Windows restart.
struct MpoState
{
    bool readable{};
    bool disabled{};
    // Any of the two values exists, i.e. not the Windows default.
    bool customized{};
};
MpoState ReadMpoState();
bool ChangeMpo(bool disable, std::wstring& error);
