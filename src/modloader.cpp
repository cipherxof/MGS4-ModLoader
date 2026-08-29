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

    ArchiveFileInfoDelegate ArchiveFileInfo = nullptr;
    ArchiveLookupDelegate ArchiveLookup = nullptr;
    ArchiveReadDelegate ArchiveRead = nullptr;
    GameFileOpenADelegate GameFileOpenA = nullptr;
    GameFileOpenWDelegate GameFileOpenW = nullptr;

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

    std::unordered_map<std::wstring, IndexedModFile> OverrideIndex;
    std::unordered_map<std::wstring, std::vector<IndexedModFile>> CnfFragmentIndex;
    std::vector<std::filesystem::path> ModPackages;
    std::mutex LoggedOverridesMutex;
    std::unordered_set<std::string> LoggedOverrides;

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
        spdlog::info("Indexed {} files from {} mod packages in {} ms ({} overrides, {} CNF fragments, {} conflicts)",
                     indexedFileCount, ModPackages.size(), elapsed.count(), OverrideIndex.size(), fragmentCount,
                     conflictCount);
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
            return ReadCnfMerge(path, merge, destination, destinationSize, offset, requestedSize);
        }

        std::filesystem::path overridePath;
        uint64_t overrideSize = 0;
        if (!ResolveOverride(path, overridePath, overrideSize))
            return ArchiveRead(path, destination, destinationSize, offset, requestedSize);

        LogOverride(path, overridePath);
        return ReadOverride(overridePath, overrideSize, destination, destinationSize, offset, requestedSize);
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
                return GameFileOpenA(file, overrideName.c_str(), options);
            }
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
                return GameFileOpenW(file, overridePath.c_str(), options);
            }
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
