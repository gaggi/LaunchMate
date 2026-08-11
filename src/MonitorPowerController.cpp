#include "MonitorPowerController.h"

#include <windows.h>

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace
{
    using DisplayPath = MonitorPowerSetup::DisplayPath;

    struct DisplayConfiguration
    {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths;
        std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    };

    std::wstring WindowsError(const std::wstring& prefix, LONG code)
    {
        wchar_t* messageBuffer = nullptr;
        FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            static_cast<DWORD>(code),
            0,
            reinterpret_cast<LPWSTR>(&messageBuffer),
            0,
            nullptr);

        std::wstring message = prefix + L" (" + std::to_wstring(code) + L")";
        if (messageBuffer != nullptr)
        {
            message += L": ";
            message += messageBuffer;
            LocalFree(messageBuffer);
        }
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n'))
        {
            message.pop_back();
        }
        return message;
    }

    bool QueryConfiguration(UINT flags, DisplayConfiguration& configuration, std::wstring* errorMessage)
    {
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            UINT32 pathCount = 0;
            UINT32 modeCount = 0;
            LONG result = GetDisplayConfigBufferSizes(flags, &pathCount, &modeCount);
            if (result != ERROR_SUCCESS)
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = WindowsError(L"GetDisplayConfigBufferSizes failed", result);
                }
                return false;
            }

            configuration.paths.resize(pathCount);
            configuration.modes.resize(modeCount);
            result = QueryDisplayConfig(
                flags,
                &pathCount,
                configuration.paths.data(),
                &modeCount,
                configuration.modes.data(),
                nullptr);
            if (result == ERROR_INSUFFICIENT_BUFFER)
            {
                continue;
            }
            if (result != ERROR_SUCCESS)
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = WindowsError(L"QueryDisplayConfig failed", result);
                }
                return false;
            }

            configuration.paths.resize(pathCount);
            configuration.modes.resize(modeCount);
            return true;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = L"The display configuration changed while it was being read.";
        }
        return false;
    }

    std::wstring SourceName(const DISPLAYCONFIG_PATH_INFO& path)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        name.header.size = sizeof(name);
        name.header.adapterId = path.sourceInfo.adapterId;
        name.header.id = path.sourceInfo.id;
        return DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS ? name.viewGdiDeviceName : L"";
    }

    std::wstring FriendlyName(const DISPLAYCONFIG_PATH_INFO& path)
    {
        DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        name.header.size = sizeof(name);
        name.header.adapterId = path.targetInfo.adapterId;
        name.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS && name.monitorFriendlyDeviceName[0] != L'\0')
        {
            return name.monitorFriendlyDeviceName;
        }
        return SourceName(path);
    }

    bool IsPrimaryDevice(const std::wstring& deviceName)
    {
        for (DWORD index = 0;; ++index)
        {
            DISPLAY_DEVICEW device{};
            device.cb = sizeof(device);
            if (!EnumDisplayDevicesW(nullptr, index, &device, 0))
            {
                return false;
            }
            if (_wcsicmp(device.DeviceName, deviceName.c_str()) == 0)
            {
                return (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
            }
        }
    }

    bool IsConnected(const std::wstring& deviceName)
    {
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        return EnumDisplaySettingsW(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &mode) != FALSE;
    }

    std::vector<DisplayPath> DetectCurrentDisplays(std::wstring* errorMessage)
    {
        DisplayConfiguration configuration;
        if (!QueryConfiguration(QDC_ONLY_ACTIVE_PATHS, configuration, errorMessage))
        {
            return {};
        }

        std::vector<DisplayPath> displays;
        for (const auto& path : configuration.paths)
        {
            if (path.targetInfo.targetAvailable == FALSE)
            {
                continue;
            }

            DisplayPath display;
            display.monitorName = FriendlyName(path);
            display.displayName = SourceName(path);
            display.sourceAdapterLowPart = path.sourceInfo.adapterId.LowPart;
            display.sourceAdapterHighPart = path.sourceInfo.adapterId.HighPart;
            display.sourceId = path.sourceInfo.id;
            display.targetAdapterLowPart = path.targetInfo.adapterId.LowPart;
            display.targetAdapterHighPart = path.targetInfo.adapterId.HighPart;
            display.targetId = path.targetInfo.id;
            display.enabled = true;
            display.isPrimary = IsPrimaryDevice(display.displayName);
            if (path.sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
                path.sourceInfo.modeInfoIdx < configuration.modes.size())
            {
                const auto& mode = configuration.modes[path.sourceInfo.modeInfoIdx];
                if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
                {
                    display.positionX = mode.sourceMode.position.x;
                    display.positionY = mode.sourceMode.position.y;
                    display.width = mode.sourceMode.width;
                }
            }
            displays.push_back(std::move(display));
        }

        std::sort(displays.begin(), displays.end(), [](const auto& left, const auto& right)
        {
            return _wcsicmp(left.displayName.c_str(), right.displayName.c_str()) < 0;
        });
        return displays;
    }

    bool SameDisplay(const DISPLAYCONFIG_PATH_INFO& path, const DisplayPath& display)
    {
        return path.sourceInfo.id == display.sourceId && path.targetInfo.id == display.targetId;
    }

    bool InProfile(const DISPLAYCONFIG_PATH_INFO& path, const std::vector<DisplayPath>& displays)
    {
        return std::any_of(displays.begin(), displays.end(), [&path](const auto& display)
        {
            return SameDisplay(path, display);
        });
    }

    bool ApplyPaths(DisplayConfiguration& configuration, std::wstring* errorMessage, const wchar_t* operation)
    {
        const LONG result = SetDisplayConfig(
            static_cast<UINT32>(configuration.paths.size()),
            configuration.paths.data(),
            static_cast<UINT32>(configuration.modes.size()),
            configuration.modes.data(),
            SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
        if (result != ERROR_SUCCESS)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = WindowsError(operation, result);
            }
            return false;
        }
        return true;
    }

    bool ApplyPartialTopology(const std::vector<DisplayPath>& displays, std::wstring* errorMessage)
    {
        DisplayConfiguration configuration;
        if (!QueryConfiguration(QDC_ALL_PATHS, configuration, errorMessage))
        {
            return false;
        }

        for (auto& path : configuration.paths)
        {
            const auto display = std::find_if(displays.begin(), displays.end(), [&path](const auto& candidate)
            {
                return SameDisplay(path, candidate);
            });
            if (display == displays.end())
            {
                continue;
            }
            if (display->enabled)
            {
                path.flags |= DISPLAYCONFIG_PATH_ACTIVE;
            }
            else
            {
                path.flags &= ~DISPLAYCONFIG_PATH_ACTIVE;
            }
        }
        return ApplyPaths(configuration, errorMessage, L"Partial display topology failed");
    }

    bool ApplyFullTopology(const std::vector<DisplayPath>& displays, std::wstring* errorMessage)
    {
        DisplayConfiguration configuration;
        if (!QueryConfiguration(QDC_ALL_PATHS, configuration, errorMessage))
        {
            return false;
        }

        for (auto& path : configuration.paths)
        {
            const auto display = std::find_if(displays.begin(), displays.end(), [&path](const auto& candidate)
            {
                return SameDisplay(path, candidate);
            });
            if (display != displays.end())
            {
                if (display->enabled)
                {
                    path.flags |= DISPLAYCONFIG_PATH_ACTIVE;
                }
                else
                {
                    path.flags &= ~DISPLAYCONFIG_PATH_ACTIVE;
                }
            }
            else if (path.targetInfo.targetAvailable != FALSE)
            {
                path.flags &= ~DISPLAYCONFIG_PATH_ACTIVE;
            }
        }
        return ApplyPaths(configuration, errorMessage, L"Full display topology failed");
    }

    bool ApplyPositions(const std::vector<DisplayPath>& displays, std::wstring* errorMessage)
    {
        DisplayConfiguration configuration;
        if (!QueryConfiguration(QDC_ALL_PATHS, configuration, errorMessage))
        {
            return false;
        }

        LONG rightEdge = 0;
        for (const auto& display : displays)
        {
            if (!display.enabled)
            {
                continue;
            }
            rightEdge = std::max(rightEdge, display.positionX + static_cast<LONG>(display.width));
            const auto path = std::find_if(configuration.paths.begin(), configuration.paths.end(), [&display](const auto& candidate)
            {
                return SameDisplay(candidate, display);
            });
            if (path == configuration.paths.end() || path->targetInfo.targetAvailable == FALSE ||
                path->sourceInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID ||
                path->sourceInfo.modeInfoIdx >= configuration.modes.size())
            {
                continue;
            }

            auto& mode = configuration.modes[path->sourceInfo.modeInfoIdx];
            if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
            {
                mode.sourceMode.position.x = display.positionX;
                mode.sourceMode.position.y = display.positionY;
            }
        }

        for (const auto& path : configuration.paths)
        {
            if (path.targetInfo.targetAvailable == FALSE || path.targetInfo.scanLineOrdering == 0 || InProfile(path, displays) ||
                path.sourceInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID ||
                path.sourceInfo.modeInfoIdx >= configuration.modes.size())
            {
                continue;
            }
            auto& mode = configuration.modes[path.sourceInfo.modeInfoIdx];
            if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
            {
                mode.sourceMode.position.x = rightEdge;
                mode.sourceMode.position.y = 0;
                rightEdge += static_cast<LONG>(mode.sourceMode.width);
            }
        }
        return ApplyPaths(configuration, errorMessage, L"Applying display positions failed");
    }

    std::vector<DisplayPath> PreparePrimary(std::vector<DisplayPath> displays)
    {
        const auto primary = std::find_if(displays.begin(), displays.end(), [](const auto& display)
        {
            return display.enabled && display.isPrimary;
        });
        if (primary == displays.end())
        {
            return displays;
        }

        const LONG offsetX = -primary->positionX;
        const LONG offsetY = -primary->positionY;
        for (auto& display : displays)
        {
            display.positionX += offsetX;
            display.positionY += offsetY;
        }
        return displays;
    }
}

