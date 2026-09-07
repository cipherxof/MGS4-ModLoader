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
                "LogOverrides = true\n"
                "LogAllFileReads = false\n"
                "LogSlotResources = false\n";
        spdlog::info("Created config file {}", path.generic_string());
    }

    config.enabled = Utils::ParseBoolean(ReadIniValue(path, L"Enabled", L"true"), true);
    config.logModOverrides = Utils::ParseBoolean(ReadIniValue(path, L"LogOverrides", L"true"), true);
    config.logAllFileReads = Utils::ParseBoolean(ReadIniValue(path, L"LogAllFileReads", L"false"), false);
    config.logSlotResources = Utils::ParseBoolean(ReadIniValue(path, L"LogSlotResources", L"false"), false);
    config.dumpSlotResources = Utils::ParseBoolean(ReadIniValue(path, L"DumpSlotResources", L"false"), false);
    config.modPacks = ReadIniValue(path, L"ModPacks", L"");
    {
        const std::wstring sc = ReadIniValue(path, L"SlotConditions", L"");
        const std::wstring pc = ReadIniValue(path, L"PathConditions", L"");
        config.slotConditions.assign(sc.begin(), sc.end());
        config.pathConditions.assign(pc.begin(), pc.end());
    }
    config.hudNameEnabled = Utils::ParseBoolean(ReadIniValue(path, L"HudNameEnabled", L"false"), false);
    config.hudNameOutfit = _wtoi(ReadIniValue(path, L"HudNameOutfit", L"-1").c_str());
    config.hudNameFace = _wtoi(ReadIniValue(path, L"HudNameFace", L"-1").c_str());
    {
        const std::wstring m = ReadIniValue(path, L"HudNameMatch", L"OLD SNAKE");
        const std::wstring r = ReadIniValue(path, L"HudNameReplace", L"BIG BOSS");
        config.hudNameMatch.assign(m.begin(), m.end());
        config.hudNameReplace.assign(r.begin(), r.end());
    }
    config.customFaceEnabled = Utils::ParseBoolean(ReadIniValue(path, L"CustomFaceEnabled", L"false"), false);
    config.customFaceId = _wtoi(ReadIniValue(path, L"CustomFaceId", L"3").c_str());
    config.customFaceBaseId = _wtoi(ReadIniValue(path, L"CustomFaceBaseId", L"9").c_str());
    config.customFaceThumb = static_cast<unsigned int>(wcstoul(ReadIniValue(path, L"CustomFaceThumb", L"0").c_str(), nullptr, 16));
    config.customOutfitEnabled = Utils::ParseBoolean(ReadIniValue(path, L"CustomOutfitEnabled", L"false"), false);
    config.customOutfitId = _wtoi(ReadIniValue(path, L"CustomOutfitId", L"6").c_str());
    config.customOutfitBaseId = _wtoi(ReadIniValue(path, L"CustomOutfitBaseId", L"4").c_str());
    config.customOutfitThumb = static_cast<unsigned int>(wcstoul(ReadIniValue(path, L"CustomOutfitThumb", L"0").c_str(), nullptr, 16));
    {
        const std::wstring fn = ReadIniValue(path, L"CustomFaceName", L"BIG BOSS (TRUE)");
        const std::wstring fd = ReadIniValue(path, L"CustomFaceDesc", L"FaceCamo (Big Boss, true model)");
        const std::wstring on = ReadIniValue(path, L"CustomOutfitName", L"BIG BOSS (TRUE)");
        const std::wstring od = ReadIniValue(path, L"CustomOutfitDesc", L"Big Boss coat (true model)");
        config.customFaceName.assign(fn.begin(), fn.end());
        config.customFaceDesc.assign(fd.begin(), fd.end());
        config.customOutfitName.assign(on.begin(), on.end());
        config.customOutfitDesc.assign(od.begin(), od.end());
    }
    config.modsDirectory = ReadIniValue(path, L"ModsDirectory", L"mods");
    if (config.modsDirectory.empty())
        config.modsDirectory = L"mods";

    spdlog::info(
        "Config: enabled={}, modsDirectory={}, logOverrides={}, logAllFileReads={}, logSlotResources={}",
        config.enabled, std::filesystem::path(config.modsDirectory).generic_string(), config.logModOverrides,
        config.logAllFileReads, config.logSlotResources);
    return true;
}
