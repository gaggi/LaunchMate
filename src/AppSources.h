#pragma once

#include "Models.h"

#include <atomic>
#include <string>
#include <vector>

// Programs the user can add to a rule: installed apps, running processes and known
// background apps. All functions may take a while and run on worker threads.

struct RunningProcessEntry
{
    std::wstring displayName;
    std::wstring processName;
    std::wstring executablePath;
    unsigned long processId{};
    double cpuUsagePercent{};
    unsigned long long memoryUsageBytes{};
    bool hasCpuUsage{};
    bool hasMemoryUsage{};
};

struct DetectedProcessEntry
{
    std::wstring displayName;
    std::wstring processName;
    std::wstring executablePath;
    std::wstring category;
    std::wstring effect;
    bool running{};
    bool verifiedRunningPath{};
    bool allowStop{};
};

// Manually added programs from `catalog`, known tools such as SimHub and installed apps
// with a resolvable executable.
std::vector<CatalogProgram> FindInstalledApps(const std::vector<CatalogProgram>& catalog, const std::atomic_bool& cancelled);
// One entry per executable; `watchedProcessName` is left out.
std::vector<RunningProcessEntry> CaptureRunningProcesses(const std::wstring& watchedProcessName, const std::atomic_bool& cancelled);
// Known background apps (cloud sync, launchers, overlays ...) with an estimated effect.
std::vector<DetectedProcessEntry> CaptureBackgroundApps(const std::wstring& watchedProcessName, const std::atomic_bool& cancelled);
bool ContainsInsensitive(const std::wstring& haystack, const std::wstring& needle);
// Executable names of running processes, sorted and without duplicates. Fast.
std::vector<std::wstring> RunningProcessNames();
