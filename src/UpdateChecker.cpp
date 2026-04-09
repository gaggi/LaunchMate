#include "UpdateChecker.h"

#include "JsonLite.h"
#include "Utils.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

namespace
{
#ifndef LAUNCHMATE_VERSION
#define LAUNCHMATE_VERSION "0.0.0-dev"
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

    bool HttpGet(const std::wstring& url, std::vector<char>& body, DWORD& statusCode, std::wstring& errorMessage)
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

        UniqueInternetHandle session(WinHttpOpen(L"LaunchMate Update", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        UniqueInternetHandle connection(session ? WinHttpConnect(reinterpret_cast<HINTERNET>(session.get()), host.c_str(), components.nPort, 0) : nullptr);
        UniqueInternetHandle request(connection
            ? WinHttpOpenRequest(reinterpret_cast<HINTERNET>(connection.get()), L"GET", resource.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
            : nullptr);

        if (!session || !connection || !request)
        {
            errorMessage = L"Could not initialize the GitHub update request.";
            return false;
        }

        const wchar_t* headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        if (!WinHttpSendRequest(reinterpret_cast<HINTERNET>(request.get()), headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(reinterpret_cast<HINTERNET>(request.get()), nullptr))
        {
            errorMessage = L"The GitHub update request failed.";
            return false;
        }

        DWORD statusSize = sizeof(statusCode);
        if (!WinHttpQueryHeaders(reinterpret_cast<HINTERNET>(request.get()), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX))
        {
            errorMessage = L"Could not read the GitHub update status code.";
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

    std::wstring EscapeBatchValue(const std::wstring& value)
    {
        std::wstring escaped;
        for (const wchar_t ch : value)
        {
            if (ch == L'%')
            {
                escaped += L"%%";
            }
            else
            {
                escaped.push_back(ch);
            }
        }
        return escaped;
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
        if (!IsNewerVersion(tagName, CurrentVersion()))
        {
            result.state = UpdateCheckState::UpToDate;
            result.message = L"No update available.";
            return result;
        }

        result.state = UpdateCheckState::UpdateAvailable;
        result.release.versionTag = tagName;
        result.release.versionDisplay = NormalizeVersion(tagName);
        result.release.releasePageUrl = ReadWideString(object, "html_url");

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

    DWORD statusCode = 0;
    std::vector<char> responseBody;
    if (!HttpGet(release.assetDownloadUrl, responseBody, statusCode, errorMessage))
    {
        return false;
    }

    if (statusCode != 200)
    {
        errorMessage = L"GitHub returned HTTP " + std::to_wstring(statusCode) + L" while downloading the update.";
        return false;
    }

    const auto updateDirectory = std::filesystem::temp_directory_path() / L"LaunchMate-updates";
    std::filesystem::create_directories(updateDirectory);
    downloadedPath = updateDirectory / (L"pending-" + (release.assetName.empty() ? std::wstring(L"LaunchMate-update.exe") : release.assetName));

    std::ofstream stream(downloadedPath, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        errorMessage = L"Could not create the temporary update file.";
        return false;
    }

    stream.write(responseBody.data(), static_cast<std::streamsize>(responseBody.size()));
    return stream.good();
}

bool UpdateChecker::LaunchSelfUpdater(const std::filesystem::path& downloadedPath, DWORD processId, std::wstring& errorMessage)
{
    wchar_t modulePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
    if (length == 0 || length >= std::size(modulePath))
    {
        errorMessage = L"Could not determine the current LaunchMate executable path.";
        return false;
    }

    const auto currentExecutablePath = std::filesystem::path(modulePath);
    const auto scriptPath = downloadedPath.parent_path() / L"launchmate-self-update.cmd";

    std::wofstream script(scriptPath, std::ios::binary | std::ios::trunc);
    if (!script)
    {
        errorMessage = L"Could not create the temporary update helper.";
        return false;
    }

    script << L"@echo off\r\n";
    script << L"setlocal\r\n";
    script << L"set \"TARGET=" << EscapeBatchValue(currentExecutablePath.wstring()) << L"\"\r\n";
    script << L"set \"UPDATE=" << EscapeBatchValue(downloadedPath.wstring()) << L"\"\r\n";
    script << L"set \"PID=" << processId << L"\"\r\n";
    script << L":wait\r\n";
    script << L"tasklist /FI \"PID eq %PID%\" | find \"%PID%\" >nul\r\n";
    script << L"if not errorlevel 1 (\r\n";
    script << L"    timeout /t 1 /nobreak >nul\r\n";
    script << L"    goto wait\r\n";
    script << L")\r\n";
    script << L"copy /Y \"%UPDATE%\" \"%TARGET%\" >nul\r\n";
    script << L"if errorlevel 1 goto cleanup\r\n";
    script << L"start \"\" \"%TARGET%\"\r\n";
    script << L":cleanup\r\n";
    script << L"del /Q \"%UPDATE%\" >nul 2>nul\r\n";
    script << L"del /Q \"%~f0\" >nul 2>nul\r\n";
    script << L"endlocal\r\n";

    if (!script.good())
    {
        errorMessage = L"Could not finalize the temporary update helper.";
        return false;
    }

    const std::wstring commandLine = L"\"C:\\Windows\\System32\\cmd.exe\" /c \"" + scriptPath.wstring() + L"\"";
    std::vector<wchar_t> commandBuffer(commandLine.begin(), commandLine.end());
    commandBuffer.push_back(L'\0');

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInformation{};
    if (!CreateProcessW(nullptr, commandBuffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInformation))
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
