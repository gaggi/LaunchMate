#pragma once

#include "Models.h"

#include <filesystem>

class ConfigStore
{
public:
    ConfigStore();

    const std::filesystem::path& Path() const noexcept;
    AppConfiguration Load() const;
    void Save(const AppConfiguration& configuration) const;

private:
    std::filesystem::path configPath_;
};
