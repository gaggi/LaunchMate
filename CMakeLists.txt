cmake_minimum_required(VERSION 3.21)
project(LaunchMate LANGUAGES CXX)

include(CheckIPOSupported)

set(LAUNCHMATE_VERSION_DEFAULT "0.1.0")
set(LAUNCHMATE_VERSION "${LAUNCHMATE_VERSION_DEFAULT}" CACHE STRING "LaunchMate version")
set(LAUNCHMATE_GITHUB_OWNER "gaggi" CACHE STRING "LaunchMate GitHub owner")
set(LAUNCHMATE_GITHUB_REPOSITORY "LaunchMate" CACHE STRING "LaunchMate GitHub repository")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(LaunchMate WIN32
    src/main.cpp
    src/App.cpp
    src/MainWindow.cpp
    src/RuleActionsDialog.cpp
    src/MonitorPowerController.cpp
    src/ProcessMonitor.cpp
    src/ConfigStore.cpp
    src/TrayIcon.cpp
    src/JsonLite.cpp
    src/StartupRegistration.cpp
    src/UpdateChecker.cpp
    resources/resources.rc
)

add_executable(DisplayProfileTest WIN32
    tools/DisplayProfileTest.cpp
)

target_include_directories(LaunchMate PRIVATE
    src
)
target_compile_definitions(LaunchMate PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    LAUNCHMATE_VERSION="${LAUNCHMATE_VERSION}"
    LAUNCHMATE_GITHUB_OWNER="${LAUNCHMATE_GITHUB_OWNER}"
    LAUNCHMATE_GITHUB_REPOSITORY="${LAUNCHMATE_GITHUB_REPOSITORY}"
)

target_compile_definitions(DisplayProfileTest PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(LaunchMate PRIVATE
    comctl32
    dxva2
    gdiplus
    shell32
    shlwapi
    winhttp
)

target_link_libraries(DisplayProfileTest PRIVATE
    comctl32
    user32
)

check_ipo_supported(RESULT launchmate_ipo_supported OUTPUT launchmate_ipo_error)
if(launchmate_ipo_supported)
    set_property(TARGET LaunchMate PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET LaunchMate PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
else()
    message(STATUS "IPO/LTO not enabled: ${launchmate_ipo_error}")
endif()

if(MSVC)
    target_compile_options(LaunchMate PRIVATE
        $<$<CONFIG:Release>:/O2 /Oi /Gy /GF /Gw>
        $<$<CONFIG:RelWithDebInfo>:/O2 /Oi /Gy /GF /Gw>
    )

    target_link_options(LaunchMate PRIVATE
        $<$<CONFIG:Release>:/OPT:REF /OPT:ICF /INCREMENTAL:NO>
        $<$<CONFIG:RelWithDebInfo>:/OPT:REF /OPT:ICF /INCREMENTAL:NO>
    )
endif()
