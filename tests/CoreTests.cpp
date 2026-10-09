#include "AtomicFile.h"
#include "ConfigStore.h"
#include "ui/BackgroundTask.h"
#include <future>
#include "JsonLite.h"
#include "ProcessMonitor.h"
#include "IRacingPerformance.h"
#include "IRacingServices.h"
#include "MonitorPowerController.h"
#include "DisplayLayout.h"
#include "Utils.h"

#include <fstream>
#include <iostream>
#include <limits>
#include <locale>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    int powerApplied = 0, powerRestored = 0, servicesRestored = 0, displaysApplied = 0;
    bool pendingServices = false;
    // Fake display state: the name of the arrangement currently "on screen".
    std::wstring displayState = L"original";
    bool displayCaptureFails = false;
    int displayApplyFailures = 0;
}

// Deliberate fakes: tests must never modify Windows services, power plans or displays.
bool ActivatePowerScheme(const std::wstring&, GUID&) { ++powerApplied; return true; }
bool RestorePowerScheme(const GUID&) { ++powerRestored; return true; }
bool HasPendingIRacingServiceRestore() { return pendingServices; }
bool ApplyIRacingServices(const std::vector<std::wstring>&, std::wstring&)
{
    pendingServices = true;
    return false; // Partial success: a service was stopped before a later failure.
}
bool RestoreIRacingServices(std::wstring&) { if (pendingServices) ++servicesRestored; pendingServices = false; return true; }
bool MonitorPowerController::CaptureSetup(MonitorPowerSetup& setup, std::wstring*)
{
    if (displayCaptureFails) return false;
    setup.name = displayState;
    return true;
}
bool MonitorPowerController::ApplySetup(const MonitorPowerSetup& setup, const std::function<void(const std::wstring&)>&, std::wstring*)
{
    ++displaysApplied;
    if (displayApplyFailures > 0) { --displayApplyFailures; return false; }
    if (!setup.name.empty()) displayState = setup.name;
    return true;
}

