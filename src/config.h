#pragma once

#include <filesystem>
#include <string>

struct ModLoaderConfig
{
    bool enabled = true;
    bool logModOverrides = true;
    bool logAllFileReads = false;
    bool logSlotResources = false;
    bool dumpSlotResources = false;   // write every original slot resource to logs/dump/<slot>/<page>/<id>.bin
    std::wstring modsDirectory = L"mods";
    std::wstring modPacks;            // optional ';'-separated whitelist of pack folder names (empty = all)
    // Conditional overrides. "slot_sna_body/00005a25=outfit:6;slot_sna_face/00003e66=face:3"
    // and "ww/textures/PC_TXN_UP/slot/slot_sna_body/00005a25/=outfit:6" (path prefix).
    // outfit:N / face:N compare with the player's equipped outfit / FaceCamo id
    // ([linkvarbuf+0xB26] / [linkvarbuf+0xB27]).
    std::string slotConditions;
    std::string pathConditions;
    // HUD name swap: while the outfit/face condition holds, a UI label equal to hudNameMatch is
    // displayed as hudNameReplace (e.g. "OLD SNAKE" -> "BIG BOSS"). -1 disables that part of the condition.
    bool hudNameEnabled = false;
    int hudNameOutfit = -1;
    int hudNameFace = -1;
    std::string hudNameMatch = "OLD SNAKE";
    std::string hudNameReplace = "BIG BOSS";
    // Custom menu entries. Each adds one row to the FaceCamo / Costume list of the Camouflage menu.
    // The game itself keeps wearing the base id; the loader remembers that the custom entry is
    // selected and exposes it to the conditions above as face:<customFaceId> / outfit:<customOutfitId>.
    bool customFaceEnabled = false;
    int customFaceId = 3;              // unused face id, used as the row id and in conditions
    int customFaceBaseId = 9;          // FaceCamo actually equipped (9 = BIG BOSS)
    unsigned int customFaceThumb = 0;  // strcode of the thumbnail texture (0 = same as the base entry)
    std::string customFaceName = "BIG BOSS (TRUE)";
    std::string customFaceDesc = "FaceCamo (Big Boss, true model)";
    bool customOutfitEnabled = false;
    int customOutfitId = 6;
    int customOutfitBaseId = 4;        // costume actually equipped (4 = SUIT)
    unsigned int customOutfitThumb = 0;
    std::string customOutfitName = "BIG BOSS (TRUE)";
    std::string customOutfitDesc = "Big Boss coat (true model)";
};

bool LoadConfig(const std::filesystem::path& path, ModLoaderConfig& config);