bool MonitorPowerController::CaptureSetup(MonitorPowerSetup& setup, std::wstring* errorMessage)
{
    setup.displayPaths = DetectCurrentDisplays(errorMessage);
    if (setup.displayPaths.empty())
    {
        if (errorMessage != nullptr && errorMessage->empty())
        {
            *errorMessage = L"Windows did not report any active monitors.";
        }
        return false;
    }
    return true;
}

bool MonitorPowerController::ApplySetup(
    const MonitorPowerSetup& setup,
    const std::function<void(const std::wstring&)>& logger,
    std::wstring* errorMessage)
{
    const auto enabledCount = std::count_if(setup.displayPaths.begin(), setup.displayPaths.end(), [](const auto& display)
    {
        return display.enabled;
    });
    const auto primaryCount = std::count_if(setup.displayPaths.begin(), setup.displayPaths.end(), [](const auto& display)
    {
        return display.enabled && display.isPrimary;
    });
    if (enabledCount == 0 || primaryCount != 1)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = L"Enable at least one monitor and select exactly one enabled monitor as Primary.";
        }
        return false;
    }

    auto targetDisplays = PreparePrimary(setup.displayPaths);
    const bool allConnected = std::all_of(targetDisplays.begin(), targetDisplays.end(), [](const auto& display)
    {
        return IsConnected(display.displayName);
    });
    if (allConnected)
    {
        if (logger)
        {
            logger(L"Preparing the saved primary display and monitor positions.");
        }
        if (!ApplyPositions(targetDisplays, errorMessage))
        {
            return false;
        }
    }

    std::wstring detectError;
    const auto currentDisplays = DetectCurrentDisplays(&detectError);
    std::unordered_set<UINT> activeTargetIds;
    for (const auto& display : currentDisplays)
    {
        activeTargetIds.insert(display.targetId);
    }

    std::vector<DisplayPath> phaseOne;
    for (const auto& display : targetDisplays)
    {
        if (display.enabled && activeTargetIds.contains(display.targetId))
        {
            phaseOne.push_back(display);
        }
    }
    if (!phaseOne.empty())
    {
        if (logger)
        {
            logger(L"Applying the partial topology for monitors that are already active.");
        }
        if (!ApplyPartialTopology(phaseOne, errorMessage))
        {
            return false;
        }
        Sleep(750);
    }

    if (logger)
    {
        logger(L"Applying the complete target topology.");
    }
    if (!ApplyFullTopology(targetDisplays, errorMessage))
    {
        return false;
    }

    if (logger)
    {
        logger(L"Applying final monitor positions.");
    }
    ApplyPositions(targetDisplays, &detectError);
    return true;
}
