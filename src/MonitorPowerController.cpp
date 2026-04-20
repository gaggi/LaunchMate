#include "MonitorPowerController.h"

#include <physicalmonitorenumerationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <windows.h>

#include <algorithm>
#include <memory>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr BYTE kPowerModeVcpCode = 0xD6;
    constexpr DWORD kPowerModeOn = 0x01;
    constexpr DWORD kPowerModeOff = 0x05;

    std::wstring BuildWin32ErrorMessage(const wchar_t* prefix, DWORD errorCode)
    {
        wchar_t* systemMessage = nullptr;
        const DWORD messageLength = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            errorCode,
            0,
            reinterpret_cast<LPWSTR>(&systemMessage),
            0,
            nullptr);

        std::wstring message = prefix;
        message += L" (";
        message += std::to_wstring(errorCode);
        message += L")";

        if (messageLength > 0 && systemMessage != nullptr)
        {
            message += L": ";
            message.append(systemMessage, messageLength);
            while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n'))
            {
                message.pop_back();
            }
        }

        if (systemMessage != nullptr)
        {
            LocalFree(systemMessage);
        }

        return message;
    }

    struct PhysicalMonitorDeleter
    {
        void operator()(std::vector<PHYSICAL_MONITOR>* monitors) const noexcept
        {
            if (monitors == nullptr)
            {
                return;
            }

            if (!monitors->empty())
            {
                DestroyPhysicalMonitors(static_cast<DWORD>(monitors->size()), monitors->data());
            }

            delete monitors;
        }
    };

    struct MonitorLookupContext
    {
        const std::wstring* displayName{nullptr};
        std::vector<PHYSICAL_MONITOR>* result{nullptr};
        std::wstring* errorMessage{nullptr};
    };

    BOOL CALLBACK MonitorEnumProc(HMONITOR monitorHandle, HDC, LPRECT, LPARAM contextValue)
    {
        auto* context = reinterpret_cast<MonitorLookupContext*>(contextValue);
        if (context == nullptr || context->displayName == nullptr || context->result == nullptr)
        {
            return FALSE;
        }

        MONITORINFOEXW monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (!GetMonitorInfoW(monitorHandle, &monitorInfo))
        {
            return TRUE;
        }

        if (_wcsicmp(monitorInfo.szDevice, context->displayName->c_str()) != 0)
        {
            return TRUE;
        }

        DWORD physicalMonitorCount = 0;
        if (!GetNumberOfPhysicalMonitorsFromHMONITOR(monitorHandle, &physicalMonitorCount))
        {
            if (context->errorMessage != nullptr)
            {
                *context->errorMessage = BuildWin32ErrorMessage(L"GetNumberOfPhysicalMonitorsFromHMONITOR failed", GetLastError());
            }
            return FALSE;
        }

        context->result->resize(physicalMonitorCount);
        if (!GetPhysicalMonitorsFromHMONITOR(monitorHandle, physicalMonitorCount, context->result->data()))
        {
            if (context->errorMessage != nullptr)
            {
                *context->errorMessage = BuildWin32ErrorMessage(L"GetPhysicalMonitorsFromHMONITOR failed", GetLastError());
            }
            context->result->clear();
            return FALSE;
        }

        return FALSE;
    }

    bool FindPhysicalMonitorsForDisplay(
        const std::wstring& displayName,
        std::vector<PHYSICAL_MONITOR>& physicalMonitors,
        std::wstring* errorMessage)
    {
        physicalMonitors.clear();
        MonitorLookupContext context{&displayName, &physicalMonitors, errorMessage};
        EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&context));
        if (physicalMonitors.empty())
        {
            if (errorMessage != nullptr && errorMessage->empty())
            {
                *errorMessage = L"No physical monitor was found for " + displayName + L".";
            }
            return false;
        }
        return true;
    }

}