struct ProcessMonitorTestAccess
{
    static std::wstring TestExecutable()
    {
        wchar_t executable[32768]{};
        Require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "Test executable unavailable");
        return executable;
    }

    // Disposable copy of the test executable; terminated when the last reference goes.
    static std::shared_ptr<void> SpawnChild(const std::wstring& path, const wchar_t* mode)
    {
        std::wstring command = L"\"" + path + L"\" " + mode;
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION info{};
        Require(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &info) != FALSE, "Could not create disposable test child");
        CloseHandle(info.hThread);
        return std::shared_ptr<void>(info.hProcess, [](void* process)
        {
            if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) TerminateProcess(process, 0);
            WaitForSingleObject(process, 2000);
            CloseHandle(process);
        });
    }

    static bool WaitForTopLevelWindow(DWORD processId)
    {
        struct Search { DWORD processId; bool found; } search{processId, false};
        for (int attempt = 0; attempt < 100 && !search.found; ++attempt)
        {
            EnumWindows([](HWND window, LPARAM parameter) -> BOOL
            {
                auto& search = *reinterpret_cast<Search*>(parameter);
                DWORD owner = 0;
                GetWindowThreadProcessId(window, &owner);
                if (owner == search.processId) search.found = true;
                return !search.found;
            }, reinterpret_cast<LPARAM>(&search));
            if (!search.found) Sleep(50);
        }
        return search.found;
    }

    static bool Running(const std::shared_ptr<void>& process)
    {
        return WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT;
    }

    static void TestGracefulProgramClose()
    {
        const auto executable = TestExecutable();
        const auto windowed = SpawnChild(executable, L"--idle-child-pump");
        Require(WaitForTopLevelWindow(GetProcessId(windowed.get())), "Windowed test child did not create its window");
        FILETIME launchTime{};
        GetSystemTimeAsFileTime(&launchTime);
        const auto unrelated = SpawnChild(executable, L"--idle-child");

        ProcessMonitor monitor([](const auto&) {});
        monitor.running_ = true;
        ProcessMonitor::RuntimeRule rule;
        rule.processKey = L"graceful-test";
        ProcessMonitor::LaunchedProgramRecord record;
        record.program.displayName = L"graceful child";
        record.executablePath = executable;
        record.launchTime = launchTime;
        record.rootProcessKnown = true;
        record.startedProcessHandles.push_back(windowed);
        monitor.startedPrograms_[rule.processKey].push_back(std::move(record));
        const auto start = GetTickCount64();
        monitor.StopProgramsForRule(rule);
        DWORD exitCode = 0;
        Require(GetExitCodeProcess(windowed.get(), &exitCode) != FALSE && exitCode == 7,
            "Launched program was terminated instead of closed through its window");
        Require(GetTickCount64() - start < 2500, "Graceful close waited for the full grace period");
        Require(Running(unrelated), "Same-executable process started by someone else was closed");
        std::cout << "Launched program closed gracefully; unrelated same-path instance kept.\n";
    }

    static void TestLaunchIgnoresUnrelatedWindows()
    {
        const auto executable = TestExecutable();
        const auto directory = std::filesystem::temp_directory_path() /
            (L"LaunchMate-unrelated-test-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directory(directory);
        const auto childPath = directory / L"LaunchMateOwnedChild.exe";
        std::filesystem::copy_file(executable, childPath, std::filesystem::copy_options::overwrite_existing);
        struct Cleanup
        {
            std::filesystem::path file, directory;
            ~Cleanup() { std::error_code error; std::filesystem::remove(file, error); std::filesystem::remove(directory, error); }
        } cleanup{childPath, directory};

        ProcessMonitor monitor([](const auto&) {});
        monitor.running_ = true;
        ProcessMonitor::RuntimeRule rule;
        rule.processKey = L"unrelated-window-test";
        LaunchProgram program;
        program.displayName = L"owned child";
        program.filePath = childPath.wstring();
        program.arguments = L"--idle-child";
        rule.programsToLaunch.push_back(program);

        // A window the user opens while the launch settles must not be adopted.
        std::shared_ptr<void> unrelated;
        std::thread opener([&] { Sleep(300); unrelated = SpawnChild(executable, L"--idle-child-window"); });
        monitor.StartProgramsForRule(rule);
        opener.join();
        Require(unrelated != nullptr, "Unrelated test window was not opened");
        const auto& records = monitor.startedPrograms_.at(rule.processKey);
        Require(records.size() == 1 && records[0].rootProcessKnown, "Launch record missing");
        Require(!records[0].startedProcessIds.contains(GetProcessId(unrelated.get())),
            "Unrelated windowed process was adopted by a launch");
        const auto owned = records[0].startedProcessHandles.front();
        monitor.StopProgramsForRule(rule);
        Require(!Running(owned), "Launched program was not closed");
        Require(Running(unrelated), "Unrelated windowed process was closed with the launched program");
        std::cout << "Launch ownership ignores unrelated windows.\n";
    }

    static void TestStopActionCapturesRestartArguments()
    {
        const auto executable = TestExecutable();
        const auto child = SpawnChild(executable, L"--idle-child");
        ProcessMonitor monitor([](const auto&) {});
        monitor.running_ = true;
        ProcessMonitor::RuntimeRule rule;
        rule.processKey = L"restart-arguments-test";
        ProcessStopAction action;
        action.processName = std::filesystem::path(executable).filename().wstring();
        action.restartAfterWatchProcessEnds = true;
        action.forceAfterMilliseconds = 500;
        rule.processesToStop.push_back(action);
        monitor.StopConfiguredProcesses(rule);
        Require(!Running(child), "Stop action did not stop the test child");
        const auto& records = monitor.stoppedProcesses_.at(rule.processKey);
        Require(records.size() == 1, "Stopped process was not recorded for restart");
        Require(records[0].arguments == L"--idle-child", "Restart arguments were not captured");
        Require(_wcsicmp(records[0].executablePath.c_str(), executable.c_str()) == 0,
            "Restart path was not captured from the running process");
        std::cout << "Stop action captured restart path and arguments.\n";
    }

    static void TestEtwInstanceHandles()
    {
        using EventType = EtwProcessListener::ProcessEvent::Type;
        const auto executable = TestExecutable();
        const auto child = SpawnChild(executable, L"--idle-child");
        std::vector<std::wstring> statuses;
        ProcessMonitor monitor([&](const auto& status) { statuses.push_back(status); });
        AppConfiguration config;
        WatchedProcessRule rule;
        rule.displayName = L"Handle test";
        rule.processName = std::filesystem::path(executable).filename().wstring();
        config.watchedProcesses.push_back(rule);
        monitor.UpdateConfiguration(config);
        const auto configuration = monitor.runtimeConfiguration_;
        const auto key = configuration->watchedRules.at(0).processKey;
        monitor.running_ = true;
        monitor.usingEtw_ = true;
        monitor.etwInitialSnapshotComplete_ = true;

        const DWORD childId = GetProcessId(child.get());
        monitor.pendingEtwEvents_.push_back({EventType::ProcessStarted, key, childId});
        monitor.ProcessEtwEvents();
        Require(monitor.activeRules_.contains(key) && monitor.watchedInstances_.at(key).at(childId) != nullptr,
            "ETW start did not track the watched instance handle");

        // No ETW stop event: the process handle alone must end the session.
        Require(TerminateProcess(child.get(), 0) != FALSE, "Could not stop handle test child");
        ResetEvent(monitor.wakeEvent_);
        monitor.WaitForWork(*configuration, 2000);
        Require(monitor.activeRules_.empty(), "Watched exit was not detected from its process handle");
        monitor.pendingEtwEvents_.push_back({EventType::ProcessStopped, key, childId});
        monitor.ProcessEtwEvents();
        const auto ended = std::count_if(statuses.begin(), statuses.end(), [](const auto& status)
        {
            return status.find(L"session ended") != std::wstring::npos;
        });
        Require(ended == 1, "A late ETW stop event ended the session twice");

        monitor.pendingEtwEvents_.push_back({EventType::EventsLost});
        monitor.ProcessEtwEvents();
        Require(!monitor.etwInitialSnapshotComplete_, "Lost ETW events did not request a resynchronization");
        monitor.usingEtw_ = false;
        monitor.Stop();
        std::cout << "ETW sessions end through process handles; lost events trigger a resync.\n";
    }

    static void TestReviewRegressions()
    {
        using EventType = EtwProcessListener::ProcessEvent::Type;
        const auto executable = TestExecutable();
        auto key = std::filesystem::path(executable).filename().wstring();
        std::transform(key.begin(), key.end(), key.begin(), towlower);
        const auto first = SpawnChild(executable, L"--idle-child");
        std::shared_ptr<void> second;

        // A second instance of a watched program gets the rule's priorities too.
        {
            ProcessMonitor monitor([](const auto&) {});
            monitor.running_ = true;
            monitor.usingEtw_ = true;
            auto config = std::make_shared<ProcessMonitor::RuntimeConfiguration>();
            ProcessMonitor::RuntimeRule rule;
            rule.processKey = key;
            rule.displayName = L"Second instance";
            ProcessPerformanceAction action;
            action.processName = key;
            action.cpuPriorityClass = BELOW_NORMAL_PRIORITY_CLASS;
            rule.processPerformanceActions.push_back(action);
            config->watchedRules.push_back(rule);
            config->watchedProcessKeys.insert(key);
            monitor.runtimeConfiguration_ = config;
            monitor.pendingEtwEvents_.push_back({EventType::ProcessStarted, key, GetProcessId(first.get())});
            monitor.ProcessEtwEvents();
            // Started after the session began, like a real second instance.
            second = SpawnChild(executable, L"--idle-child");
            monitor.pendingEtwEvents_.push_back({EventType::ProcessStarted, key, GetProcessId(second.get())});
            monitor.ProcessEtwEvents();
            Require(GetPriorityClass(second.get()) == BELOW_NORMAL_PRIORITY_CLASS, "Second watched instance kept its priority");
            monitor.usingEtw_ = false;
            monitor.Stop();
        }

        // One ETW start event must not drop the state of other running instances.
        {
            ProcessMonitor monitor([](const auto&) {});
            monitor.running_ = true;
            monitor.usingEtw_ = true;
            monitor.runtimeConfiguration_ = std::make_shared<ProcessMonitor::RuntimeConfiguration>();
            ProcessMonitor::RuntimeRule rule;
            rule.processKey = L"sparse-snapshot-test.exe";
            ProcessPerformanceAction action;
            action.processName = key;
            action.cpuPriorityClass = BELOW_NORMAL_PRIORITY_CLASS;
            rule.processPerformanceActions.push_back(action);
            monitor.activeRules_[rule.processKey] = rule;
            for (const auto& child : {first, second})
            {
                monitor.pendingEtwEvents_.push_back({EventType::ProcessStarted, key, GetProcessId(child.get())});
                monitor.ProcessEtwEvents();
            }
            Require(monitor.performanceTargetStates_.at(rule.processKey).at(0).size() == 2,
                "A single ETW start event cleared the state of another instance");
            monitor.usingEtw_ = false;
            monitor.Stop();
        }

        // Only one rule controls the displays, so overlapping sessions restore the original.
        std::vector<std::wstring> problems;
        {
            displayState = L"original";
            ProcessMonitor monitor([](const auto&) {});
            monitor.SetProblemCallback([&](const std::wstring& problem) { if (!problem.empty()) problems.push_back(problem); });
            monitor.running_ = true;
            ProcessMonitor::RuntimeRule a, b;
            a.processKey = L"display-a.exe"; b.processKey = L"display-b.exe";
            a.displayName = L"A"; b.displayName = L"B";
            a.hasMonitorPowerSetup = b.hasMonitorPowerSetup = true;
            a.monitorPowerSetup.name = L"A"; b.monitorPowerSetup.name = L"B";
            monitor.ExecuteStartActions(a);
            monitor.ExecuteStartActions(b);
            Require(displayState == L"A", "A second rule switched displays another rule controls");
            Require(problems.size() == 1 && problems[0].find(L"another watched rule") != std::wstring::npos,
                "Skipping the second display configuration was not reported");
            monitor.FinishRule(a);
            Require(displayState == L"original", "The display owner did not restore the original arrangement");
            monitor.FinishRule(b);
            Require(displayState == L"original", "A rule that never switched displays restored an arrangement");

            // Without a saved arrangement the displays are not switched at all.
            problems.clear();
            displayCaptureFails = true;
            const int applied = displaysApplied;
            monitor.ExecuteStartActions(a);
            displayCaptureFails = false;
            Require(displaysApplied == applied && displayState == L"original", "Displays were switched although they could not be restored");
            Require(problems.size() == 1, "The failed display capture was not reported");
            monitor.FinishRule(a);
            Require(monitor.displayOwnerKey_.empty(), "A rule that did not switch displays kept them");

            // A restore that fails once is tried again.
            monitor.ExecuteStartActions(a);
            displayApplyFailures = 1;
            monitor.FinishRule(a);
            Require(displayApplyFailures == 0 && displayState == L"original", "A failed display restore was not retried");
            Require(monitor.previousMonitorSetups_.empty() && monitor.displayOwnerKey_.empty(), "Display restore left stale state");
            monitor.Stop();
        }

        // The 32-bit build cannot address CPUs above 31 and says so instead of truncating.
        if constexpr (sizeof(DWORD_PTR) == 4)
        {
            std::vector<std::wstring> statuses;
            ProcessMonitor monitor([&](const auto& status) { statuses.push_back(status); });
            ProcessMonitor::RuntimeRule rule;
            rule.processKey = L"affinity-width-test.exe";
            ProcessPerformanceAction action;
            action.processName = key;
            action.affinityMask = std::uint64_t{1} << 40;
            rule.processPerformanceActions.push_back(action);
            ProcessMonitor::ProcessSnapshot snapshot;
            snapshot.valid = true;
            snapshot.processIdsByName[key] = {GetProcessId(first.get())};
            monitor.ApplyPerformanceActions(rule, snapshot);
            Require(std::any_of(statuses.begin(), statuses.end(), [](const auto& status) { return status.find(L"32-bit") != std::wstring::npos; }),
                "An affinity mask beyond the 32-bit range was applied silently");
        }

        // A damaged settings file is kept before defaults are used.
        {
            const auto directory = std::filesystem::temp_directory_path() / (L"LaunchMate-config-test-" + std::to_wstring(GetCurrentProcessId()));
            std::filesystem::create_directories(directory);
            struct Cleanup { std::filesystem::path directory; ~Cleanup() { std::error_code error; std::filesystem::remove_all(directory, error); } } cleanup{directory};
            const auto file = directory / L"config.json";
            { std::ofstream damaged(file, std::ios::binary); damaged << "{\"WatchedProcesses\": [ {\"ProcessName\": "; }
            ConfigStore store(file);
            const auto loaded = store.Load();
            Require(loaded.watchedProcesses.empty() && !store.LoadProblem().empty(), "A damaged settings file was not reported");
            size_t backups = 0;
            for (const auto& entry : std::filesystem::directory_iterator(directory))
                if (entry.path().filename().wstring().starts_with(L"config.json.unreadable-")) ++backups;
            Require(backups == 1, "The damaged settings file was not kept");
            Require(store.Save(loaded), "Saving after a kept damaged file failed");
            ConfigStore reread(file);
            reread.Load();
            Require(reread.LoadProblem().empty(), "The settings saved after recovery could not be read");
        }
        std::cout << "Second instances, single ETW events, display ownership and damaged settings regression tests passed.\n";
    }

    static void TestPerformanceSettings()
    {
        HANDLE currentToken = nullptr;
        TOKEN_ELEVATION currentElevation{};
        DWORD currentTokenSize = 0;
        const bool tokenElevationKnown = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &currentToken) &&
            GetTokenInformation(currentToken, TokenElevation, &currentElevation, sizeof(currentElevation), &currentTokenSize) != FALSE;
        if (currentToken) CloseHandle(currentToken);
        wchar_t executable[MAX_PATH]{};
        Require(GetModuleFileNameW(nullptr, executable, MAX_PATH) != 0, "Test executable unavailable");
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --idle-child";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION child{};
        Require(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &child) != FALSE, "Could not create performance test child");
        CloseHandle(child.hThread);
        struct ChildCleanup
        {
            HANDLE process;
            ~ChildCleanup()
            {
                if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) TerminateProcess(process, 0);
                WaitForSingleObject(process, 2000);
                CloseHandle(process);
            }
        } cleanup{child.hProcess};

        std::wstring lastStatus;
        ProcessMonitor monitor([&](const auto& status) { lastStatus = status; });
        ProcessMonitor::RuntimeRule rule;
        rule.processKey = L"performance-watch-test.exe";
        ProcessPerformanceAction action;
        action.processName = std::filesystem::path(executable).filename().wstring();
        action.cpuPriorityClass = BELOW_NORMAL_PRIORITY_CLASS;
        action.ioPriority = 3; // High exercises the privilege path in both token modes.
        action.memoryPriority = 6; // Invalid on purpose so we can verify per-setting retry.
        action.affinityMask = 1;
        action.efficiencyMode = 1;
        rule.processPerformanceActions.push_back(action);
        ProcessMonitor::ProcessSnapshot snapshot;
        snapshot.valid = true;
        auto processKey = action.processName;
        std::transform(processKey.begin(), processKey.end(), processKey.begin(), towlower);
        snapshot.processIdsByName[processKey] = {child.dwProcessId};
        monitor.ApplyPerformanceActions(rule, snapshot);
        const auto& firstState = monitor.performanceTargetStates_.at(rule.processKey).at(0).at(child.dwProcessId);
        Require(!firstState.successReported && firstState.appliedSettings != 0,
            "Partial performance application was not retained for retry");
        const std::wstring highIoStatus = lastStatus;
        Require(highIoStatus.find(L"Memory priority 6") != std::wstring::npos,
            "Invalid memory priority failure was not reported");

        rule.processPerformanceActions[0].memoryPriority = 1;
        rule.processPerformanceActions[0].ioPriority = 1;
        monitor.ApplyPerformanceActions(rule, snapshot);
        const auto& finalState = monitor.performanceTargetStates_.at(rule.processKey).at(0).at(child.dwProcessId);
        Require(finalState.successReported, "Failed performance setting was not retried");

        ProcessMonitor::RuntimeRule invalidRule;
        invalidRule.processKey = L"performance-retry-limit-test.exe";
        ProcessPerformanceAction invalid;
        invalid.processName = action.processName;
        invalid.memoryPriority = 6;
        invalidRule.processPerformanceActions.push_back(invalid);
        for (int attempt = 0; attempt < 8; ++attempt) monitor.ApplyPerformanceActions(invalidRule, snapshot);
        Require(monitor.performanceTargetStates_.at(invalidRule.processKey).at(0).at(child.dwProcessId).attempts == 5,
            "Permanently failing performance settings were retried without limit");

        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, child.dwProcessId);
        Require(process != nullptr, "Could not inspect performance test child");
        Require(GetPriorityClass(process) == BELOW_NORMAL_PRIORITY_CLASS, "CPU priority was not applied");
        MEMORY_PRIORITY_INFORMATION memory{};
        Require(GetProcessInformation(process, ProcessMemoryPriority, &memory, sizeof(memory)) != FALSE && memory.MemoryPriority == 1,
            "Memory priority was not applied");
        DWORD_PTR processMask = 0, systemMask = 0;
        Require(GetProcessAffinityMask(process, &processMask, &systemMask) != FALSE && processMask == 1,
            "CPU affinity was not applied");
        PROCESS_POWER_THROTTLING_STATE throttling{};
        throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        Require(GetProcessInformation(process, ProcessPowerThrottling, &throttling, sizeof(throttling)) != FALSE &&
            (throttling.ControlMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) != 0 &&
            (throttling.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) != 0,
            "Efficiency mode was not applied");
        CloseHandle(process);
        std::cout << "Performance settings and retry passed; test token elevated: "
            << (tokenElevationKnown && currentElevation.TokenIsElevated ? "yes" : "no/unknown")
            << "; initial I/O High result: " << ToUtf8(highIoStatus) << "\n";
    }

    static void TestWatchLifecycle(bool useEtw)
    {
        wchar_t executable[32768]{};
        Require(GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))) != 0,
            "Test executable unavailable");
        const auto directory = std::filesystem::temp_directory_path() /
            (L"LaunchMate-watch-lifecycle-" + std::to_wstring(GetCurrentProcessId()) + (useEtw ? L"-etw" : L"-poll"));
        std::filesystem::create_directory(directory);
        const auto childPath = directory / L"LaunchMateLifecycleChild.exe";
        std::filesystem::copy_file(executable, childPath, std::filesystem::copy_options::overwrite_existing);
        struct FileCleanup
        {
            std::filesystem::path file, directory;
            ~FileCleanup() { std::error_code error; std::filesystem::remove(file, error); std::filesystem::remove(directory, error); }
        } fileCleanup{childPath, directory};

        std::mutex statusMutex;
        std::condition_variable statusChanged;
        std::vector<std::wstring> statuses;
        ProcessMonitor monitor([&](const std::wstring& status)
        {
            {
                std::scoped_lock lock(statusMutex);
                statuses.push_back(status);
            }
            statusChanged.notify_all();
        });
        monitor.SetPollInterval(100);
        monitor.SetActivePollInterval(100);
        AppConfiguration config;
        config.useEtw = useEtw;
        WatchedProcessRule rule;
        rule.displayName = L"Lifecycle test";
        rule.processName = childPath.filename().wstring();
        rule.executablePath = childPath.wstring();
        config.watchedProcesses.push_back(rule);
        monitor.UpdateConfiguration(config);
        monitor.Start();

        std::wstring command = L"\"" + childPath.wstring() + L"\" --idle-child";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION child{};
        Require(CreateProcessW(childPath.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, directory.c_str(), &startup, &child) != FALSE, "Could not start disposable watch child");
        CloseHandle(child.hThread);
        struct ChildCleanup
        {
            HANDLE process;
            ~ChildCleanup()
            {
                if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) TerminateProcess(process, 0);
                WaitForSingleObject(process, 2000);
                CloseHandle(process);
            }
        } childCleanup{child.hProcess};

        const auto waitForStatus = [&](const std::wstring& fragment)
        {
            std::unique_lock lock(statusMutex);
            return statusChanged.wait_for(lock, std::chrono::seconds(8), [&]
            {
                return std::any_of(statuses.begin(), statuses.end(), [&](const auto& status)
                {
                    return status.find(fragment) != std::wstring::npos;
                });
            });
        };
        Require(waitForStatus(L"Lifecycle test detected"), "Watch start was not detected with selected monitoring mode");
        Require(TerminateProcess(child.hProcess, 0) != FALSE, "Could not stop disposable watch child");
        WaitForSingleObject(child.hProcess, 2000);
        Require(waitForStatus(L"Lifecycle test session ended"), "Watch exit was not detected with selected monitoring mode");
        monitor.Stop();

        std::wstring mode = useEtw ? L"ETW requested" : L"polling";
        {
            std::scoped_lock lock(statusMutex);
            const auto fallback = std::find_if(statuses.begin(), statuses.end(), [](const auto& status)
            {
                return status.find(L"ETW could not start; using process polling") != std::wstring::npos;
            });
            const auto active = std::find_if(statuses.begin(), statuses.end(), [](const auto& status)
            {
                return status.find(L"ETW process monitoring active") != std::wstring::npos;
            });
            if (fallback != statuses.end()) mode += L" (polling fallback)";
            else if (active != statuses.end()) mode += L" (active)";
        }
        std::cout << "Watch start and exit passed with " << ToUtf8(mode) << ".\n";
    }

    static void TestDetectionAndLaunch()
    {
        wchar_t executable[32768]{};
        Require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "Test executable unavailable");
        const auto directory = std::filesystem::temp_directory_path() /
            (L"LaunchMate-launch-test-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directory(directory);
        const auto childPath = directory / L"LaunchMateTestChild.exe";
        std::filesystem::copy_file(executable, childPath, std::filesystem::copy_options::overwrite_existing);
        struct Cleanup
        {
            std::filesystem::path file, directory;
            ~Cleanup() { std::error_code error; std::filesystem::remove(file, error); std::filesystem::remove(directory, error); }
        } cleanup{childPath, directory};
        std::wstring lastStatus;
        ProcessMonitor monitor([&](const auto& status) { lastStatus = status; });
        AppConfiguration config;
        WatchedProcessRule rule;
        rule.processName = L"  \"" + std::wstring(executable) + L"\"  ";
        rule.executablePath = executable;
        LaunchProgram child;
        child.filePath = childPath.wstring();
        child.arguments = L"--idle-child-check-directory";
        rule.programsToLaunch.push_back(child);
        config.watchedProcesses.push_back(rule);
        WatchedProcessRule absent;
        absent.processName = L"launchmate-nonexistent-regression-test.exe";
        config.watchedProcesses.push_back(absent);
        const auto states = monitor.GetProcessStates(config.watchedProcesses);
        Require(states[0] == L"Running" && states[1] == L"Stopped", "Live process detection failed");
        monitor.UpdateConfiguration(config);
        monitor.running_ = true;
        struct RestorePriority
        {
            DWORD previous{GetPriorityClass(GetCurrentProcess())};
            ~RestorePriority()
            {
                SetPriorityClass(GetCurrentProcess(), previous);
                PROCESS_POWER_THROTTLING_STATE automatic{PROCESS_POWER_THROTTLING_CURRENT_VERSION};
                SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &automatic, sizeof(automatic));
            }
        } restorePriority;
        // Same state as LaunchMate itself: idle priority plus EcoQoS (efficiency mode).
        Require(SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS) != FALSE, "Cannot lower test parent priority");
        PROCESS_POWER_THROTTLING_STATE eco{PROCESS_POWER_THROTTLING_CURRENT_VERSION,
            PROCESS_POWER_THROTTLING_EXECUTION_SPEED, PROCESS_POWER_THROTTLING_EXECUTION_SPEED};
        Require(SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &eco, sizeof(eco)) != FALSE,
            "Cannot enable test parent efficiency mode");
        monitor.CheckRules();
        Require(GetPriorityClass(GetCurrentProcess()) == IDLE_PRIORITY_CLASS, "Launch did not restore parent low priority");
        Require(monitor.activeRules_.size() == 1, "Real snapshot failed to activate watched rule");
        Require(monitor.startedPrograms_.size() == 1, "Launch ownership missing");
        const auto& records = monitor.startedPrograms_.begin()->second;
        if (records.size() != 1 || records[0].startedProcessHandles.empty())
            throw std::runtime_error("Program did not launch: " + ToUtf8(lastStatus));
        const auto owned = records[0].startedProcessHandles.front();
        Require(GetPriorityClass(owned.get()) == NORMAL_PRIORITY_CLASS, "Launched app inherited parent low priority");
        PROCESS_POWER_THROTTLING_STATE childThrottling{PROCESS_POWER_THROTTLING_CURRENT_VERSION};
        Require(GetProcessInformation(owned.get(), ProcessPowerThrottling, &childThrottling, sizeof(childThrottling)) != FALSE &&
            (childThrottling.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) == 0,
            "Launched app inherited parent efficiency mode");
        if (WaitForSingleObject(owned.get(), 0) != WAIT_TIMEOUT)
        {
            DWORD exitCode = 0;
            GetExitCodeProcess(owned.get(), &exitCode);
            throw std::runtime_error("Launched child exited: " + std::to_string(exitCode));
        }
        HANDLE childToken = nullptr;
        Require(OpenProcessToken(owned.get(), TOKEN_QUERY, &childToken) != FALSE, "Cannot inspect launched child token");
        TOKEN_ELEVATION childElevation{};
        DWORD tokenSize = 0;
        const bool tokenRead = GetTokenInformation(childToken, TokenElevation, &childElevation,
            sizeof(childElevation), &tokenSize) != FALSE;
        CloseHandle(childToken);
        Require(tokenRead && !childElevation.TokenIsElevated, "Launched child unexpectedly has administrator rights");
        monitor.Stop();
        Require(WaitForSingleObject(owned.get(), 0) == WAIT_OBJECT_0, "Launched child was not cleaned up");
        std::cout << "Real snapshot detection, launch without inherited low priority or efficiency mode, and owned-child cleanup passed.\n";
    }

    static void TestHandoffOwnership()
    {
        wchar_t executable[32768]{};
        Require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "Test executable unavailable");
        const auto directory = std::filesystem::temp_directory_path() /
            (L"LaunchMate-handoff-test-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directory(directory);
        const auto childPath = directory / L"LaunchMateHandoffChild.exe";
        std::filesystem::copy_file(executable, childPath, std::filesystem::copy_options::overwrite_existing);
        struct Cleanup
        {
            std::filesystem::path file, directory;
            ~Cleanup() { std::error_code error; std::filesystem::remove(file, error); std::filesystem::remove(directory, error); }
        } cleanup{childPath, directory};

        ProcessMonitor monitor([](const auto&) {});
        WatchedProcessRule rule;
        rule.processName = std::filesystem::path(executable).filename().wstring();
        rule.executablePath = executable;
        LaunchProgram program;
        program.displayName = L"handoff child";
        program.filePath = std::filesystem::path(std::getenv("ComSpec")).wstring();
        program.arguments = L"/c \"\"" + childPath.wstring() + L"\" --idle-child-window\"";
        rule.programsToLaunch.push_back(program);
        AppConfiguration config;
        config.watchedProcesses.push_back(rule);
        monitor.UpdateConfiguration(config);
        monitor.running_ = true;
        monitor.CheckRules();
        Require(!monitor.startedPrograms_.empty(), "Handoff launch record missing");
        const auto& records = monitor.startedPrograms_.begin()->second;
        Require(records.size() == 1 && !records[0].startedProcessHandles.empty(),
            "Handoff child was not discovered after its starter exited");
        const auto childIt = std::find_if(records[0].startedProcessHandles.begin(), records[0].startedProcessHandles.end(),
            [](const auto& process) { return WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT; });
        Require(childIt != records[0].startedProcessHandles.end(), "Handoff child is not running");
        const auto child = *childIt;
        monitor.Stop();
        Require(WaitForSingleObject(child.get(), 0) == WAIT_OBJECT_0, "Handoff child was not closed");
        std::cout << "Launcher handoff ownership and cleanup passed.\n";
    }

    static void TestBatchStop()
    {
        wchar_t executable[32768]{};
        Require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "Test executable unavailable");
        std::vector<std::shared_ptr<void>> children;
        for (int i = 0; i < 5; ++i)
        {
            std::wstring command = L"\"" + std::wstring(executable) + L"\" --idle-child";
            STARTUPINFOW startup{sizeof(startup)};
            PROCESS_INFORMATION info{};
            Require(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, nullptr, &startup, &info) != FALSE, "Could not create disposable test child");
            CloseHandle(info.hThread);
            children.emplace_back(info.hProcess, [](void* process)
            {
                if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) TerminateProcess(process, 0);
                WaitForSingleObject(process, 2000);
                CloseHandle(process);
            });
        }
        ProcessMonitor monitor([](const auto&) {});
        monitor.running_ = true;
        ProcessMonitor::RuntimeRule rule;
        rule.processKey = L"batch-test";
        ProcessMonitor::LaunchedProgramRecord owned;
        owned.executablePath = executable;
        owned.startedProcessHandles.push_back(children.front());
        monitor.startedPrograms_[rule.processKey].push_back(std::move(owned));
        monitor.StopProgramsForRule(rule);
        Require(WaitForSingleObject(children.front().get(), 0) == WAIT_OBJECT_0, "Owned process was not stopped");
        for (size_t i = 1; i < children.size(); ++i)
            Require(WaitForSingleObject(children[i].get(), 0) == WAIT_TIMEOUT, "Unrelated same-executable process was stopped");
        // Simulate a shell launch whose PID was unavailable during the initial scan.
        ProcessMonitor::LaunchedProgramRecord late;
        late.executablePath = executable;
        FILETIME exited{}, kernel{}, user{};
        Require(GetProcessTimes(children[1].get(), &late.launchTime, &exited, &kernel, &user) != FALSE,
            "Cannot read late-launch creation time");
        for (size_t i = 2; i < children.size(); ++i) late.existingProcessIds.insert(GetProcessId(children[i].get()));
        monitor.startedPrograms_[rule.processKey].push_back(std::move(late));
        monitor.StopProgramsForRule(rule);
        Require(WaitForSingleObject(children[1].get(), 0) == WAIT_OBJECT_0,
            "Exit-time lookup failed to close a late/unreported process");
        for (size_t i = 2; i < children.size(); ++i)
            Require(WaitForSingleObject(children[i].get(), 0) == WAIT_TIMEOUT,
                "Exit-time lookup stopped a pre-existing process");
        ProcessStopAction action;
        action.processName = std::filesystem::path(executable).filename().wstring();
        action.executablePath = executable;
        action.forceAfterMilliseconds = 500;
        rule.processesToStop.push_back(action);
        const auto start = GetTickCount64();
        monitor.ExecuteStartActions(rule);
        const auto elapsed = GetTickCount64() - start;
        for (const auto& child : children)
            Require(WaitForSingleObject(child.get(), 0) == WAIT_OBJECT_0, "Disposable child was not stopped");
        Require(elapsed < 1600, "Grace period was applied per process instead of per action");
        std::cout << "Batch stop: four disposable processes, 500 ms grace, " << elapsed << " ms total.\n";
        monitor.Stop();
    }

    static void Run()
    {
        ProcessMonitor monitor([](const auto&) {});
        monitor.running_ = true;
        ProcessMonitor::RuntimeRule rule;
        rule.processKey = L"launchmate-test-watched.exe";
        rule.displayName = L"Original session";
        rule.powerSchemeGuid = L"fake-test-plan";
        rule.servicesToStop = {L"fake-test-service"};
        rule.hasMonitorPowerSetup = true;
        ProcessMonitor::RuntimeConfiguration configuration;
        configuration.watchedRules = {rule};
        ProcessMonitor::ProcessSnapshot running;
        running.valid = true;
        running.processIdsByName[rule.processKey] = {123};
        monitor.ApplySnapshot(configuration, running);
        Require(powerApplied == 1 && displaysApplied == 1, "Session was not started");
        Require(monitor.serviceOwnerKey_ == rule.processKey, "Partial service changes lost their owner");

        configuration.watchedRules[0].displayName = L"Edited session";
        monitor.ApplySnapshot(configuration, running);
        Require(powerApplied == 1, "Editing restarted the active session");
        Require(monitor.activeRules_.at(rule.processKey).displayName == L"Original session", "Active settings were overwritten");
        configuration.watchedRules.clear();
        monitor.ApplySnapshot(configuration, running);
        Require(monitor.activeRules_.size() == 1, "Deleting a rule lost its running session");

        ProcessMonitor::ProcessSnapshot failed;
        monitor.ApplySnapshot(configuration, failed);
        Require(monitor.activeRules_.size() == 1 && powerRestored == 0, "Failed snapshot looked like process exit");
        ProcessMonitor::ProcessSnapshot exited;
        exited.valid = true;
        monitor.ApplySnapshot(configuration, exited);
        Require(monitor.activeRules_.empty() && powerRestored == 1 && servicesRestored == 1 && displaysApplied == 2,
            "Deleted rule did not restore its session on exit");

        configuration.watchedRules = {rule};
        monitor.ApplySnapshot(configuration, running);
        monitor.Stop();
        Require(powerRestored == 2 && servicesRestored == 2 && displaysApplied == 4, "Stop did not restore all session state");
        Require(monitor.startedPrograms_.empty() && monitor.previousMonitorSetups_.empty(), "Stop retained stale session data");

        monitor.running_ = true;
        const auto start = GetTickCount64();
        ResetEvent(monitor.stopEvent_);
        std::thread waiter([&] { Require(!monitor.WaitForDelay(60000), "Cancelled delay completed normally"); });
        Sleep(60);
        monitor.running_ = false;
        SetEvent(monitor.stopEvent_);
        waiter.join();
        Require(GetTickCount64() - start < 2000, "User delay did not cancel promptly");
        Require(monitor.CaptureProcessSnapshot(false).valid, "Real process snapshot was marked invalid");
    }
};

