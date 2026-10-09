#include "ProcessMonitor.h"

#include "MonitorPowerController.h"
#include "IRacingPerformance.h"
#include "IRacingServices.h"
#include "Utils.h"

#include <algorithm>
#include <TlHelp32.h>
#include <cwctype>
#include <filesystem>
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <winhttp.h>

#ifndef LAUNCHMATE_VERSION
#define LAUNCHMATE_VERSION "0.3.0"
#endif

#define LAUNCHMATE_WIDEN_IMPL(value) L##value
#define LAUNCHMATE_WIDEN(value) LAUNCHMATE_WIDEN_IMPL(value)

namespace
{
    constexpr wchar_t kLaunchMateUserAgent[] = L"LaunchMate/" LAUNCHMATE_WIDEN(LAUNCHMATE_VERSION);
    constexpr DWORD kProgramLaunchSettleMs = 1200;
    constexpr DWORD kMinimumPollIntervalMs = 100;
    constexpr DWORD kMaximumPollIntervalMs = 300000;
    thread_local const wchar_t* launchStage = L"launch";

    using NtSetInformationProcessFn = LONG (NTAPI*)(HANDLE, ULONG, PVOID, ULONG);
    using RtlNtStatusToDosErrorFn = ULONG (WINAPI*)(LONG);
    constexpr ULONG kProcessIoPriorityInformation = 33;

    LONG SetProcessIoPriority(HANDLE process, int priority)
    {
        static const auto setInformation = reinterpret_cast<NtSetInformationProcessFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationProcess"));
        if (!setInformation) return static_cast<LONG>(0xC00000BBL); // STATUS_NOT_SUPPORTED
        ULONG value = static_cast<ULONG>(priority);
        return setInformation(process, kProcessIoPriorityInformation, &value, sizeof(value));
    }

    std::wstring IoPriorityName(int value)
    {
        switch (value)
        {
        case 0: return L"Very low";
        case 1: return L"Low";
        case 2: return L"Normal";
        case 3: return L"High";
        default: return std::to_wstring(value);
        }
    }

    std::wstring MemoryPriorityName(int value)
    {
        switch (value)
        {
        case 1: return L"Very low";
        case 2: return L"Low";
        case 3: return L"Medium";
        case 4: return L"Below normal";
        case 5: return L"Normal";
        default: return std::to_wstring(value);
        }
    }

    std::wstring NtStatusDescription(LONG status)
    {
        wchar_t hexadecimal[16]{};
        swprintf_s(hexadecimal, L"0x%08lX", static_cast<ULONG>(status));
        static const auto toDosError = reinterpret_cast<RtlNtStatusToDosErrorFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
        if (!toDosError) return hexadecimal;
        return std::wstring(hexadecimal) + L", Windows error " + std::to_wstring(toDosError(status));
    }

    bool EnableIncreaseBasePriorityPrivilege(DWORD& error)
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        {
            error = GetLastError();
            return false;
        }
        LUID privilege{};
        TOKEN_PRIVILEGES privileges{};
        const bool lookedUp = LookupPrivilegeValueW(nullptr, SE_INC_BASE_PRIORITY_NAME, &privilege) != FALSE;
        if (lookedUp)
        {
            privileges.PrivilegeCount = 1;
            privileges.Privileges[0].Luid = privilege;
            privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            SetLastError(ERROR_SUCCESS);
            AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
            error = GetLastError();
        }
        else error = GetLastError();
        CloseHandle(token);
        return lookedUp && error == ERROR_SUCCESS;
    }

    std::wstring NormalizePath(const std::wstring& path)
    {
        if (path.empty())
        {
            return {};
        }

        try
        {
            return std::filesystem::weakly_canonical(path).wstring();
        }
        catch (...)
        {
            return path;
        }
    }

    std::wstring NormalizeProcessKey(std::wstring processName)
    {
        const auto first = processName.find_first_not_of(L" \t\r\n\"");
        if (first == std::wstring::npos) return {};
        processName = processName.substr(first, processName.find_last_not_of(L" \t\r\n\"") - first + 1);
        processName = std::filesystem::path(processName).filename().wstring();
        for (auto& character : processName)
        {
            character = static_cast<wchar_t>(std::towlower(character));
        }

        if (!processName.empty() && !processName.ends_with(L".exe"))
        {
            processName += L".exe";
        }

        return processName;
    }

    std::wstring QueryProcessImagePath(HANDLE process)
    {
        std::wstring buffer(1024, L'\0');
        DWORD size = static_cast<DWORD>(buffer.size());
        if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &size))
        {
            return {};
        }

        buffer.resize(size);
        return buffer;
    }

    struct WindowOwnerSearch
    {
        DWORD processId{};
        bool found{};
    };

    BOOL CALLBACK FindVisibleTopLevelWindow(HWND window, LPARAM parameter)
    {
        auto& search = *reinterpret_cast<WindowOwnerSearch*>(parameter);
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (processId == search.processId && GetWindow(window, GW_OWNER) == nullptr && IsWindowVisible(window))
        {
            search.found = true;
            return FALSE;
        }
        return TRUE;
    }

    bool HasVisibleTopLevelWindow(DWORD processId)
    {
        WindowOwnerSearch search{processId};
        EnumWindows(FindVisibleTopLevelWindow, reinterpret_cast<LPARAM>(&search));
        return search.found;
    }

    struct ProgramLaunchResult
    {
        bool success{};
        DWORD processId{};
    };

    // ShellExecute has no creation-priority flag. Serialize the brief normal-
    // priority scope so launched apps do not inherit LaunchMate's low priority.
    class ShellLaunchPriorityScope
    {
    public:
        ShellLaunchPriorityScope() : lock_(mutex_), previous_(GetPriorityClass(GetCurrentProcess()))
        {
            changed_ = previous_ == IDLE_PRIORITY_CLASS || previous_ == BELOW_NORMAL_PRIORITY_CLASS;
            ready_ = !changed_ || SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS) != FALSE;
        }
        ~ShellLaunchPriorityScope()
        {
            const DWORD error = GetLastError();
            if (changed_ && ready_) SetPriorityClass(GetCurrentProcess(), previous_);
            SetLastError(error);
        }
        bool Ready() const { return ready_; }
    private:
        inline static std::mutex mutex_;
        std::unique_lock<std::mutex> lock_;
        DWORD previous_{};
        bool changed_{};
        bool ready_{};
    };

    ProgramLaunchResult LaunchProgramProcess(const LaunchProgram& program, std::shared_ptr<void>* launchedProcess = nullptr)
    {
        SHELLEXECUTEINFOW info{};
        launchStage = L"shell execute";
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        info.lpFile = program.filePath.c_str();
        info.lpParameters = program.arguments.empty() ? nullptr : program.arguments.c_str();
        info.nShow = SW_SHOWNORMAL;
        info.lpVerb = L"open";
        const auto directory = std::filesystem::path(program.filePath).parent_path().wstring();
        info.lpDirectory = directory.empty() ? nullptr : directory.c_str();

        ShellLaunchPriorityScope priorityScope;
        if (!priorityScope.Ready()) { launchStage = L"prepare launch priority"; return {}; }
        if (ShellExecuteExW(&info))
        {
            if (!info.hProcess) return {true, 0};
            const DWORD processId = GetProcessId(info.hProcess);
            if (launchedProcess) *launchedProcess = std::shared_ptr<void>(info.hProcess, CloseHandle);
            else CloseHandle(info.hProcess);
            return {true, processId};
        }

        return {};
    }

    // Launched programs get the same grace period as the stop-action default.
    constexpr DWORD kLaunchedProgramCloseGraceMs = 3000;
    constexpr DWORD kTerminateWaitMs = 4000;
    constexpr unsigned kMaxPerformanceAttempts = 5;

    struct CloseTarget
    {
        std::shared_ptr<void> process;
        DWORD graceMilliseconds{};
        // Targets of one stop action or launched program; see CloseProcesses.
        size_t group{};
    };

    struct CloseWindowsContext
    {
        const std::unordered_set<DWORD>* processIds{};
        std::unordered_set<DWORD> withWindows;
    };

    BOOL CALLBACK CloseProcessWindows(HWND window, LPARAM parameter)
    {
        auto& context = *reinterpret_cast<CloseWindowsContext*>(parameter);
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (context.processIds->contains(processId) && GetWindow(window, GW_OWNER) == nullptr)
        {
            context.withWindows.insert(processId);
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
        return TRUE;
    }

    // Asks all targets to close at once and terminates each one when its own grace
    // period ends, so several programs share one wait instead of queueing. A group in
    // which no process owns a window cannot react to WM_CLOSE and is terminated
    // immediately. Returns, per target, whether the process has exited.
    std::vector<bool> CloseProcesses(const std::vector<CloseTarget>& targets)
    {
        std::unordered_set<DWORD> gracefulIds;
        for (const auto& target : targets)
        {
            if (target.graceMilliseconds > 0 && WaitForSingleObject(target.process.get(), 0) == WAIT_TIMEOUT)
                gracefulIds.insert(GetProcessId(target.process.get()));
        }
        CloseWindowsContext context{&gracefulIds};
        if (!gracefulIds.empty()) EnumWindows(CloseProcessWindows, reinterpret_cast<LPARAM>(&context));
        std::unordered_set<size_t> groupsWithWindows;
        for (const auto& target : targets)
        {
            if (context.withWindows.contains(GetProcessId(target.process.get()))) groupsWithWindows.insert(target.group);
        }

        const ULONGLONG start = GetTickCount64();
        std::vector<ULONGLONG> deadlines(targets.size());
        std::vector<size_t> order(targets.size());
        for (size_t index = 0; index < targets.size(); ++index)
        {
            order[index] = index;
            deadlines[index] = start + (groupsWithWindows.contains(targets[index].group) ? targets[index].graceMilliseconds : 0);
        }
        std::stable_sort(order.begin(), order.end(), [&deadlines](size_t left, size_t right)
        {
            return deadlines[left] < deadlines[right];
        });
        // Handles stay open throughout, so a PID cannot be reused by another process.
        for (const size_t index : order)
        {
            const auto now = GetTickCount64();
            const auto process = targets[index].process.get();
            if (WaitForSingleObject(process, deadlines[index] > now ? static_cast<DWORD>(deadlines[index] - now) : 0) == WAIT_TIMEOUT)
                TerminateProcess(process, 0);
        }

        std::vector<bool> exited(targets.size());
        const auto deadline = GetTickCount64() + kTerminateWaitMs;
        for (size_t index = 0; index < targets.size(); ++index)
        {
            const auto now = GetTickCount64();
            exited[index] = WaitForSingleObject(targets[index].process.get(),
                now < deadline ? static_cast<DWORD>(deadline - now) : 0) == WAIT_OBJECT_0;
        }
        return exited;
    }

    bool CreatedAtOrAfter(HANDLE process, const FILETIME& since)
    {
        FILETIME created{}, exited{}, kernel{}, user{};
        return GetProcessTimes(process, &created, &exited, &kernel, &user) && CompareFileTime(&created, &since) >= 0;
    }

    // Layout of UNICODE_STRING, as returned for ProcessCommandLineInformation.
    struct CommandLineString
    {
        USHORT length;
        USHORT maximumLength;
        PWSTR buffer;
    };

    std::wstring QueryProcessCommandLine(HANDLE process)
    {
        using QueryInformationProcessFn = LONG (NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        constexpr ULONG kProcessCommandLineInformation = 60;
        static const auto query = reinterpret_cast<QueryInformationProcessFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
        if (!query) return {};
        ULONG size = 0;
        query(process, kProcessCommandLineInformation, nullptr, 0, &size);
        if (size < sizeof(CommandLineString) || size > 70000) return {};
        std::vector<BYTE> buffer(size);
        if (query(process, kProcessCommandLineInformation, buffer.data(), size, &size) < 0) return {};
        const auto* text = reinterpret_cast<const CommandLineString*>(buffer.data());
        return text->buffer ? std::wstring(text->buffer, text->length / sizeof(wchar_t)) : std::wstring{};
    }

    // Drops the executable token; the remainder is handed to ShellExecute unchanged.
    std::wstring ArgumentsFromCommandLine(const std::wstring& commandLine)
    {
        size_t position = 0;
        const auto skipSpaces = [&]
        {
            while (position < commandLine.size() && std::iswspace(commandLine[position])) ++position;
        };
        skipSpaces();
        if (position < commandLine.size() && commandLine[position] == L'"')
        {
            const auto closing = commandLine.find(L'"', position + 1);
            position = closing == std::wstring::npos ? commandLine.size() : closing + 1;
        }
        else
        {
            while (position < commandLine.size() && !std::iswspace(commandLine[position])) ++position;
        }
        skipSpaces();
        return commandLine.substr(position);
    }

    bool PathMatchesProcess(DWORD processId, const std::wstring& expectedPath)
    {
        if (expectedPath.empty()) return true;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (!process) return false;
        const auto actualPath = NormalizePath(QueryProcessImagePath(process));
        CloseHandle(process);
        return !actualPath.empty() && _wcsicmp(actualPath.c_str(), NormalizePath(expectedPath).c_str()) == 0;
    }

    bool PostWebhook(const HomeAssistantAction& action)
    {
        URL_COMPONENTSW parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwSchemeLength = static_cast<DWORD>(-1);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(action.webhookUrl.c_str(), 0, 0, &parts)) return false;

        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength > 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        HINTERNET session = WinHttpOpen(kLaunchMateUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
        if (!session) return false;
        WinHttpSetTimeouts(session, 5000, 5000, 5000, 10000);
        HINTERNET connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
        const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET request = connection ? WinHttpOpenRequest(connection, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags) : nullptr;
        const auto payload = ToUtf8(action.jsonPayload.empty() ? L"{}" : action.jsonPayload);
        const wchar_t* headers = L"Content-Type: application/json\r\n";
        bool success = request && WinHttpSendRequest(request, headers, static_cast<DWORD>(-1),
            const_cast<char*>(payload.data()), static_cast<DWORD>(payload.size()), static_cast<DWORD>(payload.size()), 0) &&
            WinHttpReceiveResponse(request, nullptr);
        if (success)
        {
            DWORD status = 0;
            DWORD size = sizeof(status);
            success = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) && status >= 200 && status < 300;
        }
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return success;
    }

    const std::unordered_set<std::wstring>* ProcessKeyFilterPointer(const std::unordered_set<std::wstring>& filter)
    {
        return filter.empty() ? nullptr : &filter;
    }
}