std::vector<MonitorPowerController::DisplayInfo> MonitorPowerController::EnumerateDisplays()
{
    std::vector<DisplayInfo> displays;
    DISPLAY_DEVICEW adapter{};
    adapter.cb = sizeof(adapter);
    for (DWORD adapterIndex = 0; EnumDisplayDevicesW(nullptr, adapterIndex, &adapter, 0); ++adapterIndex)
    {
        if ((adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0)
        {
            adapter.cb = sizeof(adapter);
            continue;
        }

        if ((adapter.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) != 0)
        {
            adapter.cb = sizeof(adapter);
            continue;
        }

        DisplayInfo info;
        info.displayName = adapter.DeviceName;
        info.displayLabel = adapter.DeviceString[0] != L'\0'
            ? std::wstring(adapter.DeviceString) + L"  |  " + adapter.DeviceName
            : std::wstring(adapter.DeviceName);
        displays.push_back(std::move(info));

        adapter.cb = sizeof(adapter);
    }

    if (displays.empty())
    {
        displays.push_back({L"\\\\.\\DISPLAY1", L"Monitor 1  |  \\\\.\\DISPLAY1"});
        displays.push_back({L"\\\\.\\DISPLAY2", L"Monitor 2  |  \\\\.\\DISPLAY2"});
    }

    std::sort(
        displays.begin(),
        displays.end(),
        [](const DisplayInfo& left, const DisplayInfo& right)
        {
            return _wcsicmp(left.displayName.c_str(), right.displayName.c_str()) < 0;
        });
    return displays;
}

bool MonitorPowerController::SetPower(
    const std::wstring& displayName,
    bool powerOn,
    const std::function<void(const std::wstring&)>& logger,
    std::wstring* errorMessage)
{
    if (displayName.empty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = L"No display name was provided.";
        }
        return false;
    }

    auto ownedMonitors = std::unique_ptr<std::vector<PHYSICAL_MONITOR>, PhysicalMonitorDeleter>(new std::vector<PHYSICAL_MONITOR>());
    if (!FindPhysicalMonitorsForDisplay(displayName, *ownedMonitors, errorMessage))
    {
        return false;
    }

    const DWORD targetValue = powerOn ? kPowerModeOn : kPowerModeOff;
    if (logger)
    {
        logger(std::wstring(L"Setting ") + displayName + (powerOn ? L" to power on." : L" to power off."));
    }

    auto& physicalMonitor = ownedMonitors->front();
    if (!SetVCPFeature(physicalMonitor.hPhysicalMonitor, kPowerModeVcpCode, targetValue))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = BuildWin32ErrorMessage(
                powerOn
                    ? L"SetVCPFeature failed while powering on the monitor"
                    : L"SetVCPFeature failed while powering off the monitor",
                GetLastError());
        }
        return false;
    }

    if (logger)
    {
        logger(L"Power command sent to " + displayName + L".");
    }

    return true;
}

bool MonitorPowerController::TogglePower(
    const std::wstring& displayName,
    const std::function<void(const std::wstring&)>& logger,
    std::wstring* errorMessage)
{
    if (displayName.empty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = L"No display name was provided.";
        }
        return false;
    }

    MC_VCP_CODE_TYPE codeType = MC_MOMENTARY;
    DWORD currentValue = 0;
    DWORD maximumValue = 0;
    auto ownedMonitors = std::unique_ptr<std::vector<PHYSICAL_MONITOR>, PhysicalMonitorDeleter>(new std::vector<PHYSICAL_MONITOR>());
    if (!FindPhysicalMonitorsForDisplay(displayName, *ownedMonitors, errorMessage))
    {
        return false;
    }

    auto& physicalMonitor = ownedMonitors->front();
    DWORD targetValue = kPowerModeOn;
    if (!GetVCPFeatureAndVCPFeatureReply(
            physicalMonitor.hPhysicalMonitor,
            kPowerModeVcpCode,
            &codeType,
            &currentValue,
            &maximumValue))
    {
        const DWORD getError = GetLastError();
        if (logger)
        {
            logger(BuildWin32ErrorMessage(
                L"GetVCPFeatureAndVCPFeatureReply failed. Falling back to a direct power-on attempt",
                getError));
        }
        targetValue = kPowerModeOn;
    }
    else
    {
        targetValue = currentValue == kPowerModeOn ? kPowerModeOff : kPowerModeOn;
        if (logger)
        {
            std::wostringstream stream;
            stream << L"Display " << displayName
                   << L" current power mode=" << currentValue
                   << L", target=" << targetValue;
            logger(stream.str());
        }
    }

    if (!SetVCPFeature(physicalMonitor.hPhysicalMonitor, kPowerModeVcpCode, targetValue))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = BuildWin32ErrorMessage(
                L"SetVCPFeature failed. The monitor may reject power toggles over DDC/CI",
                GetLastError());
        }
        return false;
    }

    if (logger)
    {
        logger(L"Power toggle command sent to " + displayName + L".");
    }

    return true;
}

bool MonitorPowerController::ApplySetup(
    const std::vector<std::wstring>& enabledDisplays,
    const std::function<void(const std::wstring&)>& logger,
    std::wstring* errorMessage)
{
    if (enabledDisplays.empty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = L"Select at least one monitor to remain powered on.";
        }
        return false;
    }

    const auto displays = EnumerateDisplays();
    if (displays.empty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = L"No monitors were detected.";
        }
        return false;
    }

    std::unordered_set<std::wstring> enabledSet;
    enabledSet.reserve(enabledDisplays.size());
    for (const auto& display : enabledDisplays)
    {
        enabledSet.insert(display);
    }

    for (const auto& display : displays)
    {
        const bool powerOn = enabledSet.contains(display.displayName);
        if (!SetPower(display.displayName, powerOn, logger, errorMessage))
        {
            return false;
        }
    }

    return true;
}
