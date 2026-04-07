#pragma once

#include "Models.h"

#include <atomic>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ProcessMonitor
{
public:
    using StatusCallback = std::function<void(const std::wstring&)>;

    explicit ProcessMonitor(StatusCallback callback);
    ~ProcessMonitor();

    void UpdateConfiguration(const AppConfiguration& configuration);
    void SetPollInterval(DWORD pollIntervalMs);
    void Start();
    void Stop();
    bool IsRunning() const noexcept;

private:
    struct RuntimeRule
    {
        std::wstring processKey;
        std::wstring displayName;
        std::vector<LaunchProgram> programsToLaunch;
    };

    struct RuntimeConfiguration
    {
        std::vector<LaunchProgram> globalPrograms;
        std::vector<RuntimeRule> watchedRules;
    };

    struct LaunchedProgramRecord
    {
        LaunchProgram program;
        std::wstring executablePath;
        std::unordered_set<DWORD> existingProcessIds;
        std::unordered_set<DWORD> startedProcessIds;
    };

    struct ProcessSnapshot
    {
        std::unordered_map<std::wstring, std::vector<DWORD>> processIdsByName;
        std::unordered_map<DWORD, std::vector<DWORD>> childrenByParent;
    };

    void WorkerLoop();
    void CheckRules();
    void StartProgramsForRule(const RuntimeConfiguration& runtimeConfiguration, const RuntimeRule& rule);
    void StopProgramsForRule(const RuntimeRule& rule);
    ProcessSnapshot CaptureProcessSnapshot(bool includeProcessTree) const;
    bool IsProcessRunning(const ProcessSnapshot& snapshot, const std::wstring& processKey) const;
    std::unordered_set<DWORD> FindMatchingProcesses(const ProcessSnapshot& snapshot, const std::wstring& executablePath) const;
    std::unordered_set<DWORD> FindMatchingProcesses(
        const ProcessSnapshot& snapshot,
        const std::wstring& executablePath,
        const std::wstring& normalizedExecutablePath) const;
    std::unordered_set<DWORD> BuildChildProcessSet(const ProcessSnapshot& snapshot, DWORD rootProcessId) const;

    std::shared_ptr<const RuntimeConfiguration> runtimeConfiguration_;
    StatusCallback statusCallback_;
    std::atomic<bool> running_{false};
    std::atomic<DWORD> pollIntervalMs_{2000};
    std::thread worker_;
    std::mutex mutex_;
    std::unordered_set<std::wstring> activeRules_;
    std::map<std::wstring, std::vector<LaunchedProgramRecord>> startedPrograms_;
};
