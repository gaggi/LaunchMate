#include "ConfigStore.h"

#include "JsonLite.h"
#include "Utils.h"

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

    Value ToJson(const LaunchProgram& program)
    {
        Object object;
        object["DisplayName"] = ToUtf8(program.displayName);
        object["FilePath"] = ToUtf8(program.filePath);
        object["Arguments"] = ToUtf8(program.arguments);
        object["CloseWhenGameStops"] = program.closeWhenGameStops;
        object["WaitTimeMilliseconds"] = static_cast<double>(program.waitTimeMilliseconds);
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
        return program;
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

    Value ToJson(const MonitorPowerSetup& setup)
    {
        Object object;
        object["Name"] = ToUtf8(setup.name);
        object["HotkeyModifiers"] = static_cast<double>(setup.hotkeyModifiers);
        object["HotkeyVirtualKey"] = static_cast<double>(setup.hotkeyVirtualKey);

        Array enabledDisplays;
        for (const auto& display : setup.enabledDisplays)
        {
            enabledDisplays.push_back(ToUtf8(display));
        }
        object["EnabledDisplays"] = enabledDisplays;
        return object;
    }

    MonitorPowerSetup MonitorPowerSetupFromJson(const Object& object)
    {
        MonitorPowerSetup setup;
        setup.name = ReadWideString(object, "Name");
        setup.hotkeyModifiers = static_cast<UINT>(ReadInt(object, "HotkeyModifiers", 0));
        setup.hotkeyVirtualKey = static_cast<UINT>(ReadInt(object, "HotkeyVirtualKey", 0));

        const auto it = object.find("EnabledDisplays");
        if (it != object.end() && it->second.IsArray())
        {
            for (const auto& item : it->second.AsArray())
            {
                if (item.IsString())
                {
                    setup.enabledDisplays.push_back(ToWide(item.AsString()));
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
        return object;
    }

    WatchedProcessRule WatchedProcessFromJson(const Object& object)
    {
        WatchedProcessRule rule;
        rule.displayName = ReadWideString(object, "DisplayName");
        rule.processName = ReadWideString(object, "ProcessName");
        rule.executablePath = ReadWideString(object, "ExecutablePath");
        rule.enabled = ReadBool(object, "Enabled", true);

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
