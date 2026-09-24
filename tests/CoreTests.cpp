#include "AtomicFile.h"
#include "BackgroundTask.h"
#include <future>
#include "JsonLite.h"
#include "ProcessMonitor.h"
#include "IRacingPerformance.h"
#include "IRacingServices.h"
#include "MonitorPowerController.h"

#include <fstream>
#include <iostream>
#include <limits>
#include <locale>
#include <stdexcept>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    int powerApplied = 0, powerRestored = 0, servicesRestored = 0, displaysApplied = 0;
    bool pendingServices = false;
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
bool MonitorPowerController::CaptureSetup(MonitorPowerSetup&, std::wstring*) { return true; }
bool MonitorPowerController::ApplySetup(const MonitorPowerSetup&, const std::function<void(const std::wstring&)>&, std::wstring*)
{
    ++displaysApplied;
    return true;
}

struct ProcessMonitorTestAccess
{
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
    if (argc == 2 && std::string(argv[1]) == "--idle-child") { Sleep(60000); return 0; }
    try
    {
        TestJson();
        TestBackgroundTasks();
        TestAtomicFile();
        ProcessMonitorTestAccess::Run();
        ProcessMonitorTestAccess::TestBatchStop();
        std::cout << "JSON, atomic persistence, and monitor session regression tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
