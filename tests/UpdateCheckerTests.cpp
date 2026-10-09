// Include the implementation to exercise version parsing and the self-update swap
// without network requests. The same file is used by LaunchMate, TPMate and RaceMate.
#include "UpdateChecker.cpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    std::string ReadAll(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    void WriteAll(const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
    }

    bool StagedFileLeft(const std::filesystem::path& folder)
    {
        for (const auto& entry : std::filesystem::directory_iterator(folder))
            if (entry.path().filename().wstring().find(L".update-") != std::wstring::npos) return true;
        return false;
    }

    // Starts this test program with --wait, so ApplyUpdate has a process to wait for.
    PROCESS_INFORMATION StartWaitingProcess()
    {
        wchar_t self[32768]{};
        GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
        std::wstring command = L"\"" + std::wstring(self) + L"\" --wait";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        Require(CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE,
            "Start a process to wait for");
        return process;
    }

    // An id whose process has exited, so ApplyUpdate does not wait.
    DWORD FinishedProcessId()
    {
        const auto process = StartWaitingProcess();
        WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return process.dwProcessId;
    }

    HANDLE Lock(const std::filesystem::path& path)
    {
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(file != INVALID_HANDLE_VALUE, "Lock the target");
        return file;
    }

    void VersionTests()
    {
        Require(!UpdateChecker::CurrentVersion().empty(), "Current version");
        Require(UpdateChecker::IsNewerVersion(L"v0.1.1", L"0.1.0"), "Patch update with v prefix");
        Require(UpdateChecker::IsNewerVersion(L"0.10.0", L"0.9.0"), "Numeric version comparison");
        Require(!UpdateChecker::IsNewerVersion(L"0.1.0", L"0.1.0"), "Equal version");
        Require(!UpdateChecker::IsNewerVersion(L"0.0.9", L"0.1.0"), "Older version");
        Require(!UpdateChecker::IsNewerVersion(L"0.1.0-beta", L"0.1.0"), "Prerelease comparison");
        Require(ParseVersion(L"999999999999999999999").numbers.empty(), "Version overflow");
    }

    void ReleaseTests()
    {
        const auto release = UpdateChecker::ParseReleaseMetadata(R"({"tag_name":"v999.0.0","html_url":"https://github.com/gaggi/App/releases/tag/v999.0.0","assets":[)"
            R"({"name":"App-v999.0.0-windows-x86.exe","browser_download_url":"https://github.com/gaggi/App/releases/download/v999.0.0/App-v999.0.0-windows-x86.exe","digest":"sha256:86"},)"
            R"({"name":"App-v999.0.0-windows-x64.exe","browser_download_url":"https://github.com/gaggi/App/releases/download/v999.0.0/App-v999.0.0-windows-x64.exe","digest":"sha256:64"}]})");
        Require(release.state == UpdateCheckState::UpdateAvailable, "New release");
        Require(EndsWithInsensitive(release.release.assetName, PreferredAssetSuffix()), "Matching architecture");
        Require(release.release.assetDigest == (PreferredAssetSuffix() == L"windows-x64.exe" ? L"sha256:64" : L"sha256:86"), "Digest of the matching asset");
        std::string current = "{\"tag_name\":\"v";
        for (const wchar_t c : UpdateChecker::CurrentVersion()) current.push_back(static_cast<char>(c));
        current += "\"}";
        Require(UpdateChecker::ParseReleaseMetadata(current).state == UpdateCheckState::UpToDate, "Current release");
        const auto noAsset = UpdateChecker::ParseReleaseMetadata(R"({"tag_name":"v999.0.0","assets":[]})");
        Require(noAsset.state == UpdateCheckState::UpdateAvailable && noAsset.release.assetDownloadUrl.empty(), "Release without matching asset");
        Require(UpdateChecker::ParseReleaseMetadata("invalid json").state == UpdateCheckState::Failed, "Malformed response");
        Require(UpdateChecker::ParseReleaseMetadata("[]").state == UpdateCheckState::Failed, "Wrong response shape");
        Require(UpdateChecker::ParseReleaseMetadata(R"({"tag_name":"invalid"})").state == UpdateCheckState::Failed, "Invalid tag");
    }

    // The swap on temporary files: the old executable must stay whole whenever it fails.
    void SwapTests()
    {
        const auto folder = std::filesystem::temp_directory_path() / (L"update-tests-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::remove_all(folder);
        std::filesystem::create_directories(folder);
        const auto target = folder / L"App.exe";
        const auto download = folder / L"pending-update.exe";
        // Taken up front: getting it waits for a helper process, which would outlast the short lock below.
        const DWORD finished = FinishedProcessId();
        std::wstring error;

        // Success, after waiting for the running program to exit.
        WriteAll(target, "old version"); WriteAll(download, "new version");
        const auto running = StartWaitingProcess();
        const bool replaced = UpdateChecker::ApplyUpdate(running.dwProcessId, target, download, error);
        const bool exited = WaitForSingleObject(running.hProcess, 0) == WAIT_OBJECT_0;
        CloseHandle(running.hThread); CloseHandle(running.hProcess);
        Require(replaced && exited, "Swap after the program exits");
        Require(ReadAll(target) == "new version" && std::filesystem::exists(download) && !StagedFileLeft(folder), "Swapped content, download kept for the caller");

        // The target stays locked: the old version is kept, and so is the download.
        WriteAll(target, "old version");
        HANDLE locked = Lock(target);
        const bool lockedResult = UpdateChecker::ApplyUpdate(finished, target, download, error, 2);
        CloseHandle(locked);
        Require(!lockedResult && !error.empty(), "Locked target fails");
        Require(ReadAll(target) == "old version" && ReadAll(download) == "new version" && !StagedFileLeft(folder), "Locked target keeps old version and download");

        // A short lock, as by a virus scanner right after exit: a retry succeeds.
        WriteAll(target, "old version");
        locked = Lock(target);
        std::thread release([locked] { Sleep(700); CloseHandle(locked); });
        const bool retried = UpdateChecker::ApplyUpdate(finished, target, download, error);
        release.join();
        Require(retried && ReadAll(target) == "new version" && !StagedFileLeft(folder), "Retry after a short lock");

        // A missing download changes nothing.
        WriteAll(target, "old version");
        std::filesystem::remove(download);
        Require(!UpdateChecker::ApplyUpdate(finished, target, download, error, 2) && ReadAll(target) == "old version", "Missing download keeps old version");

        std::filesystem::remove_all(folder);
    }
}

int main(int argc, char** argv)
{
    const int helperResult = UpdateChecker::HandleSelfUpdateCommandLine();
    if (helperResult >= 0) return helperResult;
    if (argc == 2 && std::string_view(argv[1]) == "--wait") { Sleep(800); return 0; }
    try
    {
        VersionTests();
        ReleaseTests();
        SwapTests();
        std::cout << "Update checks passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
