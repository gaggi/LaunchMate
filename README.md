# LaunchMate

LaunchMate is a native Windows desktop app for monitoring processes and automatically running related actions.

## Features

- Watch processes and launch linked programs for each rule
- Gracefully close or force-stop selected processes when a watched process starts
- Optionally restart stopped processes after the watched process exits
- Trigger Home Assistant webhooks with an optional JSON payload
- Apply a saved monitor config and restore the previous display state afterward
- See every rule as a card with its status and actions; open a rule to see what happens when the program starts and exits
- Detect known companion apps from common install paths and add them quickly to a watched process
- Optional Windows autostart
- Administrator approval is requested only for individual system actions that need it
- Optional tray mode
- Runs in Windows efficiency mode (low CPU priority plus EcoQoS); launched programs start at normal priority without throttling
- Optional GitHub update checks on startup
- Global MPO control in Settings with two registry values and a Windows Default action
- Store configuration as JSON in the roaming profile
- Set a power plan and per-process CPU, I/O and memory priorities, CPU affinity and Windows efficiency mode for each rule
- iRacing rules get an **iRacing settings** page with app.ini texture loading, Defender exclusions and a system check for display refresh rates and RTSS/MSI Afterburner
- Optionally switch power plans for any watched process and restore the prior plan when it exits
- Show detected cloud-sync apps, game launchers, overlays, communication and work apps with verified executable paths, running status, and a measurement-based potential-effect estimate

For a rule watching `iRacingSim64DX11.exe`, open the rule and click **iRacing settings** in its header. The `app.ini` check uses the Windows Documents known folder, including redirected Documents folders. Applying the suggested texture preloading values creates a timestamped backup first and requires iRacing to be closed. Defender exclusions require a separate confirmation and administrator approval; they remain in effect after the session.
Open a rule and click **Start apps** or **Close apps** to pick installed, running or detected background apps. The **Detected** list shows only supported programs that are running or have a verified executable path. Click a row's arrow to edit its settings in place. Stop actions still request a graceful close and force termination after three seconds, which can lose unsaved work in browsers, work apps, or cloud-sync tools. Potential-effect labels are estimates from three short CPU samples, not guaranteed FPS gains; inactive programs show **Unknown**.
**Stop Windows services** is available for every rule and lets you opt in to the services listed in the community pre-launch script. LaunchMate records their running state and startup type before a session, restores the original state on exit, and keeps recovery information for the next launch if interrupted. Windows requests administrator approval only when service actions run. If two active rules request Power Plan or Services actions concurrently, the second action is skipped to protect the first rule's restoration state. Changes on these pages, in Settings, and on the Displays page are saved immediately. Existing iRacing configuration keys are read and migrated to generic keys when saved.

The **Multiplane overlay (MPO)** section in **Settings** is a global Windows tweak, not a rule action. It displays both current DWORD values. **Disable MPO** sets `HKLM\SOFTWARE\Microsoft\Windows\Dwm\OverlayTestMode` to `5` and `HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\DisableOverlays` to `1`; **Windows Default** deletes both values. Changes are verified, and LaunchMate attempts to roll back the first value if changing the second fails. There are no confirmation or success dialogs; errors appear in the MPO section. Administrator rights and a Windows restart are required for changes to take effect, but LaunchMate does not restart Windows. The `DisableOverlays` value is a community-documented workaround, not a Microsoft-documented setting, and can affect game overlays or some DirectX 12 games. LaunchMate does not change `ForceDisableFrameBuffer` or `OverlayMinFPS`.

**Start with Windows** uses a logon task for the current user. When **Start as Administrator** (or ETW) is enabled, LaunchMate creates that task with the highest privileges, so it starts elevated at logon without a UAC prompt. Windows requests approval only once when this startup setting is saved or changed.

## Build

Requirements:

- Visual Studio 2022 with the C++ toolchain or MSVC Build Tools
- CMake 3.21+

Recommended with presets:

```powershell
cmake --preset x64-release
cmake --build --preset build-x64-release
```

For the Ninja presets, open an `x64 Native Tools` shell first so MSVC resolves to the 64-bit toolchain.

Debug build with presets:

```powershell
cmake --preset x64-debug
cmake --build --preset build-x64-debug
```

Visual Studio 2022 x64:

```powershell
cmake --preset vs2022-x64
cmake --build --preset build-vs2022-x64-release
```

Visual Studio 2022 x86:

```powershell
cmake --preset vs2022-x86
cmake --build --preset build-vs2022-x86-release
```

Optimized builds automatically enable compiler and linker optimizations. If supported by the active toolchain, LaunchMate also uses IPO/LTO for Release and RelWithDebInfo binaries.

## Configuration

The configuration is stored at `%APPDATA%\\LaunchMate\\config.json`.

By default, LaunchMate checks the latest GitHub release on startup. That behavior can be disabled in the app settings.

## Command Line

Optional runtime flags:

- `--poll-interval <value>` sets the idle polling interval in milliseconds
- `--active-poll-interval <value>` sets the polling interval in milliseconds while at least one watched process is active (default: 1000 ms)
- `--log` enables logging to `%APPDATA%\\LaunchMate\\launchmate.log`

## Autostart

When `Start with Windows` is enabled, LaunchMate creates a current-user logon task rather than a Run-key entry.

## Updates

Tagged GitHub releases publish direct `windows-x64.exe` and `windows-x86.exe` assets in addition to the ZIP packages. LaunchMate uses those direct executable assets for its built-in self-update flow.

### Release 0.3.0

After committing and pushing the release changes, create and push the tag:

```powershell
git tag v0.3.0
git push origin v0.3.0
```

The release workflow takes the version from the tag and builds both x64 and x86 packages as version `0.3.0`. For an existing local build directory, configure with `-DLAUNCHMATE_VERSION:STRING=0.3.0` to replace any previously cached version.

### Release 0.2.1

After committing and pushing the release changes, create and push the tag:

```powershell
git tag v0.2.1
git push origin v0.2.1
```

The release workflow takes the version from the tag and builds both x64 and x86 packages as version `0.2.1`. For an existing local build directory, configure with `-DLAUNCHMATE_VERSION:STRING=0.2.1` to replace any previously cached version.

### Release 0.2.0

After committing and pushing the release changes, create and push the tag:

```powershell
git tag v0.2.0
git push origin v0.2.0
```

The release workflow takes the version from the tag and builds both x64 and x86 packages as version `0.2.0`. For an existing local build directory, configure with `-DLAUNCHMATE_VERSION:STRING=0.2.0` to replace any previously cached version.
