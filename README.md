# LaunchMate

LaunchMate is a native Windows desktop app for monitoring processes and automatically starting related programs.

## Features

- Watch processes and launch linked programs for each rule
- Detect known companion apps from common install paths and add them quickly to a watched process
- Optional Windows autostart
- Optional tray mode
- Optional GitHub update checks on startup
- Store configuration as JSON in the roaming profile

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
- `--active-poll-interval <value>` sets the polling interval in milliseconds while at least one watched process is active
- `--log` enables logging to `%APPDATA%\\LaunchMate\\launchmate.log`

## Autostart

When `Start with Windows` is enabled, LaunchMate writes an entry under:

`HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run`

## Updates

Tagged GitHub releases publish direct `windows-x64.exe` and `windows-x86.exe` assets in addition to the ZIP packages. LaunchMate uses those direct executable assets for its built-in self-update flow.
