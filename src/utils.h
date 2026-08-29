#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Utils
{
    inline constexpr uint32_t GV_StrCodeMask = 0x00ffffff;

    struct ExecutableSection
    {
        uint8_t* begin = nullptr;
        uintptr_t size = 0;
    };

    std::filesystem::path GetModulePath(HMODULE module);
    bool FindModuleSection(HMODULE module, const char* name, ExecutableSection& result);
    uint8_t* PatternScanRange(void* begin, uintptr_t size, const char* signature, int skip = 0);
    bool ToLowerAscii(const std::wstring& value, std::string& result);
    bool TryParseHexUint32(const std::wstring& value, uint32_t& result);
    uint32_t GV_StrCode(std::string_view value);
    bool ParseBoolean(std::wstring value, bool fallback);
    void LogAddress(const char* name, uintptr_t address, uintptr_t moduleBase);
}