void TestBackgroundTasks()
{
    BackgroundTask<int> task;
    auto release = std::make_shared<std::promise<void>>();
    auto gate = release->get_future().share();
    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    auto cancelled = std::make_shared<std::promise<bool>>();
    auto cancelledFuture = cancelled->get_future();
    Require(task.Start([gate, &entered, cancelled](const std::atomic_bool& stop)
    {
        entered.set_value();
        gate.wait();
        cancelled->set_value(stop.load());
        return 1;
    }), "Worker did not start");
    enteredFuture.wait();
    Require(!task.Start([](const auto&) { return 2; }), "Duplicate refresh was allowed");
    std::optional<int> result;
    Require(!task.Poll(result), "Unfinished result was published");
    const auto before = GetTickCount64();
    task.Cancel();
    Require(GetTickCount64() - before < 250, "Cancellation blocked the UI");
    Require(task.Start([](const auto&) { return 42; }), "Replacement task failed");
    release->set_value();
    Require(cancelledFuture.get(), "Worker did not see cancellation");
    const auto poll = [&]()
    {
        const auto deadline = GetTickCount64() + 5000;
        while (!task.Poll(result))
        {
            Require(GetTickCount64() < deadline, "Background task did not finish");
            Sleep(1);
        }
    };
    poll();
    Require(result && *result == 42, "Cancelled result replaced newer data");
    Require(!task.Running(), "Completed worker stayed busy");
    Require(task.Start([](const auto&) -> int { throw std::runtime_error("test failure"); }), "Exception test did not start");
    poll();
    Require(!result && !task.Running(), "Worker failure was not recoverable");

    auto done = std::make_shared<std::promise<bool>>();
    auto doneFuture = done->get_future();
    auto closeGate = std::make_shared<std::promise<void>>();
    const auto closeFuture = closeGate->get_future().share();
    {
        BackgroundTask<int> closing;
        Require(closing.Start([done, closeFuture](const auto& stop)
        {
            closeFuture.wait();
            done->set_value(stop.load());
            return 99;
        }), "Close test failed to start");
    }
    closeGate->set_value();
    Require(doneFuture.get(), "Owner destruction did not cancel publication");
}