ProcessMonitor::ProcessMonitor(StatusCallback callback)
    : runtimeConfiguration_(std::make_shared<RuntimeConfiguration>()),
      statusCallback_(std::move(callback)),
      wakeEvent_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
      stopEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      etwProcessListener_([this](EtwProcessListener::ProcessEvent event)
      {
          std::scoped_lock lock(etwEventsMutex_);
          pendingEtwEvents_.push_back(std::move(event));
          if (running_.load()) WakeWorker();
      })
{
}

ProcessMonitor::~ProcessMonitor()
{
    Stop();

    if (wakeEvent_ != nullptr)
    {
        CloseHandle(wakeEvent_);
    }
    if (stopEvent_ != nullptr) CloseHandle(stopEvent_);
}

void ProcessMonitor::SetProblemCallback(ProblemCallback callback)
{
    problemCallback_ = std::move(callback);
}

void ProcessMonitor::ReportProblem(const std::wstring& text) const
{
    statusCallback_(text);
    if (problemCallback_) problemCallback_(text);
}

void ProcessMonitor::UpdateConfiguration(const AppConfiguration& configuration)
{
    auto prepared = std::make_shared<RuntimeConfiguration>();
    prepared->useEtw = configuration.useEtw;
    prepared->watchedRules.reserve(configuration.watchedProcesses.size());
    prepared->watchedProcessKeys.reserve(configuration.watchedProcesses.size());

    for (const auto& rule : configuration.watchedProcesses)
    {
        if (!rule.enabled || (rule.processName.empty() && rule.executablePath.empty()))
        {
            continue;
        }

        RuntimeRule runtimeRule;
        runtimeRule.processKey = NormalizeProcessKey(rule.processName.empty() ? rule.executablePath : rule.processName);
        runtimeRule.displayName = rule.displayName.empty() ? rule.processName : rule.displayName;
        runtimeRule.programsToLaunch = rule.programsToLaunch;
        runtimeRule.processesToStop = rule.processesToStop;
        runtimeRule.homeAssistantActions = rule.homeAssistantActions;
        runtimeRule.processPerformanceActions = rule.processPerformanceActions;
        runtimeRule.monitorPowerSetupDelayMilliseconds = rule.monitorPowerSetupDelayMilliseconds;
        runtimeRule.restoreMonitorPowerSetupOnExit = rule.restoreMonitorPowerSetupOnExit;
        runtimeRule.restoreMonitorPowerSetupDelayMilliseconds = rule.restoreMonitorPowerSetupDelayMilliseconds;
        runtimeRule.powerSchemeGuid = rule.powerSchemeGuid;
        runtimeRule.servicesToStop = rule.servicesToStop;
        const auto monitorSetup = std::find_if(
            configuration.monitorPowerSetups.begin(),
            configuration.monitorPowerSetups.end(),
            [&rule](const MonitorPowerSetup& setup) { return setup.name == rule.monitorPowerSetupName; });
        if (monitorSetup != configuration.monitorPowerSetups.end())
        {
            runtimeRule.monitorPowerSetup = *monitorSetup;
            runtimeRule.hasMonitorPowerSetup = true;
        }
        prepared->watchedProcessKeys.insert(runtimeRule.processKey);
        prepared->watchedRules.push_back(std::move(runtimeRule));
    }

    std::scoped_lock lock(mutex_);
    runtimeConfiguration_ = std::move(prepared);
    {
        std::scoped_lock stateLock(processStatesMutex_);
        cachedProcessStates_.clear();
        cachedProcessStatesKnown_ = false;
    }
    etwInitialSnapshotComplete_ = false;
    // The worker refreshes the ETW filter (including performance targets) and
    // resynchronizes with one snapshot before it handles further events.
    WakeWorker();
}

