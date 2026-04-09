cmake_minimum_required(VERSION 3.21)
project(LaunchMate LANGUAGES CXX)

include(CheckIPOSupported)

set(LAUNCHMATE_VERSION "0.0.0-dev" CACHE STRING "LaunchMate version")
set(LAUNCHMATE_GITHUB_OWNER "gaggi" CACHE STRING "LaunchMate GitHub owner")
set(LAUNCHMATE_GITHUB_REPOSITORY "LaunchMate" CACHE STRING "LaunchMate GitHub repository")

if(LAUNCHMATE_VERSION STREQUAL "0.0.0-dev")
    find_package(Git QUIET)
    if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE launchmate_git_tag
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(launchmate_git_tag)
            string(REGEX REPLACE "^v" "" LAUNCHMATE_VERSION "${launchmate_git_tag}")
        endif()
    endif()
endif()

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
    src/UpdateChecker.cpp
    resources/resources.rc
)

target_include_directories(LaunchMate PRIVATE src)

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
    comctl32
    gdiplus
    shell32
    shlwapi
    winhttp
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
