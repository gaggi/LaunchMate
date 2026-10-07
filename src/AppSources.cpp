#include "AppSources.h"

#include "CatalogPaths.h"
#include "DetectedProcessCatalog.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>
#include <TlHelp32.h>
#include <objbase.h>
#include <psapi.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace
{
    bool IsProtectedProcessName(const std::wstring& processName)
    {
        constexpr const wchar_t* protectedNames[] = {
            L"System", L"Registry", L"smss.exe", L"csrss.exe", L"wininit.exe",
            L"services.exe", L"lsass.exe", L"winlogon.exe", L"fontdrvhost.exe",
            L"svchost.exe", L"dwm.exe", L"Secure System", L"Memory Compression"
        };
        return std::any_of(std::begin(protectedNames), std::end(protectedNames), [&processName](const wchar_t* name)
        {
            return _wcsicmp(processName.c_str(), name) == 0;
        });
    }

    std::wstring QueryProcessPath(HANDLE process)
    {
        std::wstring path(1024, L'\0');
        DWORD length = static_cast<DWORD>(path.size());
        if (!QueryFullProcessImageNameW(process, 0, path.data(), &length)) length = 0;
        path.resize(length);
        return path;
    }

    unsigned long long FileTimeValue(const FILETIME& value)
    {
        ULARGE_INTEGER result{};
        result.LowPart = value.dwLowDateTime;
        result.HighPart = value.dwHighDateTime;
        return result.QuadPart;
    }

    std::wstring ToLowerCopy(std::wstring text)
    {
        for (auto& character : text)
        {
            character = static_cast<wchar_t>(::towlower(character));
        }
        return text;
    }

    std::wstring ExpandEnvironmentPath(const std::wstring& path)
    {
        if (path.empty())
        {
            return {};
        }

        const DWORD requiredSize = ExpandEnvironmentStringsW(path.c_str(), nullptr, 0);
        if (requiredSize == 0)
        {
            return path;
        }

        std::wstring expanded(requiredSize, L'\0');
        const DWORD copiedSize = ExpandEnvironmentStringsW(path.c_str(), expanded.data(), requiredSize);
        if (copiedSize == 0 || copiedSize > expanded.size())
        {
            return path;
        }

        if (!expanded.empty() && expanded.back() == L'\0')
        {
            expanded.pop_back();
        }

        return expanded;
    }

    struct InstalledAppRecord
    {
        std::wstring name;
        std::wstring location;
        std::wstring icon;
    };

    struct AppPathRecord
    {
        std::wstring executableName;
        std::wstring executablePath;
    };

    struct StartMenuProgram
    {
        std::wstring name;
        std::wstring path;
    };

    std::wstring NormalizeProgramName(const std::wstring& value)
    {
        std::wstring normalized;
        for (const wchar_t character : value)
            if (std::iswalnum(character)) normalized += std::towlower(character);
        return normalized;
    }

    int ProgramNameScore(const std::wstring& candidate, const std::wstring& displayName)
    {
        const auto left = NormalizeProgramName(candidate);
        const auto right = NormalizeProgramName(displayName);
        if (left.empty() || right.empty()) return 0;
        if (left == right) return 100;
        if (left.find(right) != std::wstring::npos || right.find(left) != std::wstring::npos) return 60;
        return 0;
    }

    std::wstring ResolveShortcutTarget(const std::filesystem::path& shortcut)
    {
        IShellLinkW* link = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&link)))) return {};
        IPersistFile* persist = nullptr;
        const HRESULT queried = link->QueryInterface(IID_PPV_ARGS(&persist));
        if (FAILED(queried)) { link->Release(); return {}; }
        const HRESULT loaded = persist->Load(shortcut.c_str(), STGM_READ);
        persist->Release();
        if (FAILED(loaded)) { link->Release(); return {}; }
        wchar_t target[32768]{};
        const HRESULT resolved = link->GetPath(target, static_cast<int>(std::size(target)), nullptr, SLGP_RAWPATH);
        link->Release();
        std::error_code error;
        return SUCCEEDED(resolved) && std::filesystem::is_regular_file(target, error) ? target : std::wstring{};
    }

    std::vector<StartMenuProgram> EnumerateStartMenuPrograms(const std::atomic_bool& cancelled)
    {
        std::vector<StartMenuProgram> programs;
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return programs;
        for (const KNOWNFOLDERID folder : {FOLDERID_Programs, FOLDERID_CommonPrograms})
        {
            PWSTR root = nullptr;
            if (FAILED(SHGetKnownFolderPath(folder, 0, nullptr, &root))) continue;
            const std::filesystem::path directory(root);
            CoTaskMemFree(root);
            std::error_code error;
            for (std::filesystem::recursive_directory_iterator it(directory,
                    std::filesystem::directory_options::skip_permission_denied, error), end;
                !error && it != end; it.increment(error))
            {
                if (cancelled) break;
                if (!it->is_regular_file(error) || _wcsicmp(it->path().extension().c_str(), L".lnk") != 0) continue;
                const auto target = ResolveShortcutTarget(it->path());
                if (!target.empty()) programs.push_back({it->path().stem().wstring(), target});
            }
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
        return programs;
    }

    std::wstring ReadRegistryText(HKEY key, const wchar_t* valueName)
    {
        wchar_t buffer[2048]{};
        DWORD size = sizeof(buffer);
        if (RegGetValueW(key, nullptr, valueName, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            nullptr, buffer, &size) != ERROR_SUCCESS) return {};
        return ExpandEnvironmentPath(buffer);
    }

    std::wstring ExtractExecutablePath(std::wstring value)
    {
        while (!value.empty() && std::iswspace(value.front())) value.erase(value.begin());
        while (!value.empty() && std::iswspace(value.back())) value.pop_back();
        if (value.empty()) return {};
        if (value.front() == L'\"')
        {
            const auto quote = value.find(L'\"', 1);
            value = quote == std::wstring::npos ? std::wstring{} : value.substr(1, quote - 1);
        }
        else if (const auto comma = value.find_last_of(L','); comma != std::wstring::npos)
        {
            const auto suffix = value.substr(comma + 1);
            const bool iconIndex = !suffix.empty() && std::all_of(suffix.begin(), suffix.end(), [](wchar_t ch)
            {
                return std::iswspace(ch) || ch == L'-' || std::iswdigit(ch);
            });
            if (iconIndex) value.resize(comma);
        }
        std::error_code error;
        const auto path = ExpandEnvironmentPath(value);
        return _wcsicmp(std::filesystem::path(path).extension().c_str(), L".exe") == 0 &&
            std::filesystem::is_regular_file(path, error) ? path : std::wstring{};
    }

    std::vector<InstalledAppRecord> EnumerateInstalledApps()
    {
        std::vector<InstalledAppRecord> apps;
        constexpr const wchar_t* uninstall = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
        for (const auto& [root, view] : {
            std::pair{HKEY_CURRENT_USER, REGSAM(0)},
            std::pair{HKEY_LOCAL_MACHINE, REGSAM(KEY_WOW64_64KEY)},
            std::pair{HKEY_LOCAL_MACHINE, REGSAM(KEY_WOW64_32KEY)}})
        {
            HKEY parent{};
            if (RegOpenKeyExW(root, uninstall, 0, KEY_ENUMERATE_SUB_KEYS | view, &parent) != ERROR_SUCCESS) continue;
            for (DWORD index = 0;; ++index)
            {
                wchar_t name[512]{};
                DWORD length = static_cast<DWORD>(std::size(name));
                const LONG result = RegEnumKeyExW(parent, index, name, &length, nullptr, nullptr, nullptr, nullptr);
                if (result == ERROR_NO_MORE_ITEMS) break;
                if (result != ERROR_SUCCESS) continue;
                HKEY item{};
                if (RegOpenKeyExW(parent, name, 0, KEY_QUERY_VALUE | view, &item) != ERROR_SUCCESS) continue;
                InstalledAppRecord app{ReadRegistryText(item, L"DisplayName"),
                    ReadRegistryText(item, L"InstallLocation"), ReadRegistryText(item, L"DisplayIcon")};
                RegCloseKey(item);
                if (!app.name.empty()) apps.push_back(std::move(app));
            }
            RegCloseKey(parent);
        }
        return apps;
    }

    std::vector<AppPathRecord> EnumerateAppPaths()
    {
        std::vector<AppPathRecord> paths;
        constexpr const wchar_t* appPaths = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths";
        for (const auto& [root, view] : {
            std::pair{HKEY_CURRENT_USER, REGSAM(0)},
            std::pair{HKEY_LOCAL_MACHINE, REGSAM(KEY_WOW64_64KEY)},
            std::pair{HKEY_LOCAL_MACHINE, REGSAM(KEY_WOW64_32KEY)}})
        {
            HKEY parent{};
            if (RegOpenKeyExW(root, appPaths, 0, KEY_ENUMERATE_SUB_KEYS | view, &parent) != ERROR_SUCCESS) continue;
            for (DWORD index = 0;; ++index)
            {
                wchar_t name[512]{};
                DWORD length = static_cast<DWORD>(std::size(name));
                const LONG result = RegEnumKeyExW(parent, index, name, &length, nullptr, nullptr, nullptr, nullptr);
                if (result == ERROR_NO_MORE_ITEMS) break;
                if (result != ERROR_SUCCESS) continue;
                HKEY item{};
                if (RegOpenKeyExW(parent, name, 0, KEY_QUERY_VALUE | view, &item) != ERROR_SUCCESS) continue;
                const auto path = ExtractExecutablePath(ReadRegistryText(item, nullptr));
                RegCloseKey(item);
                if (!path.empty()) paths.push_back({name, path});
            }
            RegCloseKey(parent);
        }
        return paths;
    }

    std::wstring InstalledExecutablePath(const std::vector<InstalledAppRecord>& apps,
        const DetectedProcessCandidate& candidate)
    {
        for (const auto& app : apps)
        {
            if (!ContainsInsensitive(app.name, candidate.name)) continue;
            std::error_code error;
            if (!app.location.empty())
            {
                const auto path = std::filesystem::path(app.location) / candidate.executable;
                if (std::filesystem::is_regular_file(path, error)) return path.wstring();
            }
            const auto icon = ExtractExecutablePath(app.icon);
            if (_wcsicmp(std::filesystem::path(icon).filename().c_str(), candidate.executable) == 0 &&
                std::filesystem::is_regular_file(icon, error)) return icon;
        }
        return {};
    }

    std::wstring InstalledAppExecutablePath(const InstalledAppRecord& app,
        const std::vector<StartMenuProgram>& startMenuPrograms, const std::vector<AppPathRecord>& appPaths)
    {
        const auto icon = ExtractExecutablePath(app.icon);
        std::error_code error;
        if (!icon.empty() && ProgramNameScore(std::filesystem::path(icon).stem().wstring(), app.name) >= 60)
            return icon;

        int bestScore = 0;
        std::wstring bestPath;
        for (const auto& program : startMenuPrograms)
        {
            int score = ProgramNameScore(program.name, app.name);
            if (!app.location.empty() && ToLowerCopy(program.path).starts_with(ToLowerCopy(app.location))) score += 50;
            if (score > bestScore) { bestScore = score; bestPath = program.path; }
        }
        if (bestScore >= 60) return bestPath;

        for (const auto& appPath : appPaths)
        {
            int score = ProgramNameScore(appPath.executableName, app.name);
            if (!app.location.empty() && ToLowerCopy(appPath.executablePath).starts_with(ToLowerCopy(app.location))) score += 50;
            if (score > bestScore) { bestScore = score; bestPath = appPath.executablePath; }
        }
        if (bestScore >= 60) return bestPath;

        if (app.location.empty() || !std::filesystem::is_directory(app.location, error)) return {};
        for (std::filesystem::recursive_directory_iterator it(app.location,
                std::filesystem::directory_options::skip_permission_denied, error), end;
            !error && it != end; it.increment(error))
        {
            if (it.depth() > 3) { it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file(error) || _wcsicmp(it->path().extension().c_str(), L".exe") != 0) continue;
            const auto stem = it->path().stem().wstring();
            int score = ProgramNameScore(stem, app.name);
            const auto lower = ToLowerCopy(stem);
            if (lower.find(L"unins") != std::wstring::npos || lower.find(L"uninstall") != std::wstring::npos ||
                lower.find(L"updat") != std::wstring::npos || lower.find(L"setup") != std::wstring::npos) continue;
            if (score > bestScore) { bestScore = score; bestPath = it->path().wstring(); }
        }
        return bestScore >= 60 ? bestPath : std::wstring{};
    }

    bool TryAppendCatalogProgram(
        std::vector<CatalogProgram>& programs,
        std::unordered_set<std::wstring>& seenPaths,
        const std::wstring& displayName,
        const std::wstring& filePath,
        bool manuallyAdded = false)
    {
        if (displayName.empty() || filePath.empty())
        {
            return false;
        }

        const auto key = filePath.empty()
            ? L"name:" + ToLowerCopy(displayName)
            : L"path:" + ToLowerCopy(filePath);
        if (!seenPaths.insert(key).second)
        {
            return false;
        }

        programs.push_back({displayName, filePath, manuallyAdded});
        return true;
    }

}