void ProcessMonitor::SetPollInterval(DWORD pollIntervalMs)
{
    idlePollIntervalMs_.store(std::clamp<DWORD>(pollIntervalMs, kMinimumPollIntervalMs, kMaximumPollIntervalMs));
    WakeWorker();
}

void ProcessMonitor::SetActivePollInterval(DWORD pollIntervalMs)
{
    activePollIntervalMs_.store(std::clamp<DWORD>(pollIntervalMs, kMinimumPollIntervalMs, kMaximumPollIntervalMs));
    WakeWorker();
}

void ProcessMonitor::Start()
{
    std::scoped_lock lifecycle(lifecycleMutex_);
    if (running_.exchange(true))
    {
        return;
    }

    if (stopEvent_) ResetEvent(stopEvent_);
    std::shared_ptr<const RuntimeConfiguration> runtimeConfiguration;
    {
        std::scoped_lock lock(mutex_);
        runtimeConfiguration = runtimeConfiguration_;
    }
    etwRestarts_ = 0;
    if (runtimeConfiguration && runtimeConfiguration->useEtw)
    {
        std::wstring error;
        if (etwProcessListener_.Start(EtwProcessKeys(*runtimeConfiguration), error))
        {
            usingEtw_.store(true);
            etwInitialSnapshotComplete_ = false;
            statusCallback_(L"ETW process monitoring active.");
        }
        else
        {
            usingEtw_.store(false);
            statusCallback_(L"ETW could not start; using process polling. " + error);
        }
    }
    worker_ = std::thread([this] { WorkerLoop(); });
    statusCallback_(L"Monitoring active.");
}

void ProcessMonitor::Stop()
{
    // Serializes concurrent stops (background stop and application shutdown): the
    // second caller returns only after the first one has restored everything.
    std::scoped_lock lifecycle(lifecycleMutex_);
    if (!running_.exchange(false))
    {
        return;
    }

    if (stopEvent_) SetEvent(stopEvent_);
    WakeWorker();

    if (worker_.joinable())
    {
        worker_.join();
    }
    // The worker may restart the listener, so it is stopped only after the worker ended.
    etwProcessListener_.Stop();
    usingEtw_.store(false);
    etwInitialSnapshotComplete_ = false;
    {
        std::scoped_lock lock(etwEventsMutex_);
        pendingEtwEvents_.clear();
    }

    for (const auto& [key, rule] : activeRules_) FinishRule(rule);
    for (const auto& [processKey, previous] : previousPowerSchemes_)
    {
        if (!RestorePowerScheme(previous)) ReportProblem(L"Could not restore the previous power plan for " + processKey + L".");
    }
    previousPowerSchemes_.clear();
    serviceOwnerKey_.clear();
    displayOwnerKey_.clear();
    activeRules_.clear();
    watchedInstances_.clear();
    {
        std::scoped_lock lock(processStatesMutex_);
        runningSince_.clear();
    }
    if (HasPendingIRacingServiceRestore())
    {
        std::wstring serviceReport;
        if (!RestoreIRacingServices(serviceReport))
            ReportProblem(L"Monitoring stopped; Windows services still need restoration: " + serviceReport);
        else
            statusCallback_(L"Monitoring stopped.");
    }
    else statusCallback_(L"Monitoring stopped.");
}

void ProcessMonitor::RecoverIRacingServices()
{
    if (!HasPendingIRacingServiceRestore()) return;
    std::wstring report;
    if (RestoreIRacingServices(report)) statusCallback_(L"Restored Windows services from the previous session.");
    else ReportProblem(L"Windows services still need restoration. Run LaunchMate as Administrator. " + report);
}

bool ProcessMonitor::IsRunning() const noexcept
{
    return running_.load();
}

bool ProcessMonitor::IsUsingEtw() const noexcept
{
    return usingEtw_.load();
}

std::vector<ULONGLONG> ProcessMonitor::GetRunningSince(const std::vector<WatchedProcessRule>& rules) const
{
    std::vector<ULONGLONG> since;
    since.reserve(rules.size());
    std::scoped_lock lock(processStatesMutex_);
    for (const auto& rule : rules)
    {
        const auto found = runningSince_.find(NormalizeProcessKey(rule.processName.empty() ? rule.executablePath : rule.processName));
        since.push_back(found != runningSince_.end() ? found->second : 0);
    }
    return since;
}

std::vector<std::wstring> ProcessMonitor::GetProcessStates(const std::vector<WatchedProcessRule>& rules) const
{
    std::vector<std::wstring> states;
    states.reserve(rules.size());

    // While monitoring, this is the exact snapshot that was used to decide
    // whether start or exit actions must run.  Do not enumerate processes again.
    if (running_.load())
    {
        std::scoped_lock lock(processStatesMutex_);
        if (cachedProcessStatesKnown_)
        {
            for (const auto& rule : rules)
            {
                const auto key = NormalizeProcessKey(rule.processName.empty() ? rule.executablePath : rule.processName);
                const auto state = cachedProcessStates_.find(key);
                states.push_back(state != cachedProcessStates_.end() && state->second ? L"Running" : L"Stopped");
            }
            return states;
        }
        if (usingEtw_.load())
        {
            states.assign(rules.size(), L"Unknown");
            return states;
        }
    }

    // Monitoring is off (or its first snapshot has not arrived yet), so status
    // remains useful without starting a worker or executing any actions.
    const auto snapshot = CaptureProcessSnapshot(false);
    for (const auto& rule : rules)
    {
        const auto key = NormalizeProcessKey(rule.processName.empty() ? rule.executablePath : rule.processName);
        states.push_back(!snapshot.valid ? L"Unknown" : IsProcessRunning(snapshot, key) ? L"Running" : L"Stopped");
    }
    return states;
}

ProcessMonitor::ProcessSnapshot ProcessMonitor::CaptureProcessSnapshot(
    bool includeProcessTree,
    const std::unordered_set<std::wstring>* processKeyFilter) const
{
    ProcessSnapshot snapshot;
    if (processKeyFilter != nullptr)
    {
        snapshot.processIdsByName.reserve(processKeyFilter->size());
    }

    HANDLE processSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processSnapshot == INVALID_HANDLE_VALUE)
    {
        return snapshot;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(processSnapshot, &entry))
    {
        do
        {
            const auto processKey = NormalizeProcessKey(entry.szExeFile);
            if (processKeyFilter == nullptr || processKeyFilter->contains(processKey))
            {
                snapshot.processIdsByName[processKey].push_back(entry.th32ProcessID);
            }

            if (includeProcessTree)
            {
                snapshot.childrenByParent[entry.th32ParentProcessID].push_back(entry.th32ProcessID);
            }
        }
        while (Process32NextW(processSnapshot, &entry));
        snapshot.valid = GetLastError() == ERROR_NO_MORE_FILES;
    }

    CloseHandle(processSnapshot);
    return snapshot;
}

bool ProcessMonitor::IsProcessRunning(const ProcessSnapshot& snapshot, const std::wstring& processKey) const
{
    const auto it = snapshot.processIdsByName.find(processKey);
    return it != snapshot.processIdsByName.end() && !it->second.empty();
}

std::unordered_set<DWORD> ProcessMonitor::FindMatchingProcesses(
    const ProcessSnapshot& snapshot,
    const std::wstring& executablePath) const
{
    return FindMatchingProcesses(snapshot, executablePath, NormalizePath(executablePath));
}

std::unordered_set<DWORD> ProcessMonitor::FindMatchingProcesses(
    const ProcessSnapshot& snapshot,
    const std::wstring& executablePath,
    const std::wstring& normalizedExecutablePath) const
{
    std::unordered_set<DWORD> results;
    if (normalizedExecutablePath.empty())
    {
        return results;
    }

    const auto processName = NormalizeProcessKey(std::filesystem::path(executablePath).stem().wstring());
    const auto it = snapshot.processIdsByName.find(processName);
    if (it == snapshot.processIdsByName.end())
    {
        return results;
    }

    for (const auto processId : it->second)
    {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (!process)
        {
            continue;
        }

        const auto processPath = NormalizePath(QueryProcessImagePath(process));
        CloseHandle(process);

        if (!processPath.empty() && _wcsicmp(processPath.c_str(), normalizedExecutablePath.c_str()) == 0)
        {
            results.insert(processId);
        }
    }

    return results;
}

std::unordered_set<DWORD> ProcessMonitor::BuildChildProcessSet(
    const ProcessSnapshot& snapshot,
    DWORD rootProcessId) const
{
    std::unordered_set<DWORD> descendants;
    std::vector<DWORD> queue{rootProcessId};

    for (size_t index = 0; index < queue.size(); ++index)
    {
        const auto it = snapshot.childrenByParent.find(queue[index]);
        if (it == snapshot.childrenByParent.end())
        {
            continue;
        }

        for (const auto child : it->second)
        {
            if (descendants.insert(child).second)
            {
                queue.push_back(child);
            }
        }
    }

    return descendants;
}