void TestDisplayOverlaps()
{
    // A 4K main monitor captured alone at 0,0 plus three monitors from another
    // desktop at 0, 2560 and 5120: all four must end up side by side.
    const auto display = [](const wchar_t* name, LONG x, UINT width, bool enabled, bool primary)
    {
        MonitorPowerSetup::DisplayPath path;
        path.displayName = name;
        path.positionX = x;
        path.width = width;
        path.enabled = enabled;
        path.isPrimary = primary;
        return path;
    };
    std::vector<MonitorPowerSetup::DisplayPath> displays{
        display(L"LG1", 0, 2560, false, false), display(L"LG2", 5120, 2560, true, false),
        display(L"LG3", 2560, 2560, true, false), display(L"U28", 0, 3840, true, true)};
    SeparateOverlappingDisplays(displays);
    Require(displays[3].positionX == 0, "Main monitor keeps its place");
    Require(displays[0].positionX == 3840 && displays[2].positionX == 6400 && displays[1].positionX == 8960,
        "Overlapping monitors move right in their order");

    auto enabledOnly = std::vector<MonitorPowerSetup::DisplayPath>{
        display(L"LG1", 0, 2560, false, false), display(L"U28", 0, 3840, true, true), display(L"LG3", 2560, 2560, true, false)};
    SeparateOverlappingDisplays(enabledOnly, true);
    Require(enabledOnly[0].positionX == 0 && enabledOnly[2].positionX == 3840, "Disabled monitors are ignored when applying");

    std::vector<MonitorPowerSetup::DisplayPath> clean{
        display(L"A", -2560, 2560, true, false), display(L"B", 0, 2560, true, true), display(L"C", 2560, 2560, true, false)};
    const auto cleanBefore = clean;
    SeparateOverlappingDisplays(clean);
    for (size_t index = 0; index < clean.size(); ++index)
        Require(clean[index].positionX == cleanBefore[index].positionX, "A layout without overlaps stays unchanged");
    auto before = display(L"U28", 0, 3840, true, true);
    before.monitorName = L"U28D590";
    before.targetId = 512;
    before.targetAdapterLowPart = 59966;
    auto afterRestart = before;
    afterRestart.targetAdapterLowPart = 61234;
    Require(IsSameMonitor(before, afterRestart), "A monitor stays the same when Windows assigns new adapter ids");
    auto otherMonitor = afterRestart;
    otherMonitor.targetId = 4353;
    Require(!IsSameMonitor(before, otherMonitor), "Monitors on different targets differ");
    std::cout << "Overlapping monitors are placed side by side." << std::endl;
}

