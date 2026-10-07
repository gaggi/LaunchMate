#pragma once

#include "Models.h"

#include <filesystem>
#include <string>
#include <vector>

struct PowerSchemeInfo
{
    GUID id{};
    std::wstring name;
};

bool IsIRacingRule(const WatchedProcessRule& rule);
std::vector<PowerSchemeInfo> EnumeratePowerSchemes();
// "{8c5e7fda-...}" as stored in WatchedProcessRule::powerSchemeGuid.
std::wstring PowerSchemeGuidText(const GUID& guid);
bool ActivatePowerScheme(const std::wstring& schemeGuid, GUID& previousScheme);
bool RestorePowerScheme(const GUID& scheme);

// What LaunchMate checks for an iRacing setup. Reading it takes a moment (it asks
// Defender through PowerShell), so call CheckIRacingSetup on a worker thread.
struct IRacingCheck
{
    std::filesystem::path documents;
    std::filesystem::path install;
    bool iniReadable{};
    bool carPreload{};
    bool trackPreload{};
    int streamingTextureSize{256};
    // Reading Defender exclusions may need administrator rights.
    bool defenderReadable{};
    bool installExcluded{};
    bool documentsExcluded{};
    std::wstring activePowerPlan;
    bool x3dProcessor{};
    std::wstring rtss;
    std::wstring afterburner;
    struct Display
    {
        std::wstring name;
        unsigned long current{};
        unsigned long maximum{};
    };
    std::vector<Display> displays;
};

IRacingCheck CheckIRacingSetup(const WatchedProcessRule& rule);
bool IsIRacingRunning();
// Writes the texture settings to Documents\iRacing\app.ini after making a backup.
bool SaveIRacingIni(const std::filesystem::path& documents, bool carPreload, bool trackPreload, int streamingTextureSize, std::wstring& error);
// Asks for administrator approval and verifies the result.
bool ChangeIRacingDefenderExclusions(const std::filesystem::path& install, const std::filesystem::path& documents, bool remove, std::wstring& error);
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
