#include "IRacingPerformance.h"
#include "IRacingServices.h"

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

    bool ReadIniValues(const std::filesystem::path& path, std::array<std::string, 3>& values)
    {
        std::string contents;
        if (!ReadIni(path, contents)) return false;
        bool inGraphics = false;
        values = {};
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
                        if (key == LowerAscii(kIniSettings[index].first))
                        {
                            const auto value = line.substr(equal + 1);
                            values[index] = Trim(value.substr(0, value.find_first_of(";#")));
                        }
                }
            }
            if (end == std::string::npos) break;
            cursor = end + 1;
        }
        return true;
    }

    bool ApplyIniSettings(const std::filesystem::path& path, const std::array<std::string, 3>& values, std::wstring& error)
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
                    const auto equalInOriginal = lines[index].find('=');
                    const auto comment = lines[index].find_first_of(";#", equalInOriginal + 1);
                    lines[index] = lines[index].substr(0, equalInOriginal + 1) + values[setting] +
                        (comment == std::string::npos ? "" : " " + lines[index].substr(comment));
                    found[setting] = true;
                }
            }
        }
        for (size_t setting = 0; setting < kIniSettings.size(); ++setting)
            if (!found[setting]) lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(graphicsEnd++), kIniSettings[setting].first + "=" + values[setting]);
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

    struct DefenderState { bool available{}; bool installExcluded{}; bool documentsExcluded{}; };

    DefenderState ReadDefender(const std::wstring& install, const std::wstring& documents)
    {
        const std::wstring script = L"$ErrorActionPreference='Stop'; $p=@((Get-MpPreference).ExclusionPath); "
            L"if ($p -match '^N/A:|Must be an administrator to view exclusions') { exit 3 }; "
            L"'LM_DEFENDER:' + [int]($p -contains '" + EscapePowerShell(install) + L"') + ':' + [int]($p -contains '" + EscapePowerShell(documents) + L"')";
        std::string output;
        DefenderState state;
        if (!RunPowerShell(script, output)) return state;
        const std::string marker = "LM_DEFENDER:";
        const size_t position = output.find(marker);
        if (position == std::string::npos) return state;
        const size_t first = position + marker.size(), second = first + 2;
        if (output.size() <= second || output[first + 1] != ':' ||
            (output[first] != '0' && output[first] != '1') || (output[second] != '0' && output[second] != '1')) return state;
        state.available = true;
        state.installExcluded = output[first] == '1';
        state.documentsExcluded = output[second] == '1';
        return state;
    }

    std::wstring DefenderExclusionScript(const std::wstring& install, const std::wstring& documents, bool remove)
    {
        const std::wstring change = remove
            ? L"if ($paths -contains $target) { Remove-MpPreference -ExclusionPath $target -ErrorAction Stop }"
            : L"if ($paths -notcontains $target) { Add-MpPreference -ExclusionPath $target -ErrorAction Stop }";
        const std::wstring failed = remove ? L"$paths -contains $target" : L"$paths -notcontains $target";
        return L"$ErrorActionPreference='Stop'; try { "
            L"$targets=@('" + EscapePowerShell(install) + L"','" + EscapePowerShell(documents) + L"'); "
            L"$paths=@((Get-MpPreference).ExclusionPath); "
            L"if ($paths -match '^N/A:|Must be an administrator to view exclusions') { exit 3 }; "
            L"foreach ($target in $targets) { " + change + L" }; "
            L"$paths=@((Get-MpPreference).ExclusionPath); "
            L"foreach ($target in $targets) { if (" + failed + L") { exit 2 } }; "
            L"exit 0 } catch { exit 4 }";
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

}

std::wstring PowerSchemeGuidText(const GUID& guid)
{
    return GuidText(guid);
}

bool IsIRacingRunning()
{
    return ProcessRunning(L"iRacingSim64DX11.exe");
}

