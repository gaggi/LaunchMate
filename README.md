# LaunchMate

LaunchMate is a native Windows desktop app for monitoring processes and automatically starting related programs.

## Features

- Start global programs when monitoring begins
- Watch processes and launch linked programs for each rule
- Optional Windows autostart
- Optional tray mode
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

## Command Line

Optional runtime flags:

- `--poll-interval <value>` sets the polling interval in milliseconds
- `--log` enables logging to `%APPDATA%\\LaunchMate\\launchmate.log`

## Autostart

When `Start with Windows` is enabled, LaunchMate writes an entry under:

`HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run`
