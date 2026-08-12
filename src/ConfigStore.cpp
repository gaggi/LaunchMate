#include "ConfigStore.h"

#include "JsonLite.h"
#include "Utils.h"

#include <algorithm>
#include <fstream>
#include <shlobj.h>

namespace
{
    using jsonlite::Array;
    using jsonlite::Object;
    using jsonlite::Value;

    std::wstring ReadWideString(const Object& object, const char* key, const std::wstring& fallback = {})
    {
        const auto it = object.find(key);
        return it != object.end() && it->second.IsString() ? ToWide(it->second.AsString()) : fallback;
    }

    bool ReadBool(const Object& object, const char* key, bool fallback = false)
    {
        const auto it = object.find(key);
        return it != object.end() ? it->second.AsBool(fallback) : fallback;
    }

    int ReadInt(const Object& object, const char* key, int fallback = 0)
    {
        const auto it = object.find(key);
        return it != object.end() ? static_cast<int>(it->second.AsNumber(fallback)) : fallback;
    }

    double ReadNumber(const Object& object, const char* key, double fallback = 0.0)
    {
        const auto it = object.find(key);
        return it != object.end() ? it->second.AsNumber(fallback) : fallback;
    }

    Value ToJson(const LaunchProgram& program)
    {
        Object object;
        object["DisplayName"] = ToUtf8(program.displayName);
        object["FilePath"] = ToUtf8(program.filePath);
        object["Arguments"] = ToUtf8(program.arguments);
        object["CloseWhenGameStops"] = program.closeWhenGameStops;
        object["WaitTimeMilliseconds"] = static_cast<double>(program.waitTimeMilliseconds);
        object["CloseDelayMilliseconds"] = static_cast<double>(program.closeDelayMilliseconds);
        return object;
    }

    LaunchProgram LaunchProgramFromJson(const Object& object)
    {
        LaunchProgram program;
        program.displayName = ReadWideString(object, "DisplayName");
        program.filePath = ReadWideString(object, "FilePath");
        program.arguments = ReadWideString(object, "Arguments");
        program.closeWhenGameStops = ReadBool(object, "CloseWhenGameStops", true);
        program.waitTimeMilliseconds = ReadInt(object, "WaitTimeMilliseconds");
        program.closeDelayMilliseconds = ReadInt(object, "CloseDelayMilliseconds");
        return program;
    }

    Value ToJson(const ProcessStopAction& action)
    {
        Object object;
        object["DisplayName"] = ToUtf8(action.displayName);
        object["ProcessName"] = ToUtf8(action.processName);
        object["ExecutablePath"] = ToUtf8(action.executablePath);
        object["GracefulCloseFirst"] = action.gracefulCloseFirst;
        object["ForceAfterMilliseconds"] = static_cast<double>(action.forceAfterMilliseconds);
        object["RestartAfterWatchProcessEnds"] = action.restartAfterWatchProcessEnds;
        object["RestartDelayMilliseconds"] = static_cast<double>(action.restartDelayMilliseconds);
        return object;
    }

    ProcessStopAction ProcessStopActionFromJson(const Object& object)
    {
        ProcessStopAction action;
        action.displayName = ReadWideString(object, "DisplayName");
        action.processName = ReadWideString(object, "ProcessName");
        action.executablePath = ReadWideString(object, "ExecutablePath");
        action.gracefulCloseFirst = ReadBool(object, "GracefulCloseFirst", true);
        action.forceAfterMilliseconds = ReadInt(object, "ForceAfterMilliseconds", 3000);
        action.restartAfterWatchProcessEnds = ReadBool(object, "RestartAfterWatchProcessEnds", false);
        action.restartDelayMilliseconds = ReadInt(object, "RestartDelayMilliseconds");
        return action;
    }

    Value ToJson(const HomeAssistantAction& action)
    {
        Object object;
        object["DisplayName"] = ToUtf8(action.displayName);
        object["WebhookUrl"] = ToUtf8(action.webhookUrl);
        object["JsonPayload"] = ToUtf8(action.jsonPayload);
        object["WaitTimeMilliseconds"] = static_cast<double>(action.waitTimeMilliseconds);
        return object;
    }

    HomeAssistantAction HomeAssistantActionFromJson(const Object& object)
    {
        HomeAssistantAction action;
        action.displayName = ReadWideString(object, "DisplayName");
        action.webhookUrl = ReadWideString(object, "WebhookUrl");
        action.jsonPayload = ReadWideString(object, "JsonPayload", L"{}");
        action.waitTimeMilliseconds = ReadInt(object, "WaitTimeMilliseconds");
        return action;
    }

