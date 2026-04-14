#pragma once

struct CatalogPathCandidate
{
    const wchar_t* displayName;
    const wchar_t* path;
};

inline constexpr CatalogPathCandidate kCatalogPathCandidates[] = {
    {L"SimHub", L"%ProgramFiles(x86)%\\SimHub\\SimHubWPF.exe"},
    {L"SimHub", L"%ProgramFiles%\\SimHub\\SimHubWPF.exe"},
    {L"CrewChief", L"%ProgramFiles(x86)%\\Britton IT Ltd\\CrewChiefV4\\CrewChiefV4.exe"},
    {L"CrewChief", L"%ProgramFiles%\\Britton IT Ltd\\CrewChiefV4\\CrewChiefV4.exe"},
    {L"CrewChiefV4", L"%ProgramFiles(x86)%\\Britton IT Ltd\\CrewChiefV4\\CrewChiefV4.exe"},
    {L"CrewChiefV4", L"%ProgramFiles%\\Britton IT Ltd\\CrewChiefV4\\CrewChiefV4.exe"},
    {L"iOverlay", L"%LocalAppData%\\Programs\\iOverlay\\iOverlay.exe"},
    {L"iOverlay", L"%ProgramFiles%\\iOverlay\\iOverlay.exe"},
    {L"iOverlay", L"%ProgramFiles(x86)%\\iOverlay\\iOverlay.exe"},
    {L"irDash", L"%ProgramFiles(x86)%\\irDash\\irDash.exe"},
    {L"irDash", L"%ProgramFiles%\\irDash\\irDash.exe"},
    {L"irDash", L"%LocalAppData%\\Programs\\irDash\\irDash.exe"},
    {L"irDashies", L"%LocalAppData%\\irdashies\\irdashies.exe"},
    {L"Garage61", L"%AppData%\\garage61-install\\garage61-launcher.exe"},
    {L"Garage61", L"%LocalAppData%\\Garage61\\garage61-launcher.exe"},
    {L"Garage61", L"%LocalAppData%\\Programs\\Garage61\\garage61-launcher.exe"},
    {L"Garage61", L"%ProgramFiles%\\Garage61\\garage61-launcher.exe"},
    {L"RacelabApps", L"%LocalAppData%\\Programs\\RacelabApps\\RacelabApps.exe"},
    {L"Trading Paints", L"%LocalAppData%\\Programs\\trading-paints\\TradingPaints.exe"},
    {L"OBS Studio", L"%ProgramFiles%\\obs-studio\\bin\\64bit\\obs64.exe"},
    {L"OBS Studio", L"%ProgramFiles(x86)%\\obs-studio\\bin\\32bit\\obs32.exe"},
    {L"Stream Deck", L"%ProgramFiles%\\Elgato\\StreamDeck\\StreamDeck.exe"},
    {L"OpenKneeboard", L"%ProgramFiles%\\OpenKneeboard\\OpenKneeboardApp.exe"},
    {L"OpenKneeboard", L"%LocalAppData%\\Programs\\OpenKneeboard\\OpenKneeboardApp.exe"}
};
