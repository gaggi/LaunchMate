#include "IRacingPerformance.h"
#include "BackgroundTask.h"
#include "IRacingServices.h"
#include "ListViewHelpers.h"

#include "resource.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>
#include <powrprof.h>
#include <shellapi.h>
#include <shlobj.h>
#include <TlHelp32.h>
#include <wincrypt.h>

namespace
{
    std::wstring GuidText(const GUID& guid)
    {
        wchar_t value[40]{};
        return StringFromGUID2(guid, value, static_cast<int>(std::size(value))) ? value : L"";
    }

    bool ParseGuid(const std::wstring& text, GUID& guid)
    {
        return !text.empty() && CLSIDFromString(text.c_str(), &guid) == S_OK;
    }

    std::filesystem::path IRacingDocuments()
    {
        PWSTR documents = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents))) return {};
        const std::filesystem::path path = std::filesystem::path(documents) / L"iRacing";
        CoTaskMemFree(documents);
        return path;
    }

    std::wstring EscapePowerShell(const std::wstring& value)
    {
        std::wstring escaped;
        for (const wchar_t character : value)
        {
            escaped += character;
            if (character == L'\'') escaped += character;
        }
        return escaped;
    }

    std::wstring EncodePowerShell(const std::wstring& script)
    {
        DWORD count = 0;
        const auto* bytes = reinterpret_cast<const BYTE*>(script.data());
        const DWORD length = static_cast<DWORD>(script.size() * sizeof(wchar_t));
        if (!CryptBinaryToStringW(bytes, length, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &count)) return {};
        std::wstring encoded(count, L'\0');
        if (!CryptBinaryToStringW(bytes, length, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, encoded.data(), &count)) return {};
        encoded.resize(count);
        return encoded;
    }

    std::wstring PowerShellPath()
    {
        wchar_t system[MAX_PATH]{};
        if (!GetSystemDirectoryW(system, MAX_PATH)) return {};
        return (std::filesystem::path(system) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe").wstring();
    }

    bool RunPowerShell(const std::wstring& script, std::string& output)
    {
        const auto executable = PowerShellPath();
        const auto encoded = EncodePowerShell(script);
        if (executable.empty() || encoded.empty()) return false;
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        HANDLE readPipe = nullptr;
        HANDLE writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &attributes, 0)) return false;
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.hStdOutput = writePipe;
        startup.hStdError = writePipe;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION process{};
        std::wstring command = L"\"" + executable + L"\" -NoProfile -NonInteractive -EncodedCommand " + encoded;
        const bool started = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | NORMAL_PRIORITY_CLASS, nullptr, nullptr, &startup, &process) != FALSE;
        CloseHandle(writePipe);
        if (!started) { CloseHandle(readPipe); return false; }
        output.clear();
        const auto deadline = GetTickCount64() + 15000;
        DWORD wait = WAIT_TIMEOUT;
        bool outputOk = true;
        // Drain while the child runs; waiting first can deadlock on a full pipe.
        const auto drain = [&]()
        {
            for (;;)
            {
                DWORD available = 0;
                if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr))
                    return GetLastError() == ERROR_BROKEN_PIPE;
                if (available == 0) return true;
                char buffer[4096];
                DWORD read = 0;
                if (!ReadFile(readPipe, buffer, std::min<DWORD>(available, sizeof(buffer)), &read, nullptr) || read == 0)
                    return false;
                if (output.size() + read > 1024 * 1024 || GetTickCount64() >= deadline) return false;
                output.append(buffer, read);
            }
        };
        while ((outputOk = drain()) && GetTickCount64() < deadline)
        {
            wait = WaitForSingleObject(process.hProcess, 50);
            if (wait != WAIT_TIMEOUT) break;
        }
        if (wait == WAIT_OBJECT_0 && outputOk) outputOk = drain();
        if (wait != WAIT_OBJECT_0 || !outputOk)
        {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 2000);
        }
        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(readPipe);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return outputOk && wait == WAIT_OBJECT_0 && exitCode == 0;
    }

    DWORD RunElevatedPowerShell(const std::wstring& script)
    {
        const auto executable = PowerShellPath();
        const auto encoded = EncodePowerShell(script);
        if (executable.empty() || encoded.empty()) return DWORD(-1);
        const std::wstring parameters = L"-NoProfile -NonInteractive -EncodedCommand " + encoded;
        SHELLEXECUTEINFOW info{sizeof(info)};
        info.fMask = SEE_MASK_NOCLOSEPROCESS;
        info.hwnd = nullptr;
        info.lpVerb = L"runas";
        info.lpFile = executable.c_str();
        info.lpParameters = parameters.c_str();
        info.nShow = SW_HIDE;
        if (!ShellExecuteExW(&info)) return DWORD(-1);
        const DWORD wait = WaitForSingleObject(info.hProcess, 30000);
        DWORD exitCode = 1;
        GetExitCodeProcess(info.hProcess, &exitCode);
        CloseHandle(info.hProcess);
        return wait == WAIT_OBJECT_0 ? exitCode : DWORD(-2);
    }

    std::wstring LowerAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return std::wstring(value.begin(), value.end());
    }

    std::string Trim(std::string value)
    {
        if (value.starts_with("\xEF\xBB\xBF")) value.erase(0, 3);
        const auto first = value.find_first_not_of(" \t\r");
        if (first == std::string::npos) return {};
        const auto last = value.find_last_not_of(" \t\r");
        return value.substr(first, last - first + 1);
    }

    const std::array<std::pair<std::string, std::string>, 3> kIniSettings{{
        {"carPreloadAll", "1"},
        {"trackTexturePreload", "1"},
        {"streamingTextureSize", "256"}}};

    bool ReadIni(const std::filesystem::path& path, std::string& contents)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input) return false;
        contents.assign(std::istreambuf_iterator<char>(input), {});
        if (contents.size() >= 2 && ((static_cast<unsigned char>(contents[0]) == 0xff && static_cast<unsigned char>(contents[1]) == 0xfe) ||
            (static_cast<unsigned char>(contents[0]) == 0xfe && static_cast<unsigned char>(contents[1]) == 0xff))) return false;
        return true;
    }

    std::wstring IniSummary(const std::filesystem::path& path)
    {
        std::string contents;
        if (!ReadIni(path, contents)) return L"app.ini: missing, unreadable, or unsupported encoding";
        bool inGraphics = false;
        std::array<std::string, 3> values{};
        size_t cursor = 0;
        while (cursor < contents.size())
        {
            const size_t end = contents.find('\n', cursor);
            const std::string line = Trim(contents.substr(cursor, end == std::string::npos ? end : end - cursor));
            if (line.starts_with("[") && line.ends_with("]")) inGraphics = LowerAscii(line) == L"[graphics]";
            else if (inGraphics)
            {
                const size_t equal = line.find('=');
                if (equal != std::string::npos)
                {
                    const auto key = LowerAscii(Trim(line.substr(0, equal)));
                    for (size_t index = 0; index < kIniSettings.size(); ++index)
                        if (key == LowerAscii(kIniSettings[index].first)) values[index] = Trim(line.substr(equal + 1));
                }
            }
            if (end == std::string::npos) break;
            cursor = end + 1;
        }
        std::wstring summary = L"app.ini [Graphics]:";
        for (size_t index = 0; index < kIniSettings.size(); ++index)
        {
            summary += L"\r\n  " + std::wstring(kIniSettings[index].first.begin(), kIniSettings[index].first.end()) + L" = ";
            summary += values[index].empty() ? L"missing" : std::wstring(values[index].begin(), values[index].end());
            if (values[index] != kIniSettings[index].second) summary += L" (suggested: " + std::wstring(kIniSettings[index].second.begin(), kIniSettings[index].second.end()) + L")";
        }
        return summary;
    }

    bool ApplyIniSettings(const std::filesystem::path& path, std::wstring& error)
    {
        std::string contents;
        if (!ReadIni(path, contents)) { error = L"The app.ini file is missing, unreadable, or uses UTF-16."; return false; }
        const std::string newline = contents.find("\r\n") == std::string::npos ? "\n" : "\r\n";
        std::vector<std::string> lines;
        size_t cursor = 0;
        while (cursor < contents.size())
        {
            const size_t end = contents.find('\n', cursor);
            std::string line = contents.substr(cursor, end == std::string::npos ? end : end - cursor);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
            if (end == std::string::npos) break;
            cursor = end + 1;
        }
        size_t graphicsStart = lines.size();
        size_t graphicsEnd = lines.size();
        for (size_t index = 0; index < lines.size(); ++index)
        {
            const auto line = Trim(lines[index]);
            if (line.starts_with("[") && line.ends_with("]"))
            {
                if (graphicsStart != lines.size()) { graphicsEnd = index; break; }
                if (LowerAscii(line) == L"[graphics]") graphicsStart = index;
            }
        }
        if (graphicsStart == lines.size()) { error = L"The [Graphics] section was not found in app.ini."; return false; }
        std::array<bool, 3> found{};
        for (size_t index = graphicsStart + 1; index < graphicsEnd; ++index)
        {
            const auto line = Trim(lines[index]);
            const size_t equal = line.find('=');
            if (equal == std::string::npos) continue;
            const auto key = LowerAscii(Trim(line.substr(0, equal)));
            for (size_t setting = 0; setting < kIniSettings.size(); ++setting)
            {
                if (key == LowerAscii(kIniSettings[setting].first))
                {
                    lines[index] = kIniSettings[setting].first + "=" + kIniSettings[setting].second;
                    found[setting] = true;
                }
            }
        }
        for (size_t setting = 0; setting < kIniSettings.size(); ++setting)
            if (!found[setting]) lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(graphicsEnd++), kIniSettings[setting].first + "=" + kIniSettings[setting].second);
        std::string updated;
        for (const auto& line : lines) updated += line + newline;
        if (updated == contents) return true;
        auto backup = path;
        backup += L".launchmate-" + std::to_wstring(GetTickCount64()) + L".bak";
        if (!CopyFileW(path.c_str(), backup.c_str(), TRUE)) { error = L"Could not create a backup of app.ini."; return false; }
        auto temporary = path;
        temporary += L".launchmate-" + std::to_wstring(GetTickCount64()) + L".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(updated.data(), static_cast<std::streamsize>(updated.size()));
            if (!output) { error = L"Could not write the updated app.ini."; return false; }
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            error = L"Could not replace app.ini. The backup was preserved.";
            return false;
        }
        return true;
    }

    bool ProcessRunning(const wchar_t* name)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W entry{sizeof(entry)};
        bool found = false;
        if (Process32FirstW(snapshot, &entry))
            do { if (_wcsicmp(entry.szExeFile, name) == 0) { found = true; break; } } while (Process32NextW(snapshot, &entry));
        CloseHandle(snapshot);
        return found;
    }

    bool InstalledApplication(const wchar_t* name)
    {
        constexpr wchar_t uninstall[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
        for (const HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER})
        {
            for (const REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY})
            {
                HKEY list = nullptr;
                if (RegOpenKeyExW(root, uninstall, 0, KEY_READ | view, &list) != ERROR_SUCCESS) continue;
                for (DWORD index = 0;; ++index)
                {
                    wchar_t subkey[256]{};
                    DWORD length = static_cast<DWORD>(std::size(subkey));
                    if (RegEnumKeyExW(list, index, subkey, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                    HKEY application = nullptr;
                    if (RegOpenKeyExW(list, subkey, 0, KEY_READ | view, &application) != ERROR_SUCCESS) continue;
                    wchar_t displayName[512]{};
                    DWORD bytes = sizeof(displayName);
                    DWORD type = 0;
                    const bool match = RegQueryValueExW(application, L"DisplayName", nullptr, &type,
                        reinterpret_cast<BYTE*>(displayName), &bytes) == ERROR_SUCCESS && type == REG_SZ && bytes < sizeof(displayName) &&
                        std::wstring(displayName).find(name) != std::wstring::npos;
                    RegCloseKey(application);
                    if (match) { RegCloseKey(list); return true; }
                }
                RegCloseKey(list);
            }
        }
        return false;
    }

    bool HasX3DProcessor()
    {
        HKEY processor = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0,
            KEY_QUERY_VALUE, &processor) != ERROR_SUCCESS) return false;
        wchar_t name[256]{};
        DWORD bytes = sizeof(name);
        DWORD type = 0;
        const bool result = RegQueryValueExW(processor, L"ProcessorNameString", nullptr, &type,
            reinterpret_cast<BYTE*>(name), &bytes) == ERROR_SUCCESS && type == REG_SZ && bytes < sizeof(name) &&
            std::wstring(name).find(L"X3D") != std::wstring::npos;
        RegCloseKey(processor);
        return result;
    }

    std::wstring DisplaySummary()
    {
        std::wstring result;
        for (DWORD index = 0;; ++index)
        {
            DISPLAY_DEVICEW device{sizeof(device)};
            if (!EnumDisplayDevicesW(nullptr, index, &device, 0)) break;
            if (!(device.StateFlags & DISPLAY_DEVICE_ACTIVE) || !(device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
            DEVMODEW current{sizeof(current)};
            if (!EnumDisplaySettingsW(device.DeviceName, ENUM_CURRENT_SETTINGS, &current)) continue;
            DWORD maximum = current.dmDisplayFrequency;
            for (DWORD modeIndex = 0;; ++modeIndex)
            {
                DEVMODEW mode{sizeof(mode)};
                if (!EnumDisplaySettingsW(device.DeviceName, modeIndex, &mode)) break;
                if (mode.dmPelsWidth == current.dmPelsWidth && mode.dmPelsHeight == current.dmPelsHeight &&
                    mode.dmBitsPerPel == current.dmBitsPerPel)
                    maximum = std::max(maximum, mode.dmDisplayFrequency);
            }
            result += L"\r\n  " + std::wstring(device.DeviceName) + L": " + std::to_wstring(current.dmDisplayFrequency) + L" Hz";
            if (maximum > current.dmDisplayFrequency) result += L" (" + std::to_wstring(maximum) + L" Hz available at this resolution)";
        }
        return result.empty() ? L"\r\n  unavailable" : result;
    }

    struct DefenderState { bool available{}; bool installExcluded{}; bool documentsExcluded{}; };

    DefenderState ReadDefender(const std::wstring& install, const std::wstring& documents)
    {
        const std::wstring script = L"$ErrorActionPreference='Stop'; $p=@((Get-MpPreference).ExclusionPath); "
            L"if ($p -match '^N/A:|Must be an administrator to view exclusions') { exit 3 }; "
            L"[int]($p -contains '" + EscapePowerShell(install) + L"'); [int]($p -contains '" + EscapePowerShell(documents) + L"')";
        std::string output;
        DefenderState state;
        if (!RunPowerShell(script, output)) return state;
        const size_t first = output.find_first_of("01");
        const size_t second = first == std::string::npos ? first : output.find_first_of("01", first + 1);
        if (second == std::string::npos) return state;
        state.available = true;
        state.installExcluded = output[first] == '1';
        state.documentsExcluded = output[second] == '1';
        return state;
    }

    struct DiagnosticResult
    {
        std::wstring summary;
        std::vector<PowerSchemeInfo> schemes;
        std::filesystem::path documents;
        std::filesystem::path install;
    };

    struct DialogState
    {
        WatchedProcessRule* rule{};
        std::vector<PowerSchemeInfo> schemes;
        std::filesystem::path documents;
        std::filesystem::path install;
        bool defenderVerifiedThisSession{};
        bool loaded{};
        bool refreshAgain{};
        BackgroundTask<DiagnosticResult> refresh;
    };

    std::vector<std::wstring> RunningProcessNames(const WatchedProcessRule* rule = nullptr)
    {
        std::vector<std::wstring> names;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return names;
        PROCESSENTRY32W entry{sizeof(entry)};
        if (Process32FirstW(snapshot, &entry))
        {
            do { names.emplace_back(entry.szExeFile); } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        // These processes are meaningful performance targets even before a
        // session starts: the watched executable itself and every configured
        // Start-program action.  They supplement (rather than replace) the
        // live process list.
        if (rule)
        {
            const auto watched = rule->processName.empty()
                ? std::filesystem::path(rule->executablePath).filename().wstring()
                : std::filesystem::path(rule->processName).filename().wstring();
            if (!watched.empty()) names.push_back(watched);
            for (const auto& program : rule->programsToLaunch)
            {
                const auto name = std::filesystem::path(program.filePath).filename().wstring();
                if (!name.empty()) names.push_back(name);
            }
        }
        std::sort(names.begin(), names.end(), [](const auto& left, const auto& right) { return _wcsicmp(left.c_str(), right.c_str()) < 0; });
        names.erase(std::unique(names.begin(), names.end(), [](const auto& left, const auto& right) { return _wcsicmp(left.c_str(), right.c_str()) == 0; }), names.end());
        return names;
    }

    std::wstring PriorityText(int value)
    {
        switch (value)
        {
        case IDLE_PRIORITY_CLASS: return L"Low";
        case BELOW_NORMAL_PRIORITY_CLASS: return L"Below normal";
        case NORMAL_PRIORITY_CLASS: return L"Normal";
        case ABOVE_NORMAL_PRIORITY_CLASS: return L"Above normal";
        case HIGH_PRIORITY_CLASS: return L"High";
        case REALTIME_PRIORITY_CLASS: return L"Real time";
        default: return L"Do not change";
        }
    }

    std::wstring IoPriorityText(int value)
    {
        switch (value)
        {
        case 0: return L"Very low";
        case 1: return L"Low";
        case 2: return L"Normal";
        case 3: return L"High";
        default: return L"Do not change";
        }
    }

    std::wstring MemoryPriorityText(int value)
    {
        switch (value)
        {
        case 1: return L"Very low";
        case 2: return L"Low";
        case 3: return L"Medium";
        case 4: return L"Below normal";
        case 5: return L"Normal";
        default: return L"Do not change";
        }
    }

    void EnsureDefaultPerformanceActions(WatchedProcessRule& rule)
    {
        const auto addIfMissing = [&](const std::wstring& candidate)
        {
            const auto name = std::filesystem::path(candidate).filename().wstring();
            if (name.empty()) return;
            const bool exists = std::any_of(rule.processPerformanceActions.begin(), rule.processPerformanceActions.end(),
                [&](const ProcessPerformanceAction& action) { return _wcsicmp(action.processName.c_str(), name.c_str()) == 0; });
            if (!exists) rule.processPerformanceActions.push_back({name});
        };

        addIfMissing(rule.processName.empty() ? rule.executablePath : rule.processName);
        for (const auto& program : rule.programsToLaunch) addIfMissing(program.filePath);
    }

    struct PerformanceActionDialogState
    {
        ProcessPerformanceAction* action{};
        const WatchedProcessRule* rule{};
        std::vector<std::wstring> processes;
        bool accepted{};
    };

    void AddChoice(HWND combo, const wchar_t* label, int value, int selectedValue)
    {
        const int index = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label)));
        SendMessageW(combo, CB_SETITEMDATA, index, value);
        if (value == selectedValue) SendMessageW(combo, CB_SETCURSEL, index, 0);
    }

    int ChoiceValue(HWND combo, int fallback)
    {
        const int selected = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
        return selected >= 0 ? static_cast<int>(SendMessageW(combo, CB_GETITEMDATA, selected, 0)) : fallback;
    }

    INT_PTR CALLBACK PerformanceActionProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* state = reinterpret_cast<PerformanceActionDialogState*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
        if (message == WM_INITDIALOG)
        {
            state = reinterpret_cast<PerformanceActionDialogState*>(lParam);
            SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            state->processes = RunningProcessNames(state->rule);
            if (std::none_of(state->processes.begin(), state->processes.end(), [&](const auto& name) { return _wcsicmp(name.c_str(), state->action->processName.c_str()) == 0; }) && !state->action->processName.empty())
                state->processes.push_back(state->action->processName);
            std::sort(state->processes.begin(), state->processes.end(), [](const auto& left, const auto& right) { return _wcsicmp(left.c_str(), right.c_str()) < 0; });
            HWND processCombo = GetDlgItem(dialog, IDC_PERF_PROCESS);
            for (size_t index = 0; index < state->processes.size(); ++index)
            {
                SendMessageW(processCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(state->processes[index].c_str()));
                if (_wcsicmp(state->processes[index].c_str(), state->action->processName.c_str()) == 0)
                    SendMessageW(processCombo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
            }
            HWND priority = GetDlgItem(dialog, IDC_PERF_CPU_PRIORITY);
            AddChoice(priority, L"Do not change", 0, state->action->cpuPriorityClass);
            AddChoice(priority, L"Low", IDLE_PRIORITY_CLASS, state->action->cpuPriorityClass);
            AddChoice(priority, L"Below normal", BELOW_NORMAL_PRIORITY_CLASS, state->action->cpuPriorityClass);
            AddChoice(priority, L"Normal", NORMAL_PRIORITY_CLASS, state->action->cpuPriorityClass);
            AddChoice(priority, L"Above normal", ABOVE_NORMAL_PRIORITY_CLASS, state->action->cpuPriorityClass);
            AddChoice(priority, L"High", HIGH_PRIORITY_CLASS, state->action->cpuPriorityClass);
            AddChoice(priority, L"Real time", REALTIME_PRIORITY_CLASS, state->action->cpuPriorityClass);
            HWND io = GetDlgItem(dialog, IDC_PERF_IO_PRIORITY);
            AddChoice(io, L"Do not change", -1, state->action->ioPriority);
            AddChoice(io, L"Very low", 0, state->action->ioPriority);
            AddChoice(io, L"Low", 1, state->action->ioPriority);
            AddChoice(io, L"Normal", 2, state->action->ioPriority);
            AddChoice(io, L"High", 3, state->action->ioPriority);
            HWND memory = GetDlgItem(dialog, IDC_PERF_MEMORY_PRIORITY);
            AddChoice(memory, L"Do not change", -1, state->action->memoryPriority);
            AddChoice(memory, L"Very low", 1, state->action->memoryPriority);
            AddChoice(memory, L"Low", 2, state->action->memoryPriority);
            AddChoice(memory, L"Medium", 3, state->action->memoryPriority);
            AddChoice(memory, L"Below normal", 4, state->action->memoryPriority);
            AddChoice(memory, L"Normal", 5, state->action->memoryPriority);
            const DWORD count = std::min<DWORD>(GetActiveProcessorCount(0), 64);
            for (DWORD cpu = 0; cpu < count; ++cpu)
            {
                // Keep sibling logical CPUs visually together: evens on the
                // first row, odds directly beneath them.  With 16 CPUs this
                // is exactly the requested two-row layout.
                const int column = static_cast<int>((cpu % 16) / 2);
                const int row = static_cast<int>((cpu % 2) + (cpu / 16) * 2);
                const std::wstring label = L"CPU " + std::to_wstring(cpu);
                RECT position{12 + column * 60, 152 + row * 20, 12 + column * 60 + 56, 152 + row * 20 + 16};
                MapDialogRect(dialog, &position);
                HWND checkbox = CreateWindowExW(0, L"Button", label.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                    position.left, position.top, position.right - position.left, position.bottom - position.top, dialog,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PERF_AFFINITY_FIRST + cpu)), GetModuleHandleW(nullptr), nullptr);
                if (state->action->affinityMask == 0 || (state->action->affinityMask & (std::uint64_t{1} << cpu)) != 0)
                    SendMessageW(checkbox, BM_SETCHECK, BST_CHECKED, 0);
            }
            return TRUE;
        }
        if (message != WM_COMMAND) return FALSE;
        if (LOWORD(wParam) == IDOK)
        {
            const int selected = static_cast<int>(SendDlgItemMessageW(dialog, IDC_PERF_PROCESS, CB_GETCURSEL, 0, 0));
            if (selected < 0 || static_cast<size_t>(selected) >= state->processes.size())
            {
                MessageBoxW(dialog, L"Select a currently running process.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            state->action->processName = state->processes[static_cast<size_t>(selected)];
            state->action->cpuPriorityClass = ChoiceValue(GetDlgItem(dialog, IDC_PERF_CPU_PRIORITY), 0);
            state->action->ioPriority = ChoiceValue(GetDlgItem(dialog, IDC_PERF_IO_PRIORITY), -1);
            state->action->memoryPriority = ChoiceValue(GetDlgItem(dialog, IDC_PERF_MEMORY_PRIORITY), -1);
            const DWORD count = std::min<DWORD>(GetActiveProcessorCount(0), 64);
            std::uint64_t mask = 0;
            for (DWORD cpu = 0; cpu < count; ++cpu)
                if (IsDlgButtonChecked(dialog, IDC_PERF_AFFINITY_FIRST + cpu) == BST_CHECKED) mask |= (std::uint64_t{1} << cpu);
            const std::uint64_t all = count == 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << count) - 1);
            state->action->affinityMask = mask == all ? 0 : mask;
            state->accepted = true;
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
        return FALSE;
    }

    bool EditPerformanceAction(HWND owner, const WatchedProcessRule& rule, ProcessPerformanceAction& action)
    {
        PerformanceActionDialogState state{&action, &rule};
        DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_PROCESS_PERFORMANCE_ACTION), owner, PerformanceActionProc, reinterpret_cast<LPARAM>(&state));
        return state.accepted;
    }

    void RefreshPerformanceActions(HWND dialog, DialogState& state)
    {
        HWND list = GetDlgItem(dialog, IDC_PERF_ACTION_LIST);
        ConfigureListView(list, {{L"Process", 3}, {L"CPU priority", 2}, {L"I/O priority", 2}, {L"Memory priority", 2}, {L"CPU affinity", 3}});
        for (const auto& action : state.rule->processPerformanceActions)
        {
            std::wstring affinity = L"All CPUs";
            if (action.affinityMask != 0)
            {
                affinity.clear();
                for (DWORD cpu = 0; cpu < std::min<DWORD>(GetActiveProcessorCount(0), 64); ++cpu)
                    if ((action.affinityMask & (std::uint64_t{1} << cpu)) != 0)
                        affinity += (affinity.empty() ? L"" : L", ") + std::to_wstring(cpu);
            }
            AddListViewRow(list, {action.processName, PriorityText(action.cpuPriorityClass), IoPriorityText(action.ioPriority), MemoryPriorityText(action.memoryPriority), affinity});
        }
    }

    INT_PTR RulePaneBrush(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        // The tab control paints its body at RGB(249, 249, 249) with the current
        // Windows theme. The embedded dialogs must match it, not COLOR_WINDOW.
        static const HBRUSH paneBrush = CreateSolidBrush(RGB(249, 249, 249));
        const bool statusEdit = message == WM_CTLCOLOREDIT ||
            (message == WM_CTLCOLORSTATIC && reinterpret_cast<HWND>(lParam) ==
                GetDlgItem(dialog, IDC_IRACING_STATUS));
        const HBRUSH brush = statusEdit ? GetSysColorBrush(COLOR_WINDOW) : paneBrush;
        if (message == WM_CTLCOLORDLG) return reinterpret_cast<INT_PTR>(brush);
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkColor(dc, statusEdit ? GetSysColor(COLOR_WINDOW) : RGB(249, 249, 249));
        return reinterpret_cast<INT_PTR>(brush);
    }

    constexpr wchar_t kDwmRegistryPath[] = L"SOFTWARE\\Microsoft\\Windows\\Dwm";
    constexpr wchar_t kGraphicsRegistryPath[] = L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers";

    struct MpoRegistryValue
    {
        bool readable{};
        bool present{};
        DWORD value{};
    };

    MpoRegistryValue ReadMpoRegistryValue(const wchar_t* path, const wchar_t* name)
    {
        MpoRegistryValue state;
        HKEY key{};
        const LONG opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0,
            KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
        if (opened == ERROR_FILE_NOT_FOUND) { state.readable = true; return state; }
        if (opened != ERROR_SUCCESS) return state;
        DWORD type{};
        DWORD size = sizeof(state.value);
        const LONG result = RegQueryValueExW(key, name, nullptr, &type,
            reinterpret_cast<BYTE*>(&state.value), &size);
        RegCloseKey(key);
        if (result == ERROR_FILE_NOT_FOUND) { state.readable = true; return state; }
        if (result == ERROR_SUCCESS && type == REG_DWORD && size == sizeof(state.value))
        {
            state.readable = true;
            state.present = true;
        }
        return state;
    }

    bool WriteMpoRegistryValue(const wchar_t* path, const wchar_t* name,
        bool present, DWORD value, std::wstring& error)
    {
        HKEY key{};
        const LONG opened = present
            ? RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, nullptr, 0,
                KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr)
            : RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0,
                KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
        if (!present && opened == ERROR_FILE_NOT_FOUND) return true;
        if (opened != ERROR_SUCCESS)
        {
            error = opened == ERROR_ACCESS_DENIED
                ? L"Administrator rights are required to change the MPO registry values."
                : std::wstring(L"Could not open ") + name + L" (Windows error " +
                    std::to_wstring(opened) + L").";
            return false;
        }
        const LONG result = present
            ? RegSetValueExW(key, name, 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&value), sizeof(value))
            : RegDeleteValueW(key, name);
        RegCloseKey(key);
        if (result == ERROR_SUCCESS || (!present && result == ERROR_FILE_NOT_FOUND)) return true;
        error = std::wstring(L"Could not change ") + name + L" (Windows error " +
            std::to_wstring(result) + L").";
        return false;
    }

    bool ApplyMpoSettings(bool disable, std::wstring& error)
    {
        const auto oldDwm = ReadMpoRegistryValue(kDwmRegistryPath, L"OverlayTestMode");
        const auto oldGraphics = ReadMpoRegistryValue(kGraphicsRegistryPath, L"DisableOverlays");
        if (!oldDwm.readable || !oldGraphics.readable)
        {
            error = L"At least one MPO registry value could not be read safely. No change was made.";
            return false;
        }
        const auto matchesTarget = [disable](const MpoRegistryValue& item, DWORD target)
        {
            return item.readable && (disable ? item.present && item.value == target : !item.present);
        };
        if (matchesTarget(oldDwm, 5) && matchesTarget(oldGraphics, 1)) return true;

        if (!WriteMpoRegistryValue(kDwmRegistryPath, L"OverlayTestMode",
            disable, 5, error)) return false;
        if (!WriteMpoRegistryValue(kGraphicsRegistryPath, L"DisableOverlays",
            disable, 1, error))
        {
            std::wstring rollbackError;
            if (!WriteMpoRegistryValue(kDwmRegistryPath, L"OverlayTestMode",
                oldDwm.present, oldDwm.value, rollbackError))
                error += L" Rollback of OverlayTestMode also failed: " + rollbackError;
            return false;
        }
        const auto newDwm = ReadMpoRegistryValue(kDwmRegistryPath, L"OverlayTestMode");
        const auto newGraphics = ReadMpoRegistryValue(kGraphicsRegistryPath, L"DisableOverlays");
        if (!matchesTarget(newDwm, 5) || !matchesTarget(newGraphics, 1))
        {
            std::wstring rollbackError;
            const bool restoredDwm = WriteMpoRegistryValue(kDwmRegistryPath, L"OverlayTestMode",
                oldDwm.present, oldDwm.value, rollbackError);
            const bool restoredGraphics = WriteMpoRegistryValue(kGraphicsRegistryPath, L"DisableOverlays",
                oldGraphics.present, oldGraphics.value, rollbackError);
            error = L"Windows did not retain both MPO values.";
            if (!restoredDwm || !restoredGraphics) error += L" The previous state could not be fully restored.";
            return false;
        }
        return true;
    }

    std::wstring MpoPowerShellScript(bool disable)
    {
        const std::wstring apply = disable
            ? L"New-Item -Path 'HKLM:\\SOFTWARE\\Microsoft\\Windows\\Dwm' -Force | Out-Null; "
              L"New-ItemProperty -Path 'HKLM:\\SOFTWARE\\Microsoft\\Windows\\Dwm' -Name 'OverlayTestMode' -PropertyType DWord -Value 5 -Force | Out-Null; "
              L"New-ItemProperty -Path 'HKLM:\\SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers' -Name 'DisableOverlays' -PropertyType DWord -Value 1 -Force | Out-Null; "
              L"if ((Get-ItemPropertyValue -Path 'HKLM:\\SOFTWARE\\Microsoft\\Windows\\Dwm' -Name 'OverlayTestMode') -ne 5 -or (Get-ItemPropertyValue -Path 'HKLM:\\SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers' -Name 'DisableOverlays') -ne 1) { exit 2 }"
            : L"Remove-ItemProperty -Path 'HKLM:\\SOFTWARE\\Microsoft\\Windows\\Dwm' -Name 'OverlayTestMode' -ErrorAction SilentlyContinue; "
              L"Remove-ItemProperty -Path 'HKLM:\\SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers' -Name 'DisableOverlays' -ErrorAction SilentlyContinue;";
        return L"$ErrorActionPreference='Stop'; try { " + apply + L"; exit 0 } catch { exit 1 }";
    }

    bool ApplyMpoSettingsWithElevation(bool disable, std::wstring& error)
    {
        if (CanManageIRacingServices()) return ApplyMpoSettings(disable, error);

        const DWORD result = RunElevatedPowerShell(MpoPowerShellScript(disable));
        if (result != 0)
        {
            error = result == DWORD(-1)
                ? L"Administrator approval was cancelled or could not be requested."
                : L"Windows could not apply the MPO settings with administrator rights.";
            return false;
        }
        return ApplyMpoSettings(disable, error);
    }

    std::wstring DescribeMpoRegistryValue(const MpoRegistryValue& state)
    {
        if (!state.readable) return L"Unreadable or not a DWORD";
        if (!state.present) return L"Not present (Windows default)";
        return L"DWORD " + std::to_wstring(state.value);
    }

    void RefreshMpoStatus(HWND dialog, const std::wstring& error = {})
    {
        const auto dwm = ReadMpoRegistryValue(kDwmRegistryPath, L"OverlayTestMode");
        const auto graphics = ReadMpoRegistryValue(kGraphicsRegistryPath, L"DisableOverlays");
        SetDlgItemTextW(dialog, IDC_MPO_DWM_STATUS, DescribeMpoRegistryValue(dwm).c_str());
        SetDlgItemTextW(dialog, IDC_MPO_GRAPHICS_STATUS, DescribeMpoRegistryValue(graphics).c_str());
        SetDlgItemTextW(dialog, IDC_MPO_MESSAGE, error.c_str());
        const bool editable = dwm.readable && graphics.readable;
        EnableWindow(GetDlgItem(dialog, IDC_MPO_DISABLE), editable &&
            !(dwm.present && dwm.value == 5 && graphics.present && graphics.value == 1));
        EnableWindow(GetDlgItem(dialog, IDC_MPO_RESTORE), editable &&
            (dwm.present || graphics.present));
    }

    INT_PTR CALLBACK MpoDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM)
    {
        if (message == WM_INITDIALOG)
        {
            RefreshMpoStatus(dialog);
            return TRUE;
        }
        if (message != WM_COMMAND) return FALSE;
        switch (LOWORD(wParam))
        {
        case IDC_MPO_DISABLE:
        case IDC_MPO_RESTORE:
        {
            std::wstring error;
            ApplyMpoSettingsWithElevation(LOWORD(wParam) == IDC_MPO_DISABLE, error);
            RefreshMpoStatus(dialog, error);
            return TRUE;
        }
        case IDOK:
        case IDCANCEL:
            EndDialog(dialog, LOWORD(wParam));
            return TRUE;
        }
        return FALSE;
    }

    struct ServicesDialogState
    {
        WatchedProcessRule* rule{};
        BackgroundTask<std::vector<std::wstring>> refresh;
    };

    INT_PTR CALLBACK ServicesDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC ||
            message == WM_CTLCOLORBTN)
            return RulePaneBrush(dialog, message, wParam, lParam);
        auto* state = reinterpret_cast<ServicesDialogState*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
        if (message == WM_INITDIALOG)
        {
            state = reinterpret_cast<ServicesDialogState*>(lParam);
            SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            const auto& options = IRacingServiceOptions();
            for (size_t index = 0; index < options.size(); ++index)
            {
                const auto& option = options[index];
                const int id = IDC_IRACING_SERVICE_FIRST + static_cast<int>(index);
                const std::wstring label = std::wstring(option.label) + L" (" + option.name + L": loading...)";
                SetDlgItemTextW(dialog, id, label.c_str());
                const bool selected = std::any_of(state->rule->servicesToStop.begin(), state->rule->servicesToStop.end(), [&option](const std::wstring& name)
                {
                    return _wcsicmp(name.c_str(), option.name) == 0;
                });
                CheckDlgButton(dialog, id, selected ? BST_CHECKED : BST_UNCHECKED);
            }
            const bool started = SetTimer(dialog, 83, 100, nullptr) &&
                state->refresh.Start([](const std::atomic_bool& cancelled)
                {
                    std::vector<std::wstring> labels;
                    for (const auto& option : IRacingServiceOptions())
                    {
                        if (cancelled) break;
                        labels.push_back(std::wstring(option.label) + L" (" + option.name + L": " + DescribeIRacingService(option.name) + L")");
                    }
                    return labels;
                });
            if (!started)
            {
                KillTimer(dialog, 83);
                for (size_t i = 0; i < options.size(); ++i)
                {
                    const auto text = std::wstring(options[i].label) + L" (status unavailable)";
                    SetDlgItemTextW(dialog, IDC_IRACING_SERVICE_FIRST + static_cast<int>(i), text.c_str());
                }
            }
            return TRUE;
        }
        if (message == WM_TIMER && wParam == 83 && state)
        {
            std::optional<std::vector<std::wstring>> labels;
            if (state->refresh.Poll(labels))
            {
                KillTimer(dialog, 83);
                for (size_t i = 0; i < IRacingServiceOptions().size(); ++i)
                {
                    const auto text = labels && i < labels->size() ? (*labels)[i] :
                        std::wstring(IRacingServiceOptions()[i].label) + L" (status unavailable)";
                    SetDlgItemTextW(dialog, IDC_IRACING_SERVICE_FIRST + static_cast<int>(i), text.c_str());
                }
            }
            return TRUE;
        }
        if (message == WM_NCDESTROY)
        {
            KillTimer(dialog, 83);
            delete state;
            SetWindowLongPtrW(dialog, GWLP_USERDATA, 0);
            return FALSE;
        }
        if (message != WM_COMMAND) return FALSE;
        switch (LOWORD(wParam))
        {
        case IDC_IRACING_SERVICE_ALL:
            for (size_t index = 0; index < IRacingServiceOptions().size(); ++index)
                CheckDlgButton(dialog, IDC_IRACING_SERVICE_FIRST + static_cast<int>(index), BST_CHECKED);
            return TRUE;
        }
        return FALSE;
    }

    std::wstring BuildStatus(const WatchedProcessRule& rule, const DiagnosticResult& state, bool defenderVerifiedThisSession)
    {
        GUID active{};
        GUID* activePointer = nullptr;
        std::wstring summary = L"Power plan: unavailable";
        std::wstring activeName;
        if (PowerGetActiveScheme(nullptr, &activePointer) == ERROR_SUCCESS && activePointer)
        {
            active = *activePointer;
            LocalFree(activePointer);
            for (const auto& scheme : state.schemes)
                if (IsEqualGUID(scheme.id, active)) { activeName = scheme.name; summary = L"Power plan: " + scheme.name; }
        }
        if (HasX3DProcessor() && !activeName.empty() && activeName.find(L"Balanced") == std::wstring::npos &&
            activeName.find(L"Ausbalanciert") == std::wstring::npos)
            summary += L" (X3D: consider a Balanced plan; compare frame times)";
        summary += L"\r\nWindows services selected: " + std::to_wstring(rule.servicesToStop.size());
        if (!rule.servicesToStop.empty() && !CanManageIRacingServices())
            summary += L" (administrator approval is requested when the rule runs)";
        if (HasPendingIRacingServiceRestore()) summary += L"\r\nService restoration is pending.";
        if (!IsIRacingRule(rule))
        {
            return summary;
        }
        summary += L"\r\nRTSS: ";
        summary += ProcessRunning(L"RTSS.exe") ? L"running" : InstalledApplication(L"RivaTuner Statistics Server") ? L"installed, not running" : L"not detected";
        summary += L" | MSI Afterburner: ";
        summary += ProcessRunning(L"MSIAfterburner.exe") ? L"running" : InstalledApplication(L"MSI Afterburner") ? L"installed, not running" : L"not detected";
        summary += L"\r\n  Use the rule's Stop processes tab to close an overlay for a session.";
        summary += L"\r\nDisplay refresh rates:" + DisplaySummary();
        summary += L"\r\nDocuments: " + (state.documents.empty() ? std::wstring(L"unavailable") : state.documents.wstring()) + L"\r\n";
        summary += state.documents.empty() ? L"app.ini: Documents folder unavailable" : IniSummary(state.documents / L"app.ini");
        summary += L"\r\n\r\nDefender exclusions:";
        if (state.install.empty()) summary += L"\r\n  iRacing executable path is unavailable";
        else
        {
            const auto defender = ReadDefender(state.install.wstring(), state.documents.wstring());
            if (!defender.available)
                summary += defenderVerifiedThisSession
                    ? L"\r\n  added and verified with administrator rights this session; current list requires administrator rights to view"
                    : L"\r\n  exclusions cannot be read here; use administrator rights to check";
            else
            {
                summary += L"\r\n  Install folder: "; summary += defender.installExcluded ? L"present" : L"missing";
                summary += L"\r\n  Documents folder: "; summary += defender.documentsExcluded ? L"present" : L"missing";
            }
        }
        return summary;
    }

    void RefreshStatus(HWND dialog, DialogState& state)
    {
        if (state.refresh.Running()) { state.refreshAgain = true; return; }
        SetDlgItemTextW(dialog, IDC_IRACING_STATUS, L"Loading system settings...");
        EnableWindow(GetDlgItem(dialog, IDC_IRACING_REFRESH), FALSE);
        EnableWindow(GetDlgItem(dialog, IDC_IRACING_INI), FALSE);
        EnableWindow(GetDlgItem(dialog, IDC_IRACING_DEFENDER), FALSE);
        const bool started = SetTimer(dialog, 82, 100, nullptr) && state.refresh.Start(
            [rule = *state.rule, verified = state.defenderVerifiedThisSession](const std::atomic_bool& cancelled)
        {
            DiagnosticResult result;
            result.documents = IRacingDocuments();
            if (!rule.executablePath.empty())
            {
                const std::filesystem::path executable(rule.executablePath);
                if (_wcsicmp(executable.filename().c_str(), L"iRacingSim64DX11.exe") == 0)
                    result.install = executable.parent_path();
            }
            if (cancelled) return result;
            result.schemes = EnumeratePowerSchemes();
            if (!cancelled) result.summary = BuildStatus(rule, result, verified);
            return result;
        });
        if (!started)
        {
            KillTimer(dialog, 82);
            SetDlgItemTextW(dialog, IDC_IRACING_STATUS, L"Could not start diagnostics. Please refresh.");
            EnableWindow(GetDlgItem(dialog, IDC_IRACING_REFRESH), TRUE);
        }
    }

    INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC ||
            message == WM_CTLCOLORBTN || message == WM_CTLCOLOREDIT)
            return RulePaneBrush(dialog, message, wParam, lParam);
        auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
        if (message == WM_INITDIALOG)
        {
            state = reinterpret_cast<DialogState*>(lParam);
            SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            SendDlgItemMessageW(dialog, IDC_IRACING_PLAN, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Loading power plans..."));
            SendDlgItemMessageW(dialog, IDC_IRACING_PLAN, CB_SETCURSEL, 0, 0);
            EnableWindow(GetDlgItem(dialog, IDC_IRACING_PLAN), FALSE);
            InitializeReportListView(GetDlgItem(dialog, IDC_PERF_ACTION_LIST));
            RefreshPerformanceActions(dialog, *state);
            if (!IsIRacingRule(*state->rule))
            {
                ShowWindow(GetDlgItem(dialog, IDC_IRACING_INI), SW_HIDE);
                ShowWindow(GetDlgItem(dialog, IDC_IRACING_DEFENDER), SW_HIDE);
            }
            RefreshStatus(dialog, *state);
            return TRUE;
        }
        if (message == WM_TIMER && wParam == 82 && state)
        {
            std::optional<DiagnosticResult> result;
            if (!state->refresh.Poll(result)) return TRUE;
            KillTimer(dialog, 82);
            if (state->refreshAgain)
            {
                state->refreshAgain = false;
                RefreshStatus(dialog, *state);
                return TRUE;
            }
            EnableWindow(GetDlgItem(dialog, IDC_IRACING_REFRESH), TRUE);
            if (!result)
            {
                SetDlgItemTextW(dialog, IDC_IRACING_STATUS, L"Could not read system settings. Please refresh.");
                return TRUE;
            }
            std::wstring selectedGuid = state->rule->powerSchemeGuid;
            if (state->loaded)
            {
                const int selected = static_cast<int>(SendDlgItemMessageW(dialog, IDC_IRACING_PLAN, CB_GETCURSEL, 0, 0));
                selectedGuid = selected > 0 && static_cast<size_t>(selected - 1) < state->schemes.size()
                    ? GuidText(state->schemes[selected - 1].id) : L"";
            }
            state->documents = std::move(result->documents);
            state->install = std::move(result->install);
            state->schemes = std::move(result->schemes);
            SendDlgItemMessageW(dialog, IDC_IRACING_PLAN, CB_RESETCONTENT, 0, 0);
            SendDlgItemMessageW(dialog, IDC_IRACING_PLAN, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Do not change power plan"));
            int selected = 0;
            for (size_t i = 0; i < state->schemes.size(); ++i)
            {
                SendDlgItemMessageW(dialog, IDC_IRACING_PLAN, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(state->schemes[i].name.c_str()));
                if (_wcsicmp(GuidText(state->schemes[i].id).c_str(), selectedGuid.c_str()) == 0) selected = static_cast<int>(i) + 1;
            }
            // Do not silently erase a configured plan that is currently unavailable.
            if (!selectedGuid.empty() && selected == 0)
            {
                GUID id{};
                if (ParseGuid(selectedGuid, id))
                {
                    state->schemes.push_back({id, L"Unavailable power plan"});
                    SendDlgItemMessageW(dialog, IDC_IRACING_PLAN, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Unavailable power plan"));
                    selected = static_cast<int>(state->schemes.size());
                }
            }
            SendDlgItemMessageW(dialog, IDC_IRACING_PLAN, CB_SETCURSEL, selected, 0);
            state->loaded = true;
            EnableWindow(GetDlgItem(dialog, IDC_IRACING_PLAN), TRUE);
            EnableWindow(GetDlgItem(dialog, IDC_IRACING_INI), TRUE);
            EnableWindow(GetDlgItem(dialog, IDC_IRACING_DEFENDER), TRUE);
            SetDlgItemTextW(dialog, IDC_IRACING_STATUS, result->summary.c_str());
            return TRUE;
        }
        if (message == WM_NCDESTROY)
        {
            KillTimer(dialog, 82);
            delete state;
            SetWindowLongPtrW(dialog, GWLP_USERDATA, 0);
            return FALSE;
        }
        if (message == WM_NOTIFY)
        {
            const auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->idFrom == IDC_PERF_ACTION_LIST && header->code == NM_DBLCLK)
            {
                const int selected = SelectedListViewRow(GetDlgItem(dialog, IDC_PERF_ACTION_LIST));
                if (selected >= 0 && static_cast<size_t>(selected) < state->rule->processPerformanceActions.size() &&
                    EditPerformanceAction(dialog, *state->rule, state->rule->processPerformanceActions[static_cast<size_t>(selected)]))
                    RefreshPerformanceActions(dialog, *state);
                return TRUE;
            }
        }
        if (message != WM_COMMAND) return FALSE;
        if (LOWORD(wParam) == IDC_PERF_ACTION_ADD)
        {
            ProcessPerformanceAction action;
            if (EditPerformanceAction(dialog, *state->rule, action))
            {
                state->rule->processPerformanceActions.push_back(std::move(action));
                RefreshPerformanceActions(dialog, *state);
            }
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_PERF_ACTION_EDIT)
        {
            const int selected = SelectedListViewRow(GetDlgItem(dialog, IDC_PERF_ACTION_LIST));
            if (selected >= 0 && static_cast<size_t>(selected) < state->rule->processPerformanceActions.size() &&
                EditPerformanceAction(dialog, *state->rule, state->rule->processPerformanceActions[static_cast<size_t>(selected)]))
                RefreshPerformanceActions(dialog, *state);
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_PERF_ACTION_REMOVE)
        {
            const int selected = SelectedListViewRow(GetDlgItem(dialog, IDC_PERF_ACTION_LIST));
            if (selected >= 0 && static_cast<size_t>(selected) < state->rule->processPerformanceActions.size())
            {
                state->rule->processPerformanceActions.erase(state->rule->processPerformanceActions.begin() + selected);
                RefreshPerformanceActions(dialog, *state);
            }
            return TRUE;
        }
        switch (LOWORD(wParam))
        {
        case IDC_IRACING_REFRESH: RefreshStatus(dialog, *state); return TRUE;
        case IDC_IRACING_INI:
        {
            if (state->documents.empty())
            {
                MessageBoxW(dialog, L"The Windows Documents folder could not be found.", L"LaunchMate", MB_OK | MB_ICONERROR);
                return TRUE;
            }
            if (ProcessRunning(L"iRacingSim64DX11.exe"))
            {
                MessageBoxW(dialog, L"Close iRacing before editing app.ini so the game does not overwrite the changes.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            const auto path = state->documents / L"app.ini";
            const std::wstring prompt = L"Set carPreloadAll=1, trackTexturePreload=1 and streamingTextureSize=256 in:\n" + path.wstring() +
                L"\n\nLaunchMate will first save a timestamped backup. Apply now?";
            if (MessageBoxW(dialog, prompt.c_str(), L"iRacing app.ini", MB_YESNO | MB_ICONQUESTION) == IDYES)
            {
                std::wstring error;
                if (!ApplyIniSettings(path, error)) MessageBoxW(dialog, error.c_str(), L"LaunchMate", MB_OK | MB_ICONERROR);
                RefreshStatus(dialog, *state);
            }
            return TRUE;
        }
        case IDC_IRACING_DEFENDER:
        {
            if (state->install.empty() || state->documents.empty() || !std::filesystem::exists(state->install) || !std::filesystem::exists(state->documents))
            {
                MessageBoxW(dialog, L"Both iRacing folders must exist before exclusions can be added.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            // The elevated script checks and verifies existing exclusions itself;
            // do not run another synchronous Defender query on the UI thread.
            const std::wstring prompt = L"Add permanent Microsoft Defender folder exclusions for:\n" + state->install.wstring() +
                L"\n" + state->documents.wstring() + L"\n\nFiles in these folders will receive less antivirus scanning. Administrator approval is required.";
            if (MessageBoxW(dialog, prompt.c_str(), L"Defender exclusions", MB_YESNO | MB_ICONWARNING) != IDYES) return TRUE;
            const std::wstring script = L"$ErrorActionPreference='Stop'; try { "
                L"$targets=@('" + EscapePowerShell(state->install.wstring()) + L"','" + EscapePowerShell(state->documents.wstring()) + L"'); "
                L"$paths=@((Get-MpPreference).ExclusionPath); "
                L"if ($paths -match '^N/A:|Must be an administrator to view exclusions') { exit 3 }; "
                L"foreach ($target in $targets) { if ($paths -notcontains $target) { Add-MpPreference -ExclusionPath $target -ErrorAction Stop } }; "
                L"$paths=@((Get-MpPreference).ExclusionPath); "
                L"foreach ($target in $targets) { if ($paths -notcontains $target) { exit 2 } }; "
                L"exit 0 } catch { exit 4 }";
            const DWORD result = RunElevatedPowerShell(script);
            if (result == 0)
            {
                state->defenderVerifiedThisSession = true;
                MessageBoxW(dialog, L"Both Defender exclusions were verified in the administrator session.", L"LaunchMate", MB_OK | MB_ICONINFORMATION);
            }
            else
            {
                const wchar_t* message = result == 2
                    ? L"Defender did not retain one or both exclusions. Check Windows Security or organization policies."
                    : result == 3
                        ? L"Defender still did not allow the exclusions to be viewed with administrator rights."
                        : L"The Defender exclusions could not be added. Check the administrator prompt and Defender settings.";
                MessageBoxW(dialog, message, L"LaunchMate", MB_OK | MB_ICONERROR);
            }
            RefreshStatus(dialog, *state);
            return TRUE;
        }
        }
        return FALSE;
    }
}

bool IsIRacingRule(const WatchedProcessRule& rule)
{
    std::wstring name = rule.processName;
    if (name.empty() && !rule.executablePath.empty()) name = std::filesystem::path(rule.executablePath).filename().wstring();
    const auto stem = std::filesystem::path(name).stem().wstring();
    return _wcsicmp(stem.c_str(), L"iRacingSim64DX11") == 0;
}

std::vector<PowerSchemeInfo> EnumeratePowerSchemes()
{
    std::vector<PowerSchemeInfo> schemes;
    for (ULONG index = 0;; ++index)
    {
        GUID scheme{};
        DWORD size = sizeof(scheme);
        if (PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, index,
            reinterpret_cast<UCHAR*>(&scheme), &size) != ERROR_SUCCESS) break;
        DWORD nameSize = 0;
        PowerReadFriendlyName(nullptr, &scheme, nullptr, nullptr, nullptr, &nameSize);
        std::vector<UCHAR> name(nameSize);
        if (name.empty() || PowerReadFriendlyName(nullptr, &scheme, nullptr, nullptr, name.data(), &nameSize) != ERROR_SUCCESS) continue;
        name.resize(nameSize + sizeof(wchar_t));
        schemes.push_back({scheme, std::wstring(reinterpret_cast<const wchar_t*>(name.data()))});
    }
    return schemes;
}

bool ActivatePowerScheme(const std::wstring& schemeGuid, GUID& previousScheme)
{
    GUID target{};
    if (!ParseGuid(schemeGuid, target)) return false;
    GUID* previous = nullptr;
    if (PowerGetActiveScheme(nullptr, &previous) != ERROR_SUCCESS || !previous) return false;
    previousScheme = *previous;
    LocalFree(previous);
    if (IsEqualGUID(target, previousScheme)) return true;
    return PowerSetActiveScheme(nullptr, &target) == ERROR_SUCCESS;
}

bool RestorePowerScheme(const GUID& scheme)
{
    return PowerSetActiveScheme(nullptr, &scheme) == ERROR_SUCCESS;
}

HWND CreateIRacingPerformancePane(HINSTANCE instance, HWND parent, WatchedProcessRule& rule)
{
    EnsureDefaultPerformanceActions(rule);
    auto* state = new DialogState{&rule};
    HWND pane = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_IRACING_PERFORMANCE_PANE), parent, DialogProc, reinterpret_cast<LPARAM>(state));
    if (!pane) delete state;
    return pane;
}

void SaveIRacingPerformancePane(HWND pane)
{
    if (!pane) return;
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(pane, GWLP_USERDATA));
    if (!state || !state->loaded) return;
    const int selected = static_cast<int>(SendDlgItemMessageW(pane, IDC_IRACING_PLAN, CB_GETCURSEL, 0, 0));
    state->rule->powerSchemeGuid = selected > 0 && static_cast<size_t>(selected - 1) < state->schemes.size()
        ? GuidText(state->schemes[static_cast<size_t>(selected - 1)].id) : L"";
}

