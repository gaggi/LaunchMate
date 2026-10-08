#include "UpdateChecker.h"

#include "JsonLite.h"
#include "Utils.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

namespace
{
#ifndef LAUNCHMATE_VERSION
#define LAUNCHMATE_VERSION "0.3.0"
#endif

#ifndef LAUNCHMATE_GITHUB_OWNER
#define LAUNCHMATE_GITHUB_OWNER "gaggi"
#endif

#ifndef LAUNCHMATE_GITHUB_REPOSITORY
#define LAUNCHMATE_GITHUB_REPOSITORY "LaunchMate"
#endif

#define LAUNCHMATE_WIDEN_IMPL(value) L##value
#define LAUNCHMATE_WIDEN(value) LAUNCHMATE_WIDEN_IMPL(value)

    using jsonlite::Object;

    constexpr wchar_t kCurrentVersion[] = LAUNCHMATE_WIDEN(LAUNCHMATE_VERSION);
    constexpr wchar_t kLatestReleaseUrl[] =
        L"https://api.github.com/repos/" LAUNCHMATE_WIDEN(LAUNCHMATE_GITHUB_OWNER) L"/"
        LAUNCHMATE_WIDEN(LAUNCHMATE_GITHUB_REPOSITORY) L"/releases/latest";

    struct InternetHandleCloser
    {
        void operator()(HINTERNET handle) const noexcept
        {
            if (handle != nullptr)
            {
                WinHttpCloseHandle(handle);
            }
        }
    };

    using UniqueInternetHandle = std::unique_ptr<void, InternetHandleCloser>;

    struct ParsedVersion
    {
        std::vector<int> numbers;
        bool hasSuffix{false};
    };

    std::wstring ReadWideString(const Object& object, const char* key)
    {
        const auto it = object.find(key);
        return it != object.end() && it->second.IsString() ? ToWide(it->second.AsString()) : std::wstring{};
    }

    std::wstring NormalizeVersion(std::wstring_view version)
    {
        size_t start = 0;
        while (start < version.size() && std::iswspace(static_cast<wint_t>(version[start])) != 0)
        {
            ++start;
        }

        size_t end = version.size();
        while (end > start && std::iswspace(static_cast<wint_t>(version[end - 1])) != 0)
        {
            --end;
        }

        std::wstring normalized(version.substr(start, end - start));
        if (!normalized.empty() && (normalized.front() == L'v' || normalized.front() == L'V'))
        {
            normalized.erase(normalized.begin());
        }

        return normalized;
    }

    ParsedVersion ParseVersion(std::wstring_view version)
    {
        const std::wstring normalized = NormalizeVersion(version);
        ParsedVersion parsed;

        size_t index = 0;
        while (index < normalized.size())
        {
            if (std::iswdigit(static_cast<wint_t>(normalized[index])) == 0)
            {
                parsed.hasSuffix = !parsed.numbers.empty() || !normalized.empty();
                break;
            }

            int value = 0;
            while (index < normalized.size() && std::iswdigit(static_cast<wint_t>(normalized[index])) != 0)
            {
                value = (value * 10) + (normalized[index] - L'0');
                ++index;
            }

            parsed.numbers.push_back(value);
            if (index >= normalized.size())
            {
                break;
            }

            if (normalized[index] == L'.')
            {
                ++index;
                continue;
            }

            parsed.hasSuffix = true;
            break;
        }

        return parsed;
    }

    bool IsNewerVersion(std::wstring_view candidate, std::wstring_view current)
    {
        const ParsedVersion candidateVersion = ParseVersion(candidate);
        const ParsedVersion currentVersion = ParseVersion(current);

        const size_t componentCount = std::max(candidateVersion.numbers.size(), currentVersion.numbers.size());
        for (size_t index = 0; index < componentCount; ++index)
        {
            const int candidatePart = index < candidateVersion.numbers.size() ? candidateVersion.numbers[index] : 0;
            const int currentPart = index < currentVersion.numbers.size() ? currentVersion.numbers[index] : 0;
            if (candidatePart != currentPart)
            {
                return candidatePart > currentPart;
            }
        }

        return !candidateVersion.hasSuffix && currentVersion.hasSuffix;
    }

    std::wstring PreferredAssetSuffix()
    {
#if defined(_WIN64)
        return L"windows-x64.exe";
#else
        return L"windows-x86.exe";
#endif
    }

    bool EndsWithInsensitive(const std::wstring& value, const std::wstring& suffix)
    {
        if (suffix.size() > value.size())
        {
            return false;
        }

        const size_t offset = value.size() - suffix.size();
        for (size_t index = 0; index < suffix.size(); ++index)
        {
            if (std::towlower(static_cast<wint_t>(value[offset + index])) != std::towlower(static_cast<wint_t>(suffix[index])))
            {
                return false;
            }
        }

        return true;
    }

