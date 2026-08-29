#pragma once

#include <filesystem>
#include <string>

struct ModLoaderConfig
{
    bool enabled = true;
    bool logModOverrides = true;
    std::wstring modsDirectory = L"mods";
};

bool LoadConfig(const std::filesystem::path& path, ModLoaderConfig& config);