void RefreshIRacingPerformancePane(HWND pane)
{
    if (!pane) return;
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(pane, GWLP_USERDATA));
    if (state) RefreshStatus(pane, *state);
}

HWND CreateIRacingServicesPane(HINSTANCE instance, HWND parent, WatchedProcessRule& rule)
{
    auto* state = new ServicesDialogState{&rule};
    HWND pane = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_IRACING_SERVICES_PANE), parent, ServicesDialogProc, reinterpret_cast<LPARAM>(state));
    if (!pane) delete state;
    return pane;
}

void SaveIRacingServicesPane(HWND pane)
{
    if (!pane) return;
    auto* state = reinterpret_cast<ServicesDialogState*>(GetWindowLongPtrW(pane, GWLP_USERDATA));
    if (!state) return;
    state->rule->servicesToStop.clear();
    for (size_t index = 0; index < IRacingServiceOptions().size(); ++index)
        if (IsDlgButtonChecked(pane, IDC_IRACING_SERVICE_FIRST + static_cast<int>(index)) == BST_CHECKED)
            state->rule->servicesToStop.push_back(IRacingServiceOptions()[index].name);
}

void ShowMpoSettingsDialog(HINSTANCE instance, HWND parent)
{
    DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_MPO_SETTINGS), parent, MpoDialogProc, 0);
}