void ProcessMonitor::WorkerLoop()
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto currentConfiguration = [this]
    {
        std::scoped_lock lock(mutex_);
        return runtimeConfiguration_;
    };
    while (running_.load())
    {
        if (usingEtw_.load())
        {
            // One snapshot covers processes that were already running when ETW was
            // enabled, the configuration changed or ETW reported lost events. From
            // then on, ETW events and the instance handles are the source of truth.
            if (!etwInitialSnapshotComplete_.load())
            {
                RefreshEtwProcessKeys(*currentConfiguration());
                CheckRules();
            }
            ProcessEtwEvents();
        }
        else CheckRules();

        if (!running_.load())
        {
            break;
        }

        const auto runtimeConfiguration = currentConfiguration();
        DWORD waitDurationMs = idlePollIntervalMs_.load();
        if (runtimeConfiguration->watchedRules.empty() && activeRules_.empty())
        {
            waitDurationMs = INFINITE;
        }
        else if (usingEtw_.load())
        {
            // A failed resynchronization snapshot is retried at the idle interval.
            if (etwInitialSnapshotComplete_.load()) waitDurationMs = INFINITE;
        }
        else if (!activeRules_.empty() && !AllActiveInstancesTracked())
        {
            // Exits of tracked instances wake the worker; only untracked ones need polling.
            waitDurationMs = activePollIntervalMs_.load();
        }

        WaitForWork(*runtimeConfiguration, waitDurationMs);
    }
    if (SUCCEEDED(comResult)) CoUninitialize();
}

void ProcessMonitor::CheckRules()
{
    std::shared_ptr<const RuntimeConfiguration> runtimeConfiguration;
    {
        std::scoped_lock lock(mutex_);
        runtimeConfiguration = runtimeConfiguration_;
    }

    if (!runtimeConfiguration) return;
    if (runtimeConfiguration->watchedRules.empty() && activeRules_.empty()) return;

    auto keys = runtimeConfiguration->watchedProcessKeys;
    for (const auto& [key, rule] : activeRules_)
    {
        keys.insert(key);
        for (const auto& action : rule.processPerformanceActions)
            keys.insert(NormalizeProcessKey(action.processName));
    }
    const auto snapshot = CaptureProcessSnapshot(false, &keys);
    ApplySnapshot(*runtimeConfiguration, snapshot);
}

void ProcessMonitor::ApplySnapshot(const RuntimeConfiguration& configuration, const ProcessSnapshot& snapshot)
{
    // A failed enumeration is not evidence that a watched program has exited.
    if (!snapshot.valid) return;

    CacheProcessStates(configuration, snapshot);

    if (usingEtw_.load() && !etwInitialSnapshotComplete_.load())
    {
        // Stop events identify a process by PID only, so register what already runs.
        for (const auto& [processKey, processIds] : snapshot.processIdsByName)
        {
            if (!configuration.watchedProcessKeys.contains(processKey) && !activeRules_.contains(processKey)) continue;
            for (const DWORD processId : processIds) etwProcessListener_.TrackExistingProcess(processId, processKey);
        }
        etwInitialSnapshotComplete_.store(true);
    }

    for (auto it = activeRules_.begin(); it != activeRules_.end();)
    {
        if (!IsProcessRunning(snapshot, it->first))
        {
            FinishRule(it->second);
            it = activeRules_.erase(it);
            RefreshEtwProcessKeys(configuration);
        }
        else ++it;
    }

    for (const auto& rule : configuration.watchedRules)
    {
        if (!running_.load()) break;
        if (IsProcessRunning(snapshot, rule.processKey) && !activeRules_.contains(rule.processKey))
            ActivateRule(configuration, rule, snapshot.processIdsByName.at(rule.processKey));
    }

    for (const auto& [key, rule] : activeRules_)
    {
        const auto found = snapshot.processIdsByName.find(key);
        if (found != snapshot.processIdsByName.end()) SyncWatchedInstances(key, found->second);
    }
    for (const auto& [key, rule] : activeRules_) ApplyPerformanceActions(rule, snapshot);
}

void ProcessMonitor::ActivateRule(
    const RuntimeConfiguration& configuration,
    const RuntimeRule& rule,
    const std::vector<DWORD>& processIds)
{
    activeRules_.emplace(rule.processKey, rule);
    // Track the instances before the (possibly long) start actions, so an exit
    // during them is still noticed afterwards.
    SyncWatchedInstances(rule.processKey, processIds);
    RefreshEtwProcessKeys(configuration);
    statusCallback_(rule.displayName + L" detected. Running actions.");
    if (problemCallback_) problemCallback_({});
    ExecuteStartActions(rule);
}

void ProcessMonitor::SyncWatchedInstances(const std::wstring& processKey, const std::vector<DWORD>& processIds)
{
    auto& instances = watchedInstances_[processKey];
    std::erase_if(instances, [&processIds](const auto& instance)
    {
        return std::find(processIds.begin(), processIds.end(), instance.first) == processIds.end();
    });
    for (const DWORD processId : processIds)
    {
        if (!instances.contains(processId)) TrackWatchedInstance(processKey, processId);
    }
}

void ProcessMonitor::TrackWatchedInstance(const std::wstring& processKey, DWORD processId)
{
    std::shared_ptr<void> process;
    if (HANDLE handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId))
    {
        process.reset(handle, CloseHandle);
        // The PID may belong to a different program by now; only keep the watched one.
        const auto imagePath = QueryProcessImagePath(handle);
        if (!imagePath.empty() && NormalizeProcessKey(imagePath) != processKey) return;
    }
    else if (GetLastError() == ERROR_INVALID_PARAMETER)
    {
        return; // Already gone.
    }
    // A null handle (e.g. a protected process) still counts as running; its exit is
    // then detected by an ETW stop event or the next polling snapshot.
    watchedInstances_[processKey][processId] = std::move(process);
}

void ProcessMonitor::OnWatchedInstanceGone(
    const RuntimeConfiguration& configuration,
    const std::wstring& processKey,
    DWORD processId)
{
    const auto instances = watchedInstances_.find(processKey);
    if (instances == watchedInstances_.end() || instances->second.erase(processId) == 0) return;
    // When polling, the snapshot taken right after this wake-up decides.
    if (!usingEtw_.load() || !instances->second.empty()) return;

    watchedInstances_.erase(instances);
    CacheProcessState(processKey, false);
    const auto active = activeRules_.find(processKey);
    if (active == activeRules_.end()) return;
    FinishRule(active->second);
    activeRules_.erase(active);
    RefreshEtwProcessKeys(configuration);
}

bool ProcessMonitor::AllActiveInstancesTracked() const
{
    size_t handleCount = 0;
    for (const auto& [key, rule] : activeRules_)
    {
        const auto instances = watchedInstances_.find(key);
        if (instances == watchedInstances_.end() || instances->second.empty()) return false;
        for (const auto& [processId, process] : instances->second)
        {
            if (!process) return false;
            ++handleCount;
        }
    }
    // WaitForWork can wait on 63 instances next to its wake event.
    return handleCount < MAXIMUM_WAIT_OBJECTS;
}

void ProcessMonitor::WaitForWork(const RuntimeConfiguration& configuration, DWORD timeoutMs)
{
    if (wakeEvent_ == nullptr)
    {
        Sleep(std::min<DWORD>(timeoutMs, kMinimumPollIntervalMs));
        return;
    }

    std::vector<HANDLE> handles{wakeEvent_};
    std::vector<std::pair<std::wstring, DWORD>> owners(1);
    for (const auto& [processKey, instances] : watchedInstances_)
    {
        for (const auto& [processId, process] : instances)
        {
            if (!process || handles.size() >= MAXIMUM_WAIT_OBJECTS) continue;
            handles.push_back(process.get());
            owners.emplace_back(processKey, processId);
        }
    }

    const DWORD result = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, timeoutMs);
    if (result > WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + handles.size())
    {
        const auto [processKey, processId] = owners[result - WAIT_OBJECT_0];
        OnWatchedInstanceGone(configuration, processKey, processId);
    }
}

std::unordered_set<std::wstring> ProcessMonitor::EtwProcessKeys(const RuntimeConfiguration& configuration) const
{
    auto keys = configuration.watchedProcessKeys;
    for (const auto& [key, rule] : activeRules_)
    {
        // A session survives edits and deletion of its rule, so keep watching it.
        keys.insert(key);
        for (const auto& action : rule.processPerformanceActions)
            keys.insert(NormalizeProcessKey(action.processName));
    }
    return keys;
}

void ProcessMonitor::RefreshEtwProcessKeys(const RuntimeConfiguration& configuration)
{
    if (!usingEtw_.load()) return;
    etwProcessListener_.UpdateWatchedProcessKeys(EtwProcessKeys(configuration));
}

