#pragma once

#include <string>
#include <vector>

struct IRacingServiceOption
{
    const wchar_t* name;
    const wchar_t* label;
    bool disableWhileRacing;
    const wchar_t* description;
};

const std::vector<IRacingServiceOption>& IRacingServiceOptions();
bool IsIRacingServiceName(const std::wstring& name);
bool CanManageIRacingServices();
std::wstring DescribeIRacingService(const wchar_t* name);
bool HasPendingIRacingServiceRestore();
bool ApplyIRacingServices(const std::vector<std::wstring>& selected, std::wstring& report);
bool RestoreIRacingServices(std::wstring& report);