    Value ToJson(const CatalogProgram& program)
    {
        Object object;
        object["DisplayName"] = ToUtf8(program.displayName);
        object["FilePath"] = ToUtf8(program.filePath);
        return object;
    }

    CatalogProgram CatalogProgramFromJson(const Object& object)
    {
        CatalogProgram program;
        program.displayName = ReadWideString(object, "DisplayName");
        program.filePath = ReadWideString(object, "FilePath");
        return program;
    }

    Value ToJson(const MonitorPowerSetup::DisplayPath& path)
    {
        Object object;
        object["DisplayName"] = ToUtf8(path.displayName);
        object["MonitorName"] = ToUtf8(path.monitorName);
        object["SourceAdapterLowPart"] = static_cast<double>(path.sourceAdapterLowPart);
        object["SourceAdapterHighPart"] = static_cast<double>(path.sourceAdapterHighPart);
        object["SourceId"] = static_cast<double>(path.sourceId);
        object["TargetAdapterLowPart"] = static_cast<double>(path.targetAdapterLowPart);
        object["TargetAdapterHighPart"] = static_cast<double>(path.targetAdapterHighPart);
        object["TargetId"] = static_cast<double>(path.targetId);
        object["PositionX"] = static_cast<double>(path.positionX);
        object["PositionY"] = static_cast<double>(path.positionY);
        object["Width"] = static_cast<double>(path.width);
        object["Enabled"] = path.enabled;
        object["IsPrimary"] = path.isPrimary;
        return object;
    }

    MonitorPowerSetup::DisplayPath DisplayPathFromJson(const Object& object)
    {
        MonitorPowerSetup::DisplayPath path;
        path.displayName = ReadWideString(object, "DisplayName");
        path.monitorName = ReadWideString(object, "MonitorName");
        path.sourceAdapterLowPart = static_cast<DWORD>(ReadNumber(object, "SourceAdapterLowPart"));
        path.sourceAdapterHighPart = static_cast<LONG>(ReadNumber(object, "SourceAdapterHighPart"));
        path.sourceId = static_cast<UINT>(ReadNumber(object, "SourceId"));
        path.targetAdapterLowPart = static_cast<DWORD>(ReadNumber(object, "TargetAdapterLowPart"));
        path.targetAdapterHighPart = static_cast<LONG>(ReadNumber(object, "TargetAdapterHighPart"));
        path.targetId = static_cast<UINT>(ReadNumber(object, "TargetId"));
        path.positionX = static_cast<LONG>(ReadNumber(object, "PositionX"));
        path.positionY = static_cast<LONG>(ReadNumber(object, "PositionY"));
        path.width = static_cast<UINT>(ReadNumber(object, "Width"));
        path.enabled = ReadBool(object, "Enabled", true);
        path.isPrimary = ReadBool(object, "IsPrimary", path.positionX == 0 && path.positionY == 0);
        return path;
    }

    Value ToJson(const MonitorPowerSetup& setup)
    {
        Object object;
        object["Name"] = ToUtf8(setup.name);
        object["HotkeyModifiers"] = static_cast<double>(setup.hotkeyModifiers);
        object["HotkeyVirtualKey"] = static_cast<double>(setup.hotkeyVirtualKey);

        Array displayPaths;
        for (const auto& path : setup.displayPaths)
        {
            displayPaths.push_back(ToJson(path));
        }
        object["DisplayPaths"] = displayPaths;
        return object;
    }

    MonitorPowerSetup MonitorPowerSetupFromJson(const Object& object)
    {
        MonitorPowerSetup setup;
        setup.name = ReadWideString(object, "Name");
        setup.hotkeyModifiers = static_cast<UINT>(ReadInt(object, "HotkeyModifiers", 0));
        setup.hotkeyVirtualKey = static_cast<UINT>(ReadInt(object, "HotkeyVirtualKey", 0));

        const auto pathsIt = object.find("DisplayPaths");
        if (pathsIt != object.end() && pathsIt->second.IsArray())
        {
            for (const auto& item : pathsIt->second.AsArray())
            {
                if (!item.IsObject())
                {
                    continue;
                }

                auto path = DisplayPathFromJson(item.AsObject());
                if (!path.displayName.empty())
                {
                    setup.displayPaths.push_back(std::move(path));
                }
            }
        }

        return setup;
    }

