#include "UpdateChecker.h"

#include "JsonLite.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>
#include <limits>
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

    std::wstring ToWide(const std::string& text)
    {
        if (text.empty()) return {};
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (count <= 0) return {};
        std::wstring result(count, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count);
        return result;
    }

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
                const int digit = normalized[index] - L'0';
                if (value > ((std::numeric_limits<int>::max)() - digit) / 10)
                    return {};
                value = (value * 10) + digit;
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

    bool NewerVersion(std::wstring_view candidate, std::wstring_view current)
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

        if (components.nScheme != INTERNET_SCHEME_HTTPS)
        {
            errorMessage = L"The update URL must use HTTPS.";
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

        HANDLE file = CreateFileW(outputPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
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
                if (CloseHandle(file)) return true;
                file = INVALID_HANDLE_VALUE;
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

            DWORD writtenBytes = 0;
            if (!WriteFile(file, buffer.data(), downloadedBytes, &writtenBytes, nullptr) || writtenBytes != downloadedBytes)
            {
                errorMessage = L"Could not write the temporary update file.";
                break;
            }
        }

        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
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
        wchar_t modulePath[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
        return length == 0 || length >= std::size(modulePath) ? std::filesystem::path{} : std::filesystem::path(modulePath);
    }

    // The helper runs with LaunchMate's rights; in a protected folder such as Program Files
    // the swap would fail after LaunchMate had already exited.
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

    void StartProgram(const std::filesystem::path& target)
    {
        std::wstring command = L"\"" + target.wstring() + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (CreateProcessW(target.c_str(), command.data(), nullptr, nullptr, FALSE,
            NORMAL_PRIORITY_CLASS, nullptr, target.parent_path().c_str(), &startup, &process))
        {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
    }
}

bool UpdateChecker::IsNewerVersion(std::wstring_view candidate, std::wstring_view current)
{
    return NewerVersion(candidate, current);
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

    if (statusCode == 404)
    {
        result.state = UpdateCheckState::UpToDate;
        result.message = L"No published release is available yet.";
        return result;
    }
    if (statusCode != 200)
    {
        result.message = L"GitHub returned HTTP " + std::to_wstring(statusCode) + L" while checking for updates.";
        return result;
    }

    return ParseReleaseMetadata(std::string(responseBody.begin(), responseBody.end()));
}

UpdateCheckResult UpdateChecker::ParseReleaseMetadata(const std::string& metadata)
{
    UpdateCheckResult result;
    try
    {
        const auto root = jsonlite::Parse(metadata);
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
        if (ParseVersion(tagName).numbers.empty())
        {
            result.message = L"The GitHub release has an invalid version tag.";
            return result;
        }
        if (!NewerVersion(tagName, CurrentVersion()))
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

    const auto currentExecutable = CurrentExecutablePath();
    if (currentExecutable.empty() || !CanReplaceExecutable(currentExecutable))
    {
        errorMessage = L"LaunchMate cannot replace itself in \"" + currentExecutable.parent_path().wstring() +
            L"\" without administrator rights. Please download the new version from the release page.";
        return false;
    }

    const auto updateDirectory = std::filesystem::temp_directory_path() / L"LaunchMate-updates" / std::to_wstring(GetCurrentProcessId());
    std::filesystem::create_directories(updateDirectory);
    downloadedPath = updateDirectory / L"pending-update.exe";

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

    // Never hand anything but a Windows executable to the self-updater.
    char signature[2]{};
    DWORD read = 0;
    HANDLE file = CreateFileW(downloadedPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool executable = file != INVALID_HANDLE_VALUE && ReadFile(file, signature, sizeof(signature), &read, nullptr) &&
        read == sizeof(signature) && signature[0] == 'M' && signature[1] == 'Z';
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!executable)
    {
        std::error_code removeError;
        std::filesystem::remove(downloadedPath, removeError);
        errorMessage = L"The downloaded update is not a valid Windows executable.";
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

bool UpdateChecker::OpenReleasePage(const std::wstring& releasePageUrl)
{
    if (releasePageUrl.empty())
    {
        return false;
    }

    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", releasePageUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

bool UpdateChecker::LaunchSelfUpdater(const std::filesystem::path& downloadedPath, DWORD processId, std::wstring& errorMessage)
{
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (!length || length >= std::size(executable))
    {
        errorMessage = L"Could not determine the LaunchMate executable path.";
        return false;
    }
    const auto helper = downloadedPath.parent_path() / L"update-helper.exe";
    if (!CopyFileW(executable, helper.c_str(), FALSE))
    {
        errorMessage = L"Could not prepare the update helper.";
        return false;
    }
    std::wstring command = L"\"" + helper.wstring() + L"\" --apply-update " + std::to_wstring(processId) +
        L" \"" + executable + L"\" \"" + downloadedPath.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | NORMAL_PRIORITY_CLASS, nullptr, nullptr, &startup, &process))
    {
        errorMessage = L"Could not launch the update helper.";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool UpdateChecker::ApplyUpdate(DWORD processId, const std::filesystem::path& target, const std::filesystem::path& download,
    std::wstring& errorMessage, int attempts)
{
    HANDLE previous = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (previous)
    {
        const DWORD waitResult = WaitForSingleObject(previous, 120000);
        CloseHandle(previous);
        if (waitResult != WAIT_OBJECT_0)
        {
            errorMessage = L"The running LaunchMate did not exit, so it was not updated.";
            return false;
        }
    }
    else if (GetLastError() != ERROR_INVALID_PARAMETER)
    {
        errorMessage = L"Could not wait for the running LaunchMate to exit.";
        return false;
    }

    // Stage alongside the target, then replace it in one step: the old executable stays
    // whole until the new one is complete. A virus scanner may hold either file briefly.
    const auto staged = target.parent_path() / (target.filename().wstring() + L".update-" + std::to_wstring(processId));
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        if (attempt > 0) Sleep(500);
        if (CopyFileW(download.c_str(), staged.c_str(), FALSE) &&
            MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
    }
    DeleteFileW(staged.c_str());
    errorMessage = L"The update could not replace LaunchMate. The existing version was kept.\n"
        L"The downloaded update is still in the temporary LaunchMate-updates folder.";
    return false;
}

int UpdateChecker::HandleSelfUpdateCommandLine()
{
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return -1;
    if (count < 2 || std::wstring_view(arguments[1]) != L"--apply-update")
    {
        LocalFree(arguments);
        return -1;
    }
    if (count != 5) { LocalFree(arguments); return 1; }
    wchar_t* end = nullptr;
    const DWORD processId = wcstoul(arguments[2], &end, 10);
    const bool valid = processId != 0 && end && *end == L'\0';
    const std::filesystem::path target(arguments[3]);
    const std::filesystem::path download(arguments[4]);
    LocalFree(arguments);
    if (!valid) return 1;

    std::wstring errorMessage;
    if (!ApplyUpdate(processId, target, download, errorMessage))
    {
        MessageBoxW(nullptr, errorMessage.c_str(), L"LaunchMate Update", MB_OK | MB_ICONERROR);
        // Keep LaunchMate running: start the version that is still in place.
        StartProgram(target);
        return 1;
    }
    StartProgram(target);
    DeleteFileW(download.c_str());
    return 0;
}
