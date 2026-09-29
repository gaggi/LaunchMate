#pragma once

struct DetectedProcessCandidate
{
    const wchar_t* name;
    const wchar_t* executable;
    const wchar_t* fallbackPath;
    const wchar_t* category;
    bool allowStop;
};

// Paths are only used after checking that the executable really exists.
inline constexpr DetectedProcessCandidate kDetectedProcessCandidates[] = {
    {L"OneDrive", L"OneDrive.exe", L"%LocalAppData%\\Microsoft\\OneDrive\\OneDrive.exe", L"Cloud sync", true},
    {L"Dropbox", L"Dropbox.exe", L"%ProgramFiles(x86)%\\Dropbox\\Client\\Dropbox.exe", L"Cloud sync", true},
    {L"Google Drive", L"GoogleDriveFS.exe", L"", L"Cloud sync", true},
    {L"Backblaze", L"bzbui.exe", L"%ProgramFiles(x86)%\\Backblaze\\bzbui.exe", L"Cloud sync", true},
    {L"Steam", L"steam.exe", L"%ProgramFiles(x86)%\\Steam\\steam.exe", L"Game launcher", true},
    {L"Epic Games Launcher", L"EpicGamesLauncher.exe", L"%ProgramFiles(x86)%\\Epic Games\\Launcher\\Portal\\Binaries\\Win64\\EpicGamesLauncher.exe", L"Game launcher", true},
    {L"GOG Galaxy", L"GalaxyClient.exe", L"%ProgramFiles(x86)%\\GOG Galaxy\\GalaxyClient.exe", L"Game launcher", true},
    {L"Ubisoft Connect", L"UbisoftConnect.exe", L"%ProgramFiles(x86)%\\Ubisoft\\Ubisoft Game Launcher\\UbisoftConnect.exe", L"Game launcher", true},
    {L"EA app", L"EADesktop.exe", L"%ProgramFiles%\\Electronic Arts\\EA Desktop\\EA Desktop\\EADesktop.exe", L"Game launcher", true},
    {L"Battle.net", L"Battle.net.exe", L"%ProgramFiles(x86)%\\Battle.net\\Battle.net.exe", L"Game launcher", true},
    {L"Xbox app", L"XboxPcApp.exe", L"", L"Game launcher", true},
    {L"Discord", L"Discord.exe", L"", L"Communication", true},
    {L"Teams", L"ms-teams.exe", L"", L"Communication", true},
    {L"Teams", L"Teams.exe", L"", L"Communication", true},
    {L"RTSS", L"RTSS.exe", L"%ProgramFiles(x86)%\\RivaTuner Statistics Server\\RTSS.exe", L"Overlay", true},
    {L"MSI Afterburner", L"MSIAfterburner.exe", L"%ProgramFiles(x86)%\\MSI Afterburner\\MSIAfterburner.exe", L"Overlay", true},
    {L"Outlook", L"OUTLOOK.EXE", L"%ProgramFiles%\\Microsoft Office\\root\\Office16\\OUTLOOK.EXE", L"Work app", true},
    {L"Chrome", L"chrome.exe", L"%ProgramFiles%\\Google\\Chrome\\Application\\chrome.exe", L"Browser", true},
    {L"Edge", L"msedge.exe", L"%ProgramFiles(x86)%\\Microsoft\\Edge\\Application\\msedge.exe", L"Browser", true},
    {L"PDF24", L"pdf24.exe", L"%ProgramFiles%\\PDF24\\pdf24.exe", L"Work app", true},
    {L"Adobe Creative Cloud", L"Creative Cloud.exe", L"%ProgramFiles%\\Adobe\\Adobe Creative Cloud\\ACC\\Creative Cloud.exe", L"Work app", true},
    {L"Codex", L"Codex.exe", L"", L"Work app", true},
    {L"NordVPN", L"NordVPN.exe", L"%ProgramFiles%\\NordVPN\\NordVPN.exe", L"VPN", true},
};