void TestJson()
{
    using namespace jsonlite;
    Require(Parse(" \n {\"a\": [true, null, -1.25e+2]} \t").IsObject(), "Valid JSON rejected");
    Require(Parse(R"("\u00e4\u20ac\ud83d\ude00")").AsString() == "\xC3\xA4\xE2\x82\xAC\xF0\x9F\x98\x80", "Unicode decoding failed");
    Require(Parse(R"("\u0000")").AsString() == std::string(1, '\0'), "NUL escape failed");
    std::string controls;
    for (int i = 0; i < 32; ++i) controls.push_back(static_cast<char>(i));
    Require(Parse(Serialize(Value(controls))).AsString() == controls, "Control character round trip failed");
    for (const auto& invalid : {"true trailing", "01", "1-2", "1.", "+1", "1e", "--1", "1e999", "[1,]",
        "\"raw\nnewline\"", "\"\\uD800\"", "\"\\uDC00\"", "\"\\uD800\\u0041\"", "\"\\uZZZZ\"", "\vnull"})
    {
        bool rejected = false;
        try { Parse(invalid); } catch (const std::exception&) { rejected = true; }
        Require(rejected, invalid);
    }
    bool rejected = false;
    try { Parse(std::string(200, '[') + "0" + std::string(200, ']')); }
    catch (const std::exception&) { rejected = true; }
    Require(rejected, "Excessive nesting accepted");
    struct CommaPunctuation : std::numpunct<char> { char do_decimal_point() const override { return ','; } };
    const auto previous = std::locale();
    std::locale::global(std::locale(previous, new CommaPunctuation));
    const auto text = Serialize(Value(1.25));
    std::locale::global(previous);
    Require(text == "1.25", "Serialization depends on system locale");
    const double precise = 1.2345678901234567;
    Require(Parse(Serialize(Value(precise))).AsNumber() == precise, "Floating point precision lost");
    rejected = false;
    try { Serialize(Value(std::numeric_limits<double>::infinity())); }
    catch (const std::exception&) { rejected = true; }
    Require(rejected, "Non-finite number serialized");
}

