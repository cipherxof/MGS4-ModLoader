#include "config.h"

#include "utils.h"

#include <windows.h>

#include <array>
#include <fstream>
#include <spdlog/spdlog.h>

namespace
{
    std::wstring ReadIniValue(const std::filesystem::path& path, const wchar_t* key, const wchar_t* fallback)
    {
        std::array<wchar_t, 32768> value{};
        GetPrivateProfileStringW(L"ModLoader", key, fallback, value.data(), static_cast<DWORD>(value.size()),
                                 path.c_str());
        return value.data();
    }
}

bool LoadConfig(const std::filesystem::path& path, ModLoaderConfig& config)
{
    std::error_code error;
    if (!std::filesystem::exists(path, error))
    {
        std::ofstream file(path);
        if (!file)
        {
            spdlog::error("Failed to create config file {}", path.generic_string());
            return false;
        }
        file << "[ModLoader]\n"
                "Enabled = true\n"
                "ModsDirectory = mods\n"
                "LogOverrides = true\n";
        spdlog::info("Created config file {}", path.generic_string());
    }

    config.enabled = Utils::ParseBoolean(ReadIniValue(path, L"Enabled", L"true"), true);
    config.logModOverrides = Utils::ParseBoolean(ReadIniValue(path, L"LogOverrides", L"true"), true);
    config.modsDirectory = ReadIniValue(path, L"ModsDirectory", L"mods");
    if (config.modsDirectory.empty())
        config.modsDirectory = L"mods";

    spdlog::info("Config: enabled={}, modsDirectory={}, logOverrides={}", config.enabled,
                 std::filesystem::path(config.modsDirectory).generic_string(), config.logModOverrides);
    return true;
}
