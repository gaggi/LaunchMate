#pragma once

#include "Models.h"
#include "RuleActionsDialog.h"

#include <functional>
#include <windows.h>

// What the main window shares with the pages it embeds.
struct PageContext
{
    HINSTANCE instance{};
    HFONT headingFont{};
    HFONT textFont{};
    AppConfiguration* configuration{};
    // Saves once edits pause; used for ordinary changes.
    std::function<void()> scheduleSave;
    // Saves right away; needed when saving has side effects such as the startup task.
    std::function<void()> saveNow;
};

// Each function creates a page as a child of `parent` (the page scroll host). The
// page owns itself and is gone when its window is destroyed. Rules passed by
// reference must outlive the page.
HWND CreateSettingsPage(const PageContext& context, HWND parent, std::function<void()> checkForUpdates);
HWND CreateDisplaysPage(const PageContext& context, HWND parent, std::function<bool(size_t)> applySetup);
HWND CreateServicesPage(const PageContext& context, HWND parent, WatchedProcessRule& rule);
// For RuleSection::StartPrograms and RuleSection::StopProcesses.
HWND CreateRuleAppsPage(const PageContext& context, HWND parent, WatchedProcessRule& rule, RuleSection section);