bool ContainsInsensitive(const std::wstring& haystack, const std::wstring& needle)
{
    if (needle.empty()) return true;
    return ToLowerCopy(haystack).find(ToLowerCopy(needle)) != std::wstring::npos;
}

std::vector<CatalogProgram> FindInstalledApps(const std::vector<CatalogProgram>& catalog, const std::atomic_bool& cancelled)
{
    std::vector<CatalogProgram> programs;
    std::unordered_set<std::wstring> seen;
    const auto startMenuPrograms = EnumerateStartMenuPrograms(cancelled);
    const auto appPaths = EnumerateAppPaths();
    if (cancelled) return programs;
    for (const auto& program : catalog)
    {
        if (cancelled) return programs;
        std::error_code error;
        if (program.manuallyAdded && !program.filePath.empty() && std::filesystem::exists(program.filePath, error))
            TryAppendCatalogProgram(programs, seen, program.displayName, program.filePath, true);
    }
    for (const auto& candidate : kCatalogPathCandidates)
    {
        if (cancelled) return programs;
        const auto path = ExpandEnvironmentPath(candidate.path);
        std::error_code error;
        if (!path.empty() && std::filesystem::exists(path, error))
            TryAppendCatalogProgram(programs, seen, candidate.displayName, path);
    }
    for (const auto& app : EnumerateInstalledApps())
    {
        if (cancelled) return programs;
        TryAppendCatalogProgram(programs, seen, app.name,
            InstalledAppExecutablePath(app, startMenuPrograms, appPaths));
    }
    std::sort(programs.begin(), programs.end(), [](const CatalogProgram& left, const CatalogProgram& right)
    {
        return _wcsicmp(left.displayName.c_str(), right.displayName.c_str()) < 0;
    });
    return programs;
}