    bool OpenHttpRequest(
        const std::wstring& url,
        UniqueInternetHandle& session,
        UniqueInternetHandle& connection,
        UniqueInternetHandle& request,
        std::wstring& errorMessage)
    {
        URL_COMPONENTSW components{};
        components.dwStructSize = sizeof(components);
        components.dwHostNameLength = static_cast<DWORD>(-1);
        components.dwUrlPathLength = static_cast<DWORD>(-1);
        components.dwExtraInfoLength = static_cast<DWORD>(-1);

        std::wstring mutableUrl = url;
        if (!WinHttpCrackUrl(mutableUrl.data(), static_cast<DWORD>(mutableUrl.size()), 0, &components))
        {
            errorMessage = L"Could not parse the update URL.";
            return false;
        }

        const std::wstring host(components.lpszHostName, components.dwHostNameLength);
        std::wstring resource(components.lpszUrlPath, components.dwUrlPathLength);
        if (components.dwExtraInfoLength > 0 && components.lpszExtraInfo != nullptr)
        {
            resource.append(components.lpszExtraInfo, components.dwExtraInfoLength);
        }

        session.reset(WinHttpOpen(L"LaunchMate Update", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (session) WinHttpSetTimeouts(session.get(), 5000, 5000, 10000, 15000);
        connection.reset(session ? WinHttpConnect(reinterpret_cast<HINTERNET>(session.get()), host.c_str(), components.nPort, 0) : nullptr);
        request.reset(connection
            ? WinHttpOpenRequest(reinterpret_cast<HINTERNET>(connection.get()), L"GET", resource.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
            : nullptr);

        if (!session || !connection || !request)
        {
            errorMessage = L"Could not initialize the GitHub update request.";
            return false;
        }

        return true;
    }

    bool SendHttpRequest(HINTERNET requestHandle, DWORD& statusCode, std::wstring& errorMessage)
    {
        const wchar_t* headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        if (!WinHttpSendRequest(requestHandle, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(requestHandle, nullptr))
        {
            errorMessage = L"The GitHub update request failed.";
            return false;
        }

        DWORD statusSize = sizeof(statusCode);
        if (!WinHttpQueryHeaders(requestHandle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX))
        {
            errorMessage = L"Could not read the GitHub update status code.";
            return false;
        }

        return true;
    }

    bool HttpGet(const std::wstring& url, std::vector<char>& body, DWORD& statusCode, std::wstring& errorMessage)
    {
        UniqueInternetHandle session;
        UniqueInternetHandle connection;
        UniqueInternetHandle request;
        if (!OpenHttpRequest(url, session, connection, request, errorMessage) ||
            !SendHttpRequest(reinterpret_cast<HINTERNET>(request.get()), statusCode, errorMessage))
        {
            return false;
        }

        body.clear();
        for (;;)
        {
            DWORD availableBytes = 0;
            if (!WinHttpQueryDataAvailable(reinterpret_cast<HINTERNET>(request.get()), &availableBytes))
            {
                errorMessage = L"Could not read the GitHub update response.";
                return false;
            }

            if (availableBytes == 0)
            {
                return true;
            }

            const size_t previousSize = body.size();
            constexpr size_t maxMetadataBytes = 4 * 1024 * 1024;
            if (availableBytes > maxMetadataBytes - previousSize)
            {
                errorMessage = L"The GitHub release response is too large.";
                return false;
            }
            body.resize(previousSize + availableBytes);

            DWORD downloadedBytes = 0;
            if (!WinHttpReadData(reinterpret_cast<HINTERNET>(request.get()), body.data() + previousSize, availableBytes, &downloadedBytes))
            {
                errorMessage = L"Could not download the GitHub update response.";
                return false;
            }

            body.resize(previousSize + downloadedBytes);
        }
    }

    bool HttpDownloadToFile(const std::wstring& url, const std::filesystem::path& outputPath, DWORD& statusCode, std::wstring& errorMessage)
    {
        UniqueInternetHandle session;
        UniqueInternetHandle connection;
        UniqueInternetHandle request;
        if (!OpenHttpRequest(url, session, connection, request, errorMessage) ||
            !SendHttpRequest(reinterpret_cast<HINTERNET>(request.get()), statusCode, errorMessage))
        {
            return false;
        }

        std::ofstream stream(outputPath, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            errorMessage = L"Could not create the temporary update file.";
            return false;
        }

        std::vector<char> buffer(64 * 1024);
        for (;;)
        {
            DWORD availableBytes = 0;
            if (!WinHttpQueryDataAvailable(reinterpret_cast<HINTERNET>(request.get()), &availableBytes))
            {
                errorMessage = L"Could not read the update download response.";
                break;
            }

            if (availableBytes == 0)
            {
                stream.close();
                if (stream.good()) return true;
                errorMessage = L"Could not finalize the temporary update file.";
                break;
            }

            const DWORD bytesToRead = std::min<DWORD>(availableBytes, static_cast<DWORD>(buffer.size()));
            DWORD downloadedBytes = 0;
            if (!WinHttpReadData(reinterpret_cast<HINTERNET>(request.get()), buffer.data(), bytesToRead, &downloadedBytes))
            {
                errorMessage = L"Could not download the update file.";
                break;
            }

            stream.write(buffer.data(), static_cast<std::streamsize>(downloadedBytes));
            if (!stream.good())
            {
                errorMessage = L"Could not write the temporary update file.";
                break;
            }
        }

        stream.close();
        std::error_code removeError;
        std::filesystem::remove(outputPath, removeError);
        return false;
    }

    std::wstring Sha256Hex(const std::filesystem::path& path)
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
        BCRYPT_HASH_HANDLE hash = nullptr;
        std::wstring result;
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0)
        {
            std::ifstream input(path, std::ios::binary);
            bool hashed = static_cast<bool>(input);
            std::vector<char> buffer(64 * 1024);
            while (hashed && input)
            {
                input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto count = input.gcount();
                if (count > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) < 0)
                    hashed = false;
            }
            UCHAR digest[32]{};
            if (hashed && !input.bad() && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0)
            {
                constexpr wchar_t kHex[] = L"0123456789abcdef";
                for (const UCHAR value : digest)
                {
                    result.push_back(kHex[value >> 4]);
                    result.push_back(kHex[value & 0x0f]);
                }
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return result;
    }

    std::filesystem::path CurrentExecutablePath()
    {
        wchar_t modulePath[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
        return length == 0 || length >= std::size(modulePath) ? std::filesystem::path{} : std::filesystem::path(modulePath);
    }

    // The update helper runs with LaunchMate's rights; in a protected folder such as
    // Program Files its copy would fail after LaunchMate had already exited.
    bool CanReplaceExecutable(const std::filesystem::path& executable)
    {
        const auto probe = executable.parent_path() /
            (L".launchmate-update-check-" + std::to_wstring(GetCurrentProcessId()));
        const HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        CloseHandle(file);
        return true;
    }
}

std::wstring UpdateChecker::CurrentVersion()
{
    return NormalizeVersion(kCurrentVersion);
}

UpdateCheckResult UpdateChecker::CheckForUpdate()
{
    UpdateCheckResult result;
    DWORD statusCode = 0;
    std::vector<char> responseBody;
    if (!HttpGet(kLatestReleaseUrl, responseBody, statusCode, result.message))
    {
        return result;
    }

    if (statusCode != 200)
    {
        result.message = L"GitHub returned HTTP " + std::to_wstring(statusCode) + L" while checking for updates.";
        return result;
    }

    try
    {
        const auto root = jsonlite::Parse(std::string(responseBody.begin(), responseBody.end()));
        if (!root.IsObject())
        {
            result.message = L"The GitHub release response was invalid.";
            return result;
        }

        const auto& object = root.AsObject();
        const std::wstring tagName = ReadWideString(object, "tag_name");
        result.release.versionTag = tagName;
        result.release.versionDisplay = NormalizeVersion(tagName);
        result.release.releasePageUrl = ReadWideString(object, "html_url");
        if (!IsNewerVersion(tagName, CurrentVersion()))
        {
            result.state = UpdateCheckState::UpToDate;
            result.message = L"No update available.";
            return result;
        }

        result.state = UpdateCheckState::UpdateAvailable;
        const std::wstring preferredSuffix = PreferredAssetSuffix();
        const auto assetsIt = object.find("assets");
        if (assetsIt != object.end() && assetsIt->second.IsArray())
        {
            for (const auto& item : assetsIt->second.AsArray())
            {
                if (!item.IsObject())
                {
                    continue;
                }

                const auto& assetObject = item.AsObject();
                const std::wstring assetName = ReadWideString(assetObject, "name");
                if (!EndsWithInsensitive(assetName, preferredSuffix))
                {
                    continue;
                }

                result.release.assetName = assetName;
                result.release.assetDownloadUrl = ReadWideString(assetObject, "browser_download_url");
                result.release.assetDigest = ReadWideString(assetObject, "digest");
                break;
            }
        }

        result.message = L"Update available.";
        return result;
    }
    catch (...)
    {
        result.message = L"Could not parse the GitHub release metadata.";
        return result;
    }
}

bool UpdateChecker::DownloadReleaseAsset(const UpdateReleaseInfo& release, std::filesystem::path& downloadedPath, std::wstring& errorMessage)
{
    if (release.assetDownloadUrl.empty())
    {
        errorMessage = L"The latest release does not provide a direct updater package for this architecture.";
        return false;
    }

    const auto executable = CurrentExecutablePath();
    if (executable.empty() || !CanReplaceExecutable(executable))
    {
        errorMessage = L"LaunchMate cannot replace itself in \"" + executable.parent_path().wstring() +
            L"\" without administrator rights. Please download the new version from the release page.";
        return false;
    }

    const auto updateDirectory = std::filesystem::temp_directory_path() / L"LaunchMate-updates";
    std::filesystem::create_directories(updateDirectory);
    downloadedPath = updateDirectory / (L"pending-" + (release.assetName.empty() ? std::wstring(L"LaunchMate-update.exe") : release.assetName));

    DWORD statusCode = 0;
    if (!HttpDownloadToFile(release.assetDownloadUrl, downloadedPath, statusCode, errorMessage))
    {
        return false;
    }

    if (statusCode != 200)
    {
        std::error_code removeError;
        std::filesystem::remove(downloadedPath, removeError);
        errorMessage = L"GitHub returned HTTP " + std::to_wstring(statusCode) + L" while downloading the update.";
        return false;
    }

    constexpr std::wstring_view kSha256Prefix = L"sha256:";
    if (release.assetDigest.starts_with(kSha256Prefix))
    {
        auto expected = release.assetDigest.substr(kSha256Prefix.size());
        std::transform(expected.begin(), expected.end(), expected.begin(), [](wchar_t value)
        {
            return static_cast<wchar_t>(std::towlower(value));
        });
        if (Sha256Hex(downloadedPath) != expected)
        {
            std::error_code removeError;
            std::filesystem::remove(downloadedPath, removeError);
            errorMessage = L"The downloaded update does not match the checksum published on GitHub.";
            return false;
        }
    }

    return true;
}

bool UpdateChecker::LaunchSelfUpdater(const std::filesystem::path& downloadedPath, DWORD processId, std::wstring& errorMessage)
{
    const auto currentExecutablePath = CurrentExecutablePath();
    if (currentExecutablePath.empty())
    {
        errorMessage = L"Could not determine the current LaunchMate executable path.";
        return false;
    }

    // The script itself stays ASCII; paths arrive as arguments, because cmd reads
    // batch files in the OEM code page and would garble e.g. umlauts in user names.
    const auto scriptPath = downloadedPath.parent_path() / L"launchmate-self-update.cmd";
    std::ofstream script(scriptPath, std::ios::binary | std::ios::trunc);
    if (!script)
    {
        errorMessage = L"Could not create the temporary update helper.";
        return false;
    }

    script << "@echo off\r\n";
    script << "setlocal\r\n";
    script << "set \"TARGET=%~1\"\r\n";
    script << "set \"UPDATE=%~2\"\r\n";
    script << "set \"PID=%~3\"\r\n";
    script << ":wait\r\n";
    script << "tasklist /FI \"PID eq %PID%\" | find \"%PID%\" >nul\r\n";
    script << "if not errorlevel 1 (\r\n";
    script << "    timeout /t 1 /nobreak >nul\r\n";
    script << "    goto wait\r\n";
    script << ")\r\n";
    script << "copy /Y \"%UPDATE%\" \"%TARGET%\" >nul\r\n";
    script << "rem Start LaunchMate even if the copy failed, so the previous version keeps running.\r\n";
    script << "start \"\" \"%TARGET%\"\r\n";
    script << "del /Q \"%UPDATE%\" >nul 2>nul\r\n";
    script << "del /Q \"%~f0\" >nul 2>nul\r\n";
    script << "endlocal\r\n";

    script.close(); // The helper must be complete and unlocked before it is launched.
    if (!script.good())
    {
        errorMessage = L"Could not finalize the temporary update helper.";
        return false;
    }

    // The outer quotes keep cmd /c from stripping the quotes of the first argument.
    const std::wstring commandLine = L"\"C:\\Windows\\System32\\cmd.exe\" /c \"\"" + scriptPath.wstring() + L"\" \"" +
        currentExecutablePath.wstring() + L"\" \"" + downloadedPath.wstring() + L"\" " + std::to_wstring(processId) + L"\"";
    std::vector<wchar_t> commandBuffer(commandLine.begin(), commandLine.end());
    commandBuffer.push_back(L'\0');

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInformation{};
    if (!CreateProcessW(nullptr, commandBuffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | NORMAL_PRIORITY_CLASS, nullptr, nullptr, &startupInfo, &processInformation))
    {
        errorMessage = L"Could not launch the temporary update helper.";
        return false;
    }

    CloseHandle(processInformation.hThread);
    CloseHandle(processInformation.hProcess);
    return true;
}

bool UpdateChecker::OpenReleasePage(const std::wstring& releasePageUrl)
{
    if (releasePageUrl.empty())
    {
        return false;
    }

    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", releasePageUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}
