#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr wchar_t kWindowClassName[] = L"LaunchMateDisplayProfileTest";
    constexpr int kProfileListId = 100;
    constexpr int kProfileNameId = 101;
    constexpr int kNewProfileId = 102;
    constexpr int kDeleteProfileId = 103;
    constexpr int kDetectCurrentId = 104;
    constexpr int kApplyProfileId = 105;
    constexpr int kEnabledBaseId = 1000;
    constexpr int kPrimaryBaseId = 2000;

    struct DisplayConfiguration
    {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths;
        std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    };

    struct DisplayEntry
    {
        std::wstring friendlyName;
        std::wstring deviceName;
        LUID adapterId{};
        UINT sourceId{0};
        UINT targetId{0};
        LONG positionX{0};
        LONG positionY{0};
        UINT width{0};
        bool enabled{true};
        bool primary{false};
    };

    struct Profile
    {
        std::wstring name;
        std::vector<DisplayEntry> displays;
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

    bool QueryConfiguration(UINT flags, DisplayConfiguration& configuration, std::wstring& error)
    {
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            UINT32 pathCount = 0;
            UINT32 modeCount = 0;
            LONG result = GetDisplayConfigBufferSizes(flags, &pathCount, &modeCount);
            if (result != ERROR_SUCCESS)
            {
                error = WindowsError(L"GetDisplayConfigBufferSizes failed", result);
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
                error = WindowsError(L"QueryDisplayConfig failed", result);
                return false;
            }

            configuration.paths.resize(pathCount);
            configuration.modes.resize(modeCount);
            return true;
        }

        error = L"The display configuration changed while it was being read.";
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

    std::vector<DisplayEntry> DetectCurrentDisplays(std::wstring& error)
    {
        DisplayConfiguration configuration;
        if (!QueryConfiguration(QDC_ONLY_ACTIVE_PATHS, configuration, error))
        {
            return {};
        }

        std::vector<DisplayEntry> displays;
        for (const auto& path : configuration.paths)
        {
            if (path.targetInfo.targetAvailable == FALSE)
            {
                continue;
            }

            DisplayEntry entry;
            entry.friendlyName = FriendlyName(path);
            entry.deviceName = SourceName(path);
            entry.adapterId = path.sourceInfo.adapterId;
            entry.sourceId = path.sourceInfo.id;
            entry.targetId = path.targetInfo.id;
            entry.primary = IsPrimaryDevice(entry.deviceName);
            if (path.sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
                path.sourceInfo.modeInfoIdx < configuration.modes.size())
            {
                const auto& mode = configuration.modes[path.sourceInfo.modeInfoIdx];
                if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
                {
                    entry.positionX = mode.sourceMode.position.x;
                    entry.positionY = mode.sourceMode.position.y;
                    entry.width = mode.sourceMode.width;
                }
            }
            displays.push_back(std::move(entry));
        }

        std::sort(displays.begin(), displays.end(), [](const auto& left, const auto& right)
        {
            return _wcsicmp(left.deviceName.c_str(), right.deviceName.c_str()) < 0;
        });
        return displays;
    }

    bool SameDisplay(const DISPLAYCONFIG_PATH_INFO& path, const DisplayEntry& display)
    {
        // DisplayProfileManager identifies profile paths by source and target ID.
        return path.sourceInfo.id == display.sourceId && path.targetInfo.id == display.targetId;
    }

    bool InProfile(const DISPLAYCONFIG_PATH_INFO& path, const std::vector<DisplayEntry>& displays)
    {
        return std::any_of(displays.begin(), displays.end(), [&path](const auto& display)
        {
            return SameDisplay(path, display);
        });
    }

    bool ApplyPaths(DisplayConfiguration& configuration, std::wstring& error, const wchar_t* operation)
    {
        const LONG result = SetDisplayConfig(
            static_cast<UINT32>(configuration.paths.size()),
            configuration.paths.data(),
            static_cast<UINT32>(configuration.modes.size()),
            configuration.modes.data(),
            SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
        if (result != ERROR_SUCCESS)
        {
            error = WindowsError(operation, result);
            return false;
        }
        return true;
    }

    bool ApplyPartialTopology(const std::vector<DisplayEntry>& displays, std::wstring& error)
    {
        DisplayConfiguration configuration;
        if (!QueryConfiguration(QDC_ALL_PATHS, configuration, error))
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
        return ApplyPaths(configuration, error, L"Partial display topology failed");
    }

    bool ApplyFullTopology(const std::vector<DisplayEntry>& displays, std::wstring& error)
    {
        DisplayConfiguration configuration;
        if (!QueryConfiguration(QDC_ALL_PATHS, configuration, error))
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
        return ApplyPaths(configuration, error, L"Full display topology failed");
    }

    bool ApplyPositions(const std::vector<DisplayEntry>& displays, std::wstring& error)
    {
        DisplayConfiguration configuration;
        if (!QueryConfiguration(QDC_ALL_PATHS, configuration, error))
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
        return ApplyPaths(configuration, error, L"Applying display positions failed");
    }

    std::vector<DisplayEntry> PreparePrimary(std::vector<DisplayEntry> displays)
    {
        const auto primary = std::find_if(displays.begin(), displays.end(), [](const auto& display)
        {
            return display.enabled && display.primary;
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

    bool ApplyProfile(const Profile& profile, std::wstring& error)
    {
        const auto enabledCount = std::count_if(profile.displays.begin(), profile.displays.end(), [](const auto& display)
        {
            return display.enabled;
        });
        const auto primaryCount = std::count_if(profile.displays.begin(), profile.displays.end(), [](const auto& display)
        {
            return display.enabled && display.primary;
        });
        if (enabledCount == 0 || primaryCount != 1)
        {
            error = L"Enable at least one monitor and select exactly one enabled monitor as Primary.";
            return false;
        }

        auto targetDisplays = PreparePrimary(profile.displays);
        const bool allConnected = std::all_of(targetDisplays.begin(), targetDisplays.end(), [](const auto& display)
        {
            return IsConnected(display.deviceName);
        });
        if (allConnected && !ApplyPositions(targetDisplays, error))
        {
            return false;
        }

        std::wstring detectError;
        const auto currentDisplays = DetectCurrentDisplays(detectError);
        std::unordered_set<UINT> activeTargetIds;
        for (const auto& display : currentDisplays)
        {
            activeTargetIds.insert(display.targetId);
        }

        std::vector<DisplayEntry> phaseOne;
        for (const auto& display : targetDisplays)
        {
            if (display.enabled && activeTargetIds.contains(display.targetId))
            {
                phaseOne.push_back(display);
            }
        }
        if (!phaseOne.empty())
        {
            if (!ApplyPartialTopology(phaseOne, error))
            {
                return false;
            }
            Sleep(750);
        }

        if (!ApplyFullTopology(targetDisplays, error))
        {
            return false;
        }
        ApplyPositions(targetDisplays, detectError);
        return true;
    }

    class TestWindow
    {
    public:
        explicit TestWindow(HINSTANCE instance) : instance_(instance) {}

        int Run(int showCommand)
        {
            INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
            InitCommonControlsEx(&controls);

            WNDCLASSW windowClass{};
            windowClass.lpfnWndProc = WindowProc;
            windowClass.hInstance = instance_;
            windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            windowClass.lpszClassName = kWindowClassName;
            RegisterClassW(&windowClass);

            window_ = CreateWindowExW(
                0,
                kWindowClassName,
                L"Display Profile Test",
                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                980,
                520,
                nullptr,
                nullptr,
                instance_,
                this);
            if (window_ == nullptr)
            {
                return -1;
            }

            ShowWindow(window_, showCommand);
            UpdateWindow(window_);
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            return static_cast<int>(message.wParam);
        }

    private:
        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
        {
            auto* self = reinterpret_cast<TestWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == WM_NCCREATE)
            {
                const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
                self = static_cast<TestWindow*>(create->lpCreateParams);
                self->window_ = window;
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            return self != nullptr ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
        }

        HWND CreateControl(const wchar_t* type, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id = 0)
        {
            HWND control = CreateWindowExW(
                0,
                type,
                text,
                WS_CHILD | WS_VISIBLE | style,
                x,
                y,
                width,
                height,
                window_,
                id == 0 ? nullptr : reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                instance_,
                nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            return control;
        }

        LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
        {
            switch (message)
            {
            case WM_CREATE:
                font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
                CreateBaseControls();
                AddProfile(true);
                return 0;
            case WM_COMMAND:
                HandleCommand(LOWORD(wParam), HIWORD(wParam));
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            }
            return DefWindowProcW(window_, message, wParam, lParam);
        }

        void CreateBaseControls()
        {
            CreateControl(L"STATIC", L"Profiles", 0, 16, 14, 160, 20);
            profileList_ = CreateControl(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 16, 36, 190, 330, kProfileListId);
            CreateControl(L"BUTTON", L"New profile", BS_PUSHBUTTON, 16, 378, 92, 28, kNewProfileId);
            CreateControl(L"BUTTON", L"Delete", BS_PUSHBUTTON, 114, 378, 92, 28, kDeleteProfileId);

            CreateControl(L"STATIC", L"Profile name", 0, 230, 14, 100, 20);
            nameEdit_ = CreateControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 230, 36, 260, 24, kProfileNameId);
            CreateControl(L"BUTTON", L"Detect Current", BS_PUSHBUTTON, 510, 34, 120, 28, kDetectCurrentId);
            CreateControl(L"BUTTON", L"Apply profile", BS_DEFPUSHBUTTON, 640, 34, 120, 28, kApplyProfileId);

            CreateControl(L"STATIC", L"Monitor", 0, 230, 82, 70, 20);
            CreateControl(L"STATIC", L"Name", 0, 305, 82, 380, 20);
            CreateControl(L"STATIC", L"Enabled", 0, 720, 82, 70, 20);
            CreateControl(L"STATIC", L"Primary", 0, 815, 82, 70, 20);
            status_ = CreateControl(L"STATIC", L"Detect the current Windows layout, then create and apply profiles.", 0, 230, 438, 700, 24);
        }

        void AddProfile(bool detect)
        {
            SyncRowsToProfile();
            Profile profile;
            profile.name = L"Profile " + std::to_wstring(profiles_.size() + 1);
            if (!profiles_.empty())
            {
                profile.displays = profiles_[static_cast<size_t>(std::max(selectedProfile_, 0))].displays;
            }
            else if (detect)
            {
                std::wstring error;
                profile.displays = DetectCurrentDisplays(error);
                if (!error.empty())
                {
                    SetWindowTextW(status_, error.c_str());
                }
            }
            profiles_.push_back(std::move(profile));
            selectedProfile_ = static_cast<int>(profiles_.size()) - 1;
            RefreshProfileList();
            LoadProfile();
        }

        void RefreshProfileList()
        {
            SendMessageW(profileList_, LB_RESETCONTENT, 0, 0);
            for (const auto& profile : profiles_)
            {
                SendMessageW(profileList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(profile.name.c_str()));
            }
            SendMessageW(profileList_, LB_SETCURSEL, selectedProfile_, 0);
        }

        void DestroyRows()
        {
            for (HWND control : rowControls_)
            {
                DestroyWindow(control);
            }
            rowControls_.clear();
            enabledChecks_.clear();
            primaryRadios_.clear();
        }

        void LoadProfile()
        {
            DestroyRows();
            if (selectedProfile_ < 0 || selectedProfile_ >= static_cast<int>(profiles_.size()))
            {
                SetWindowTextW(nameEdit_, L"");
                return;
            }

            const auto& profile = profiles_[static_cast<size_t>(selectedProfile_)];
            SetWindowTextW(nameEdit_, profile.name.c_str());
            int y = 108;
            for (size_t index = 0; index < profile.displays.size(); ++index)
            {
                const auto& display = profile.displays[index];
                rowControls_.push_back(CreateControl(L"STATIC", (L"Monitor " + std::to_wstring(index + 1)).c_str(), 0, 230, y + 4, 70, 24));
                rowControls_.push_back(CreateControl(L"STATIC", display.friendlyName.c_str(), SS_LEFT, 305, y + 4, 390, 24));
                HWND enabled = CreateControl(L"BUTTON", L"", BS_AUTOCHECKBOX, 738, y, 24, 24, kEnabledBaseId + static_cast<int>(index));
                HWND primary = CreateControl(L"BUTTON", L"", BS_RADIOBUTTON, 833, y, 24, 24, kPrimaryBaseId + static_cast<int>(index));
                SendMessageW(enabled, BM_SETCHECK, display.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(primary, BM_SETCHECK, display.primary ? BST_CHECKED : BST_UNCHECKED, 0);
                enabledChecks_.push_back(enabled);
                primaryRadios_.push_back(primary);
                rowControls_.push_back(enabled);
                rowControls_.push_back(primary);
                y += 42;
            }
        }

        void SyncRowsToProfile()
        {
            if (selectedProfile_ < 0 || selectedProfile_ >= static_cast<int>(profiles_.size()))
            {
                return;
            }
            auto& profile = profiles_[static_cast<size_t>(selectedProfile_)];
            wchar_t name[256]{};
            GetWindowTextW(nameEdit_, name, static_cast<int>(std::size(name)));
            if (name[0] != L'\0')
            {
                profile.name = name;
            }
            for (size_t index = 0; index < profile.displays.size() && index < enabledChecks_.size(); ++index)
            {
                profile.displays[index].enabled = SendMessageW(enabledChecks_[index], BM_GETCHECK, 0, 0) == BST_CHECKED;
                profile.displays[index].primary = SendMessageW(primaryRadios_[index], BM_GETCHECK, 0, 0) == BST_CHECKED;
            }
        }

        void SelectPrimary(size_t selected)
        {
            if (selectedProfile_ < 0 || selectedProfile_ >= static_cast<int>(profiles_.size()))
            {
                return;
            }
            auto& displays = profiles_[static_cast<size_t>(selectedProfile_)].displays;
            if (selected >= displays.size())
            {
                return;
            }
            SendMessageW(enabledChecks_[selected], BM_SETCHECK, BST_CHECKED, 0);
            for (size_t index = 0; index < displays.size(); ++index)
            {
                SendMessageW(primaryRadios_[index], BM_SETCHECK, index == selected ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            SyncRowsToProfile();
        }

        void HandleCommand(int id, int notification)
        {
            if (id == kProfileListId && notification == LBN_SELCHANGE)
            {
                SyncRowsToProfile();
                selectedProfile_ = static_cast<int>(SendMessageW(profileList_, LB_GETCURSEL, 0, 0));
                LoadProfile();
                return;
            }
            if (id == kNewProfileId)
            {
                AddProfile(false);
                return;
            }
            if (id == kDeleteProfileId)
            {
                if (selectedProfile_ >= 0 && selectedProfile_ < static_cast<int>(profiles_.size()))
                {
                    profiles_.erase(profiles_.begin() + selectedProfile_);
                    selectedProfile_ = profiles_.empty() ? -1 : std::min(selectedProfile_, static_cast<int>(profiles_.size()) - 1);
                    RefreshProfileList();
                    LoadProfile();
                }
                return;
            }
            if (id == kDetectCurrentId)
            {
                if (selectedProfile_ < 0)
                {
                    AddProfile(false);
                }
                std::wstring error;
                auto displays = DetectCurrentDisplays(error);
                if (!error.empty())
                {
                    MessageBoxW(window_, error.c_str(), L"Display Profile Test", MB_OK | MB_ICONWARNING);
                    return;
                }
                profiles_[static_cast<size_t>(selectedProfile_)].displays = std::move(displays);
                LoadProfile();
                SetWindowTextW(status_, L"Current active Windows displays detected.");
                return;
            }
            if (id == kApplyProfileId)
            {
                SyncRowsToProfile();
                if (selectedProfile_ < 0)
                {
                    return;
                }
                std::wstring error;
                if (!ApplyProfile(profiles_[static_cast<size_t>(selectedProfile_)], error))
                {
                    MessageBoxW(window_, error.c_str(), L"Display Profile Test", MB_OK | MB_ICONWARNING);
                    SetWindowTextW(status_, L"Profile application failed.");
                    return;
                }
                SetWindowTextW(status_, L"Profile applied successfully.");
                return;
            }
            if (id >= kEnabledBaseId && id < kEnabledBaseId + static_cast<int>(enabledChecks_.size()))
            {
                const size_t index = static_cast<size_t>(id - kEnabledBaseId);
                if (SendMessageW(enabledChecks_[index], BM_GETCHECK, 0, 0) != BST_CHECKED &&
                    SendMessageW(primaryRadios_[index], BM_GETCHECK, 0, 0) == BST_CHECKED)
                {
                    SendMessageW(primaryRadios_[index], BM_SETCHECK, BST_UNCHECKED, 0);
                }
                SyncRowsToProfile();
                return;
            }
            if (id >= kPrimaryBaseId && id < kPrimaryBaseId + static_cast<int>(primaryRadios_.size()))
            {
                SelectPrimary(static_cast<size_t>(id - kPrimaryBaseId));
                return;
            }
            if (id == kProfileNameId && notification == EN_CHANGE)
            {
                SyncRowsToProfile();
                RefreshProfileList();
            }
        }

        HINSTANCE instance_{nullptr};
        HWND window_{nullptr};
        HWND profileList_{nullptr};
        HWND nameEdit_{nullptr};
        HWND status_{nullptr};
        HFONT font_{nullptr};
        int selectedProfile_{-1};
        std::vector<Profile> profiles_;
        std::vector<HWND> rowControls_;
        std::vector<HWND> enabledChecks_;
        std::vector<HWND> primaryRadios_;
    };
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    TestWindow window(instance);
    return window.Run(showCommand);
}