    Value ToJson(const WatchedProcessRule& rule)
    {
        Object object;
        object["DisplayName"] = ToUtf8(rule.displayName);
        object["ProcessName"] = ToUtf8(rule.processName);
        object["ExecutablePath"] = ToUtf8(rule.executablePath);
        object["Enabled"] = rule.enabled;

        Array programs;
        for (const auto& program : rule.programsToLaunch)
        {
            programs.push_back(ToJson(program));
        }
        object["ProgramsToLaunch"] = programs;

        Array stopActions;
        for (const auto& action : rule.processesToStop)
        {
            stopActions.push_back(ToJson(action));
        }
        object["ProcessesToStop"] = stopActions;

        Array homeActions;
        for (const auto& action : rule.homeAssistantActions)
        {
            homeActions.push_back(ToJson(action));
        }
        object["HomeAssistantActions"] = homeActions;
        object["MonitorPowerSetupName"] = ToUtf8(rule.monitorPowerSetupName);
        object["MonitorPowerSetupDelayMilliseconds"] = static_cast<double>(rule.monitorPowerSetupDelayMilliseconds);
        object["RestoreMonitorPowerSetupOnExit"] = rule.restoreMonitorPowerSetupOnExit;
        object["RestoreMonitorPowerSetupDelayMilliseconds"] = static_cast<double>(rule.restoreMonitorPowerSetupDelayMilliseconds);
        return object;
    }

    WatchedProcessRule WatchedProcessFromJson(const Object& object)
    {
        WatchedProcessRule rule;
        rule.displayName = ReadWideString(object, "DisplayName");
        rule.processName = ReadWideString(object, "ProcessName");
        rule.executablePath = ReadWideString(object, "ExecutablePath");
        rule.enabled = ReadBool(object, "Enabled", true);
        rule.monitorPowerSetupName = ReadWideString(object, "MonitorPowerSetupName");
        rule.monitorPowerSetupDelayMilliseconds = ReadInt(object, "MonitorPowerSetupDelayMilliseconds");
        rule.restoreMonitorPowerSetupOnExit = ReadBool(object, "RestoreMonitorPowerSetupOnExit", true);
        rule.restoreMonitorPowerSetupDelayMilliseconds = ReadInt(object, "RestoreMonitorPowerSetupDelayMilliseconds");

        const auto it = object.find("ProgramsToLaunch");
        if (it != object.end() && it->second.IsArray())
        {
            for (const auto& item : it->second.AsArray())
            {
                if (item.IsObject())
                {
                    rule.programsToLaunch.push_back(LaunchProgramFromJson(item.AsObject()));
                }
            }
        }

        const auto stopIt = object.find("ProcessesToStop");
        if (stopIt != object.end() && stopIt->second.IsArray())
        {
            for (const auto& item : stopIt->second.AsArray())
            {
                if (item.IsObject()) rule.processesToStop.push_back(ProcessStopActionFromJson(item.AsObject()));
            }
        }

        const auto homeIt = object.find("HomeAssistantActions");
        if (homeIt != object.end() && homeIt->second.IsArray())
        {
            for (const auto& item : homeIt->second.AsArray())
            {
                if (item.IsObject()) rule.homeAssistantActions.push_back(HomeAssistantActionFromJson(item.AsObject()));
            }
        }

        return rule;
    }
}

ConfigStore::ConfigStore()
{
    PWSTR roamingPath = nullptr;
    SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roamingPath);
    const std::filesystem::path basePath = roamingPath != nullptr ? roamingPath : L".";
    CoTaskMemFree(roamingPath);

    const auto appDirectory = basePath / L"LaunchMate";
    std::filesystem::create_directories(appDirectory);
    configPath_ = appDirectory / L"config.json";
}

const std::filesystem::path& ConfigStore::Path() const noexcept
{
    return configPath_;
}

