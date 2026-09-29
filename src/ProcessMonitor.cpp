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

namespace
{
    constexpr DWORD kProgramLaunchSettleMs = 1200;
    constexpr DWORD kMinimumPollIntervalMs = 100;
    constexpr DWORD kMaximumPollIntervalMs = 300000;
    thread_local const wchar_t* launchStage = L"launch";

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
    ProgramLaunchResult LaunchProgramProcess(const LaunchProgram& program, std::shared_ptr<void>* launchedProcess = nullptr)
    {
        SHELLEXECUTEINFOW info{};
        launchStage = L"shell execute";
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS;
        info.lpFile = program.filePath.c_str();
        info.lpParameters = program.arguments.empty() ? nullptr : program.arguments.c_str();
        info.nShow = SW_SHOWNORMAL;
        info.lpVerb = L"open";
        const auto directory = std::filesystem::path(program.filePath).parent_path().wstring();
        info.lpDirectory = directory.empty() ? nullptr : directory.c_str();

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

    struct CloseWindowsContext
    {
        DWORD processId{};
        bool found{};
    };

    BOOL CALLBACK CloseProcessWindows(HWND window, LPARAM parameter)
    {
        auto& context = *reinterpret_cast<CloseWindowsContext*>(parameter);
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (processId == context.processId && GetWindow(window, GW_OWNER) == nullptr)
        {
            context.found = true;
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
        return TRUE;
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

    bool StopConfiguredProcess(const ProcessStopAction& action)
    {
        const auto processKey = NormalizeProcessKey(action.processName);
        if (processKey.empty()) return false;
        const auto expectedPath = NormalizePath(action.executablePath);
        std::vector<std::shared_ptr<void>> processes;
        bool allStopped = true;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                if (entry.th32ProcessID == GetCurrentProcessId() ||
                    NormalizeProcessKey(entry.szExeFile) != processKey) continue;
                HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
                    FALSE, entry.th32ProcessID);
                if (!handle)
                {
                    if (expectedPath.empty()) allStopped = false;
                    continue;
                }
                std::shared_ptr<void> process(handle, CloseHandle);
                if (!expectedPath.empty())
                {
                    const auto actualPath = NormalizePath(QueryProcessImagePath(handle));
                    if (actualPath.empty() || _wcsicmp(actualPath.c_str(), expectedPath.c_str()) != 0) continue;
                }
                processes.push_back(std::move(process));
            }
            while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);

        // Hold the original handles throughout: Windows may otherwise reuse a PID.
        if (action.gracefulCloseFirst)
        {
            for (const auto& process : processes)
            {
                if (WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) continue;
                CloseWindowsContext context{GetProcessId(process.get())};
                EnumWindows(CloseProcessWindows, reinterpret_cast<LPARAM>(&context));
            }
            const ULONGLONG deadline = GetTickCount64() + static_cast<DWORD>(std::max(0, action.forceAfterMilliseconds));
            for (const auto& process : processes)
            {
                const auto now = GetTickCount64();
                WaitForSingleObject(process.get(), now < deadline ? static_cast<DWORD>(deadline - now) : 0);
            }
        }
        for (const auto& process : processes)
        {
            if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) continue;
            if (!TerminateProcess(process.get(), 0) &&
                WaitForSingleObject(process.get(), 0) != WAIT_OBJECT_0) allStopped = false;
        }
        const auto deadline = GetTickCount64() + 4000;
        for (const auto& process : processes)
        {
            const auto now = GetTickCount64();
            if (WaitForSingleObject(process.get(), now < deadline ? static_cast<DWORD>(deadline - now) : 0)
                != WAIT_OBJECT_0) allStopped = false;
        }
        return !processes.empty() && allStopped;
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
        HINTERNET session = WinHttpOpen(L"LaunchMate/0.2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
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
      stopEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
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

void ProcessMonitor::UpdateConfiguration(const AppConfiguration& configuration)
{
    auto prepared = std::make_shared<RuntimeConfiguration>();
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
    if (running_.exchange(true))
    {
        return;
    }

    if (stopEvent_) ResetEvent(stopEvent_);
    worker_ = std::thread([this] { WorkerLoop(); });
    statusCallback_(L"Monitoring active.");
}

void ProcessMonitor::Stop()
{
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

    for (const auto& [key, rule] : activeRules_) FinishRule(rule);
    for (const auto& [processKey, previous] : previousPowerSchemes_)
    {
        if (!RestorePowerScheme(previous)) statusCallback_(L"Could not restore the previous power plan for " + processKey + L".");
    }
    previousPowerSchemes_.clear();
    serviceOwnerKey_.clear();
    activeRules_.clear();
    if (HasPendingIRacingServiceRestore())
    {
        std::wstring serviceReport;
        if (!RestoreIRacingServices(serviceReport))
            statusCallback_(L"Monitoring stopped; Windows services still need restoration: " + serviceReport);
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
    else statusCallback_(L"Windows services still need restoration. Run LaunchMate as Administrator. " + report);
}

bool ProcessMonitor::IsRunning() const noexcept
{
    return running_.load();
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
    while (running_.load())
    {
        CheckRules();

        if (!running_.load())
        {
            break;
        }

        std::shared_ptr<const RuntimeConfiguration> runtimeConfiguration;
        {
            std::scoped_lock lock(mutex_);
            runtimeConfiguration = runtimeConfiguration_;
        }

        DWORD waitDurationMs = idlePollIntervalMs_.load();
        if ((!runtimeConfiguration || runtimeConfiguration->watchedRules.empty()) && activeRules_.empty())
        {
            waitDurationMs = INFINITE;
        }
        else if (!activeRules_.empty())
        {
            waitDurationMs = activePollIntervalMs_.load();
        }

        if (wakeEvent_ != nullptr)
        {
            WaitForSingleObject(wakeEvent_, waitDurationMs);
        }
        else
        {
            Sleep(std::min<DWORD>(waitDurationMs, kMinimumPollIntervalMs));
        }
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
    for (const auto& [key, rule] : activeRules_) keys.insert(key);
    const auto snapshot = CaptureProcessSnapshot(false, &keys);
    ApplySnapshot(*runtimeConfiguration, snapshot);
}

void ProcessMonitor::ApplySnapshot(const RuntimeConfiguration& configuration, const ProcessSnapshot& snapshot)
{
    // A failed enumeration is not evidence that a watched program has exited.
    if (!snapshot.valid) return;

    CacheProcessStates(configuration, snapshot);

    for (auto it = activeRules_.begin(); it != activeRules_.end();)
    {
        if (!IsProcessRunning(snapshot, it->first))
        {
            FinishRule(it->second);
            it = activeRules_.erase(it);
        }
        else ++it;
    }

    for (const auto& rule : configuration.watchedRules)
    {
        if (!running_.load()) break;
        if (IsProcessRunning(snapshot, rule.processKey) && !activeRules_.contains(rule.processKey))
        {
            activeRules_.emplace(rule.processKey, rule);
            statusCallback_(rule.displayName + L" detected. Running actions.");
            ExecuteStartActions(rule);
        }
    }
}

void ProcessMonitor::CacheProcessStates(const RuntimeConfiguration& configuration, const ProcessSnapshot& snapshot)
{
    std::unordered_map<std::wstring, bool> states;
    states.reserve(configuration.watchedRules.size());
    for (const auto& rule : configuration.watchedRules)
        states.emplace(rule.processKey, IsProcessRunning(snapshot, rule.processKey));

    std::scoped_lock lock(processStatesMutex_);
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
            if (!RestoreIRacingServices(report)) statusCallback_(L"Could not restore Windows services: " + report);
        }
        serviceOwnerKey_.clear();
    }
    if (const auto previous = previousPowerSchemes_.find(rule.processKey); previous != previousPowerSchemes_.end())
    {
        if (RestorePowerScheme(previous->second)) previousPowerSchemes_.erase(previous);
        else statusCallback_(L"Could not restore the previous power plan.");
    }
    RestoreMonitorSetupForRule(rule, exitTick);
    StopProgramsForRule(rule);
    ExecuteExitActions(rule, exitTick);
}

void ProcessMonitor::ExecuteStartActions(const RuntimeRule& rule)
{
    if (!rule.powerSchemeGuid.empty())
    {
        if (!previousPowerSchemes_.empty())
        {
            statusCallback_(L"Power Plan action skipped: another watched rule currently owns the power plan.");
        }
        else
        {
            GUID previous{};
            if (ActivatePowerScheme(rule.powerSchemeGuid, previous)) previousPowerSchemes_[rule.processKey] = previous;
            else statusCallback_(L"Could not activate the selected power plan.");
        }
    }
    if (rule.hasMonitorPowerSetup)
    {
        const DWORD applyDelay = static_cast<DWORD>(std::max(0, rule.monitorPowerSetupDelayMilliseconds));
        if (!WaitForDelay(applyDelay)) return;

        MonitorPowerSetup previousSetup;
        std::wstring errorMessage;
        bool capturedPrevious = false;
        if (rule.restoreMonitorPowerSetupOnExit)
        {
            previousSetup.name = L"Previous display configuration";
            capturedPrevious = MonitorPowerController::CaptureSetup(previousSetup, &errorMessage);
            if (!capturedPrevious)
            {
                statusCallback_(L"Could not capture the current monitor configuration: " + errorMessage);
            }
        }

        if (capturedPrevious)
        {
            std::scoped_lock lock(mutex_);
            previousMonitorSetups_[rule.processKey] = std::move(previousSetup);
        }
        errorMessage.clear();
        if (!MonitorPowerController::ApplySetup(rule.monitorPowerSetup, {}, &errorMessage))
        {
            statusCallback_(L"Could not apply monitor config " + rule.monitorPowerSetup.name + L": " + errorMessage);
        }
    }

    std::vector<ProcessStopAction> stoppedForRestart;
    for (const auto& action : rule.processesToStop)
    {
        if (!running_.load()) break;
        if (StopConfiguredProcess(action))
        {
            if (action.restartAfterWatchProcessEnds) stoppedForRestart.push_back(action);
        }
        else
        {
            statusCallback_(L"Process not found or could not be stopped: " + action.processName);
        }
    }
    if (!stoppedForRestart.empty())
    {
        std::scoped_lock lock(mutex_);
        stoppedProcesses_[rule.processKey] = std::move(stoppedForRestart);
    }

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
        if (!PostWebhook(*action)) statusCallback_(L"Home Assistant webhook failed: " + action->displayName);
    }

    if (!running_.load()) return;
    if (!rule.servicesToStop.empty())
    {
        if (!serviceOwnerKey_.empty()) statusCallback_(L"Services action skipped: another watched rule currently owns the service state.");
        else
        {
            std::wstring report;
            if (!ApplyIRacingServices(rule.servicesToStop, report))
                statusCallback_(L"Windows service action: " + report);
            // Partial success also leaves services that must be restored at session end.
            if (HasPendingIRacingServiceRestore()) serviceOwnerKey_ = rule.processKey;
        }
    }
    StartProgramsForRule(rule);
}

void ProcessMonitor::RestoreMonitorSetupForRule(const RuntimeRule& rule, ULONGLONG exitTick)
{
    MonitorPowerSetup previousSetup;
    {
        std::scoped_lock lock(mutex_);
        const auto monitorIt = previousMonitorSetups_.find(rule.processKey);
        if (monitorIt == previousMonitorSetups_.end()) return;
        previousSetup = std::move(monitorIt->second);
        previousMonitorSetups_.erase(monitorIt);
    }

    const DWORD restoreDelay = static_cast<DWORD>(std::max(0, rule.restoreMonitorPowerSetupDelayMilliseconds));
    const ULONGLONG elapsed = GetTickCount64() - exitTick;
    if (elapsed < restoreDelay) WaitForDelay(static_cast<DWORD>(restoreDelay - elapsed));

    std::wstring errorMessage;
    if (!MonitorPowerController::ApplySetup(previousSetup, {}, &errorMessage))
    {
        statusCallback_(L"Could not restore the previous monitor configuration: " + errorMessage);
    }
}

void ProcessMonitor::ExecuteExitActions(const RuntimeRule& rule, ULONGLONG exitTick)
{
    std::vector<ProcessStopAction> restartActions;
    {
        std::scoped_lock lock(mutex_);
        const auto processIt = stoppedProcesses_.find(rule.processKey);
        if (processIt == stoppedProcesses_.end()) return;
        restartActions = std::move(processIt->second);
        stoppedProcesses_.erase(processIt);
    }

    std::stable_sort(restartActions.begin(), restartActions.end(), [](const auto& left, const auto& right)
    {
        return left.restartDelayMilliseconds < right.restartDelayMilliseconds;
    });
    for (const auto& action : restartActions)
    {
        const DWORD delay = static_cast<DWORD>(std::max(0, action.restartDelayMilliseconds));
        const ULONGLONG elapsed = GetTickCount64() - exitTick;
        if (elapsed < delay) WaitForDelay(static_cast<DWORD>(delay - elapsed));

        const auto processKey = NormalizeProcessKey(action.processName);
        const std::unordered_set<std::wstring> filter{processKey};
        const auto snapshot = CaptureProcessSnapshot(false, &filter);
        if (!snapshot.valid) continue;
        const auto existing = snapshot.processIdsByName.find(processKey);
        if (existing != snapshot.processIdsByName.end() && std::any_of(existing->second.begin(), existing->second.end(), [&action](DWORD processId)
        {
            return PathMatchesProcess(processId, action.executablePath);
        }))
        {
            continue;
        }

        LaunchProgram program;
        program.displayName = action.displayName;
        program.filePath = action.executablePath;
        if (!LaunchProgramProcess(program).success)
        {
            statusCallback_(L"Could not restart process: " + action.displayName +
                L" (Windows error " + std::to_wstring(GetLastError()) + L"; " + launchStage + L")");
        }
        else statusCallback_(L"Restarted process: " + action.displayName + L".");
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
            statusCallback_(L"Program file not found: " + program.filePath);
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
                statusCallback_(L"Could not launch program: " + program.displayName +
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
        GetSystemTimeAsFileTime(&launchTime);
        std::shared_ptr<void> rootProcess;
        const auto launch = LaunchProgramProcess(program, &rootProcess);
        const DWORD launchedRootProcessId = launch.processId;
        if (!launch.success)
        {
            statusCallback_(L"Could not launch program: " + program.displayName +
                L" (Windows error " + std::to_wstring(GetLastError()) + L"; " + launchStage + L")");
            continue;
        }
        WaitForDelay(kProgramLaunchSettleMs);

        const auto afterSnapshot = CaptureProcessSnapshot(true);
        auto after = FindMatchingProcesses(afterSnapshot, program.filePath, normalizedPath);
        std::unordered_set<DWORD> started;
        if (beforeSnapshot.valid && afterSnapshot.valid) for (const auto pid : after)
        {
            if (!existing.contains(pid))
            {
                started.insert(pid);
                const auto descendants = BuildChildProcessSet(afterSnapshot, pid);
                started.insert(descendants.begin(), descendants.end());
            }
        }

        // For hand-offs, only retain a newly-created process if it owns a visible
        // top-level window. Direct and child processes are handled separately below.
        if (beforeSnapshot.valid && afterSnapshot.valid)
        {
            for (const auto& [name, processIds] : afterSnapshot.processIdsByName)
            {
                const auto before = beforeSnapshot.processIdsByName.find(name);
                for (const auto pid : processIds)
                {
                    const bool existed = before != beforeSnapshot.processIdsByName.end() &&
                        std::find(before->second.begin(), before->second.end(), pid) != before->second.end();
                    if (!existed && pid != GetCurrentProcessId() && HasVisibleTopLevelWindow(pid))
                    {
                        started.insert(pid);
                        const auto descendants = BuildChildProcessSet(afterSnapshot, pid);
                        started.insert(descendants.begin(), descendants.end());
                    }
                }
            }
        }

        if (launchedRootProcessId != 0)
        {
            started.insert(launchedRootProcessId);
            const auto descendants = BuildChildProcessSet(afterSnapshot, launchedRootProcessId);
            started.insert(descendants.begin(), descendants.end());
        }

        LaunchedProgramRecord record{program, normalizedPath.empty() ? program.filePath : normalizedPath,
            std::move(existing), std::move(started)};
        record.launchTime = launchTime;
        if (rootProcess) record.startedProcessHandles.push_back(std::move(rootProcess));
        for (const auto pid : record.startedProcessIds)
        {
            if (record.existingProcessIds.contains(pid) || pid == GetCurrentProcessId() || pid == launchedRootProcessId) continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
            if (process) record.startedProcessHandles.emplace_back(process, CloseHandle);
        }
        const auto liveCount = std::count_if(record.startedProcessHandles.begin(), record.startedProcessHandles.end(),
            [](const auto& process) { return WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT; });
        statusCallback_(L"Started " + program.displayName + L"; tracking " + std::to_wstring(liveCount) +
            L" running process(es) for closing.");
        if (liveCount == 0)
            statusCallback_(L"Cannot track the running app for " + program.displayName +
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

    std::stable_sort(
        records.begin(),
        records.end(),
        [](const LaunchedProgramRecord& left, const LaunchedProgramRecord& right)
        {
            return left.program.closeDelayMilliseconds < right.program.closeDelayMilliseconds;
        });

    const ULONGLONG scheduleStartTick = GetTickCount64();

    for (const auto& record : records)
    {
        if (!record.program.closeWhenGameStops)
        {
            continue;
        }

        const DWORD scheduledDelayMs = record.program.closeDelayMilliseconds > 0 ? static_cast<DWORD>(record.program.closeDelayMilliseconds) : 0;
        const ULONGLONG elapsedMs = GetTickCount64() - scheduleStartTick;
        if (elapsedMs < scheduledDelayMs)
        {
            WaitForDelay(static_cast<DWORD>(scheduledDelayMs - elapsedMs));
        }

        // Retained handles pin the original process identities. Include children
        // created later, but never unrelated instances with the same executable.
        auto ownedProcesses = record.startedProcessHandles;
        std::unordered_set<DWORD> ownedIds;
        for (const auto& process : ownedProcesses) ownedIds.insert(GetProcessId(process.get()));
        FILETIME captureTime{};
        GetSystemTimeAsFileTime(&captureTime);
        const std::unordered_set<std::wstring> names{
            NormalizeProcessKey(std::filesystem::path(record.executablePath).filename().wstring())};
        const auto snapshot = CaptureProcessSnapshot(true, &names);
        // Restore the exit-time lookup: a launcher may have returned before the
        // real executable appeared, or the application may have restarted itself.
        if (snapshot.valid && (record.launchTime.dwHighDateTime || record.launchTime.dwLowDateTime))
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
                ownedProcesses.push_back(std::move(candidate));
            }
        }
        if (snapshot.valid) for (size_t index = 0; index < ownedProcesses.size(); ++index)
        {
            const auto parent = ownedProcesses[index];
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
                ownedProcesses.push_back(std::move(child));
            }
        }
        for (const auto& process : ownedProcesses)
        {
            if (WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT)
            {
                if (!TerminateProcess(process.get(), 0))
                    statusCallback_(L"Could not close " + record.program.displayName +
                        L" (PID " + std::to_wstring(GetProcessId(process.get())) + L", Windows error " +
                        std::to_wstring(GetLastError()) + L").");
            }
        }
        const auto deadline = GetTickCount64() + 4000;
        for (const auto& process : ownedProcesses)
        {
            const auto now = GetTickCount64();
            if (WaitForSingleObject(process.get(), now < deadline ? static_cast<DWORD>(deadline - now) : 0) != WAIT_OBJECT_0)
                statusCallback_(L"Process has not exited: " + record.program.displayName +
                    L" (PID " + std::to_wstring(GetProcessId(process.get())) + L").");
        }
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