std::vector<RunningProcessEntry> CaptureRunningProcesses(const std::wstring& watchedProcessName, const std::atomic_bool& cancelled)
{
    std::vector<RunningProcessEntry> runningProcesses_;
    std::unordered_map<std::wstring, size_t> seenProcesses;
    seenProcesses.reserve(128);
    if (runningProcesses_.capacity() < 128) runningProcesses_.reserve(128);

    struct CpuSample
    {
        HANDLE process{};
        size_t processIndex{};
        unsigned long long initialTime{};
        LARGE_INTEGER sampleStart{};
    };
    std::vector<CpuSample> cpuSamples;
    cpuSamples.reserve(128);

    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return runningProcesses_;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (cancelled) break;
            const std::wstring processName = entry.szExeFile;
            if (entry.th32ProcessID == 0 || entry.th32ProcessID == GetCurrentProcessId() ||
                IsProtectedProcessName(processName) ||
                (!watchedProcessName.empty() && _wcsicmp(
                    std::filesystem::path(processName).stem().c_str(),
                    std::filesystem::path(watchedProcessName).stem().c_str()) == 0))
            {
                continue;
            }

            HANDLE process = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                FALSE,
                entry.th32ProcessID);
            if (!process)
            {
                process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            }

            const std::wstring path = process ? QueryProcessPath(process) : std::wstring{};
            std::wstring key = processName + L"|" + path;
            std::transform(key.begin(), key.end(), key.begin(), [](wchar_t character)
            {
                return static_cast<wchar_t>(std::towlower(character));
            });
            RunningProcessEntry item;
            item.displayName = std::filesystem::path(processName).stem().wstring();
            item.processName = processName;
            item.executablePath = path;
            item.processId = entry.th32ProcessID;

            if (process)
            {
                PROCESS_MEMORY_COUNTERS memory{};
                if (K32GetProcessMemoryInfo(process, &memory, sizeof(memory)))
                {
                    item.memoryUsageBytes = memory.WorkingSetSize;
                    item.hasMemoryUsage = true;
                }
            }

            const auto [existing, inserted] = seenProcesses.emplace(key, runningProcesses_.size());
            const size_t processIndex = existing->second;
            if (inserted) runningProcesses_.push_back(std::move(item));
            else
            {
                auto& combined = runningProcesses_[processIndex];
                combined.memoryUsageBytes += item.memoryUsageBytes;
                combined.hasMemoryUsage = combined.hasMemoryUsage || item.hasMemoryUsage;
            }

            if (process)
            {
                FILETIME creation{};
                FILETIME exit{};
                FILETIME kernel{};
                FILETIME user{};
                if (GetProcessTimes(process, &creation, &exit, &kernel, &user))
                {
                    LARGE_INTEGER sampleStart{};
                    QueryPerformanceCounter(&sampleStart);
                    cpuSamples.push_back({
                        process,
                        processIndex,
                        FileTimeValue(kernel) + FileTimeValue(user),
                        sampleStart});
                }
                else
                {
                    CloseHandle(process);
                }
            }
        }
        while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    if (!cpuSamples.empty()) Sleep(250);

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const double processorCount = std::max<DWORD>(1, systemInfo.dwNumberOfProcessors);

    for (const auto& sample : cpuSamples)
    {
        FILETIME creation{};
        FILETIME exit{};
        FILETIME kernel{};
        FILETIME user{};
        if (GetProcessTimes(sample.process, &creation, &exit, &kernel, &user))
        {
            LARGE_INTEGER sampleEnd{};
            QueryPerformanceCounter(&sampleEnd);
            const double elapsedSeconds = frequency.QuadPart > 0
                ? static_cast<double>(sampleEnd.QuadPart - sample.sampleStart.QuadPart) /
                    static_cast<double>(frequency.QuadPart)
                : 0.0;
            const unsigned long long finalTime = FileTimeValue(kernel) + FileTimeValue(user);
            if (elapsedSeconds > 0.0 && finalTime >= sample.initialTime)
            {
                auto& item = runningProcesses_[sample.processIndex];
                const double processSeconds = static_cast<double>(finalTime - sample.initialTime) / 10000000.0;
                item.cpuUsagePercent += std::clamp(
                    (processSeconds / elapsedSeconds / processorCount) * 100.0,
                    0.0,
                    100.0);
                item.hasCpuUsage = true;
            }
        }
        CloseHandle(sample.process);
    }

    std::sort(runningProcesses_.begin(), runningProcesses_.end(), [](const auto& left, const auto& right)
    {
        return _wcsicmp(left.displayName.c_str(), right.displayName.c_str()) < 0;
    });
    return runningProcesses_;
}