void ProcessMonitor::RestartEtw(const RuntimeConfiguration& configuration)
{
    constexpr int kMaxEtwRestarts = 3;
    etwProcessListener_.Stop();
    std::wstring error = L"The session was restarted too often.";
    if (++etwRestarts_ <= kMaxEtwRestarts && etwProcessListener_.Start(EtwProcessKeys(configuration), error))
    {
        etwInitialSnapshotComplete_.store(false);
        statusCallback_(L"ETW session ended unexpectedly; restarted it and resynchronized process state.");
    }
    else
    {
        usingEtw_.store(false);
        statusCallback_(L"ETW session ended unexpectedly; using process polling. " + error);
    }
    WakeWorker();
}

void ProcessMonitor::ProcessEtwEvents()
{
    using EventType = EtwProcessListener::ProcessEvent::Type;
    std::vector<EtwProcessListener::ProcessEvent> events;
    {
        std::scoped_lock lock(etwEventsMutex_);
        events.swap(pendingEtwEvents_);
    }
    if (events.empty()) return;

    std::shared_ptr<const RuntimeConfiguration> configuration;
    {
        std::scoped_lock lock(mutex_);
        configuration = runtimeConfiguration_;
    }
    if (!configuration) return;

    for (const auto& event : events)
    {
        if (event.type == EventType::EventsLost)
        {
            if (etwInitialSnapshotComplete_.exchange(false))
                statusCallback_(L"ETW reported lost events; resynchronizing process state.");
            WakeWorker();
            continue;
        }
        if (event.type == EventType::SessionEnded)
        {
            RestartEtw(*configuration);
            if (!usingEtw_.load()) return;
            continue;
        }

        const bool started = event.type == EventType::ProcessStarted;
        // ETW reports each start once; apply every active rule's settings to just that
        // instance, without treating it as a full snapshot.
        const auto applyToStartedInstance = [&]
        {
            ProcessSnapshot snapshot;
            snapshot.valid = true;
            snapshot.processIdsByName[event.imageName].push_back(event.processId);
            for (const auto& [key, rule] : activeRules_) ApplyPerformanceActions(rule, snapshot, false);
        };
        if (!configuration->watchedProcessKeys.contains(event.imageName) && !activeRules_.contains(event.imageName))
        {
            // This is a configured performance target, such as a helper
            // started by a Start-program action.  Its ETW start event is the
            // precise moment at which the Windows settings can be applied.
            if (!started)
            {
                for (const auto& [key, rule] : activeRules_)
                {
                    for (size_t index = 0; index < rule.processPerformanceActions.size(); ++index)
                    {
                        if (NormalizeProcessKey(rule.processPerformanceActions[index].processName) != event.imageName) continue;
                        auto ruleState = performanceTargetStates_.find(key);
                        if (ruleState == performanceTargetStates_.end()) continue;
                        auto actionState = ruleState->second.find(index);
                        if (actionState != ruleState->second.end()) actionState->second.erase(event.processId);
                    }
                }
            }
            else applyToStartedInstance();
            continue;
        }

        statusCallback_(L"ETW " + std::wstring(started ? L"start" : L"stop") + L": " +
            event.imageName + L" (PID " + std::to_wstring(event.processId) + L").");
        if (!started)
        {
            OnWatchedInstanceGone(*configuration, event.imageName, event.processId);
            continue;
        }
        if (activeRules_.contains(event.imageName))
        {
            TrackWatchedInstance(event.imageName, event.processId);
            applyToStartedInstance();
            continue;
        }
        const auto rule = std::find_if(configuration->watchedRules.begin(), configuration->watchedRules.end(),
            [&event](const auto& candidate) { return candidate.processKey == event.imageName; });
        if (rule == configuration->watchedRules.end()) continue;
        CacheProcessState(event.imageName, true);
        ActivateRule(*configuration, *rule, {event.processId});
        // Other active rules may list this program as a performance target.
        applyToStartedInstance();
    }
}

void ProcessMonitor::CacheProcessState(const std::wstring& processKey, bool running)
{
    std::scoped_lock lock(processStatesMutex_);
    cachedProcessStates_[processKey] = running;
    cachedProcessStatesKnown_ = true;
    if (!running) runningSince_.erase(processKey);
    else runningSince_.try_emplace(processKey, GetTickCount64());
}

void ProcessMonitor::CacheProcessStates(const RuntimeConfiguration& configuration, const ProcessSnapshot& snapshot)
{
    std::unordered_map<std::wstring, bool> states;
    states.reserve(configuration.watchedRules.size());
    for (const auto& rule : configuration.watchedRules)
        states.emplace(rule.processKey, IsProcessRunning(snapshot, rule.processKey));

    std::scoped_lock lock(processStatesMutex_);
    std::erase_if(runningSince_, [&states](const auto& entry)
    {
        const auto state = states.find(entry.first);
        return state == states.end() || !state->second;
    });
    const auto now = GetTickCount64();
    for (const auto& [processKey, running] : states)
    {
        if (running) runningSince_.try_emplace(processKey, now);
    }
    cachedProcessStates_ = std::move(states);
    cachedProcessStatesKnown_ = true;
}

void ProcessMonitor::FinishRule(const RuntimeRule& rule)
{
    statusCallback_(rule.displayName + L" session ended. Restoring actions.");
    const ULONGLONG exitTick = GetTickCount64();
    if (serviceOwnerKey_ == rule.processKey)
    {
        if (HasPendingIRacingServiceRestore())
        {
            std::wstring report;
            if (!RestoreIRacingServices(report)) ReportProblem(L"Could not restore Windows services: " + report);
        }
        serviceOwnerKey_.clear();
    }
    if (const auto previous = previousPowerSchemes_.find(rule.processKey); previous != previousPowerSchemes_.end())
    {
        if (RestorePowerScheme(previous->second)) previousPowerSchemes_.erase(previous);
        else ReportProblem(L"Could not restore the previous power plan.");
    }
    RestoreMonitorSetupForRule(rule, exitTick);
    StopProgramsForRule(rule);
    ExecuteExitActions(rule, exitTick);
    performanceTargetStates_.erase(rule.processKey);
    watchedInstances_.erase(rule.processKey);
}

void ProcessMonitor::ExecuteStartActions(const RuntimeRule& rule)
{
    if (!rule.processPerformanceActions.empty())
    {
        const auto snapshot = CaptureProcessSnapshot(false);
        if (snapshot.valid) ApplyPerformanceActions(rule, snapshot);
    }
    if (!rule.powerSchemeGuid.empty())
    {
        if (!previousPowerSchemes_.empty())
        {
            ReportProblem(L"Power Plan action skipped: another watched rule currently owns the power plan.");
        }
        else
        {
            GUID previous{};
            if (ActivatePowerScheme(rule.powerSchemeGuid, previous)) previousPowerSchemes_[rule.processKey] = previous;
            else ReportProblem(L"Could not activate the selected power plan.");
        }
    }
    if (rule.hasMonitorPowerSetup && !displayOwnerKey_.empty() && displayOwnerKey_ != rule.processKey)
    {
        // Two rules switching displays would restore each other's arrangement.
        ReportProblem(L"Display configuration skipped for " + rule.displayName + L": another watched rule currently controls the displays.");
    }
    else if (rule.hasMonitorPowerSetup)
    {
        const DWORD applyDelay = static_cast<DWORD>(std::max(0, rule.monitorPowerSetupDelayMilliseconds));
        if (!WaitForDelay(applyDelay)) return;

        MonitorPowerSetup previousSetup;
        std::wstring errorMessage;
        bool canApply = true;
        if (rule.restoreMonitorPowerSetupOnExit)
        {
            previousSetup.name = L"Previous display configuration";
            if (MonitorPowerController::CaptureSetup(previousSetup, &errorMessage))
            {
                std::scoped_lock lock(mutex_);
                previousMonitorSetups_[rule.processKey] = std::move(previousSetup);
            }
            else
            {
                // Without the current arrangement the displays could not be put back.
                ReportProblem(L"Display configuration not changed: the current arrangement could not be saved for restoring it later. " + errorMessage);
                canApply = false;
            }
        }
        if (canApply)
        {
            displayOwnerKey_ = rule.processKey;
            errorMessage.clear();
            if (!MonitorPowerController::ApplySetup(rule.monitorPowerSetup, {}, &errorMessage))
            {
                ReportProblem(L"Could not apply monitor config " + rule.monitorPowerSetup.name + L": " + errorMessage);
            }
        }
    }

    if (running_.load() && !rule.processesToStop.empty()) StopConfiguredProcesses(rule);

    std::vector<const HomeAssistantAction*> webhooks;
    webhooks.reserve(rule.homeAssistantActions.size());
    for (const auto& action : rule.homeAssistantActions) webhooks.push_back(&action);
    std::stable_sort(webhooks.begin(), webhooks.end(), [](const auto* left, const auto* right)
    {
        return left->waitTimeMilliseconds < right->waitTimeMilliseconds;
    });
    const ULONGLONG startTick = GetTickCount64();
    for (const auto* action : webhooks)
    {
        const DWORD delay = static_cast<DWORD>(std::max(0, action->waitTimeMilliseconds));
        const ULONGLONG elapsed = GetTickCount64() - startTick;
        if (!WaitForDelay(elapsed < delay ? static_cast<DWORD>(delay - elapsed) : 0)) return;
        if (!PostWebhook(*action)) ReportProblem(L"Home Assistant webhook failed: " + action->displayName);
    }

    if (!running_.load()) return;
    if (!rule.servicesToStop.empty())
    {
        if (!serviceOwnerKey_.empty()) ReportProblem(L"Services action skipped: another watched rule currently owns the service state.");
        else
        {
            std::wstring report;
            if (!ApplyIRacingServices(rule.servicesToStop, report))
                ReportProblem(L"Windows service action: " + report);
            // Partial success also leaves services that must be restored at session end.
            if (HasPendingIRacingServiceRestore()) serviceOwnerKey_ = rule.processKey;
        }
    }
    StartProgramsForRule(rule);
}

