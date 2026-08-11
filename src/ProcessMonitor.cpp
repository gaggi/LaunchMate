#include "ProcessMonitor.h"

#include "MonitorPowerController.h"
#include "Utils.h"

#include <algorithm>
#include <TlHelp32.h>
#include <cwctype>
#include <filesystem>
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

namespace
{
    constexpr DWORD kProgramLaunchSettleMs = 1200;
    constexpr DWORD kMinimumPollIntervalMs = 100;
    constexpr DWORD kMaximumPollIntervalMs = 300000;

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

    DWORD LaunchProgramProcess(const LaunchProgram& program)
    {
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS;
        info.lpFile = program.filePath.c_str();
        info.lpParameters = program.arguments.empty() ? nullptr : program.arguments.c_str();
        info.nShow = SW_SHOWNORMAL;
        info.lpVerb = L"open";

        if (ShellExecuteExW(&info) && info.hProcess)
        {
            const DWORD processId = GetProcessId(info.hProcess);
            CloseHandle(info.hProcess);
            return processId;
        }

        return 0;
    }

    void TryCloseProcess(DWORD processId)
    {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, processId);
        if (!process)
        {
            return;
        }

        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
        {
            CloseHandle(process);
            return;
        }

        TerminateProcess(process, 0);
        WaitForSingleObject(process, 4000);
        CloseHandle(process);
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

        bool matched = false;
        bool allStopped = true;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                if (entry.th32ProcessID == GetCurrentProcessId() || NormalizeProcessKey(entry.szExeFile) != processKey ||
                    !PathMatchesProcess(entry.th32ProcessID, action.executablePath))
                {
                    continue;
                }

                matched = true;
                if (action.gracefulCloseFirst)
                {
                    CloseWindowsContext context{entry.th32ProcessID};
                    EnumWindows(CloseProcessWindows, reinterpret_cast<LPARAM>(&context));
                    const DWORD timeout = static_cast<DWORD>(std::max(0, action.forceAfterMilliseconds));
                    HANDLE waitHandle = OpenProcess(SYNCHRONIZE, FALSE, entry.th32ProcessID);
                    if (waitHandle && WaitForSingleObject(waitHandle, timeout) == WAIT_OBJECT_0)
                    {
                        CloseHandle(waitHandle);
                        continue;
                    }
                    if (waitHandle) CloseHandle(waitHandle);
                }

                HANDLE terminateHandle = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID);
                if (terminateHandle && TerminateProcess(terminateHandle, 0))
                {
                    allStopped = WaitForSingleObject(terminateHandle, 4000) == WAIT_OBJECT_0 && allStopped;
                    CloseHandle(terminateHandle);
                }
                else
                {
                    allStopped = false;
                    if (terminateHandle) CloseHandle(terminateHandle);
                }
            }
            while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return matched && allStopped;
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
        HINTERNET session = WinHttpOpen(L"LaunchMate/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
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
      wakeEvent_(CreateEventW(nullptr, FALSE, FALSE, nullptr))
{
}

ProcessMonitor::~ProcessMonitor()
{
    Stop();

    if (wakeEvent_ != nullptr)
    {
        CloseHandle(wakeEvent_);
    }
}

void ProcessMonitor::UpdateConfiguration(const AppConfiguration& configuration)
{
    auto prepared = std::make_shared<RuntimeConfiguration>();
    prepared->watchedRules.reserve(configuration.watchedProcesses.size());
    prepared->watchedProcessKeys.reserve(configuration.watchedProcesses.size());

    for (const auto& rule : configuration.watchedProcesses)
    {
        if (!rule.enabled || rule.processName.empty())
        {
            continue;
        }

        RuntimeRule runtimeRule;
        runtimeRule.processKey = NormalizeProcessKey(rule.processName);
        runtimeRule.displayName = rule.displayName.empty() ? rule.processName : rule.displayName;
        runtimeRule.programsToLaunch = rule.programsToLaunch;
        runtimeRule.processesToStop = rule.processesToStop;
        runtimeRule.homeAssistantActions = rule.homeAssistantActions;
        runtimeRule.monitorPowerSetupDelayMilliseconds = rule.monitorPowerSetupDelayMilliseconds;
        runtimeRule.restoreMonitorPowerSetupOnExit = rule.restoreMonitorPowerSetupOnExit;
        runtimeRule.restoreMonitorPowerSetupDelayMilliseconds = rule.restoreMonitorPowerSetupDelayMilliseconds;
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

    worker_ = std::thread([this] { WorkerLoop(); });
    statusCallback_(L"Monitoring active.");
}

void ProcessMonitor::Stop()
{
    if (!running_.exchange(false))
    {
        return;
    }

    WakeWorker();

    if (worker_.joinable())
    {
        worker_.join();
    }

    statusCallback_(L"Monitoring stopped.");
}

bool ProcessMonitor::IsRunning() const noexcept
{
    return running_.load();
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
        if (!runtimeConfiguration || runtimeConfiguration->watchedRules.empty())
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
            Sleep(waitDurationMs);
        }
    }
}

void ProcessMonitor::CheckRules()
{
    std::shared_ptr<const RuntimeConfiguration> runtimeConfiguration;
    {
        std::scoped_lock lock(mutex_);
        runtimeConfiguration = runtimeConfiguration_;
    }

    if (!runtimeConfiguration || runtimeConfiguration->watchedRules.empty())
    {
        return;
    }

    const auto snapshot = CaptureProcessSnapshot(false, &runtimeConfiguration->watchedProcessKeys);

    for (const auto& rule : runtimeConfiguration->watchedRules)
    {
        const bool detected = IsProcessRunning(snapshot, rule.processKey);
        const bool alreadyRunning = activeRules_.contains(rule.processKey);

        if (detected && !alreadyRunning)
        {
            activeRules_.insert(rule.processKey);
            statusCallback_(rule.displayName + L" detected. Running actions.");
            ExecuteStartActions(*runtimeConfiguration, rule);
        }
        else if (!detected && alreadyRunning)
        {
            activeRules_.erase(rule.processKey);
            statusCallback_(rule.displayName + L" exited. Closing started programs.");
            const ULONGLONG exitTick = GetTickCount64();
            RestoreMonitorSetupForRule(rule, exitTick);
            StopProgramsForRule(rule);
            ExecuteExitActions(rule, exitTick);
        }
    }
}

