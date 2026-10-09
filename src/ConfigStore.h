#pragma once

#include "Models.h"

#include <filesystem>

#include <string>

class ConfigStore
{
public:
    ConfigStore();
    // Uses the given file instead of %APPDATA%\LaunchMate\config.json (tests).
    explicit ConfigStore(std::filesystem::path configPath);

    const std::filesystem::path& Path() const noexcept;
    // Falls back to defaults when the file is missing or unreadable. An unreadable
    // file is copied aside first; if that fails, Save() leaves the file alone.
    AppConfiguration Load() const;
    bool Save(const AppConfiguration& configuration) const;
    // Why the last Load() used defaults although a file existed; empty otherwise.
    const std::wstring& LoadProblem() const noexcept { return loadProblem_; }

private:
    AppConfiguration KeepUnreadableFile(const std::wstring& reason) const;

    std::filesystem::path configPath_;
    mutable std::wstring loadProblem_;
    // Set when an unreadable file could not be copied aside, so it is not overwritten.
    mutable bool saveBlocked_{false};
};
