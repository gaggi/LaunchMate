cmake_minimum_required(VERSION 3.21)
project(LaunchMate LANGUAGES CXX)

include(CheckIPOSupported)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(LaunchMate WIN32
    src/main.cpp
    src/App.cpp
    src/MainWindow.cpp
    src/ProcessMonitor.cpp
    src/ConfigStore.cpp
    src/TrayIcon.cpp
    src/JsonLite.cpp
    src/StartupRegistration.cpp
    resources/resources.rc
)

target_include_directories(LaunchMate PRIVATE src)

target_compile_definitions(LaunchMate PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(LaunchMate PRIVATE
    comctl32
    gdiplus
    shell32
    shlwapi
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
