#include "modloader.h"

#include "MinHook.h"
#include "utils.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
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

    struct SlotReplacementKey
    {
        uint32_t slotNameHash = 0;
        uint32_t resourceId = 0;
        std::wstring path;

        bool operator==(const SlotReplacementKey& other) const
        {
            return slotNameHash == other.slotNameHash && resourceId == other.resourceId && path == other.path;
        }
    };

    struct SlotReplacementKeyHash
    {
        size_t operator()(const SlotReplacementKey& key) const
        {
            size_t hash = std::hash<std::wstring>{}(key.path);
            hash ^= static_cast<size_t>(key.slotNameHash) + 0x9e3779b9U + (hash << 6) + (hash >> 2);
            hash ^= static_cast<size_t>(key.resourceId) + 0x9e3779b9U + (hash << 6) + (hash >> 2);
            return hash;
        }
    };

    std::unordered_map<std::wstring, IndexedModFile> OverrideIndex;
    std::unordered_map<std::wstring, std::vector<IndexedModFile>> CnfFragmentIndex;
    std::unordered_map<uint64_t, IndexedSlotFile> ExactSlotOverrideIndex;
    std::unordered_map<uint64_t, IndexedSlotFile> HashedSlotOverrideIndex;
    std::unordered_map<uint32_t, std::string> IndexedSlotNames;
    std::unordered_map<ActiveSlotKey, std::vector<std::shared_ptr<SlotReplacementBuffer>>, ActiveSlotKeyHash>
        ActiveSlotOverrides;
    std::vector<std::filesystem::path> ModPackages;
    std::mutex LoggedOverridesMutex;
    std::unordered_set<std::string> LoggedOverrides;
    std::mutex ActiveSlotOverridesMutex;
    std::unordered_map<SlotReplacementKey, std::shared_ptr<SlotReplacementBuffer>, SlotReplacementKeyHash>
        SlotReplacementCache;
    std::mutex SlotReplacementCacheMutex;

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

        if (components.size() != 3)
        {
            spdlog::warn("Ignoring slot override with invalid path (expected slots/<slot>/<file>): {}",
                         relative.generic_string());
            return true;
        }

        std::string slotName;
        std::string memberName;
        if (!Utils::ToLowerAscii(components[1].native(), slotName) ||
            !Utils::ToLowerAscii(components[2].filename().native(), memberName))
        {
            spdlog::warn("Ignoring non-ASCII slot override path: {}", relative.generic_string());
            return true;
        }

        const uint32_t slotNameHash = Utils::GV_StrCode(slotName);
        const std::wstring memberStem = components[2].stem().native();
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
        IndexedSlotFile slotFile{indexedFile, slotName, memberName, memberId, exactId};
        auto& index = exactId ? ExactSlotOverrideIndex : HashedSlotOverrideIndex;
        const uint64_t key = MakeSlotKey(slotNameHash, memberId);
        if (index.find(key) != index.end())
            ++conflictCount;
        index[key] = std::move(slotFile);
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
                ModPackages.push_back(entry.path());
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

    bool ResolveOverride(const std::filesystem::path& source, std::filesystem::path& overridePath, uint64_t& size)
    {
        std::filesystem::path relative;
        if (!MakeRelativeGamePath(source, relative))
            return false;

        const auto found = OverrideIndex.find(MakePathKey(relative));
        if (found == OverrideIndex.end())
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

    const IndexedSlotFile* ResolveSlotOverride(uint32_t slotNameHash, uint32_t resourceId)
    {
        const IndexedSlotFile* result = nullptr;
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

    std::shared_ptr<SlotReplacementBuffer> LoadSlotReplacement(const IndexedSlotFile& overrideFile,
                                                              uint32_t slotNameHash, uint32_t resourceId)
    {
        // The engine hands the same resource to SlotResourceLoad more than once - once plain and
        // then again with bit 31 set - and it expects that resource to map to the same buffer on
        // every pass. Allocating a fresh block per call makes the later passes fail, so keep one
        // buffer per (slot, resource, file) and hand the same pointer back.
        const SlotReplacementKey cacheKey{slotNameHash, resourceId & 0x7fffffffU,
                                          overrideFile.file.path.native()};
        {
            std::lock_guard<std::mutex> lock(SlotReplacementCacheMutex);
            const auto cached = SlotReplacementCache.find(cacheKey);
            if (cached != SlotReplacementCache.end())
                return cached->second;
        }

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

            std::lock_guard<std::mutex> lock(SlotReplacementCacheMutex);
            auto& cached = SlotReplacementCache[cacheKey];
            if (!cached)
                cached = buffer;
            return cached;
        }
        catch (const std::exception& exception)
        {
            spdlog::error("Failed to prepare slot override {}: {}", overrideFile.file.path.generic_string(),
                          exception.what());
            return {};
        }
    }

    int32_t __fastcall SlotResourceLoadHook(void* data, uint32_t resourceId, uint32_t flags, uint32_t size,
                                            uint32_t destination, uint32_t slotNameHash, uint32_t pageId)
    {
        const IndexedSlotFile* overrideFile = ResolveSlotOverride(slotNameHash, resourceId);
        if (!overrideFile)
        {
            LogSlotResource(slotNameHash, pageId, resourceId, size);
            return SlotResourceLoad(data, resourceId, flags, size, destination, slotNameHash, pageId);
        }

        std::shared_ptr<SlotReplacementBuffer> buffer =
            LoadSlotReplacement(*overrideFile, slotNameHash, resourceId);
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

    spdlog::info("MGS4 mod loader installed: {}", ModRoot.generic_string());
    return true;
}
