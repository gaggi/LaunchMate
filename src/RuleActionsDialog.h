#pragma once

#include "Models.h"

#include <functional>
#include <vector>
#include <windows.h>

// Sections of a rule, in the order the monitor runs them at the start of a session.
enum class RuleSection
{
    StartPrograms = 0,
    StopProcesses = 1,
    HomeAssistant = 2,
    MonitorConfig = 3,
    Performance = 4,
    WindowsServices = 5
};

// Small dialogs for one item of a rule. Return true when the user confirmed.
bool EditLaunchProgram(HINSTANCE instanceHandle, HWND owner, LaunchProgram& program);
bool EditStopAction(HINSTANCE instanceHandle, HWND owner, ProcessStopAction& action);

// The webhook, display or performance section of a rule as an embedded page (a
// child dialog of `parent`); apps and services have pages of their own. Edits apply
// to `rule` immediately and `changed` runs after each one, so the caller can save.
// `rule` and `monitorSetups` must outlive the page; DestroyWindow ends it.
HWND CreateRuleSectionPane(
    HINSTANCE instanceHandle,
    HWND parent,
    WatchedProcessRule& rule,
    const std::vector<MonitorPowerSetup>& monitorSetups,
    RuleSection section,
    std::function<void()> changed);