void ProcessMonitor::ApplyPerformanceActions(const RuntimeRule& rule, const ProcessSnapshot& snapshot, bool complete)
{
    constexpr unsigned kCpuPriorityApplied = 1u << 0;
    constexpr unsigned kIoPriorityApplied = 1u << 1;
    constexpr unsigned kMemoryPriorityApplied = 1u << 2;
    constexpr unsigned kAffinityApplied = 1u << 3;
    constexpr unsigned kEfficiencyApplied = 1u << 4;

    auto& ruleStates = performanceTargetStates_[rule.processKey];
    for (size_t actionIndex = 0; actionIndex < rule.processPerformanceActions.size(); ++actionIndex)
    {
        const auto& action = rule.processPerformanceActions[actionIndex];
        if (action.ChangesNothing()) continue;

        const auto targetKey = NormalizeProcessKey(action.processName);
        const auto found = snapshot.processIdsByName.find(targetKey);
        auto& processStates = ruleStates[actionIndex];
        if (found == snapshot.processIdsByName.end())
        {
            if (complete) processStates.clear();
            continue;
        }
        std::unordered_set<DWORD> stillRunning(found->second.begin(), found->second.end());
        for (const DWORD processId : found->second)
        {
            auto& state = processStates[processId];
            if (state.successReported || state.attempts >= kMaxPerformanceAttempts) continue;
            // Some refusals are permanent (protected processes, unsupported values);
            // retrying them on every snapshot would only repeat the same failure.
            const bool lastAttempt = ++state.attempts == kMaxPerformanceAttempts;
            const auto reportGiveUp = [&]
            {
                if (lastAttempt)
                    ReportProblem(state.lastFailure + L" Gave up after " + std::to_wstring(kMaxPerformanceAttempts) +
                        L" attempts (PID " + std::to_wstring(processId) + L").");
            };
            HANDLE process = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
            if (!process)
            {
                const std::wstring failure = L"Could not open " + action.processName + L" for performance settings (Windows error " +
                    std::to_wstring(GetLastError()) + L").";
                if (state.lastFailure != failure) statusCallback_(failure);
                state.lastFailure = failure;
                reportGiveUp();
                continue;
            }
            std::vector<std::wstring> failures;
            if (action.cpuPriorityClass != 0 && (state.appliedSettings & kCpuPriorityApplied) == 0)
            {
                if (SetPriorityClass(process, static_cast<DWORD>(action.cpuPriorityClass))) state.appliedSettings |= kCpuPriorityApplied;
                else failures.push_back(L"CPU priority (Windows error " + std::to_wstring(GetLastError()) + L")");
            }
            if (action.ioPriority >= 0 && (state.appliedSettings & kIoPriorityApplied) == 0)
            {
                DWORD privilegeError = ERROR_SUCCESS;
                const bool needsPrivilege = action.ioPriority >= 3;
                if (needsPrivilege && !EnableIncreaseBasePriorityPrivilege(privilegeError))
                {
                    failures.push_back(L"I/O priority " + IoPriorityName(action.ioPriority) +
                        L" (could not enable SeIncreaseBasePriorityPrivilege; Windows error " + std::to_wstring(privilegeError) + L")");
                }
                else
                {
                    const LONG result = SetProcessIoPriority(process, action.ioPriority);
                    if (result < 0)
                        failures.push_back(L"I/O priority " + IoPriorityName(action.ioPriority) + L" (" + NtStatusDescription(result) + L")");
                    else state.appliedSettings |= kIoPriorityApplied;
                }
            }
            if (action.memoryPriority >= 0 && (state.appliedSettings & kMemoryPriorityApplied) == 0)
            {
                MEMORY_PRIORITY_INFORMATION memory{};
                memory.MemoryPriority = static_cast<ULONG>(action.memoryPriority);
                if (!SetProcessInformation(process, ProcessMemoryPriority, &memory, sizeof(memory)))
                    failures.push_back(L"Memory priority " + MemoryPriorityName(action.memoryPriority) +
                        L" (Windows error " + std::to_wstring(GetLastError()) + L")");
                else state.appliedSettings |= kMemoryPriorityApplied;
            }
            if (action.affinityMask != 0 && (state.appliedSettings & kAffinityApplied) == 0)
            {
                if (static_cast<std::uint64_t>(static_cast<DWORD_PTR>(action.affinityMask)) != action.affinityMask)
                    failures.push_back(L"CPU affinity (it uses CPUs that the 32-bit LaunchMate cannot address)");
                else if (!SetProcessAffinityMask(process, static_cast<DWORD_PTR>(action.affinityMask)))
                    failures.push_back(L"CPU affinity (Windows error " + std::to_wstring(GetLastError()) + L")");
                else state.appliedSettings |= kAffinityApplied;
            }
            if (action.efficiencyMode >= 0 && (state.appliedSettings & kEfficiencyApplied) == 0)
            {
                // Setting the control bit without the state bit keeps Windows from
                // throttling the process on its own.
                PROCESS_POWER_THROTTLING_STATE throttling{};
                throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
                throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
                throttling.StateMask = action.efficiencyMode == 1 ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
                if (!SetProcessInformation(process, ProcessPowerThrottling, &throttling, sizeof(throttling)))
                    failures.push_back(L"Efficiency mode (Windows error " + std::to_wstring(GetLastError()) + L")");
                else state.appliedSettings |= kEfficiencyApplied;
            }
            CloseHandle(process);
            if (failures.empty())
            {
                const unsigned requestedSettings =
                    (action.cpuPriorityClass != 0 ? kCpuPriorityApplied : 0) |
                    (action.ioPriority >= 0 ? kIoPriorityApplied : 0) |
                    (action.memoryPriority >= 0 ? kMemoryPriorityApplied : 0) |
                    (action.affinityMask != 0 ? kAffinityApplied : 0) |
                    (action.efficiencyMode >= 0 ? kEfficiencyApplied : 0);
                if (!state.successReported && state.appliedSettings == requestedSettings)
                {
                    statusCallback_(L"Applied performance settings to " + action.processName + L".");
                    state.lastFailure.clear();
                    state.successReported = true;
                }
            }
            else
            {
                std::wstring message = L"Performance settings partly applied to " + action.processName + L"; failed: ";
                for (size_t index = 0; index < failures.size(); ++index)
                {
                    if (index != 0) message += L"; ";
                    message += failures[index];
                }
                message += L".";
                if (state.lastFailure != message) statusCallback_(message);
                state.lastFailure = std::move(message);
                state.successReported = false;
                reportGiveUp();
            }
        }
        if (complete)
            for (auto it = processStates.begin(); it != processStates.end();)
                if (!stillRunning.contains(it->first)) it = processStates.erase(it); else ++it;
    }
}

void ProcessMonitor::RestoreMonitorSetupForRule(const RuntimeRule& rule, ULONGLONG exitTick)
{
    if (displayOwnerKey_ != rule.processKey) return;
    MonitorPowerSetup previousSetup;
    {
        std::scoped_lock lock(mutex_);
        const auto monitorIt = previousMonitorSetups_.find(rule.processKey);
        if (monitorIt == previousMonitorSetups_.end())
        {
            displayOwnerKey_.clear(); // No restore wanted; the displays are free again.
            return;
        }
        previousSetup = monitorIt->second;
    }

    const DWORD restoreDelay = static_cast<DWORD>(std::max(0, rule.restoreMonitorPowerSetupDelayMilliseconds));
    const ULONGLONG elapsed = GetTickCount64() - exitTick;
    if (elapsed < restoreDelay) WaitForDelay(static_cast<DWORD>(restoreDelay - elapsed));

    // Monitors that are just waking up can refuse the first attempt.
    std::wstring errorMessage;
    bool restored = MonitorPowerController::ApplySetup(previousSetup, {}, &errorMessage);
    if (!restored && WaitForDelay(2000))
    {
        errorMessage.clear();
        restored = MonitorPowerController::ApplySetup(previousSetup, {}, &errorMessage);
    }
    if (!restored) ReportProblem(L"Could not restore the previous monitor configuration: " + errorMessage);
    {
        std::scoped_lock lock(mutex_);
        previousMonitorSetups_.erase(rule.processKey);
    }
    displayOwnerKey_.clear();
}

