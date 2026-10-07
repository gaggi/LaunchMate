cmake_minimum_required(VERSION 3.21)
project(LaunchMate LANGUAGES CXX)

include(CheckIPOSupported)

set(LAUNCHMATE_VERSION_DEFAULT "0.2.1")
set(LAUNCHMATE_VERSION "${LAUNCHMATE_VERSION_DEFAULT}" CACHE STRING "LaunchMate version")
if(NOT LAUNCHMATE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+([-.+][0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR "Invalid LAUNCHMATE_VERSION: '${LAUNCHMATE_VERSION}'")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(LaunchMateCoreTests tests/CoreTests.cpp src/JsonLite.cpp src/EtwProcessListener.cpp src/ProcessMonitor.cpp)
    target_include_directories(LaunchMateCoreTests PRIVATE src)
    target_compile_features(LaunchMateCoreTests PRIVATE cxx_std_20)
    target_compile_definitions(LaunchMateCoreTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(LaunchMateCoreTests PRIVATE shell32 advapi32 tdh winhttp ole32 oleaut32 uuid)
    if(MSVC)
        set_property(TARGET LaunchMateCoreTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME LaunchMateCoreTests COMMAND LaunchMateCoreTests)
endif()
set(LAUNCHMATE_GITHUB_OWNER "gaggi" CACHE STRING "LaunchMate GitHub owner")
set(LAUNCHMATE_GITHUB_REPOSITORY "LaunchMate" CACHE STRING "LaunchMate GitHub repository")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(LaunchMate WIN32
    src/main.cpp
    src/App.cpp
    src/AppSources.cpp
    src/DisplaysPage.cpp
    src/RuleAppsPage.cpp
    src/ServicesPage.cpp
    src/SettingsPage.cpp
    src/MainWindow.cpp
    src/RuleActionsDialog.cpp
    src/IRacingPerformance.cpp
    src/IRacingServices.cpp
    src/MonitorPowerController.cpp
    src/EtwProcessListener.cpp
    src/ProcessMonitor.cpp
    src/ConfigStore.cpp
    src/TrayIcon.cpp
    src/JsonLite.cpp
    src/StartupRegistration.cpp
    src/UpdateChecker.cpp
    resources/resources.rc
)

if(MSVC)
    set_property(TARGET LaunchMate PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

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

target_link_libraries(LaunchMate PRIVATE
    bcrypt
    comctl32
    uxtheme
    dxva2
    gdiplus
    shell32
    powrprof
    crypt32
    advapi32
    tdh
    ole32
    oleaut32
    shlwapi
    winhttp
    uuid
)

check_ipo_supported(RESULT launchmate_ipo_supported OUTPUT launchmate_ipo_error)
if(launchmate_ipo_supported)
    set_property(TARGET LaunchMate PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET LaunchMate PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
else()
    message(STATUS "IPO/LTO not enabled: ${launchmate_ipo_error}")
endif()

if(MSVC)
    # Sources are UTF-8; without this MSVC reads them as the system code page.
    target_compile_options(LaunchMate PRIVATE /utf-8)
    target_compile_options(LaunchMate PRIVATE
        $<$<CONFIG:Release>:/O2 /Oi /Gy /GF /Gw>
        $<$<CONFIG:RelWithDebInfo>:/O2 /Oi /Gy /GF /Gw>
    )

    target_link_options(LaunchMate PRIVATE
        $<$<CONFIG:Release>:/OPT:REF /OPT:ICF /INCREMENTAL:NO>
        $<$<CONFIG:RelWithDebInfo>:/OPT:REF /OPT:ICF /INCREMENTAL:NO>
    )
endif()