void TestAtomicFile()
{
    const auto directory = std::filesystem::temp_directory_path() /
        (L"LaunchMate-core-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    Require(std::filesystem::create_directory(directory), "Could not create isolated test directory");
    const auto path = directory / L"config.json";
    const auto read = [&]() { std::ifstream stream(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(stream), {}); };
    Require(WriteFileAtomically(path, "original"), "Initial atomic write failed");
    Require(WriteFileAtomically(path, "replacement"), "Atomic replacement failed");
    Require(read() == "replacement", "Replacement data incorrect");
    HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Require(locked != INVALID_HANDLE_VALUE, "Could not lock test file");
    const bool saved = WriteFileAtomically(path, "must not overwrite");
    CloseHandle(locked);
    Require(!saved && read() == "replacement", "Failed replacement damaged existing file");
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) { (void)entry; ++count; }
    Require(count == 1, "Temporary file leaked");
    std::filesystem::remove(path);
    std::filesystem::remove(directory);
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--idle-child-window")
    {
        const wchar_t className[] = L"LaunchMateCoreTestWindow";
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = className;
        RegisterClassW(&windowClass);
        const HWND window = CreateWindowExW(0, className, L"LaunchMate test child", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 320, 200, nullptr, nullptr, windowClass.hInstance, nullptr);
        if (!window) return 2;
        ShowWindow(window, SW_SHOW);
        Sleep(60000);
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--idle-child-pump")
    {
        // Closes on WM_CLOSE with a distinctive exit code, unlike TerminateProcess.
        const wchar_t className[] = L"LaunchMateCoreTestPumpWindow";
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = [](HWND window, UINT message, WPARAM wParam, LPARAM lParam) -> LRESULT
        {
            if (message == WM_DESTROY) { PostQuitMessage(7); return 0; }
            return DefWindowProcW(window, message, wParam, lParam);
        };
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = className;
        RegisterClassW(&windowClass);
        if (!CreateWindowExW(0, className, L"LaunchMate pump child", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 320, 200, nullptr, nullptr, windowClass.hInstance, nullptr)) return 2;
        MSG message{};
        const auto deadline = GetTickCount64() + 60000;
        while (GetTickCount64() < deadline)
        {
            if (MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT) != WAIT_OBJECT_0) continue;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT) return static_cast<int>(message.wParam);
                DispatchMessageW(&message);
            }
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--idle-child-check-directory")
    {
        wchar_t executable[32768]{};
        if (!GetModuleFileNameW(nullptr, executable, 32768) ||
            std::filesystem::current_path() != std::filesystem::path(executable).parent_path()) return 2;
        Sleep(60000);
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--idle-child") { Sleep(60000); return 0; }
    try
    {
        TestJson();
        TestDisplayOverlaps();
        TestBackgroundTasks();
        TestAtomicFile();
        ProcessMonitorTestAccess::Run();
        ProcessMonitorTestAccess::TestPerformanceSettings();
        ProcessMonitorTestAccess::TestWatchLifecycle(false);
        ProcessMonitorTestAccess::TestWatchLifecycle(true);
        ProcessMonitorTestAccess::TestBatchStop();
        ProcessMonitorTestAccess::TestDetectionAndLaunch();
        ProcessMonitorTestAccess::TestHandoffOwnership();
        ProcessMonitorTestAccess::TestGracefulProgramClose();
        ProcessMonitorTestAccess::TestLaunchIgnoresUnrelatedWindows();
        ProcessMonitorTestAccess::TestStopActionCapturesRestartArguments();
        ProcessMonitorTestAccess::TestEtwInstanceHandles();
        ProcessMonitorTestAccess::TestReviewRegressions();
        std::cout << "JSON, atomic persistence, and monitor session regression tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