void ProcessMonitor::ExecuteStartActions(const RuntimeConfiguration& runtimeConfiguration, const RuntimeRule& rule)
{
    if (rule.hasMonitorPowerSetup)
    {
        const DWORD applyDelay = static_cast<DWORD>(std::max(0, rule.monitorPowerSetupDelayMilliseconds));
        if (applyDelay > 0) Sleep(applyDelay);

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

        errorMessage.clear();
        if (!MonitorPowerController::ApplySetup(rule.monitorPowerSetup, {}, &errorMessage))
        {
            statusCallback_(L"Could not apply monitor config " + rule.monitorPowerSetup.name + L": " + errorMessage);
        }
        else if (capturedPrevious)
        {
            std::scoped_lock lock(mutex_);
            previousMonitorSetups_[rule.processKey] = std::move(previousSetup);
        }
    }

    std::vector<ProcessStopAction> stoppedForRestart;
    for (const auto& action : rule.processesToStop)
    {
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
        if (elapsed < delay) Sleep(static_cast<DWORD>(delay - elapsed));
        if (!PostWebhook(*action)) statusCallback_(L"Home Assistant webhook failed: " + action->displayName);
    }

    StartProgramsForRule(runtimeConfiguration, rule);
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
    if (elapsed < restoreDelay) Sleep(static_cast<DWORD>(restoreDelay - elapsed));

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
        if (elapsed < delay) Sleep(static_cast<DWORD>(delay - elapsed));

        const auto processKey = NormalizeProcessKey(action.processName);
        const std::unordered_set<std::wstring> filter{processKey};
        const auto snapshot = CaptureProcessSnapshot(false, &filter);
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
        if (LaunchProgramProcess(program) == 0)
        {
            statusCallback_(L"Could not restart process: " + action.displayName);
        }
    }
}

void ProcessMonitor::StartProgramsForRule(const RuntimeConfiguration& runtimeConfiguration, const RuntimeRule& rule)
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
        const auto& program = *scheduledProgram;
        if (program.filePath.empty() || !std::filesystem::exists(program.filePath))
        {
            continue;
        }

        const DWORD scheduledDelayMs = program.waitTimeMilliseconds > 0 ? static_cast<DWORD>(program.waitTimeMilliseconds) : 0;
        const ULONGLONG elapsedMs = GetTickCount64() - scheduleStartTick;
        if (elapsedMs < scheduledDelayMs)
        {
            Sleep(static_cast<DWORD>(scheduledDelayMs - elapsedMs));
        }

        const auto normalizedPath = NormalizePath(program.filePath);
        const auto processKey = NormalizeProcessKey(std::filesystem::path(program.filePath).stem().wstring());
        const std::unordered_set<std::wstring> processKeyFilter =
            processKey.empty() ? std::unordered_set<std::wstring>{} : std::unordered_set<std::wstring>{processKey};

        const auto beforeSnapshot = CaptureProcessSnapshot(false, ProcessKeyFilterPointer(processKeyFilter));
        auto existing = FindMatchingProcesses(beforeSnapshot, program.filePath, normalizedPath);
        const DWORD launchedRootProcessId = LaunchProgramProcess(program);
        Sleep(kProgramLaunchSettleMs);

        const auto afterSnapshot = CaptureProcessSnapshot(true, ProcessKeyFilterPointer(processKeyFilter));
        auto after = FindMatchingProcesses(afterSnapshot, program.filePath, normalizedPath);
        std::unordered_set<DWORD> started;
        for (const auto pid : after)
        {
            if (!existing.contains(pid))
            {
                started.insert(pid);
                const auto descendants = BuildChildProcessSet(afterSnapshot, pid);
                started.insert(descendants.begin(), descendants.end());
            }
        }

        if (launchedRootProcessId != 0)
        {
            started.insert(launchedRootProcessId);
            const auto descendants = BuildChildProcessSet(afterSnapshot, launchedRootProcessId);
            started.insert(descendants.begin(), descendants.end());
        }

        records.push_back({program, normalizedPath.empty() ? program.filePath : normalizedPath, std::move(existing), std::move(started)});
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

    std::unordered_set<std::wstring> processKeyFilter;
    processKeyFilter.reserve(records.size());
    for (const auto& record : records)
    {
        if (!record.executablePath.empty())
        {
            processKeyFilter.insert(NormalizeProcessKey(std::filesystem::path(record.executablePath).stem().wstring()));
        }
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
            Sleep(static_cast<DWORD>(scheduledDelayMs - elapsedMs));
        }

        const auto snapshot = CaptureProcessSnapshot(true, ProcessKeyFilterPointer(processKeyFilter));
        auto candidates = FindMatchingProcesses(snapshot, record.executablePath, record.executablePath);
        for (const auto pid : record.startedProcessIds)
        {
            candidates.insert(pid);
            const auto descendants = BuildChildProcessSet(snapshot, pid);
            candidates.insert(descendants.begin(), descendants.end());
        }

        for (const auto pid : candidates)
        {
            if (!record.existingProcessIds.contains(pid))
            {
                TryCloseProcess(pid);
            }
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
