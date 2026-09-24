#pragma once

#include <string>

class StartupRegistration
{
public:
    static bool Apply(bool startWithWindows, bool alwaysRunAsAdministrator);
    static bool ConfigureElevatedTask(bool enabled, const std::wstring& expectedUserSid);
    static bool IsElevated();
    static bool CanElevateCurrentUser();
};