void ProcessMonitor::ExecuteExitActions(const RuntimeRule& rule, ULONGLONG exitTick)
{
    std::vector<StoppedProcessRecord> restartRecords;
    {
        std::scoped_lock lock(mutex_);
        const auto processIt = stoppedProcesses_.find(rule.processKey);
        if (processIt == stoppedProcesses_.end()) return;
        restartRecords = std::move(processIt->second);
        stoppedProcesses_.erase(processIt);
    }

    std::stable_sort(restartRecords.begin(), restartRecords.end(), [](const auto& left, const auto& right)
    {
        return left.action.restartDelayMilliseconds < right.action.restartDelayMilliseconds;
    });
    for (const auto& record : restartRecords)
    {
        const auto& action = record.action;
        const DWORD delay = static_cast<DWORD>(std::max(0, action.restartDelayMilliseconds));
        const ULONGLONG elapsed = GetTickCount64() - exitTick;
        if (elapsed < delay) WaitForDelay(static_cast<DWORD>(delay - elapsed));

        const auto processKey = NormalizeProcessKey(action.processName);
        const std::unordered_set<std::wstring> filter{processKey};
        const auto snapshot = CaptureProcessSnapshot(false, &filter);
        if (!snapshot.valid) continue;
        const auto existing = snapshot.processIdsByName.find(processKey);
        if (existing != snapshot.processIdsByName.end() && std::any_of(existing->second.begin(), existing->second.end(), [&record](DWORD processId)
        {
            return PathMatchesProcess(processId, record.executablePath);
        }))
        {
            continue;
        }

        LaunchProgram program;
        program.displayName = action.displayName;
        program.filePath = record.executablePath;
        program.arguments = record.arguments;
        if (!LaunchProgramProcess(program).success)
        {
            ReportProblem(L"Could not restart process: " + action.displayName +
                L" (Windows error " + std::to_wstring(GetLastError()) + L"; " + launchStage + L")");
        }
        else statusCallback_(L"Restarted process: " + action.displayName + L".");
    }
}

void ProcessMonitor::StopConfiguredProcesses(const RuntimeRule& rule)
{
    struct Instance
    {
        DWORD processId{};
        DWORD parentProcessId{};
    };
    std::unordered_set<std::wstring> wantedKeys;
    for (const auto& action : rule.processesToStop) wantedKeys.insert(NormalizeProcessKey(action.processName));

    // One snapshot for all actions; every target is then closed in a single batch.
    std::unordered_map<std::wstring, std::vector<Instance>> instancesByKey;
    HANDLE processSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processSnapshot == INVALID_HANDLE_VALUE)
    {
        ReportProblem(L"Could not list running processes for the stop actions.");
        return;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(processSnapshot, &entry))
    {
        do
        {
            if (entry.th32ProcessID == GetCurrentProcessId()) continue;
            const auto key = NormalizeProcessKey(entry.szExeFile);
            if (wantedKeys.contains(key)) instancesByKey[key].push_back({entry.th32ProcessID, entry.th32ParentProcessID});
        }
        while (Process32NextW(processSnapshot, &entry));
    }
    CloseHandle(processSnapshot);

    const auto& actions = rule.processesToStop;
    std::vector<CloseTarget> targets;
    std::vector<bool> failed(actions.size());
    std::vector<StoppedProcessRecord> restartRecords(actions.size());
    for (size_t index = 0; index < actions.size(); ++index)
    {
        const auto& action = actions[index];
        const auto found = instancesByKey.find(NormalizeProcessKey(action.processName));
        if (found == instancesByKey.end()) continue;
        const auto expectedPath = NormalizePath(action.executablePath);
        std::vector<std::pair<Instance, std::shared_ptr<void>>> matched;
        for (const auto& instance : found->second)
        {
            HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
                FALSE, instance.processId);
            if (!handle)
            {
                // Without a configured path the instance is a target we cannot stop.
                if (expectedPath.empty() && GetLastError() != ERROR_INVALID_PARAMETER) failed[index] = true;
                continue;
            }
            std::shared_ptr<void> process(handle, CloseHandle);
            if (!expectedPath.empty())
            {
                const auto actualPath = NormalizePath(QueryProcessImagePath(handle));
                if (actualPath.empty() || _wcsicmp(actualPath.c_str(), expectedPath.c_str()) != 0) continue;
            }
            matched.emplace_back(instance, std::move(process));
        }
        if (matched.empty()) continue;

        if (action.restartAfterWatchProcessEnds)
        {
            // Multi-process apps start helpers from their main instance; the main one is
            // the instance whose parent is not another matched instance.
            std::unordered_set<DWORD> matchedIds;
            for (const auto& [instance, process] : matched) matchedIds.insert(instance.processId);
            const auto main = std::find_if(matched.begin(), matched.end(), [&matchedIds](const auto& item)
            {
                return !matchedIds.contains(item.first.parentProcessId);
            });
            const auto mainProcess = (main != matched.end() ? *main : matched.front()).second.get();
            auto& record = restartRecords[index];
            record.action = action;
            record.executablePath = QueryProcessImagePath(mainProcess);
            if (record.executablePath.empty()) record.executablePath = action.executablePath;
            record.arguments = ArgumentsFromCommandLine(QueryProcessCommandLine(mainProcess));
        }
        const DWORD grace = action.gracefulCloseFirst ? static_cast<DWORD>(std::max(0, action.forceAfterMilliseconds)) : 0;
        for (auto& [instance, process] : matched) targets.push_back({std::move(process), grace, index});
    }

    const auto exited = CloseProcesses(targets);
    std::vector<bool> stopped(actions.size());
    for (size_t index = 0; index < targets.size(); ++index)
    {
        if (exited[index]) stopped[targets[index].group] = true;
        else failed[targets[index].group] = true;
    }

    std::vector<StoppedProcessRecord> stoppedForRestart;
    for (size_t index = 0; index < actions.size(); ++index)
    {
        if (failed[index]) ReportProblem(L"Could not stop " + actions[index].processName + L".");
        else if (stopped[index] && actions[index].restartAfterWatchProcessEnds)
        {
            if (restartRecords[index].executablePath.empty())
                ReportProblem(L"Cannot restart " + actions[index].processName + L" later: its executable path is unknown.");
            else stoppedForRestart.push_back(std::move(restartRecords[index]));
        }
    }
    if (!stoppedForRestart.empty())
    {
        std::scoped_lock lock(mutex_);
        stoppedProcesses_[rule.processKey] = std::move(stoppedForRestart);
    }
}

