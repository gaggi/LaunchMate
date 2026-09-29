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
    void SetActivePollInterval(DWORD pollIntervalMs);
    void Start();
    void Stop();
    void RecoverIRacingServices();
    bool IsRunning() const noexcept;
    std::vector<std::wstring> GetProcessStates(const std::vector<WatchedProcessRule>& rules) const;

private:
    friend struct ProcessMonitorTestAccess;
    struct RuntimeRule
    {
        std::wstring processKey;
        std::wstring displayName;
        std::vector<LaunchProgram> programsToLaunch;
        std::vector<ProcessStopAction> processesToStop;
        std::vector<HomeAssistantAction> homeAssistantActions;
        MonitorPowerSetup monitorPowerSetup;
        bool hasMonitorPowerSetup{false};
        int monitorPowerSetupDelayMilliseconds{0};
        bool restoreMonitorPowerSetupOnExit{true};
        int restoreMonitorPowerSetupDelayMilliseconds{0};
        std::wstring powerSchemeGuid;
        std::vector<std::wstring> servicesToStop;
    };

    struct RuntimeConfiguration
    {
        std::vector<RuntimeRule> watchedRules;
        std::unordered_set<std::wstring> watchedProcessKeys;
    };

    struct LaunchedProgramRecord
    {
        LaunchProgram program;
        std::wstring executablePath;
        std::unordered_set<DWORD> existingProcessIds;
        std::unordered_set<DWORD> startedProcessIds;
        std::vector<std::shared_ptr<void>> startedProcessHandles;
        FILETIME launchTime{};
    };

    struct ProcessSnapshot
    {
        bool valid{false};
        std::unordered_map<std::wstring, std::vector<DWORD>> processIdsByName;
        std::unordered_map<DWORD, std::vector<DWORD>> childrenByParent;
    };

    void WorkerLoop();
    void CheckRules();
    void ApplySnapshot(const RuntimeConfiguration& configuration, const ProcessSnapshot& snapshot);
    void CacheProcessStates(const RuntimeConfiguration& configuration, const ProcessSnapshot& snapshot);
    void FinishRule(const RuntimeRule& rule);
    void StartProgramsForRule(const RuntimeRule& rule);
    void ExecuteStartActions(const RuntimeRule& rule);
    void RestoreMonitorSetupForRule(const RuntimeRule& rule, ULONGLONG exitTick);
    void ExecuteExitActions(const RuntimeRule& rule, ULONGLONG exitTick);
    void StopProgramsForRule(const RuntimeRule& rule);
    ProcessSnapshot CaptureProcessSnapshot(
        bool includeProcessTree,
        const std::unordered_set<std::wstring>* processKeyFilter = nullptr) const;
    bool IsProcessRunning(const ProcessSnapshot& snapshot, const std::wstring& processKey) const;
    std::unordered_set<DWORD> FindMatchingProcesses(const ProcessSnapshot& snapshot, const std::wstring& executablePath) const;
    std::unordered_set<DWORD> FindMatchingProcesses(
        const ProcessSnapshot& snapshot,
        const std::wstring& executablePath,
        const std::wstring& normalizedExecutablePath) const;
    std::unordered_set<DWORD> BuildChildProcessSet(const ProcessSnapshot& snapshot, DWORD rootProcessId) const;
    void WakeWorker() noexcept;
    bool WaitForDelay(DWORD milliseconds) const;

    std::shared_ptr<const RuntimeConfiguration> runtimeConfiguration_;
    StatusCallback statusCallback_;
    std::atomic<bool> running_{false};
    std::atomic<DWORD> idlePollIntervalMs_{1000};
    std::atomic<DWORD> activePollIntervalMs_{1000};
    HANDLE wakeEvent_{nullptr};
    HANDLE stopEvent_{nullptr};
    std::thread worker_;
    std::mutex mutex_;
    // The UI reads the same snapshot that drives rule transitions.  Keeping this
    // cache avoids a second Toolhelp process enumeration just for the status column.
    mutable std::mutex processStatesMutex_;
    std::unordered_map<std::wstring, bool> cachedProcessStates_;
    bool cachedProcessStatesKnown_{false};
    // Keep the settings that actually started a session, even if its rule is edited/deleted.
    std::unordered_map<std::wstring, RuntimeRule> activeRules_;
    std::map<std::wstring, std::vector<LaunchedProgramRecord>> startedPrograms_;
    std::map<std::wstring, std::vector<ProcessStopAction>> stoppedProcesses_;
    std::map<std::wstring, MonitorPowerSetup> previousMonitorSetups_;
    std::map<std::wstring, GUID> previousPowerSchemes_;
    std::wstring serviceOwnerKey_;
};