IRacingCheck CheckIRacingSetup(const WatchedProcessRule& rule)
{
    IRacingCheck check;
    check.documents = IRacingDocuments();
    if (!check.documents.empty())
    {
        std::array<std::string, 3> values{};
        check.iniReadable = ReadIniValues(check.documents / L"app.ini", values);
        check.carPreload = values[0] == "1";
        check.trackPreload = values[1] == "1";
        check.streamingTextureSize = values[2].empty() ? 256 : std::atoi(values[2].c_str());
    }
    if (!rule.executablePath.empty())
    {
        const std::filesystem::path executable(rule.executablePath);
        if (_wcsicmp(executable.filename().c_str(), L"iRacingSim64DX11.exe") == 0) check.install = executable.parent_path();
    }
    if (!check.install.empty() && !check.documents.empty())
    {
        const auto defender = ReadDefender(check.install.wstring(), check.documents.wstring());
        check.defenderReadable = defender.available;
        check.installExcluded = defender.installExcluded;
        check.documentsExcluded = defender.documentsExcluded;
    }

    GUID* active = nullptr;
    if (PowerGetActiveScheme(nullptr, &active) == ERROR_SUCCESS && active)
    {
        for (const auto& scheme : EnumeratePowerSchemes())
            if (IsEqualGUID(scheme.id, *active)) check.activePowerPlan = scheme.name;
        LocalFree(active);
    }
    check.x3dProcessor = HasX3DProcessor();
    const auto describe = [](const wchar_t* process, const wchar_t* application)
    {
        return std::wstring(ProcessRunning(process) ? L"Running" : InstalledApplication(application) ? L"Installed, not running" : L"Not installed");
    };
    check.rtss = describe(L"RTSS.exe", L"RivaTuner Statistics Server");
    check.afterburner = describe(L"MSIAfterburner.exe", L"MSI Afterburner");

    for (DWORD index = 0;; ++index)
    {
        DISPLAY_DEVICEW device{sizeof(device)};
        if (!EnumDisplayDevicesW(nullptr, index, &device, 0)) break;
        if (!(device.StateFlags & DISPLAY_DEVICE_ACTIVE) || !(device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        DEVMODEW current{sizeof(current)};
        if (!EnumDisplaySettingsW(device.DeviceName, ENUM_CURRENT_SETTINGS, &current)) continue;
        IRacingCheck::Display display;
        display.current = current.dmDisplayFrequency;
        display.maximum = current.dmDisplayFrequency;
        for (DWORD modeIndex = 0;; ++modeIndex)
        {
            DEVMODEW mode{sizeof(mode)};
            if (!EnumDisplaySettingsW(device.DeviceName, modeIndex, &mode)) break;
            if (mode.dmPelsWidth == current.dmPelsWidth && mode.dmPelsHeight == current.dmPelsHeight &&
                mode.dmBitsPerPel == current.dmBitsPerPel)
                display.maximum = std::max(display.maximum, mode.dmDisplayFrequency);
        }
        DISPLAY_DEVICEW monitor{sizeof(monitor)};
        display.name = EnumDisplayDevicesW(device.DeviceName, 0, &monitor, 0) && monitor.DeviceString[0]
            ? monitor.DeviceString : device.DeviceName;
        display.name += L" (" + std::to_wstring(current.dmPelsWidth) + L" x " + std::to_wstring(current.dmPelsHeight) + L")";
        check.displays.push_back(std::move(display));
    }
    return check;
}

bool SaveIRacingIni(const std::filesystem::path& documents, bool carPreload, bool trackPreload, int streamingTextureSize, std::wstring& error)
{
    if (documents.empty()) { error = L"The Windows Documents folder could not be found."; return false; }
    if (IsIRacingRunning()) { error = L"Close iRacing first, otherwise it overwrites app.ini."; return false; }
    const std::array<std::string, 3> values{carPreload ? "1" : "0", trackPreload ? "1" : "0",
        std::to_string(std::max(0, streamingTextureSize))};
    return ApplyIniSettings(documents / L"app.ini", values, error);
}

bool ChangeIRacingDefenderExclusions(const std::filesystem::path& install, const std::filesystem::path& documents, bool remove, std::wstring& error)
{
    std::error_code ignored;
    if (install.empty() || documents.empty() ||
        (!remove && (!std::filesystem::exists(install, ignored) || !std::filesystem::exists(documents, ignored))))
    {
        error = L"Both iRacing folders must exist.";
        return false;
    }
    const DWORD result = RunElevatedPowerShell(DefenderExclusionScript(install.wstring(), documents.wstring(), remove));
    if (result == 0) return true;
    error = result == 2 ? L"Defender did not apply the change. Check Windows Security or your organization's policies."
        : result == 3 ? L"Defender does not allow the exclusions to be read, even with administrator rights."
        : L"The exclusions could not be changed. The administrator prompt may have been declined.";
    return false;
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

MpoState ReadMpoState()
{
    const auto dwm = ReadMpoRegistryValue(kDwmRegistryPath, L"OverlayTestMode");
    const auto graphics = ReadMpoRegistryValue(kGraphicsRegistryPath, L"DisableOverlays");
    MpoState state;
    state.readable = dwm.readable && graphics.readable;
    state.disabled = dwm.present && dwm.value == 5 && graphics.present && graphics.value == 1;
    state.customized = dwm.present || graphics.present;
    return state;
}

bool ChangeMpo(bool disable, std::wstring& error)
{
    return ApplyMpoSettingsWithElevation(disable, error);
}
