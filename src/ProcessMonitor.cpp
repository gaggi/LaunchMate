#include "ProcessMonitor.h"

#include <algorithm>
#include <TlHelp32.h>
#include <cwctype>
#include <filesystem>
#include <windows.h>
#include <shellapi.h>

namespace
{
    constexpr DWORD kProgramLaunchSettleMs = 1200;
    constexpr DWORD kMonitorSleepSliceMs = 100;

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
}

ProcessMonitor::ProcessMonitor(StatusCallback callback)
    : runtimeConfiguration_(std::make_shared<RuntimeConfiguration>()),
      statusCallback_(std::move(callback))
{
}

ProcessMonitor::~ProcessMonitor()
{
    Stop();
}

void ProcessMonitor::UpdateConfiguration(const AppConfiguration& configuration)
{
    auto prepared = std::make_shared<RuntimeConfiguration>();
    prepared->globalPrograms = configuration.globalPrograms;
    prepared->watchedRules.reserve(configuration.watchedProcesses.size());

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
        prepared->watchedRules.push_back(std::move(runtimeRule));
    }

    std::scoped_lock lock(mutex_);
    runtimeConfiguration_ = std::move(prepared);
}

void ProcessMonitor::SetPollInterval(DWORD pollIntervalMs)
{
    pollIntervalMs_.store(std::clamp<DWORD>(pollIntervalMs, 100, 60000));
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

ProcessMonitor::ProcessSnapshot ProcessMonitor::CaptureProcessSnapshot(bool includeProcessTree) const
{
    ProcessSnapshot snapshot;

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
            snapshot.processIdsByName[NormalizeProcessKey(entry.szExeFile)].push_back(entry.th32ProcessID);
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
        DWORD remainingSleepMs = std::max<DWORD>(pollIntervalMs_.load(), kMonitorSleepSliceMs);
        while (remainingSleepMs > 0 && running_.load())
        {
            const DWORD sleepSlice = std::min(remainingSleepMs, kMonitorSleepSliceMs);
            Sleep(sleepSlice);
            remainingSleepMs -= sleepSlice;
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

    const auto snapshot = CaptureProcessSnapshot(false);

    for (const auto& rule : runtimeConfiguration->watchedRules)
    {
        const bool detected = IsProcessRunning(snapshot, rule.processKey);
        const bool alreadyRunning = activeRules_.contains(rule.processKey);

        if (detected && !alreadyRunning)
        {
            activeRules_.insert(rule.processKey);
            statusCallback_(rule.displayName + L" detected. Starting linked programs.");
            StartProgramsForRule(*runtimeConfiguration, rule);
        }
        else if (!detected && alreadyRunning)
        {
            activeRules_.erase(rule.processKey);
            statusCallback_(rule.displayName + L" exited. Closing started programs.");
            StopProgramsForRule(rule);
        }
    }
}

void ProcessMonitor::StartProgramsForRule(const RuntimeConfiguration& runtimeConfiguration, const RuntimeRule& rule)
{
    std::vector<LaunchedProgramRecord> records;
    std::vector<const LaunchProgram*> scheduledPrograms;
    scheduledPrograms.reserve(runtimeConfiguration.globalPrograms.size() + rule.programsToLaunch.size());

    for (const auto& program : runtimeConfiguration.globalPrograms)
    {
        scheduledPrograms.push_back(&program);
    }

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
        const auto beforeSnapshot = CaptureProcessSnapshot(false);
        auto existing = FindMatchingProcesses(beforeSnapshot, program.filePath, normalizedPath);
        const DWORD launchedRootProcessId = LaunchProgramProcess(program);
        Sleep(kProgramLaunchSettleMs);

        const auto afterSnapshot = CaptureProcessSnapshot(true);
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

        records = it->second;
        startedPrograms_.erase(it);
    }

    const auto snapshot = CaptureProcessSnapshot(true);

    for (const auto& record : records)
    {
        if (!record.program.closeWhenGameStops)
        {
            continue;
        }

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
