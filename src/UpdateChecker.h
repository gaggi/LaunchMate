#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <windows.h>

struct UpdateReleaseInfo
{
    std::wstring versionTag;
    std::wstring versionDisplay;
    std::wstring releasePageUrl;
    std::wstring assetName;
    std::wstring assetDownloadUrl;
    // GitHub's asset digest, e.g. "sha256:<hex>"; empty for releases without one.
    std::wstring assetDigest;
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
    static bool IsNewerVersion(std::wstring_view candidate, std::wstring_view current);
    static UpdateCheckResult CheckForUpdate();
    static UpdateCheckResult ParseReleaseMetadata(const std::string& metadata);
    static bool DownloadReleaseAsset(const UpdateReleaseInfo& release, std::filesystem::path& downloadedPath, std::wstring& errorMessage);
    static bool LaunchSelfUpdater(const std::filesystem::path& downloadedPath, DWORD processId, std::wstring& errorMessage);
    // Waits for processId to exit and swaps download in for target; the old executable
    // stays whole on failure. Retries while either file is briefly locked.
    static bool ApplyUpdate(DWORD processId, const std::filesystem::path& target, const std::filesystem::path& download,
        std::wstring& errorMessage, int attempts = 10);
    // Runs the swap when started as "<exe> --apply-update <pid> <target> <download>";
    // returns -1 for any other command line.
    static int HandleSelfUpdateCommandLine();
    static bool OpenReleasePage(const std::wstring& releasePageUrl);
};