void ProcessMonitor::StartProgramsForRule(const RuntimeRule& rule)
{
    std::vector<LaunchedProgramRecord> records;
    std::vector<const LaunchProgram*> scheduledPrograms;
    scheduledPrograms.reserve(rule.programsToLaunch.size());

    for (const auto& program : rule.programsToLaunch)
    {
        scheduledPrograms.push_back(&program);
    }

    std::stable_sort(
        scheduledPrograms.begin(),
        scheduledPrograms.end(),
        [](const LaunchProgram* left, const LaunchProgram* right)
        {
            return left->waitTimeMilliseconds < right->waitTimeMilliseconds;
        });

    const ULONGLONG scheduleStartTick = GetTickCount64();

    for (const auto* scheduledProgram : scheduledPrograms)
    {
        if (!running_.load()) break;
        const auto& program = *scheduledProgram;
        std::error_code pathError;
        if (program.filePath.empty() || !std::filesystem::exists(program.filePath, pathError))
        {
            ReportProblem(L"Program file not found: " + program.filePath);
            continue;
        }

        const DWORD scheduledDelayMs = program.waitTimeMilliseconds > 0 ? static_cast<DWORD>(program.waitTimeMilliseconds) : 0;
        const ULONGLONG elapsedMs = GetTickCount64() - scheduleStartTick;
        if (elapsedMs < scheduledDelayMs)
        {
            if (!WaitForDelay(static_cast<DWORD>(scheduledDelayMs - elapsedMs))) break;
        }

        if (!program.closeWhenGameStops)
        {
            if (!LaunchProgramProcess(program).success)
                ReportProblem(L"Could not launch program: " + program.displayName +
                    L" (Windows error " + std::to_wstring(GetLastError()) + L"; " + launchStage + L")");
            continue; // No ownership tracking or settling delay is needed.
        }

        const auto normalizedPath = NormalizePath(program.filePath);
        const auto processKey = NormalizeProcessKey(std::filesystem::path(program.filePath).stem().wstring());
        const std::unordered_set<std::wstring> processKeyFilter =
            processKey.empty() ? std::unordered_set<std::wstring>{} : std::unordered_set<std::wstring>{processKey};

        // A shell or packaged-app launcher may hand off to a different executable.
        // Keep a complete before/after snapshot so that process can be owned too.
        const auto beforeSnapshot = CaptureProcessSnapshot(false);
        auto existing = FindMatchingProcesses(beforeSnapshot, program.filePath, normalizedPath);
        FILETIME launchTime{};
        GetSystemTimePreciseAsFileTime(&launchTime);
        std::shared_ptr<void> rootProcess;
        const auto launch = LaunchProgramProcess(program, &rootProcess);
        const DWORD launchedRootProcessId = launch.processId;
        if (!launch.success)
        {
            ReportProblem(L"Could not launch program: " + program.displayName +
                L" (Windows error " + std::to_wstring(GetLastError()) + L"; " + launchStage + L")");
            continue;
        }
        WaitForDelay(kProgramLaunchSettleMs);

        const auto afterSnapshot = CaptureProcessSnapshot(true);
        const bool snapshotsValid = beforeSnapshot.valid && afterSnapshot.valid;

        LaunchedProgramRecord record;
        record.program = program;
        record.executablePath = normalizedPath.empty() ? program.filePath : normalizedPath;
        record.existingProcessIds = std::move(existing);
        record.launchTime = launchTime;
        record.rootProcessKnown = rootProcess != nullptr;
        if (rootProcess)
        {
            record.startedProcessIds.insert(launchedRootProcessId);
            record.startedProcessHandles.push_back(std::move(rootProcess));
        }
        const auto adopt = [&](DWORD pid)
        {
            if (record.startedProcessIds.contains(pid) || record.existingProcessIds.contains(pid) ||
                pid == GetCurrentProcessId()) return;
            HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
            if (!handle) return;
            std::shared_ptr<void> process(handle, CloseHandle);
            // A stale parent PID or a PID reused since the snapshot does not make a process ours.
            if (!CreatedAtOrAfter(handle, launchTime)) return;
            record.startedProcessIds.insert(pid);
            record.startedProcessHandles.push_back(std::move(process));
        };
        const auto adoptWithDescendants = [&](DWORD pid)
        {
            adopt(pid);
            for (const auto descendant : BuildChildProcessSet(afterSnapshot, pid)) adopt(descendant);
        };

        // Ownership follows ancestry: the launched process, its descendants (also when
        // the launcher exited after a hand-off) and new instances of the configured file.
        if (launchedRootProcessId != 0) adoptWithDescendants(launchedRootProcessId);
        if (snapshotsValid)
        {
            for (const auto pid : FindMatchingProcesses(afterSnapshot, program.filePath, normalizedPath))
            {
                if (!record.existingProcessIds.contains(pid)) adoptWithDescendants(pid);
            }
        }
        const auto countLive = [&record]
        {
            return std::count_if(record.startedProcessHandles.begin(), record.startedProcessHandles.end(),
                [](const auto& process) { return WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT; });
        };
        auto liveCount = countLive();

        // Brokered launches (packaged apps, DDE) leave no running process in that tree.
        // Only then fall back to new windowed processes from the settle window, which
        // could also include an unrelated program the user opened at the same moment.
        if (liveCount == 0 && snapshotsValid)
        {
            for (const auto& [name, processIds] : afterSnapshot.processIdsByName)
            {
                const auto before = beforeSnapshot.processIdsByName.find(name);
                for (const auto pid : processIds)
                {
                    const bool existed = before != beforeSnapshot.processIdsByName.end() &&
                        std::find(before->second.begin(), before->second.end(), pid) != before->second.end();
                    if (!existed && HasVisibleTopLevelWindow(pid)) adoptWithDescendants(pid);
                }
            }
            liveCount = countLive();
        }
        statusCallback_(L"Started " + program.displayName + L"; tracking " + std::to_wstring(liveCount) +
            L" running process(es) for closing.");
        if (liveCount == 0)
            ReportProblem(L"Cannot track the running app for " + program.displayName +
                L". Its launcher may have handed off to another process; automatic closing is unavailable.");
        records.push_back(std::move(record));
    }

    std::scoped_lock lock(mutex_);
    startedPrograms_[rule.processKey] = std::move(records);
}

void ProcessMonitor::StopProgramsForRule(const RuntimeRule& rule)
{
    std::vector<LaunchedProgramRecord> records;
    {
        std::scoped_lock lock(mutex_);
        const auto it = startedPrograms_.find(rule.processKey);
        if (it == startedPrograms_.end())
        {
            return;
        }

        records = std::move(it->second);
        startedPrograms_.erase(it);
    }

    const auto closeDelay = [](const LaunchedProgramRecord& record)
    {
        return static_cast<DWORD>(std::max(0, record.program.closeDelayMilliseconds));
    };
    std::stable_sort(records.begin(), records.end(), [&closeDelay](const auto& left, const auto& right)
    {
        return closeDelay(left) < closeDelay(right);
    });

    // Retained handles pin the original process identities. Include children created
    // later, but never unrelated instances with the same executable.
    const auto ownedProcesses = [this](const LaunchedProgramRecord& record)
    {
        auto owned = record.startedProcessHandles;
        std::unordered_set<DWORD> ownedIds;
        for (const auto& process : owned) ownedIds.insert(GetProcessId(process.get()));
        FILETIME captureTime{};
        // Precise: the coarse clock can lag a process creation time by up to a timer
        // tick (about 15.6 ms), which would hide a process that has just started.
        GetSystemTimePreciseAsFileTime(&captureTime);
        const std::unordered_set<std::wstring> names{
            NormalizeProcessKey(std::filesystem::path(record.executablePath).filename().wstring())};
        const auto snapshot = CaptureProcessSnapshot(true, &names);
        // Without a launched root process (shell/DDE launches) the application can
        // only be found by path. Instances started before the launch are excluded.
        if (snapshot.valid && !record.rootProcessKnown &&
            (record.launchTime.dwHighDateTime || record.launchTime.dwLowDateTime))
        {
            for (const auto pid : FindMatchingProcesses(snapshot, record.executablePath, record.executablePath))
            {
                if (pid == GetCurrentProcessId() || ownedIds.contains(pid) || record.existingProcessIds.contains(pid)) continue;
                HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
                if (!handle) continue;
                std::shared_ptr<void> candidate(handle, CloseHandle);
                FILETIME created{}, exited{}, kernel{}, user{};
                if (!GetProcessTimes(handle, &created, &exited, &kernel, &user) ||
                    CompareFileTime(&created, &record.launchTime) < 0 || CompareFileTime(&created, &captureTime) > 0 ||
                    !PathMatchesProcess(pid, record.executablePath)) continue;
                ownedIds.insert(pid);
                owned.push_back(std::move(candidate));
            }
        }
        // Children of owned processes, e.g. the real app behind a launcher that has
        // exited, or an app that restarted itself.
        if (snapshot.valid) for (size_t index = 0; index < owned.size(); ++index)
        {
            const auto parent = owned[index];
            FILETIME parentCreated{}, exited{}, kernel{}, user{};
            if (!GetProcessTimes(parent.get(), &parentCreated, &exited, &kernel, &user)) continue;
            const auto children = snapshot.childrenByParent.find(GetProcessId(parent.get()));
            if (children == snapshot.childrenByParent.end()) continue;
            for (const auto pid : children->second)
            {
                if (pid == GetCurrentProcessId() || ownedIds.contains(pid) || record.existingProcessIds.contains(pid)) continue;
                HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
                if (!handle) continue;
                std::shared_ptr<void> child(handle, CloseHandle);
                FILETIME created{};
                if (!GetProcessTimes(handle, &created, &exited, &kernel, &user) ||
                    CompareFileTime(&created, &parentCreated) < 0 || CompareFileTime(&created, &captureTime) > 0) continue;
                ownedIds.insert(pid);
                owned.push_back(std::move(child));
            }
        }
        return owned;
    };

    const ULONGLONG scheduleStartTick = GetTickCount64();
    for (size_t first = 0; first < records.size();)
    {
        // Programs sharing a close delay are closed together, so their grace periods overlap.
        size_t last = first;
        while (last < records.size() && closeDelay(records[last]) == closeDelay(records[first])) ++last;

        const ULONGLONG elapsedMs = GetTickCount64() - scheduleStartTick;
        if (elapsedMs < closeDelay(records[first]))
        {
            WaitForDelay(static_cast<DWORD>(closeDelay(records[first]) - elapsedMs));
        }

        std::vector<CloseTarget> targets;
        for (size_t index = first; index < last; ++index)
        {
            if (!records[index].program.closeWhenGameStops) continue;
            for (auto& process : ownedProcesses(records[index]))
            {
                if (WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT)
                    targets.push_back({std::move(process), kLaunchedProgramCloseGraceMs, index});
            }
        }
        const auto exited = CloseProcesses(targets);
        for (size_t index = 0; index < targets.size(); ++index)
        {
            if (exited[index]) continue;
            ReportProblem(L"Could not close " + records[targets[index].group].program.displayName +
                L" (PID " + std::to_wstring(GetProcessId(targets[index].process.get())) + L").");
        }
        first = last;
    }
}

void ProcessMonitor::WakeWorker() noexcept
{
    if (wakeEvent_ != nullptr)
    {
        SetEvent(wakeEvent_);
    }
}
bool ProcessMonitor::WaitForDelay(DWORD milliseconds) const
{
    if (!running_.load()) return false;
    if (stopEvent_) return WaitForSingleObject(stopEvent_, milliseconds) == WAIT_TIMEOUT && running_.load();
    const auto deadline = GetTickCount64() + milliseconds;
    while (running_.load())
    {
        const auto now = GetTickCount64();
        if (now >= deadline) return true;
        Sleep(static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, 50)));
    }
    return false;
}