std::vector<DetectedProcessEntry> CaptureBackgroundApps(const std::wstring& watchedProcessName, const std::atomic_bool& cancelled)
{
    std::vector<RunningProcessEntry> runningProcesses_;
    std::vector<DetectedProcessEntry> detectedProcesses_;
    struct Measurements { double cpuTotal{}; unsigned count{}; unsigned long long memory{}; };
    std::unordered_map<std::wstring, Measurements> measurements;
    // Three separate process-time intervals reduce the chance of rating a single idle instant.
    for (int sample = 0; sample < 3; ++sample)
    {
        if (cancelled) return {};
        runningProcesses_ = CaptureRunningProcesses(watchedProcessName, cancelled);
        for (const auto& process : runningProcesses_)
        {
            std::wstring key = process.processName + L"|" + process.executablePath;
            std::transform(key.begin(), key.end(), key.begin(), ::towlower);
            auto& value = measurements[key];
            if (process.hasCpuUsage) { value.cpuTotal += process.cpuUsagePercent; ++value.count; }
            if (process.hasMemoryUsage) value.memory = process.memoryUsageBytes;
        }
    }

    if (cancelled) return {};
    const auto installedApps = EnumerateInstalledApps();
    std::unordered_set<std::wstring> foundNames;
    for (const auto& candidate : kDetectedProcessCandidates)
    {
        if (cancelled) return {};
        const auto running = std::find_if(runningProcesses_.begin(), runningProcesses_.end(), [&candidate](const RunningProcessEntry& item)
        {
            return _wcsicmp(item.processName.c_str(), candidate.executable) == 0;
        });
        std::wstring path = running == runningProcesses_.end() ? L"" : running->executablePath;
        if (path.empty())
        {
            for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE})
            {
                const std::wstring key = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") + candidate.executable;
                wchar_t registryPath[2048]{};
                DWORD size = sizeof(registryPath);
                if (RegGetValueW(root, key.c_str(), nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                    nullptr, registryPath, &size) == ERROR_SUCCESS)
                {
                    std::error_code error;
                    const auto expanded = ExpandEnvironmentPath(registryPath);
                    if (std::filesystem::is_regular_file(expanded, error)) { path = expanded; break; }
                }
            }
        }
        if (path.empty() && candidate.fallbackPath[0] != L'\0')
        {
            const auto fallback = ExpandEnvironmentPath(candidate.fallbackPath);
            std::error_code error;
            if (std::filesystem::is_regular_file(fallback, error)) path = fallback;
        }
        if (path.empty()) path = InstalledExecutablePath(installedApps, candidate);
        if (running == runningProcesses_.end() && path.empty()) continue;
        std::wstring nameKey = candidate.name;
        std::transform(nameKey.begin(), nameKey.end(), nameKey.begin(), ::towlower);
        if (!foundNames.insert(nameKey).second) continue;

        DetectedProcessEntry item;
        item.displayName = candidate.name;
        item.processName = candidate.executable;
        item.executablePath = path;
        item.running = running != runningProcesses_.end();
        item.verifiedRunningPath = item.running && !running->executablePath.empty();
        item.allowStop = candidate.allowStop;
        item.category = candidate.category;
        item.effect = L"Unknown - no measurements";
        if (!item.allowStop) item.effect = L"Unknown - VPN connection";
        else if (item.running)
        {
            std::wstring processKey = running->processName + L"|" + running->executablePath;
            std::transform(processKey.begin(), processKey.end(), processKey.begin(), ::towlower);
            const auto measurement = measurements.find(processKey);
            if (measurement != measurements.end() && measurement->second.count >= 2)
            {
                const double cpu = measurement->second.cpuTotal / measurement->second.count;
                if (cpu >= 3.0) item.effect = L"High - CPU active";
                else if (cpu >= 0.5) item.effect = L"Medium - CPU active";
                else if (cpu >= 0.1 && std::wstring(candidate.category) == L"Overlay") item.effect = L"Medium - overlay active";
                else item.effect = L"Low - CPU currently quiet";
            }
        }
        detectedProcesses_.push_back(std::move(item));
    }
    return detectedProcesses_;
}

std::vector<std::wstring> RunningProcessNames()
{
    std::vector<std::wstring> names;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return names;
    PROCESSENTRY32W entry{sizeof(entry)};
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (entry.th32ProcessID != 0 && !IsProtectedProcessName(entry.szExeFile)) names.emplace_back(entry.szExeFile);
        }
        while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    std::sort(names.begin(), names.end(), [](const auto& left, const auto& right) { return _wcsicmp(left.c_str(), right.c_str()) < 0; });
    names.erase(std::unique(names.begin(), names.end(), [](const auto& left, const auto& right)
    {
        return _wcsicmp(left.c_str(), right.c_str()) == 0;
    }), names.end());
    return names;
}
