#pragma once

#include <filesystem>
#include <string>
#include <windows.h>

struct UpdateReleaseInfo
{
    std::wstring versionTag;
    std::wstring versionDisplay;
    std::wstring releasePageUrl;
    std::wstring assetName;
    std::wstring assetDownloadUrl;
};

enum class UpdateCheckState
{
    UpToDate,
    UpdateAvailable,
    Failed
};

struct UpdateCheckResult
{
    UpdateCheckState state{UpdateCheckState::Failed};
    UpdateReleaseInfo release;
    std::wstring message;
};

class UpdateChecker
{
public:
    static std::wstring CurrentVersion();
    static UpdateCheckResult CheckForUpdate();
    static bool DownloadReleaseAsset(const UpdateReleaseInfo& release, std::filesystem::path& downloadedPath, std::wstring& errorMessage);
    static bool LaunchSelfUpdater(const std::filesystem::path& downloadedPath, DWORD processId, std::wstring& errorMessage);
    static bool OpenReleasePage(const std::wstring& releasePageUrl);
};
