#pragma once

#include <string>

class StartupRegistration
{
public:
    // Updates the current user's logon task.  Elevated tasks are created via a
    // one-shot elevated helper, so Windows does not have to show UAC at logon.
    static bool Apply(bool startWithWindows, bool startAsAdministrator);
    static bool ConfigureElevatedStartup(bool startWithWindows, bool startAsAdministrator,
        const std::wstring& expectedUserSid);
};
