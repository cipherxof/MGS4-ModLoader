#include "modloader.h"

#include "MinHook.h"
#include "utils.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using ArchiveFileInfoDelegate = bool(__fastcall*)(const char* path, uint64_t* size);
    using ArchiveLookupDelegate = void*(__fastcall*)(const char* path);
    using ArchiveReadDelegate = int32_t(__fastcall*)(const char* path, void* destination, uint64_t destinationSize,
                                                     uint64_t offset, uint64_t requestedSize);
    using GameFileOpenADelegate = bool(__fastcall*)(void* file, const char* path, const uint8_t* options);
    using GameFileOpenWDelegate = bool(__fastcall*)(void* file, const wchar_t* path, const uint8_t* options);
    using SlotResourceLoadDelegate = int32_t(__fastcall*)(void* data, uint32_t resourceId, uint32_t flags,
                                                          uint32_t size, uint32_t destination,
                                                          uint32_t slotNameHash, uint32_t pageId);
    using SlotResourceUnloadDelegate = int64_t(__fastcall*)(void* data, uint32_t resourceId, uint32_t flags);

    ArchiveFileInfoDelegate ArchiveFileInfo = nullptr;
    ArchiveLookupDelegate ArchiveLookup = nullptr;
    ArchiveReadDelegate ArchiveRead = nullptr;
    GameFileOpenADelegate GameFileOpenA = nullptr;
    GameFileOpenWDelegate GameFileOpenW = nullptr;
    SlotResourceLoadDelegate SlotResourceLoad = nullptr;
    SlotResourceUnloadDelegate SlotResourceUnload = nullptr;

    std::filesystem::path GameRoot;
    std::filesystem::path ModRoot;
    HMODULE GameModule = nullptr;
    ModLoaderConfig ActiveConfig;

    // ModPacks = "PackA;PackB" restricts indexing to the listed pack folders (case-insensitive).
    bool IsPackAllowed(const std::wstring& name)
    {
        const std::wstring& list = ActiveConfig.modPacks;
        if (list.empty())
            return true;
        size_t pos = 0;
        while (pos <= list.size())
        {
            size_t end = list.find(L';', pos);
            if (end == std::wstring::npos)
                end = list.size();
            if (_wcsicmp(list.substr(pos, end - pos).c_str(), name.c_str()) == 0)
                return true;
            pos = end + 1;
        }
        return false;
    }

    struct IndexedModFile
    {
        std::filesystem::path path;
        uint64_t size = 0;
        size_t packageOrder = 0;
    };

    struct IndexedSlotFile
    {
        IndexedModFile file;
        std::string slotName;
        std::string memberName;
        uint32_t memberId = 0;
        bool exactId = false;
    };

    struct SlotReplacementBuffer
    {
        ~SlotReplacementBuffer()
        {
            if (data)
                VirtualFree(data, 0, MEM_RELEASE);
        }

        void* data = nullptr;
        uint32_t size = 0;
        std::filesystem::path path;
    };

    struct ActiveSlotKey
    {
        const void* originalData = nullptr;
        uint32_t resourceId = 0;

        bool operator==(const ActiveSlotKey& other) const
        {
            return originalData == other.originalData && resourceId == other.resourceId;
        }
    };

    struct ActiveSlotKeyHash
    {
        size_t operator()(const ActiveSlotKey& key) const
        {
            const size_t pointerHash = std::hash<const void*>{}(key.originalData);
            return pointerHash ^ (static_cast<size_t>(key.resourceId) + 0x9e3779b9U + (pointerHash << 6) +
                                  (pointerHash >> 2));
        }
    };

    std::unordered_map<std::wstring, IndexedModFile> OverrideIndex;
    std::unordered_map<std::wstring, std::vector<IndexedModFile>> CnfFragmentIndex;
    std::unordered_map<uint64_t, IndexedSlotFile> ExactSlotOverrideIndex;
    std::unordered_map<uint64_t, IndexedSlotFile> HashedSlotOverrideIndex;
    using PagedSlotKey = std::tuple<uint32_t, uint32_t, uint32_t>;
    std::map<PagedSlotKey, IndexedSlotFile> ExactPagedSlotOverrideIndex;
    std::map<PagedSlotKey, IndexedSlotFile> HashedPagedSlotOverrideIndex;
    std::unordered_map<uint32_t, std::string> IndexedSlotNames;
    std::unordered_map<ActiveSlotKey, std::vector<std::shared_ptr<SlotReplacementBuffer>>, ActiveSlotKeyHash>
        ActiveSlotOverrides;
    std::vector<std::filesystem::path> ModPackages;
    std::mutex LoggedOverridesMutex;
    std::unordered_set<std::string> LoggedOverrides;
    std::mutex ActiveSlotOverridesMutex;

    constexpr uint64_t MaximumReadChunk = 0x7ffff000;
    constexpr int32_t ArchiveReadError = 4;

    struct SyntheticArchiveEntry
    {
        uint64_t size = 0;
        uint8_t unused[0x34]{};
        uint32_t archiveIndex = 0;
    };

    static_assert(offsetof(SyntheticArchiveEntry, archiveIndex) == 0x3C);
    thread_local SyntheticArchiveEntry LooseOverrideEntry;

    struct CnfMerge
    {
        std::filesystem::path baseOverride;
        uint64_t baseSize = 0;
        std::vector<std::filesystem::path> fragments;
        std::vector<uint64_t> fragmentSizes;
        uint64_t size = 0;
    };

    constexpr char CnfFragmentSeparator[] = "\r\n";
    constexpr uint64_t CnfFragmentSeparatorSize = sizeof(CnfFragmentSeparator) - 1;

    std::wstring NormalizeSeparators(std::wstring value)
    {
        std::replace(value.begin(), value.end(), L'/', L'\\');
        while (value.size() > 3 && value.back() == L'\\')
            value.pop_back();
        return value;
    }

    bool StartsWithPath(const std::wstring& path, const std::wstring& root)
    {
        if (path.size() <= root.size() || _wcsnicmp(path.c_str(), root.c_str(), root.size()) != 0)
            return false;

        return path[root.size()] == L'\\';
    }

    bool ContainsParentTraversal(const std::filesystem::path& path)
    {
        for (const auto& component : path)
        {
            if (component == L"..")
                return true;
        }
        return false;
    }

    bool MakeRelativeGamePath(const std::filesystem::path& source, std::filesystem::path& relative)
    {
        if (source.empty() || ContainsParentTraversal(source))
            return false;

        if (source.has_root_name())
        {
            const std::wstring sourcePath = NormalizeSeparators(source.lexically_normal().native());
            const std::wstring gamePath = NormalizeSeparators(GameRoot.lexically_normal().native());
            if (!StartsWithPath(sourcePath, gamePath))
                return false;

            relative = std::filesystem::path(sourcePath.substr(gamePath.size() + 1));
        }
        else if (source.has_root_directory())
        {
            relative = source.relative_path();
        }
        else
        {
            relative = source;
        }

        std::filesystem::path clean;
        for (const auto& component : relative)
        {
            if (component.empty() || component == L".")
                continue;

            const std::wstring value = component.native();
            if (component == L".." || value.find(L':') != std::wstring::npos)
                return false;
            clean /= component;
        }

        if (clean.empty())
            return false;

        relative = clean;
        return true;
    }

    bool GetFileSize(const std::filesystem::path& path, uint64_t& size)
    {
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) ||
            (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return false;
        }

        size = (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
        return true;
    }

    bool EqualsInsensitive(const std::wstring& left, const wchar_t* right)
    {
        return _wcsicmp(left.c_str(), right) == 0;
    }

    bool EndsWithInsensitive(const std::wstring& value, const wchar_t* suffix)
    {
        const size_t suffixLength = wcslen(suffix);
        return value.size() >= suffixLength &&
               _wcsicmp(value.c_str() + value.size() - suffixLength, suffix) == 0;
    }

    std::wstring MakePathKey(const std::filesystem::path& path)
    {
        std::wstring key = NormalizeSeparators(path.lexically_normal().native());
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
        return key;
    }

    uint64_t MakeSlotKey(uint32_t slotNameHash, uint32_t memberId)
    {
        return (static_cast<uint64_t>(slotNameHash & Utils::GV_StrCodeMask) << 32) | memberId;
    }

    bool IndexSlotOverride(const std::filesystem::path& relative, const IndexedModFile& indexedFile,
        size_t& conflictCount)
    {
        std::vector<std::filesystem::path> components;
        for (const auto& component : relative)
            components.push_back(component);

        if (components.empty() || !EqualsInsensitive(components[0].native(), L"slots"))
            return false;

        if (components.size() != 3 && components.size() != 4)
        {
            spdlog::warn("Ignoring slot override with invalid path "
                "(expected slots/<slot>/<file> or slots/<slot>/<page>/<file>): {}",
                relative.generic_string());
            return true;
        }

        const bool hasPage = (components.size() == 4);
        const size_t fileIndex = hasPage ? 3 : 2;

        uint32_t pageId = 0;
        if (hasPage && !Utils::TryParseHexUint32(components[2].native(), pageId))
        {
            spdlog::warn("Ignoring slot override with non-hex page directory: {}", relative.generic_string());
            return true;
        }

        std::string slotName;
        std::string memberName;
        if (!Utils::ToLowerAscii(components[1].native(), slotName) ||
            !Utils::ToLowerAscii(components[fileIndex].filename().native(), memberName))
        {
            spdlog::warn("Ignoring non-ASCII slot override path: {}", relative.generic_string());
            return true;
        }

        const uint32_t slotNameHash = Utils::GV_StrCode(slotName);
        const std::wstring memberStem = components[fileIndex].stem().native();
        uint32_t memberId = 0;
        const bool exactId = Utils::TryParseHexUint32(memberStem, memberId);
        if (!exactId)
        {
            std::string normalizedStem;
            if (!Utils::ToLowerAscii(memberStem, normalizedStem))
            {
                spdlog::warn("Ignoring slot override with invalid member name: {}", relative.generic_string());
                return true;
            }
            memberId = Utils::GV_StrCode(normalizedStem);
        }

        IndexedSlotNames[slotNameHash] = slotName;
        IndexedSlotFile slotFile{ indexedFile, slotName, memberName, memberId, exactId };

        if (hasPage)
        {
            auto& index = exactId ? ExactPagedSlotOverrideIndex : HashedPagedSlotOverrideIndex;
            const PagedSlotKey key{ slotNameHash & Utils::GV_StrCodeMask, pageId, memberId };
            if (index.find(key) != index.end())
                ++conflictCount;
            index[key] = std::move(slotFile);
            spdlog::info("Indexed paged slot override: slot={} page={:08x} member={:08x} -> {}", slotName,
                pageId, memberId, indexedFile.path.generic_string());
        }
        else
        {
            auto& index = exactId ? ExactSlotOverrideIndex : HashedSlotOverrideIndex;
            const uint64_t key = MakeSlotKey(slotNameHash, memberId);
            if (index.find(key) != index.end())
                ++conflictCount;
            index[key] = std::move(slotFile);
        }
        return true;
    }

    bool IsCnfFragment(const std::filesystem::path& path)
    {
        const std::wstring filename = path.filename().native();
        return EqualsInsensitive(filename, L"merge.cnf") || EndsWithInsensitive(filename, L".merge.cnf");
    }

    bool IsDataCnfPath(const char* path)
    {
        if (!path || *path == '\0')
            return false;

        const char* filename = path;
        for (const char* character = path; *character != '\0'; ++character)
        {
            if (*character == '/' || *character == '\\')
                filename = character + 1;
        }
        return _stricmp(filename, "data.cnf") == 0;
    }

    bool BuildModIndex()
    {
        const auto start = std::chrono::steady_clock::now();
        OverrideIndex.clear();
        CnfFragmentIndex.clear();
        ExactSlotOverrideIndex.clear();
        HashedSlotOverrideIndex.clear();
        ExactPagedSlotOverrideIndex.clear();
        HashedPagedSlotOverrideIndex.clear();
        IndexedSlotNames.clear();
        ModPackages.clear();

        std::error_code error;
        std::filesystem::directory_iterator packageIterator(ModRoot, error);
        if (error)
        {
            spdlog::error("Failed to enumerate the MGS4 mods directory {}: {}", ModRoot.generic_string(),
                          error.message());
            return false;
        }

        for (const auto& entry : packageIterator)
        {
            if (entry.is_directory(error))
            {
                if (IsPackAllowed(entry.path().filename().native()))
                    ModPackages.push_back(entry.path());
                else
                    spdlog::warn("Mod pack ignored (not listed in ModPacks): {}", entry.path().filename().string());
            }
            error.clear();
        }

        std::sort(ModPackages.begin(), ModPackages.end(), [](const auto& left, const auto& right) {
            const std::wstring leftName = left.filename().native();
            const std::wstring rightName = right.filename().native();
            const int insensitiveOrder = _wcsicmp(leftName.c_str(), rightName.c_str());
            return insensitiveOrder != 0 ? insensitiveOrder < 0 : leftName < rightName;
        });

        size_t indexedFileCount = 0;
        size_t conflictCount = 0;
        size_t fragmentCount = 0;
        for (size_t packageOrder = 0; packageOrder < ModPackages.size(); ++packageOrder)
        {
            const auto& package = ModPackages[packageOrder];
            std::filesystem::recursive_directory_iterator iterator(
                package, std::filesystem::directory_options::skip_permission_denied, error);
            const std::filesystem::recursive_directory_iterator end;
            if (error)
            {
                spdlog::warn("Failed to enumerate mod package {}: {}", package.generic_string(), error.message());
                error.clear();
                continue;
            }

            while (iterator != end)
            {
                const auto entry = *iterator;
                iterator.increment(error);
                if (error)
                {
                    spdlog::warn("Failed while enumerating mod package {}: {}", package.generic_string(),
                                 error.message());
                    error.clear();
                }

                if (!entry.is_regular_file(error))
                {
                    error.clear();
                    continue;
                }

                uint64_t size = 0;
                if (!GetFileSize(entry.path(), size))
                    continue;

                const std::filesystem::path relative = entry.path().lexically_relative(package);
                if (relative.empty() || relative.has_root_path() || ContainsParentTraversal(relative))
                    continue;

                const IndexedModFile indexedFile{entry.path(), size, packageOrder};
                ++indexedFileCount;
                if (IndexSlotOverride(relative, indexedFile, conflictCount))
                    continue;
                if (IsCnfFragment(relative))
                {
                    const std::filesystem::path dataCnf = relative.parent_path() / L"data.cnf";
                    CnfFragmentIndex[MakePathKey(dataCnf)].push_back(indexedFile);
                    ++fragmentCount;
                    continue;
                }

                const std::wstring key = MakePathKey(relative);
                if (OverrideIndex.find(key) != OverrideIndex.end())
                    ++conflictCount;
                OverrideIndex[key] = indexedFile;
            }
        }

        for (auto& [key, fragments] : CnfFragmentIndex)
        {
            std::sort(fragments.begin(), fragments.end(), [](const auto& left, const auto& right) {
                if (left.packageOrder != right.packageOrder)
                    return left.packageOrder < right.packageOrder;

                const std::wstring leftName = left.path.filename().native();
                const std::wstring rightName = right.path.filename().native();
                const int insensitiveOrder = _wcsicmp(leftName.c_str(), rightName.c_str());
                return insensitiveOrder != 0 ? insensitiveOrder < 0 : left.path.native() < right.path.native();
            });
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        const size_t slotOverrideCount = ExactSlotOverrideIndex.size() + HashedSlotOverrideIndex.size();
        spdlog::info(
            "Indexed {} files from {} mod packages in {} ms ({} path overrides, {} slot overrides, {} CNF "
            "fragments, {} conflicts)",
            indexedFileCount, ModPackages.size(), elapsed.count(), OverrideIndex.size(), slotOverrideCount,
            fragmentCount, conflictCount);
        return true;
    }

    bool FindCnfFragments(const std::filesystem::path& source, std::vector<std::filesystem::path>& fragments,
                          std::vector<uint64_t>& fragmentSizes)
    {
        std::filesystem::path relative;
        if (!MakeRelativeGamePath(source, relative) || !EqualsInsensitive(relative.filename().native(), L"data.cnf"))
            return false;

        const auto found = CnfFragmentIndex.find(MakePathKey(relative));
        if (found == CnfFragmentIndex.end())
            return false;

        fragments.reserve(found->second.size());
        fragmentSizes.reserve(found->second.size());
        for (const auto& fragment : found->second)
        {
            fragments.push_back(fragment.path);
            fragmentSizes.push_back(fragment.size);
        }
        return !fragments.empty();
    }


    // ---- Conditional overrides --------------------------------------------------------------------
    // A slot page or a path prefix can be tied to the player's equipped outfit / FaceCamo id. The
    // override is only served while the condition holds, so a replacement can be attached to a
    // specific costume without touching the original one.
    struct OverrideCondition { int outfit = -1; int face = -1; };
    std::unordered_map<uint64_t, OverrideCondition> SlotConditions;                    // slotHash16<<16 | page16
    std::vector<std::pair<std::wstring, OverrideCondition>> PathConditions;           // normalized prefix
    uint8_t** PlayerContextPtr = nullptr;   // mgs4.exe+0x1C28B28 -> player context ("linkvarbuf")
    constexpr uintptr_t PlayerContextRva = 0x1C28B28;
    constexpr size_t PlayerOutfitOffset = 0xB26;
    constexpr size_t PlayerFaceOffset = 0xB27;

    // Hook point used by the custom menu entries (see below): a custom outfit is presented to the
    // game as its base outfit, so the real selection lives here.
    int VirtualOutfitId = -1;       // -1 = none
    bool VirtualOutfitActive = false;
    int VirtualFaceId = -1;
    bool VirtualFaceActive = false;

    bool ConditionMet(const OverrideCondition& cond)
    {
        if (!PlayerContextPtr || !*PlayerContextPtr)
            return false;
        const uint8_t* ctx = *PlayerContextPtr;
        if (cond.outfit >= 0)
        {
            if (cond.outfit == VirtualOutfitId)
            {
                if (!VirtualOutfitActive)
                    return false;
            }
            else if (ctx[PlayerOutfitOffset] != cond.outfit)
                return false;
        }
        if (cond.face >= 0)
        {
            if (cond.face == VirtualFaceId)
            {
                if (!VirtualFaceActive)
                    return false;
            }
            else if (ctx[PlayerFaceOffset] != cond.face)
                return false;
        }
        return true;
    }

    bool ParseCondition(const std::string& text, OverrideCondition& out)
    {
        if (text.rfind("face:", 0) == 0) { out.face = atoi(text.c_str() + 5); return true; }
        if (text.rfind("outfit:", 0) == 0) { out.outfit = atoi(text.c_str() + 7); return true; }
        return false;
    }

    void ParseConditions(uintptr_t gameBase)
    {
        PlayerContextPtr = reinterpret_cast<uint8_t**>(gameBase + PlayerContextRva);
        auto forEach = [](const std::string& all, auto&& fn) {
            size_t pos = 0;
            while (pos < all.size())
            {
                size_t end = all.find(';', pos);
                if (end == std::string::npos)
                    end = all.size();
                const std::string item = all.substr(pos, end - pos);
                pos = end + 1;
                const size_t eq = item.find('=');
                if (eq == std::string::npos)
                    continue;
                OverrideCondition cond;
                if (ParseCondition(item.substr(eq + 1), cond))
                    fn(item.substr(0, eq), cond);
            }
        };
        forEach(ActiveConfig.slotConditions, [](const std::string& target, const OverrideCondition& cond) {
            const size_t sl = target.find('/');
            if (sl == std::string::npos)
                return;
            std::string slot = target.substr(0, sl);
            for (auto& ch : slot)
                ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            const uint32_t slotHash = Utils::GV_StrCode(slot.c_str()) & 0xFFFF;
            const uint32_t pageId = static_cast<uint32_t>(strtoul(target.c_str() + sl + 1, nullptr, 16)) & 0xFFFF;
            SlotConditions[(static_cast<uint64_t>(slotHash) << 16) | pageId] = cond;
            spdlog::info("Slot condition: {}/{:04x} outfit={} face={}", slot, pageId, cond.outfit, cond.face);
        });
        forEach(ActiveConfig.pathConditions, [](const std::string& prefix, const OverrideCondition& cond) {
            std::wstring key(prefix.begin(), prefix.end());
            for (auto& ch : key)
            {
                if (ch == L'/')
                    ch = L'\\';
                ch = static_cast<wchar_t>(std::towlower(ch));
            }
            PathConditions.emplace_back(key, cond);
            spdlog::info("Path condition: {} outfit={} face={}", prefix, cond.outfit, cond.face);
        });
    }

    bool SlotConditionMet(uint32_t slotNameHash, uint32_t pageId)
    {
        if (SlotConditions.empty())
            return true;
        const auto it = SlotConditions.find((static_cast<uint64_t>(slotNameHash & 0xFFFF) << 16) | (pageId & 0xFFFF));
        return it == SlotConditions.end() || ConditionMet(it->second);
    }

    bool PathConditionMet(const std::wstring& key)
    {
        for (const auto& pc : PathConditions)
            if (key.compare(0, pc.first.size(), pc.first) == 0 && !ConditionMet(pc.second))
                return false;
        return true;
    }

    bool ResolveOverride(const std::filesystem::path& source, std::filesystem::path& overridePath, uint64_t& size)
    {
        std::filesystem::path relative;
        if (!MakeRelativeGamePath(source, relative))
            return false;

        const std::wstring key = MakePathKey(relative);
        const auto found = OverrideIndex.find(key);
        if (found == OverrideIndex.end())
            return false;
        if (!PathConditionMet(key))
            return false;

        overridePath = found->second.path;
        size = found->second.size;
        return true;
    }

    bool ResolveOverride(const char* source, std::filesystem::path& overridePath, uint64_t& size)
    {
        if (!source || *source == '\0')
            return false;

        try
        {
            return ResolveOverride(std::filesystem::path(source), overridePath, size);
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    bool ResolveOverride(const wchar_t* source, std::filesystem::path& overridePath, uint64_t& size)
    {
        if (!source || *source == L'\0')
            return false;

        try
        {
            return ResolveOverride(std::filesystem::path(source), overridePath, size);
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    const IndexedSlotFile* ResolveSlotOverride(uint32_t slotNameHash, uint32_t resourceId, uint32_t pageId)
    {
        if (!SlotConditionMet(slotNameHash, pageId))
            return nullptr;
        const IndexedSlotFile* result = nullptr;
        const uint32_t slot = slotNameHash & Utils::GV_StrCodeMask;

        const auto considerPaged = [&](const std::map<PagedSlotKey, IndexedSlotFile>& index, uint32_t memberId) {
            const auto found = index.find(PagedSlotKey{ slot, pageId, memberId });
            if (found == index.end())
                return;
            if (!result || found->second.file.packageOrder > result->file.packageOrder)
                result = &found->second;
            };

        considerPaged(ExactPagedSlotOverrideIndex, resourceId);
        if ((resourceId & 0x80000000U) != 0)
            considerPaged(ExactPagedSlotOverrideIndex, resourceId & 0x7fffffffU);
        considerPaged(HashedPagedSlotOverrideIndex, resourceId & Utils::GV_StrCodeMask);
        if (result)
            return result;

        const auto consider = [&](const std::unordered_map<uint64_t, IndexedSlotFile>& index, uint32_t memberId) {
            const auto found = index.find(MakeSlotKey(slotNameHash, memberId));
            if (found == index.end())
                return;
            if (!result || found->second.file.packageOrder > result->file.packageOrder)
                result = &found->second;
            };

        consider(ExactSlotOverrideIndex, resourceId);
        if ((resourceId & 0x80000000U) != 0)
            consider(ExactSlotOverrideIndex, resourceId & 0x7fffffffU);
        consider(HashedSlotOverrideIndex, resourceId & Utils::GV_StrCodeMask);
        return result;
    }

    const char* ResolveSlotName(uint32_t slotNameHash)
    {
        const auto found = IndexedSlotNames.find(slotNameHash & Utils::GV_StrCodeMask);
        return found == IndexedSlotNames.end() ? nullptr : found->second.c_str();
    }

    bool ResolveCnfMerge(const char* source, CnfMerge& merge)
    {
        if (!IsDataCnfPath(source))
            return false;

        try
        {
            const std::filesystem::path sourcePath(source);
            if (!FindCnfFragments(sourcePath, merge.fragments, merge.fragmentSizes))
                return false;

            if (!ResolveOverride(sourcePath, merge.baseOverride, merge.baseSize))
            {
                merge.baseOverride.clear();
                if (!ArchiveFileInfo(source, &merge.baseSize))
                    return false;
            }

            merge.size = merge.baseSize;
            for (const uint64_t fragmentSize : merge.fragmentSizes)
            {
                if (merge.size > std::numeric_limits<uint64_t>::max() - CnfFragmentSeparatorSize ||
                    fragmentSize > std::numeric_limits<uint64_t>::max() - merge.size - CnfFragmentSeparatorSize)
                    return false;
                merge.size += CnfFragmentSeparatorSize + fragmentSize;
            }
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    void LogOverride(const std::filesystem::path& source, const std::filesystem::path& overridePath)
    {
        if (!ActiveConfig.logModOverrides)
            return;

        const std::string key = overridePath.generic_string();
        std::lock_guard<std::mutex> lock(LoggedOverridesMutex);
        if (!LoggedOverrides.insert(key).second)
            return;

        spdlog::info("Mod override: {} -> {}", source.generic_string(), key);
    }

    void LogOverride(const char* source, const std::filesystem::path& overridePath)
    {
        try
        {
            LogOverride(std::filesystem::path(source), overridePath);
        }
        catch (const std::exception&)
        {
            LogOverride(std::filesystem::path("<invalid path>"), overridePath);
        }
    }

    void LogOverride(const wchar_t* source, const std::filesystem::path& overridePath)
    {
        try
        {
            LogOverride(std::filesystem::path(source), overridePath);
        }
        catch (const std::exception&)
        {
            LogOverride(std::filesystem::path("<invalid path>"), overridePath);
        }
    }

    void LogCnfMerge(const char* source, const CnfMerge& merge)
    {
        if (!ActiveConfig.logModOverrides)
            return;

        for (const auto& fragment : merge.fragments)
        {
            const std::string key = "merge:" + fragment.generic_string();
            std::lock_guard<std::mutex> lock(LoggedOverridesMutex);
            if (!LoggedOverrides.insert(key).second)
                continue;
            spdlog::info("CNF merge: {} + {}", source, fragment.generic_string());
        }
    }

    void LogOverrideRead(const std::filesystem::path& path, const void* destination, uint64_t destinationSize,
                         uint64_t fileSize, uint64_t offset, uint64_t requestedSize, uint64_t bytesRead)
    {
        if (!ActiveConfig.logModOverrides || offset != 0 || bytesRead < 4 || !destination ||
            !EndsWithInsensitive(path.native(), L".dlz"))
        {
            return;
        }

        const std::string key = "read:" + path.generic_string();
        std::lock_guard<std::mutex> lock(LoggedOverridesMutex);
        if (!LoggedOverrides.insert(key).second)
            return;

        const auto* bytes = static_cast<const uint8_t*>(destination);
        spdlog::info(
            "Mod read: {} file={}, destination={}, requested={}, returned={}, first={:02x} {:02x} {:02x} {:02x}",
            path.generic_string(), fileSize, destinationSize, requestedSize, bytesRead,
            static_cast<unsigned>(bytes[0]), static_cast<unsigned>(bytes[1]),
            static_cast<unsigned>(bytes[2]), static_cast<unsigned>(bytes[3]));
    }

    const char* SafePath(const char* path)
    {
        return path && *path != '\0' ? path : "<empty>";
    }

    std::string SafePath(const wchar_t* path)
    {
        if (!path || *path == L'\0')
            return "<empty>";

        try
        {
            return std::filesystem::path(path).generic_string();
        }
        catch (const std::exception&)
        {
            return "<invalid path>";
        }
    }

    void LogArchiveRead(const char* path, const char* source, const std::filesystem::path* resolvedPath,
                        uint64_t offset, uint64_t requestedSize, int32_t result)
    {
        if (!ActiveConfig.logAllFileReads)
            return;

        if (resolvedPath)
        {
            spdlog::info("Game file read: path={} source={} resolved={} offset={} requested={} status={}",
                         SafePath(path), source, resolvedPath->generic_string(), offset, requestedSize, result);
        }
        else
        {
            spdlog::info("Game file read: path={} source={} offset={} requested={} status={}", SafePath(path),
                         source, offset, requestedSize, result);
        }
    }

    template <typename Path>
    void LogLooseFileRead(const Path* path, const std::filesystem::path* resolvedPath, bool result)
    {
        if (!ActiveConfig.logAllFileReads)
            return;

        if (resolvedPath)
        {
            spdlog::info("Game file read: path={} source=loose-override resolved={} opened={}", SafePath(path),
                         resolvedPath->generic_string(), result);
        }
        else
        {
            spdlog::info("Game file read: path={} source=loose opened={}", SafePath(path), result);
        }
    }

    void LogSlotResource(uint32_t slotNameHash, uint32_t pageId, uint32_t resourceId, uint32_t size)
    {
        if (!ActiveConfig.logSlotResources)
            return;

        const char* slotName = ResolveSlotName(slotNameHash);
        if (!slotName)
            return;

        const std::string key = "slot-resource:" + std::to_string(slotNameHash) + ":" +
                                std::to_string(pageId) + ":" + std::to_string(resourceId);
        std::lock_guard<std::mutex> lock(LoggedOverridesMutex);
        if (!LoggedOverrides.insert(key).second)
            return;

        spdlog::info("Slot resource: slot={} slotHash={:06x} page={:08x} id={:08x} hash={:06x} size={}",
                     slotName, slotNameHash & Utils::GV_StrCodeMask, pageId, resourceId,
                     resourceId & Utils::GV_StrCodeMask, size);
    }

    void LogSlotOverride(const IndexedSlotFile& overrideFile, uint32_t slotNameHash, uint32_t pageId,
                         uint32_t resourceId, uint32_t originalSize, uint32_t replacementSize, int32_t result)
    {
        if (!ActiveConfig.logModOverrides)
            return;

        const std::string path = overrideFile.file.path.generic_string();
        const std::string key = "slot-override:" + path;
        std::lock_guard<std::mutex> lock(LoggedOverridesMutex);
        if (!LoggedOverrides.insert(key).second)
            return;

        spdlog::info(
            "Slot override: slot={} slotHash={:06x} page={:08x} id={:08x} hash={:06x} original={} "
            "replacement={} status={} -> {}",
            overrideFile.slotName, slotNameHash & Utils::GV_StrCodeMask, pageId, resourceId,
            resourceId & Utils::GV_StrCodeMask, originalSize, replacementSize, result, path);
    }

    bool __fastcall ArchiveFileInfoHook(const char* path, uint64_t* size)
    {
        CnfMerge merge;
        if (ResolveCnfMerge(path, merge))
        {
            if (size)
                *size = merge.size;
            if (!merge.baseOverride.empty())
                LogOverride(path, merge.baseOverride);
            LogCnfMerge(path, merge);
            return true;
        }

        std::filesystem::path overridePath;
        uint64_t overrideSize = 0;
        if (!ResolveOverride(path, overridePath, overrideSize))
            return ArchiveFileInfo(path, size);

        if (size)
            *size = overrideSize;
        LogOverride(path, overridePath);
        return true;
    }

    void* __fastcall ArchiveLookupHook(const char* path)
    {
        std::filesystem::path overridePath;
        uint64_t overrideSize = 0;
        if (!ResolveOverride(path, overridePath, overrideSize))
            return ArchiveLookup(path);

        LooseOverrideEntry.size = overrideSize;
        LogOverride(path, overridePath);
        return &LooseOverrideEntry;
    }

    int32_t ReadOverride(const std::filesystem::path& path, uint64_t fileSize, void* destination,
                         uint64_t destinationSize, uint64_t offset, uint64_t requestedSize)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            spdlog::error("Failed to open mod override {} (error {})", path.generic_string(), GetLastError());
            return ArchiveReadError;
        }

        if (offset > fileSize || (offset == fileSize && requestedSize != 0))
        {
            CloseHandle(file);
            spdlog::error("Invalid mod override read for {} at offset {}", path.generic_string(), offset);
            return ArchiveReadError;
        }

        const uint64_t available = fileSize - offset;
        const uint64_t bytesToRead = std::min(available, requestedSize);
        // The PC texture streamer requests PC_TXN_UP .data files with destinationSize == 0: the destination
        // buffer is sized from the texture record, so trust the requested size in that case.
        if (destinationSize == 0)
            destinationSize = bytesToRead;
        if (bytesToRead > destinationSize || (bytesToRead != 0 && !destination))
        {
            CloseHandle(file);
            spdlog::error("Mod override buffer is too small for {} (need {}, have {})", path.generic_string(),
                          bytesToRead, destinationSize);
            return ArchiveReadError;
        }

        if (bytesToRead == 0)
        {
            CloseHandle(file);
            return 0;
        }

        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN))
        {
            const DWORD error = GetLastError();
            CloseHandle(file);
            spdlog::error("Failed to seek mod override {} (error {})", path.generic_string(), error);
            return ArchiveReadError;
        }

        auto* output = static_cast<uint8_t*>(destination);
        uint64_t remaining = bytesToRead;
        while (remaining != 0)
        {
            const DWORD chunk = static_cast<DWORD>(std::min(remaining, MaximumReadChunk));
            DWORD bytesRead = 0;
            if (!ReadFile(file, output, chunk, &bytesRead, nullptr) || bytesRead != chunk)
            {
                const DWORD error = GetLastError();
                CloseHandle(file);
                spdlog::error("Failed to read mod override {} (error {})", path.generic_string(), error);
                return ArchiveReadError;
            }

            output += bytesRead;
            remaining -= bytesRead;
        }

        LogOverrideRead(path, destination, destinationSize, fileSize, offset, requestedSize, bytesToRead);
        CloseHandle(file);
        return 0;
    }

    std::shared_ptr<SlotReplacementBuffer> LoadSlotReplacement(const IndexedSlotFile& overrideFile)
    {
        if (overrideFile.file.size == 0 || overrideFile.file.size > std::numeric_limits<uint32_t>::max())
        {
            spdlog::error("Invalid slot override size for {}: {}", overrideFile.file.path.generic_string(),
                          overrideFile.file.size);
            return {};
        }

        try
        {
            auto buffer = std::make_shared<SlotReplacementBuffer>();
            buffer->size = static_cast<uint32_t>(overrideFile.file.size);
            buffer->path = overrideFile.file.path;
            buffer->data = VirtualAlloc(nullptr, buffer->size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!buffer->data)
            {
                spdlog::error("Failed to allocate {} bytes for slot override {} (error {})", buffer->size,
                              buffer->path.generic_string(), GetLastError());
                return {};
            }

            if (ReadOverride(buffer->path, buffer->size, buffer->data, buffer->size, 0, buffer->size) != 0)
                return {};
            return buffer;
        }
        catch (const std::exception& exception)
        {
            spdlog::error("Failed to prepare slot override {}: {}", overrideFile.file.path.generic_string(),
                          exception.what());
            return {};
        }
    }

    // DumpSlotResources = true writes the ORIGINAL data of every SlotResourceLoad call (before any override)
    // to logs/dump/<slot>/<page>/<id>.bin. Handy to recover resources that are scrambled inside the .slot file.
    void DumpSlotResource(uint32_t slotNameHash, uint32_t pageId, uint32_t resourceId, const void* data, uint32_t size)
    {
        if (!ActiveConfig.dumpSlotResources || !data || size == 0 || size > (64u << 20))
            return;
        const char* slotName = ResolveSlotName(slotNameHash);
        char fallback[16];
        snprintf(fallback, sizeof(fallback), "%06x", slotNameHash & Utils::GV_StrCodeMask);
        char page[16];
        snprintf(page, sizeof(page), "%08x", pageId);
        char file[24];
        snprintf(file, sizeof(file), "%08x.bin", resourceId);
        std::filesystem::path dir = GameRoot / L"logs" / L"dump" / (slotName ? slotName : fallback) / page;
        std::error_code error;
        std::filesystem::create_directories(dir, error);
        const std::filesystem::path out = dir / file;
        if (std::filesystem::exists(out, error))
            return;
        if (FILE* f = _wfopen(out.c_str(), L"wb"))
        {
            fwrite(data, 1, size, f);
            fclose(f);
        }
    }

    int32_t __fastcall SlotResourceLoadHook(void* data, uint32_t resourceId, uint32_t flags, uint32_t size,
                                            uint32_t destination, uint32_t slotNameHash, uint32_t pageId)
    {
        DumpSlotResource(slotNameHash, pageId, resourceId, data, size);
        const IndexedSlotFile* overrideFile = ResolveSlotOverride(slotNameHash, resourceId, pageId);
        if (!overrideFile)
        {
            LogSlotResource(slotNameHash, pageId, resourceId, size);
            return SlotResourceLoad(data, resourceId, flags, size, destination, slotNameHash, pageId);
        }

        if ((resourceId & 0x80000000U) != 0)
        {
            std::shared_ptr<SlotReplacementBuffer> existing;
            {
                const ActiveSlotKey key{ data, resourceId & 0x7fffffffU };
                std::lock_guard<std::mutex> lock(ActiveSlotOverridesMutex);
                const auto found = ActiveSlotOverrides.find(key);
                if (found != ActiveSlotOverrides.end() && !found->second.empty())
                    existing = found->second.back();
            }

            if (existing)
            {
                spdlog::info("[LOAD] -> reusing converted buffer={} size={}",
                    fmt::ptr(existing->data), existing->size);
                return SlotResourceLoad(existing->data, resourceId, flags, existing->size,
                    destination, slotNameHash, pageId);
            }
        }

        std::shared_ptr<SlotReplacementBuffer> buffer = LoadSlotReplacement(*overrideFile);
        if (!buffer)
        {
            spdlog::error("Falling back to slot data for {} (slot={}, id={:08x})",
                          overrideFile->file.path.generic_string(), overrideFile->slotName, resourceId);
            LogSlotResource(slotNameHash, pageId, resourceId, size);
            return SlotResourceLoad(data, resourceId, flags, size, destination, slotNameHash, pageId);
        }

        const int32_t result = SlotResourceLoad(buffer->data, resourceId, flags, buffer->size, destination,
                                                slotNameHash, pageId);
        {
            const ActiveSlotKey key{data, resourceId & 0x7fffffffU};
            std::lock_guard<std::mutex> lock(ActiveSlotOverridesMutex);
            ActiveSlotOverrides[key].push_back(buffer);
        }

        LogSlotOverride(*overrideFile, slotNameHash, pageId, resourceId, size, buffer->size, result);
        return result;
    }

    int64_t __fastcall SlotResourceUnloadHook(void* data, uint32_t resourceId, uint32_t flags)
    {
        std::shared_ptr<SlotReplacementBuffer> buffer;
        {
            const ActiveSlotKey key{data, resourceId & 0x7fffffffU};
            std::lock_guard<std::mutex> lock(ActiveSlotOverridesMutex);
            const auto found = ActiveSlotOverrides.find(key);
            if (found != ActiveSlotOverrides.end() && !found->second.empty())
            {
                buffer = std::move(found->second.back());
                found->second.pop_back();
                if (found->second.empty())
                    ActiveSlotOverrides.erase(found);
            }
        }

        return SlotResourceUnload(buffer ? buffer->data : data, resourceId, flags);
    }

    int32_t ReadCnfMerge(const char* path, const CnfMerge& merge, void* destination, uint64_t destinationSize,
                         uint64_t offset, uint64_t requestedSize)
    {
        if (offset > merge.size || (offset == merge.size && requestedSize != 0))
            return ArchiveReadError;

        const uint64_t bytesToRead = std::min(merge.size - offset, requestedSize);
        // The PC texture streamer requests PC_TXN_UP .data files with destinationSize == 0: the destination
        // buffer is sized from the texture record, so trust the requested size in that case.
        if (destinationSize == 0)
            destinationSize = bytesToRead;
        if (bytesToRead > destinationSize || (bytesToRead != 0 && !destination))
            return ArchiveReadError;
        if (bytesToRead == 0)
            return 0;

        auto* output = static_cast<uint8_t*>(destination);
        uint64_t outputOffset = 0;
        uint64_t segmentStart = 0;

        const auto copySegment = [&](const void* data, uint64_t size) {
            const uint64_t segmentEnd = segmentStart + size;
            const uint64_t requestEnd = offset + bytesToRead;
            if (offset < segmentEnd && requestEnd > segmentStart)
            {
                const uint64_t sourceOffset = offset > segmentStart ? offset - segmentStart : 0;
                const uint64_t destinationOffset = segmentStart > offset ? segmentStart - offset : 0;
                const uint64_t count = std::min(segmentEnd, requestEnd) - std::max(segmentStart, offset);
                std::memcpy(output + destinationOffset, static_cast<const uint8_t*>(data) + sourceOffset, count);
                outputOffset = std::max(outputOffset, destinationOffset + count);
            }
            segmentStart = segmentEnd;
        };

        const auto readFileSegment = [&](const std::filesystem::path& file, uint64_t size) -> int32_t {
            const uint64_t segmentEnd = segmentStart + size;
            const uint64_t requestEnd = offset + bytesToRead;
            if (offset < segmentEnd && requestEnd > segmentStart)
            {
                const uint64_t sourceOffset = offset > segmentStart ? offset - segmentStart : 0;
                const uint64_t destinationOffset = segmentStart > offset ? segmentStart - offset : 0;
                const uint64_t count = std::min(segmentEnd, requestEnd) - std::max(segmentStart, offset);
                const int32_t result = ReadOverride(file, size, output + destinationOffset,
                                                    bytesToRead - destinationOffset, sourceOffset, count);
                if (result != 0)
                    return result;
                outputOffset = std::max(outputOffset, destinationOffset + count);
            }
            segmentStart = segmentEnd;
            return 0;
        };

        if (!merge.baseOverride.empty())
        {
            const int32_t result = readFileSegment(merge.baseOverride, merge.baseSize);
            if (result != 0)
                return result;
        }
        else
        {
            const uint64_t segmentEnd = merge.baseSize;
            const uint64_t requestEnd = offset + bytesToRead;
            if (offset < segmentEnd && requestEnd > 0)
            {
                const uint64_t sourceOffset = offset;
                const uint64_t count = std::min(segmentEnd, requestEnd) - sourceOffset;
                const int32_t result = ArchiveRead(path, output, bytesToRead, sourceOffset, count);
                if (result != 0)
                    return result;
                outputOffset = count;
            }
            segmentStart = segmentEnd;
        }

        for (size_t index = 0; index < merge.fragments.size(); ++index)
        {
            copySegment(CnfFragmentSeparator, CnfFragmentSeparatorSize);
            const int32_t result = readFileSegment(merge.fragments[index], merge.fragmentSizes[index]);
            if (result != 0)
                return result;
        }

        return outputOffset == bytesToRead ? 0 : ArchiveReadError;
    }

    int32_t __fastcall ArchiveReadHook(const char* path, void* destination, uint64_t destinationSize,
                                       uint64_t offset, uint64_t requestedSize)
    {
        CnfMerge merge;
        if (ResolveCnfMerge(path, merge))
        {
            if (!merge.baseOverride.empty())
                LogOverride(path, merge.baseOverride);
            LogCnfMerge(path, merge);
            const int32_t result = ReadCnfMerge(path, merge, destination, destinationSize, offset, requestedSize);
            LogArchiveRead(path, "cnf-merge", merge.baseOverride.empty() ? nullptr : &merge.baseOverride, offset,
                           requestedSize, result);
            return result;
        }

        std::filesystem::path overridePath;
        uint64_t overrideSize = 0;
        if (!ResolveOverride(path, overridePath, overrideSize))
        {
            const int32_t result = ArchiveRead(path, destination, destinationSize, offset, requestedSize);
            LogArchiveRead(path, "archive", nullptr, offset, requestedSize, result);
            return result;
        }

        LogOverride(path, overridePath);
        const int32_t result = ReadOverride(overridePath, overrideSize, destination, destinationSize, offset,
                                            requestedSize);
        LogArchiveRead(path, "mod-override", &overridePath, offset, requestedSize, result);
        return result;
    }

    bool IsReadOnlyOpen(const uint8_t* options)
    {
        // The engine's byte 1 is 0 for read, 1 for write, and 2 for read/write.
        return options && options[1] == 0;
    }

    bool __fastcall GameFileOpenAHook(void* file, const char* path, const uint8_t* options)
    {
        if (IsReadOnlyOpen(options))
        {
            std::filesystem::path overridePath;
            uint64_t overrideSize = 0;
            if (ResolveOverride(path, overridePath, overrideSize))
            {
                const std::string overrideName = overridePath.string();
                LogOverride(path, overridePath);
                const bool result = GameFileOpenA(file, overrideName.c_str(), options);
                LogLooseFileRead(path, &overridePath, result);
                return result;
            }

            const bool result = GameFileOpenA(file, path, options);
            LogLooseFileRead(path, nullptr, result);
            return result;
        }

        return GameFileOpenA(file, path, options);
    }

    bool __fastcall GameFileOpenWHook(void* file, const wchar_t* path, const uint8_t* options)
    {
        if (IsReadOnlyOpen(options))
        {
            std::filesystem::path overridePath;
            uint64_t overrideSize = 0;
            if (ResolveOverride(path, overridePath, overrideSize))
            {
                LogOverride(path, overridePath);
                const bool result = GameFileOpenW(file, overridePath.c_str(), options);
                LogLooseFileRead(path, &overridePath, result);
                return result;
            }

            const bool result = GameFileOpenW(file, path, options);
            LogLooseFileRead(path, nullptr, result);
            return result;
        }

        return GameFileOpenW(file, path, options);
    }

    bool InitializeRoots()
    {
        std::wstring executablePath(32768, L'\0');
        const DWORD length = GetModuleFileNameW(GameModule, executablePath.data(), static_cast<DWORD>(executablePath.size()));
        if (length == 0 || length >= executablePath.size())
            return false;

        executablePath.resize(length);
        GameRoot = std::filesystem::path(executablePath).parent_path();

        try
        {
            std::filesystem::path configuredRoot(ActiveConfig.modsDirectory);
            ModRoot = configuredRoot.has_root_path() ? configuredRoot : GameRoot / configuredRoot;
            ModRoot = ModRoot.lexically_normal();
        }
        catch (const std::exception&)
        {
            return false;
        }

        std::error_code error;
        std::filesystem::create_directories(ModRoot, error);
        if (error)
        {
            spdlog::error("Failed to create the MGS4 mods directory {}: {}", ModRoot.generic_string(), error.message());
            return false;
        }

        return BuildModIndex();
    }

    void RemoveArchiveHooks(uint8_t* fileInfo, uint8_t* read, uint8_t* lookup)
    {
        if (lookup)
            MH_RemoveHook(lookup);
        if (read)
            MH_RemoveHook(read);
        if (fileInfo)
            MH_RemoveHook(fileInfo);
        ArchiveFileInfo = nullptr;
        ArchiveLookup = nullptr;
        ArchiveRead = nullptr;
    }

    void RemoveSlotResourceHooks(uint8_t* load, uint8_t* unload)
    {
        if (unload)
            MH_RemoveHook(unload);
        if (load)
            MH_RemoveHook(load);
        SlotResourceLoad = nullptr;
        SlotResourceUnload = nullptr;
    }

    // ---- HUD name swap ----------------------------------------------------------------------------
    // SetWidgetText(ctx, widget, text) is the universal UI text setter (every HUD / menu label goes
    // through it). When the incoming text equals HudNameMatch and the outfit/face condition holds, the
    // replacement is passed to the game instead. The label widget is remembered so the name can be
    // refreshed immediately when the player changes outfit or FaceCamo.
    constexpr char SetWidgetTextPattern[] = "48 89 4C 24 08 53 55 56 57 41 55 48 83 EC 30 33 FF 4D 8B E8 48 8B F2";
    using SetWidgetTextFn = int64_t(__fastcall*)(void* ctx, void* widget, const char* text);
    SetWidgetTextFn SetWidgetText = nullptr;
    char HudNameMatchBuffer[64] = "OLD SNAKE";
    char HudNameReplaceBuffer[64] = "BIG BOSS";
    void* HudNameCtx = nullptr;
    void* HudNameWidget = nullptr;

    bool HudNameConditionMet()
    {
        OverrideCondition cond;
        cond.outfit = ActiveConfig.hudNameOutfit;
        cond.face = ActiveConfig.hudNameFace;
        return ConditionMet(cond);
    }

    int64_t __fastcall SetWidgetTextHook(void* ctx, void* widget, const char* text)
    {
        if (text && ActiveConfig.hudNameEnabled)
        {
            if (_stricmp(text, HudNameMatchBuffer) == 0 || _stricmp(text, HudNameReplaceBuffer) == 0)
            {
                HudNameCtx = ctx;
                HudNameWidget = widget;
                if (HudNameConditionMet())
                    text = HudNameReplaceBuffer;
            }
        }
        return SetWidgetText(ctx, widget, text);
    }

    void RefreshHudName()
    {
        if (!SetWidgetText || !HudNameCtx || !HudNameWidget || !ActiveConfig.hudNameEnabled)
            return;
        // same liveness test as the game itself
        if ((*reinterpret_cast<const uint32_t*>(HudNameWidget) & 0x80000) == 0)
            return;
        SetWidgetText(HudNameCtx, HudNameWidget, HudNameConditionMet() ? HudNameReplaceBuffer : HudNameMatchBuffer);
    }

    void InstallHudNameHook(void* textBegin, uintptr_t textSize, uintptr_t gameBase)
    {
        if (!ActiveConfig.hudNameEnabled)
            return;
        strncpy(HudNameMatchBuffer, ActiveConfig.hudNameMatch.c_str(), sizeof(HudNameMatchBuffer) - 1);
        strncpy(HudNameReplaceBuffer, ActiveConfig.hudNameReplace.c_str(), sizeof(HudNameReplaceBuffer) - 1);
        uint8_t* target = Utils::PatternScanRange(textBegin, textSize, SetWidgetTextPattern);
        if (!target)
        {
            spdlog::warn("HUD name swap: UI text setter not found");
            return;
        }
        Utils::LogAddress("setWidgetText", reinterpret_cast<uintptr_t>(target), gameBase);
        if (MH_CreateHook(target, reinterpret_cast<LPVOID>(&SetWidgetTextHook), reinterpret_cast<void**>(&SetWidgetText)) != MH_OK)
        {
            spdlog::warn("HUD name swap: hook could not be created");
            SetWidgetText = nullptr;
            return;
        }
        spdlog::info("HUD name swap active: '{}' -> '{}' (outfit {}, face {})", HudNameMatchBuffer, HudNameReplaceBuffer,
                     ActiveConfig.hudNameOutfit, ActiveConfig.hudNameFace);
    }


    // ---- Custom Camouflage menu entries ---------------------------------------------------------
    // The FaceCamo list builder (sub_519a90) walks a static table of 15 face ids and writes one
    // 0xC8-byte row per face at menu+0x2C0; the Costume list builder (sub_514280) does the same with
    // a table of 5 outfit ids at menu+0x300. Both are left untouched: after they return, one extra
    // row is appended for the custom entry. Row layout (identical for both lists):
    //   +0x00 id, +0x08 name (char*), +0x20 description (char*), +0x28 thumbnail strcode,
    //   +0x2C owned, +0x30 disabled, counters at menu+0x2B8 (owned), +0x2BC/+0x268/+0x274 (rows),
    //   cursor at +0x270/+0x260/+0x264, scroll set up by sub_4d3610(menu+0x260, menu+0x264, n, min(n,10)).
    // Selecting the row makes the menu call the equip function with the custom id; the hook turns it
    // into the base id for the game and remembers the choice (VirtualOutfitActive / VirtualFaceActive),
    // which the conditional overrides and the HUD name use. Model variants, validity and thumbnails
    // are borrowed from the base id, so no page is added to any .slot file.
    constexpr char FaceListPattern[]      = "40 53 56 57 41 55 48 83 EC 28 4C 89 74 24 68 48 8D 05";
    constexpr char CostumeListPattern[]   = "48 89 5C 24 18 56 57 41 55 41 56 41 57 48 83 EC 20 45 33 FF 48 89 6C 24 50 41 8B FF 44 89 B9 F8";
    constexpr char FaceMapPattern[]       = "48 83 EC 28 48 8B 05 ? ? ? ? 0F B6 90 27 0B 00 00 44 0F B6 80 26 0B";
    constexpr char OutfitMapPattern[]     = "48 8B 05 ? ? ? ? 0F B6 90 26 0B 00 00 48 8B 81 E0 00 00 00 48 85 C0";
    constexpr char ValidityPattern[]      = "48 83 EC 48 48 8B 05 ? ? ? ? 48 85 C0 74 51 4C 8B 88 00 01 00 00";
    constexpr char OutfitValidityPattern[] = "48 83 EC 38 48 8B 05 ? ? ? ? 4C 63 C1 48 85 C0 74 48 48 8B 90 F8 00";
    constexpr char EquipPattern[]         = "40 53 57 41 55 48 83 EC 20 4C 8B 2D ? ? ? ? 8B DA 8B F9 4D 85 ED";
    constexpr char ScrollPattern[]        = "48 89 5C 24 08 48 89 7C 24 10 44 8B 11 41 8B D9 44 8B 1A 48 8B F9 45 85";
    constexpr uintptr_t PlayerVisualRva = 0x23EB5F08;   // pointer to the player's visual object

    using ListFn = int(__fastcall*)(uint8_t* menu);
    using MapFn = int(__fastcall*)(void* visual);
    using ValidityFn = int(__fastcall*)(int area, int outfit, int face);
    using OutfitValidityFn = int(__fastcall*)(int outfit);
    using EquipFn = int64_t(__fastcall*)(int outfit, int face);
    using ScrollFn = int64_t(__fastcall*)(int* cursor, int* scroll, int total, int visible);
    ListFn FaceListOrig = nullptr; ListFn CostumeListOrig = nullptr;
    MapFn FaceMapOrig = nullptr; MapFn OutfitMapOrig = nullptr;
    ValidityFn ValidityOrig = nullptr; OutfitValidityFn OutfitValidityOrig = nullptr;
    EquipFn EquipOrig = nullptr; ScrollFn ScrollList = nullptr;
    void** PlayerVisualPtr = nullptr;
    char CustomFaceName[64] = "BIG BOSS (TRUE)";
    char CustomFaceDesc[96] = "";
    char CustomOutfitName[64] = "BIG BOSS (TRUE)";
    char CustomOutfitDesc[96] = "";

    void AppendRow(uint8_t* menu, size_t rowsOffset, int n, int id, const char* name, const char* desc,
                   uint32_t thumb, bool selected)
    {
        uint8_t* row = menu + rowsOffset + n * 0xC8;
        memset(row, 0, 0x40);
        *reinterpret_cast<int32_t*>(row + 0x00) = id;
        *reinterpret_cast<const char**>(row + 0x08) = name;
        *reinterpret_cast<const char**>(row + 0x20) = desc;
        *reinterpret_cast<uint32_t*>(row + 0x28) = thumb;
        *reinterpret_cast<int32_t*>(row + 0x2C) = 1;   // owned
        *reinterpret_cast<int32_t*>(row + 0x30) = 0;   // enabled
        const int total = n + 1;
        *reinterpret_cast<int32_t*>(menu + 0x2B8) += 1;
        *reinterpret_cast<int32_t*>(menu + 0x268) = total;
        *reinterpret_cast<int32_t*>(menu + 0x274) = total;
        *reinterpret_cast<int32_t*>(menu + (rowsOffset == 0x300 ? 0x2FC : 0x2BC)) = total;
        if (selected)
        {
            *reinterpret_cast<int32_t*>(menu + 0x270) = n;
            if (*reinterpret_cast<int32_t*>(menu + 0x260) < 0)
            {
                *reinterpret_cast<int32_t*>(menu + 0x260) = n;
                *reinterpret_cast<int32_t*>(menu + 0x264) = 0;
            }
        }
        if (ScrollList)
            ScrollList(reinterpret_cast<int*>(menu + 0x260), reinterpret_cast<int*>(menu + 0x264), total, total < 10 ? total : 10);
    }

    uint32_t RowThumb(const uint8_t* rows, int n, int id)
    {
        for (int i = 0; i < n; ++i)
            if (*reinterpret_cast<const int32_t*>(rows + i * 0xC8) == id)
                return *reinterpret_cast<const uint32_t*>(rows + i * 0xC8 + 0x28);
        return 0;
    }

    int __fastcall FaceListHook(uint8_t* menu)
    {
        const int n = FaceListOrig(menu);
        if (!ActiveConfig.customFaceEnabled || !menu || n < 0 || n > 18)
            return n;
        const uint32_t thumb = ActiveConfig.customFaceThumb ? ActiveConfig.customFaceThumb
                                                            : RowThumb(menu + 0x2C0, n, ActiveConfig.customFaceBaseId);
        AppendRow(menu, 0x2C0, n, ActiveConfig.customFaceId, CustomFaceName, CustomFaceDesc, thumb, VirtualFaceActive);
        return n + 1;
    }

    int __fastcall CostumeListHook(uint8_t* menu)
    {
        const int n = CostumeListOrig(menu);
        if (!ActiveConfig.customOutfitEnabled || !menu || n < 0 || n > 8)
            return n;
        const uint32_t thumb = ActiveConfig.customOutfitThumb ? ActiveConfig.customOutfitThumb
                                                              : RowThumb(menu + 0x300, n, ActiveConfig.customOutfitBaseId);
        AppendRow(menu, 0x300, n, ActiveConfig.customOutfitId, CustomOutfitName, CustomOutfitDesc, thumb, VirtualOutfitActive);
        return n + 1;
    }

    // The menu resolves model variants / validity / thumbnails with the custom id: present the base id instead.
    int __fastcall FaceMapHook(void* visual)
    {
        uint8_t* ctx = (PlayerContextPtr && *PlayerContextPtr) ? *PlayerContextPtr : nullptr;
        if (ActiveConfig.customFaceEnabled && ctx && ctx[PlayerFaceOffset] == ActiveConfig.customFaceId)
        {
            ctx[PlayerFaceOffset] = static_cast<uint8_t>(ActiveConfig.customFaceBaseId);
            const int r = FaceMapOrig(visual);
            ctx[PlayerFaceOffset] = static_cast<uint8_t>(ActiveConfig.customFaceId);
            return r;
        }
        return FaceMapOrig(visual);
    }

    int __fastcall OutfitMapHook(void* visual)
    {
        uint8_t* ctx = (PlayerContextPtr && *PlayerContextPtr) ? *PlayerContextPtr : nullptr;
        if (ActiveConfig.customOutfitEnabled && ctx && ctx[PlayerOutfitOffset] == ActiveConfig.customOutfitId)
        {
            ctx[PlayerOutfitOffset] = static_cast<uint8_t>(ActiveConfig.customOutfitBaseId);
            const int r = OutfitMapOrig(visual);
            ctx[PlayerOutfitOffset] = static_cast<uint8_t>(ActiveConfig.customOutfitId);
            return r;
        }
        return OutfitMapOrig(visual);
    }

    int __fastcall ValidityHook(int area, int outfit, int face)
    {
        if (ActiveConfig.customFaceEnabled && face == ActiveConfig.customFaceId)
            face = ActiveConfig.customFaceBaseId;
        if (ActiveConfig.customOutfitEnabled && outfit == ActiveConfig.customOutfitId)
            outfit = ActiveConfig.customOutfitBaseId;
        return ValidityOrig(area, outfit, face);
    }

    int __fastcall OutfitValidityHook(int outfit)
    {
        if (ActiveConfig.customOutfitEnabled && outfit == ActiveConfig.customOutfitId)
            outfit = ActiveConfig.customOutfitBaseId;
        return OutfitValidityOrig(outfit);
    }

    // Equip(outfit, face): the game only ever receives base ids. A custom id turns the virtual flag on;
    // the base id sent again while the other parameter changes keeps it; anything else turns it off.
    int64_t __fastcall EquipHook(int outfit, int face)
    {
        const bool wasOutfit = VirtualOutfitActive, wasFace = VirtualFaceActive;
        const uint8_t* ctx = (PlayerContextPtr && *PlayerContextPtr) ? *PlayerContextPtr : nullptr;
        const int ctxOutfit = ctx ? ctx[PlayerOutfitOffset] : -1;
        const int ctxFace = ctx ? ctx[PlayerFaceOffset] : -1;
        const bool outfitChanged = outfit != ctxOutfit, faceChanged = face != ctxFace;
        const bool unchanged = !outfitChanged && !faceChanged;
        if (ActiveConfig.customOutfitEnabled)
        {
            if (outfit == ActiveConfig.customOutfitId) { VirtualOutfitActive = true; outfit = ActiveConfig.customOutfitBaseId; }
            else if (!(outfit == ActiveConfig.customOutfitBaseId && VirtualOutfitActive && (faceChanged || unchanged)))
                VirtualOutfitActive = false;
        }
        if (ActiveConfig.customFaceEnabled)
        {
            if (face == ActiveConfig.customFaceId) { VirtualFaceActive = true; face = ActiveConfig.customFaceBaseId; }
            else if (!(face == ActiveConfig.customFaceBaseId && VirtualFaceActive && (outfitChanged || unchanged)))
                VirtualFaceActive = false;
        }
        if ((wasOutfit != VirtualOutfitActive || wasFace != VirtualFaceActive) && PlayerVisualPtr && *PlayerVisualPtr)
        {
            // same base ids before and after: force the body/head pages to reload so the overrides flip
            uint8_t* visual = static_cast<uint8_t*>(*PlayerVisualPtr);
            *reinterpret_cast<int32_t*>(visual + 0xC4) = -1;
            *reinterpret_cast<int32_t*>(visual + 0xC8) = -1;
        }
        spdlog::debug("Equip outfit={} face={} virtualOutfit={} virtualFace={}", outfit, face, VirtualOutfitActive, VirtualFaceActive);
        const int64_t r = EquipOrig(outfit, face);
        RefreshHudName();
        return r;
    }

    void InstallCustomEntryHooks(void* textBegin, uintptr_t textSize, uintptr_t gameBase)
    {
        if (!ActiveConfig.customFaceEnabled && !ActiveConfig.customOutfitEnabled)
            return;
        VirtualFaceId = ActiveConfig.customFaceEnabled ? ActiveConfig.customFaceId : -1;
        VirtualOutfitId = ActiveConfig.customOutfitEnabled ? ActiveConfig.customOutfitId : -1;
        strncpy(CustomFaceName, ActiveConfig.customFaceName.c_str(), sizeof(CustomFaceName) - 1);
        strncpy(CustomFaceDesc, ActiveConfig.customFaceDesc.c_str(), sizeof(CustomFaceDesc) - 1);
        strncpy(CustomOutfitName, ActiveConfig.customOutfitName.c_str(), sizeof(CustomOutfitName) - 1);
        strncpy(CustomOutfitDesc, ActiveConfig.customOutfitDesc.c_str(), sizeof(CustomOutfitDesc) - 1);
        PlayerVisualPtr = reinterpret_cast<void**>(gameBase + PlayerVisualRva);
        struct Site { const char* name; const char* pattern; void* hook; void** original; bool needed; };
        const Site sites[] = {
            {"faceList", FaceListPattern, reinterpret_cast<void*>(&FaceListHook), reinterpret_cast<void**>(&FaceListOrig), ActiveConfig.customFaceEnabled},
            {"faceMap", FaceMapPattern, reinterpret_cast<void*>(&FaceMapHook), reinterpret_cast<void**>(&FaceMapOrig), ActiveConfig.customFaceEnabled},
            {"costumeList", CostumeListPattern, reinterpret_cast<void*>(&CostumeListHook), reinterpret_cast<void**>(&CostumeListOrig), ActiveConfig.customOutfitEnabled},
            {"outfitMap", OutfitMapPattern, reinterpret_cast<void*>(&OutfitMapHook), reinterpret_cast<void**>(&OutfitMapOrig), ActiveConfig.customOutfitEnabled},
            {"outfitValidity", OutfitValidityPattern, reinterpret_cast<void*>(&OutfitValidityHook), reinterpret_cast<void**>(&OutfitValidityOrig), ActiveConfig.customOutfitEnabled},
            {"validity", ValidityPattern, reinterpret_cast<void*>(&ValidityHook), reinterpret_cast<void**>(&ValidityOrig), true},
            {"equip", EquipPattern, reinterpret_cast<void*>(&EquipHook), reinterpret_cast<void**>(&EquipOrig), true},
        };
        uint8_t* scroll = Utils::PatternScanRange(textBegin, textSize, ScrollPattern);
        if (!scroll)
        {
            spdlog::warn("Custom menu entries: list scroll helper not found");
            return;
        }
        ScrollList = reinterpret_cast<ScrollFn>(scroll);
        for (const Site& site : sites)
        {
            if (!site.needed)
                continue;
            uint8_t* target = Utils::PatternScanRange(textBegin, textSize, site.pattern);
            if (!target)
            {
                spdlog::warn("Custom menu entries: '{}' not found", site.name);
                return;
            }
            Utils::LogAddress(site.name, reinterpret_cast<uintptr_t>(target), gameBase);
            if (MH_CreateHook(target, site.hook, site.original) != MH_OK)
            {
                spdlog::warn("Custom menu entries: hook '{}' could not be created", site.name);
                return;
            }
        }
        if (ActiveConfig.customFaceEnabled)
            spdlog::info("Custom FaceCamo entry: id {} (base {}) '{}'", ActiveConfig.customFaceId, ActiveConfig.customFaceBaseId, CustomFaceName);
        if (ActiveConfig.customOutfitEnabled)
            spdlog::info("Custom costume entry: id {} (base {}) '{}'", ActiveConfig.customOutfitId, ActiveConfig.customOutfitBaseId, CustomOutfitName);
    }

} // namespace

bool MGS4ModLoader_Install(HMODULE gameModule, void* textBegin, uintptr_t textSize,
                          const ModLoaderConfig& config)
{
    GameModule = gameModule;
    ActiveConfig = config;

    if (!InitializeRoots())
    {
        spdlog::error("Failed to initialize the MGS4 mod loader paths");
        return false;
    }

    constexpr char ArchiveFileInfoPattern[] = "40 53 48 83 EC 20 48 8B DA E8 72 00 00 00 48 85 C0 74 12 48 8B 08 48 85 C0 48 89 0B 0F 95 C0";
    constexpr char ArchiveReadPattern[] = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 49 8B D9 49 8B F8 48 8B F2 E8 D3 F7 FF FF 48 8B D0";
    constexpr char ArchiveLookupPattern[] = "48 89 5C 24 18 48 89 74 24 20 41 55 41 56 41 57 48 83 EC 40 45 33 F6";
    constexpr char GameFileOpenPattern[] = "48 89 5C 24 08 57 48 83 EC 40 45 0F B6 48 01 48 8B FA 48 8B D9 45 85 C9 74 16 41 83 F9 01";
    constexpr char SlotResourceLoadPattern[] = "48 83 EC 48 8B 84 24 ? ? ? ? C6 44 24 38 00 89 44 24 30 8B 44 24 78 89 44 24 28 8B 44 24 70 89 44 24 20 E8 ? ? ? ? 48 83 C4 48 C3";
    constexpr char SlotResourceUnloadPattern[] = "40 53 41 8B C0 45 8B D8 44 8B CA 48 8B D9 41 0F BA E9 1F 24 02 41 8B C0 44 0F 44 CA 45 8B D1 41 0F BA F2 1F 25 00 80 00 00 45 0F 44 D1 45 8B C2 41 81 E0 FF FF FF 7F 74 ? 48 8B 05";

    uint8_t* archiveFileInfo = Utils::PatternScanRange(textBegin, textSize, ArchiveFileInfoPattern);
    uint8_t* archiveRead = Utils::PatternScanRange(textBegin, textSize, ArchiveReadPattern);
    uint8_t* archiveLookup = Utils::PatternScanRange(textBegin, textSize, ArchiveLookupPattern);
    if (!archiveFileInfo || !archiveRead || !archiveLookup)
    {
        spdlog::error("Failed to locate the MGS4 path-based PAK API");
        return false;
    }

    const uintptr_t gameBase = reinterpret_cast<uintptr_t>(GameModule);
    Utils::LogAddress("archiveFileInfo", reinterpret_cast<uintptr_t>(archiveFileInfo), gameBase);
    Utils::LogAddress("archiveRead", reinterpret_cast<uintptr_t>(archiveRead), gameBase);
    Utils::LogAddress("archiveLookup", reinterpret_cast<uintptr_t>(archiveLookup), gameBase);

    if (MH_CreateHook(archiveFileInfo, reinterpret_cast<LPVOID>(&ArchiveFileInfoHook),
                      reinterpret_cast<void**>(&ArchiveFileInfo)) != MH_OK)
    {
        return false;
    }

    if (MH_CreateHook(archiveRead, reinterpret_cast<LPVOID>(&ArchiveReadHook),
                      reinterpret_cast<void**>(&ArchiveRead)) != MH_OK)
    {
        RemoveArchiveHooks(archiveFileInfo, nullptr, nullptr);
        return false;
    }

    if (MH_CreateHook(archiveLookup, reinterpret_cast<LPVOID>(&ArchiveLookupHook),
                      reinterpret_cast<void**>(&ArchiveLookup)) != MH_OK)
    {
        RemoveArchiveHooks(archiveFileInfo, archiveRead, nullptr);
        return false;
    }

    uint8_t* slotResourceLoad = Utils::PatternScanRange(textBegin, textSize, SlotResourceLoadPattern);
    uint8_t* slotResourceUnload = Utils::PatternScanRange(textBegin, textSize, SlotResourceUnloadPattern);
    if (slotResourceLoad && slotResourceUnload)
    {
        Utils::LogAddress("slotResourceLoad", reinterpret_cast<uintptr_t>(slotResourceLoad), gameBase);
        Utils::LogAddress("slotResourceUnload", reinterpret_cast<uintptr_t>(slotResourceUnload), gameBase);

        if (MH_CreateHook(slotResourceLoad, reinterpret_cast<LPVOID>(&SlotResourceLoadHook),
                          reinterpret_cast<void**>(&SlotResourceLoad)) != MH_OK ||
            MH_CreateHook(slotResourceUnload, reinterpret_cast<LPVOID>(&SlotResourceUnloadHook),
                          reinterpret_cast<void**>(&SlotResourceUnload)) != MH_OK)
        {
            RemoveSlotResourceHooks(SlotResourceLoad ? slotResourceLoad : nullptr,
                                    SlotResourceUnload ? slotResourceUnload : nullptr);
            spdlog::warn("Slot resource registration hooks were not installed");
        }
    }
    else
    {
        spdlog::warn("Slot resource registration functions were not found");
    }

    uint8_t* gameFileOpenA = Utils::PatternScanRange(textBegin, textSize, GameFileOpenPattern);
    uint8_t* gameFileOpenW = Utils::PatternScanRange(textBegin, textSize, GameFileOpenPattern, 1);
    if (gameFileOpenA && gameFileOpenW && gameFileOpenW == gameFileOpenA + 0xF0)
    {
        Utils::LogAddress("gameFileOpenA", reinterpret_cast<uintptr_t>(gameFileOpenA), gameBase);
        Utils::LogAddress("gameFileOpenW", reinterpret_cast<uintptr_t>(gameFileOpenW), gameBase);

        if (MH_CreateHook(gameFileOpenA, reinterpret_cast<LPVOID>(&GameFileOpenAHook),
                          reinterpret_cast<void**>(&GameFileOpenA)) != MH_OK ||
            MH_CreateHook(gameFileOpenW, reinterpret_cast<LPVOID>(&GameFileOpenWHook),
                          reinterpret_cast<void**>(&GameFileOpenW)) != MH_OK)
        {
            if (GameFileOpenA)
                MH_RemoveHook(gameFileOpenA);
            if (GameFileOpenW)
                MH_RemoveHook(gameFileOpenW);
            GameFileOpenA = nullptr;
            GameFileOpenW = nullptr;
            spdlog::warn("Loose-file wrappers were not hooked");
        }
    }
    else
    {
        spdlog::warn("Loose-file wrappers were not found");
    }

    ParseConditions(gameBase);
    InstallHudNameHook(textBegin, textSize, gameBase);
    InstallCustomEntryHooks(textBegin, textSize, gameBase);

    spdlog::info("MGS4 mod loader installed: {}", ModRoot.generic_string());
    return true;
}