AppConfiguration ConfigStore::Load() const
{
    if (!std::filesystem::exists(configPath_))
    {
        return AppConfiguration::CreateDefault();
    }

    std::ifstream stream(configPath_, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

    try
    {
        const auto root = jsonlite::Parse(content);
        if (!root.IsObject())
        {
            return AppConfiguration::CreateDefault();
        }

        const auto& object = root.AsObject();
        AppConfiguration config;
        config.minimizeToTray = ReadBool(object, "MinimizeToTray", true);
        config.closeToTray = ReadBool(object, "CloseToTray", true);
        config.startWithWindows = ReadBool(object, "StartWithWindows", false);
        config.startInTray = ReadBool(object, "StartInTray", false);
        config.startMonitoringOnLaunch = ReadBool(object, "StartMonitoringOnLaunch", false);
        config.checkForUpdatesOnStartup = ReadBool(object, "CheckForUpdatesOnStartup", true);
        config.windowWidth = ReadInt(object, "WindowWidth", 1210);
        config.windowHeight = ReadInt(object, "WindowHeight", 730);
        config.startMaximized = ReadBool(object, "StartMaximized", false);

        const auto leftIt = object.find("WindowLeft");
        const auto topIt = object.find("WindowTop");
        if (leftIt != object.end() && leftIt->second.IsNumber() && topIt != object.end() && topIt->second.IsNumber())
        {
            config.windowLeft = static_cast<int>(leftIt->second.AsNumber());
            config.windowTop = static_cast<int>(topIt->second.AsNumber());
            config.hasWindowPlacement = true;
        }

        const auto catalogProgramsIt = object.find("CatalogPrograms");
        if (catalogProgramsIt != object.end() && catalogProgramsIt->second.IsArray())
        {
            for (const auto& item : catalogProgramsIt->second.AsArray())
            {
                if (item.IsObject())
                {
                    config.catalogPrograms.push_back(CatalogProgramFromJson(item.AsObject()));
                }
            }
        }

        const auto monitorSetupsIt = object.find("MonitorPowerSetups");
        if (monitorSetupsIt != object.end() && monitorSetupsIt->second.IsArray())
        {
            for (const auto& item : monitorSetupsIt->second.AsArray())
            {
                if (item.IsObject())
                {
                    config.monitorPowerSetups.push_back(MonitorPowerSetupFromJson(item.AsObject()));
                }
            }
        }

        const auto detectedDisplaysIt = object.find("DetectedDisplays");
        if (detectedDisplaysIt != object.end() && detectedDisplaysIt->second.IsArray())
        {
            for (const auto& item : detectedDisplaysIt->second.AsArray())
            {
                if (!item.IsObject()) continue;
                auto path = DisplayPathFromJson(item.AsObject());
                if (!path.displayName.empty()) config.detectedDisplays.push_back(std::move(path));
            }
        }
        if (config.detectedDisplays.empty())
        {
            for (const auto& setup : config.monitorPowerSetups)
            {
                for (const auto& path : setup.displayPaths)
                {
                    const auto duplicate = std::find_if(
                        config.detectedDisplays.begin(),
                        config.detectedDisplays.end(),
                        [&path](const auto& existing)
                        {
                            return existing.targetAdapterLowPart == path.targetAdapterLowPart &&
                                existing.targetAdapterHighPart == path.targetAdapterHighPart &&
                                existing.targetId == path.targetId;
                        });
                    if (duplicate == config.detectedDisplays.end()) config.detectedDisplays.push_back(path);
                }
            }
        }

        const auto watchedIt = object.find("WatchedProcesses");
        if (watchedIt != object.end() && watchedIt->second.IsArray())
        {
            for (const auto& item : watchedIt->second.AsArray())
            {
                if (item.IsObject())
                {
                    config.watchedProcesses.push_back(WatchedProcessFromJson(item.AsObject()));
                }
            }
        }

        return config;
    }
    catch (...)
    {
        return AppConfiguration::CreateDefault();
    }
}

void ConfigStore::Save(const AppConfiguration& configuration) const
{
    Object object;
    object["MinimizeToTray"] = configuration.minimizeToTray;
    object["CloseToTray"] = configuration.closeToTray;
    object["StartWithWindows"] = configuration.startWithWindows;
    object["StartInTray"] = configuration.startInTray;
    object["StartMonitoringOnLaunch"] = configuration.startMonitoringOnLaunch;
    object["CheckForUpdatesOnStartup"] = configuration.checkForUpdatesOnStartup;
    object["WindowWidth"] = static_cast<double>(configuration.windowWidth);
    object["WindowHeight"] = static_cast<double>(configuration.windowHeight);
    object["StartMaximized"] = configuration.startMaximized;

    if (configuration.hasWindowPlacement)
    {
        object["WindowLeft"] = static_cast<double>(configuration.windowLeft);
        object["WindowTop"] = static_cast<double>(configuration.windowTop);
    }

    Array catalogPrograms;
    for (const auto& program : configuration.catalogPrograms)
    {
        catalogPrograms.push_back(ToJson(program));
    }
    object["CatalogPrograms"] = catalogPrograms;

    Array detectedDisplays;
    for (const auto& path : configuration.detectedDisplays)
    {
        detectedDisplays.push_back(ToJson(path));
    }
    object["DetectedDisplays"] = detectedDisplays;

    Array monitorPowerSetups;
    for (const auto& setup : configuration.monitorPowerSetups)
    {
        monitorPowerSetups.push_back(ToJson(setup));
    }
    object["MonitorPowerSetups"] = monitorPowerSetups;

    Array watchedProcesses;
    for (const auto& rule : configuration.watchedProcesses)
    {
        watchedProcesses.push_back(ToJson(rule));
    }
    object["WatchedProcesses"] = watchedProcesses;

    const auto text = jsonlite::Serialize(Value(object), 2);
    std::ofstream stream(configPath_, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
}
