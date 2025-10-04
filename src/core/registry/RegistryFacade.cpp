// RegistryFacade.cpp
#include "RegistryFacade.h"
#include <algorithm>
#include <iterator>
#include <sstream>
#include <iomanip>
#include <thread>

namespace core::registry
{

// Константы
constexpr size_t MAX_CACHE_CLEANUP_ITERATIONS = 1000;
constexpr auto DEFAULT_CLEANUP_INTERVAL = std::chrono::seconds(30);

namespace {

    std::wstring SamToString(REGSAM sam) {
        std::wstringstream ss;
        if (sam & KEY_READ) ss << L"READ ";
        if (sam & KEY_WRITE) ss << L"WRITE ";
        if (sam & KEY_EXECUTE) ss << L"EXECUTE ";
        if (sam & KEY_ALL_ACCESS) ss << L"ALL_ACCESS ";
        if (sam & KEY_QUERY_VALUE) ss << L"QUERY_VALUE ";
        if (sam & KEY_SET_VALUE) ss << L"SET_VALUE ";
        if (sam & KEY_CREATE_SUB_KEY) ss << L"CREATE_SUB_KEY ";
        if (sam & KEY_ENUMERATE_SUB_KEYS) ss << L"ENUMERATE_SUB_KEYS ";
        if (sam & KEY_NOTIFY) ss << L"NOTIFY ";
        if (sam & KEY_CREATE_LINK) ss << L"CREATE_LINK ";
        return ss.str();
    }

    std::wstring HkeyToString(HKEY hkey) {
        if (hkey == HKEY_CLASSES_ROOT) return L"HKEY_CLASSES_ROOT";
        if (hkey == HKEY_CURRENT_USER) return L"HKEY_CURRENT_USER";
        if (hkey == HKEY_LOCAL_MACHINE) return L"HKEY_LOCAL_MACHINE";
        if (hkey == HKEY_USERS) return L"HKEY_USERS";
        if (hkey == HKEY_CURRENT_CONFIG) return L"HKEY_CURRENT_CONFIG";
        if (hkey == HKEY_PERFORMANCE_DATA) return L"HKEY_PERFORMANCE_DATA";
        return L"HKEY_UNKNOWN";
    }

} // anonymous namespace

// Конструкторы и деструкторы
RegistryFacade::RegistryFacade() noexcept
    : m_cacheConfig{}
{
}

RegistryFacade::RegistryFacade(CacheConfig cacheConfig) noexcept
    : m_cacheConfig(cacheConfig)
{
}

RegistryFacade::~RegistryFacade() noexcept
{
    ClearCache();
}

RegistryFacade::RegistryFacade(RegistryFacade&& other) noexcept
{
    std::lock_guard lock1(m_configMutex);
    std::lock_guard lock2(m_cacheMutex);
    std::lock_guard lock3(m_statsMutex);

    m_cacheConfig = other.m_cacheConfig;
    m_keyCache = std::move(other.m_keyCache);
    m_valueCache = std::move(other.m_valueCache);
    m_stats = other.m_stats;
}

RegistryFacade& RegistryFacade::operator=(RegistryFacade&& other) noexcept
{
    if (this != &other) {
        std::lock(m_configMutex, m_cacheMutex, m_statsMutex);
        std::lock_guard lock1(m_configMutex, std::adopt_lock);
        std::lock_guard lock2(m_cacheMutex, std::adopt_lock);
        std::lock_guard lock3(m_statsMutex, std::adopt_lock);

        ClearCache();

        m_cacheConfig = other.m_cacheConfig;
        m_keyCache = std::move(other.m_keyCache);
        m_valueCache = std::move(other.m_valueCache);
        m_stats = other.m_stats;
    }
    return *this;
}

void RegistryFacade::ValidateRootKey(HKEY root)
{
    static const HKEY validRoots[] = {
        HKEY_CLASSES_ROOT, HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE,
        HKEY_USERS, HKEY_CURRENT_CONFIG
    };

    bool isValid = false;
    for (HKEY validRoot : validRoots) {
        if (root == validRoot) {
            isValid = true;
            break;
        }
    }

    // Также допустимы уже открытые ключи
    if (!isValid && root != nullptr && root != INVALID_HANDLE_VALUE) {
        isValid = true;
    }

    if (!isValid) {
        throw RegException(ERROR_INVALID_HANDLE,
                          "Invalid root key specified: " +
                          std::to_string(reinterpret_cast<uintptr_t>(root)));
    }
}

void RegistryFacade::ValidateSamDesired(REGSAM sam, bool forWrite)
{
    if (forWrite && !(sam & KEY_WRITE)) {
        throw RegException(ERROR_ACCESS_DENIED,
                          "Write operation requires KEY_WRITE access");
    }

    if (sam == 0) {
        throw RegException(ERROR_INVALID_PARAMETER, "REGSAM cannot be zero");
    }
}

void RegistryFacade::ClearCache() const
{
    std::lock_guard lock(m_cacheMutex);
    m_keyCache.clear();
    m_valueCache.clear();
}

void RegistryFacade::ClearKeyCache() const
{
    std::lock_guard lock(m_cacheMutex);
    m_keyCache.clear();
}

void RegistryFacade::ClearValueCache() const
{
    std::lock_guard lock(m_cacheMutex);
    m_valueCache.clear();
}

size_t RegistryFacade::GetCacheSize() const
{
    std::lock_guard lock(m_cacheMutex);
    return m_keyCache.size() + m_valueCache.size();
}

RegistryFacade::CacheConfig RegistryFacade::GetCacheConfig() const
{
    std::lock_guard lock(m_configMutex);
    return m_cacheConfig;
}

void RegistryFacade::SetCacheConfig(CacheConfig config)
{
    std::lock_guard lock(m_configMutex);
    m_cacheConfig = config;

    // Применяем новые ограничения размера
    std::lock_guard cacheLock(m_cacheMutex);
    EnforceCacheSizeLimits();
}

// Статистика
RegistryFacade::PerformanceStats RegistryFacade::GetStats() const
{
    std::lock_guard lock(m_statsMutex);
    return m_stats;
}

void RegistryFacade::ResetStats() const
{
    std::lock_guard lock(m_statsMutex);
    m_stats = PerformanceStats{};
}

void RegistryFacade::RecordCacheHit(bool hit) const
{
    std::lock_guard lock(m_statsMutex);
    if (hit)
    {
        m_stats.cacheHits++;
    } else
    {
        m_stats.cacheMisses++;
    }
}

void RegistryFacade::RecordKeyOpened() const
{
    std::lock_guard lock(m_statsMutex);
    m_stats.keysOpened++;
}

void RegistryFacade::RecordValueRead() const
{
    std::lock_guard lock(m_statsMutex);
    m_stats.valuesRead++;
}

void RegistryFacade::RecordValueWritten() const
{
    std::lock_guard lock(m_statsMutex);
    m_stats.valuesWritten++;
}

void RegistryFacade::RecordOperationTime(std::chrono::milliseconds duration) const
{
    std::lock_guard lock(m_statsMutex);
    m_stats.totalOperationTime += duration;
}

std::optional<RegistryFacade::CachedKey> RegistryFacade::FindCachedKey(HKEY root, const std::wstring& subKeyPath, REGSAM sam) const
{
    if (!m_cacheConfig.enabled) {
        return std::nullopt;
    }

    std::lock_guard lock(m_cacheMutex);
    CleanupExpiredCache();

    const auto now = std::chrono::steady_clock::now();
    const auto it = std::ranges::find_if(m_keyCache,
                                         [&](const CachedKey& cached) {
                                             return cached.root == root &&
                                                    cached.subKeyPath == subKeyPath &&
                                                    cached.sam == sam &&
                                                    cached.expiryTime > now;
                                         });

    if (it != m_keyCache.end()) {
        it->lastAccess = now;
        it->accessCount++;
        std::optional cached = std::move(*it);
        return cached;
    }

    return std::nullopt;
}

void RegistryFacade::CacheKey(HKEY root, const std::wstring& subKeyPath, REGSAM sam, RegistryKey key) const
{
    if (!m_cacheConfig.enabled || subKeyPath.empty() || (sam & KEY_WRITE)) {
        return;
    }

    std::lock_guard lock(m_cacheMutex);

    std::erase_if(m_keyCache,
                  [&](const CachedKey& cached) {
                      return cached.root == root &&
                             cached.subKeyPath == subKeyPath &&
                             cached.sam == sam;
                  });

    const auto now = std::chrono::steady_clock::now();
    CachedKey newEntry{
        root, subKeyPath, sam, std::move(key),
        now, now + m_cacheConfig.keyCacheTTL, 1
    };

    m_keyCache.push_back(std::move(newEntry));
    EnforceCacheSizeLimits();
}

std::optional<RegistryFacade::CachedValue> RegistryFacade::FindCachedValue(HKEY root, const std::wstring& subKeyPath,
                               const std::wstring& valueName, REGSAM sam) const
{
    if (!m_cacheConfig.enabled) {
        return std::nullopt;
    }

    std::lock_guard lock(m_cacheMutex);
    CleanupExpiredCache();

    auto now = std::chrono::steady_clock::now();
    auto it = std::ranges::find_if(m_valueCache,
                                   [&](const CachedValue& cached) {
                                       return cached.root == root &&
                                              cached.subKeyPath == subKeyPath &&
                                              cached.valueName == valueName &&
                                              cached.sam == sam &&
                                              cached.expiryTime > now;
                                   });

    if (it != m_valueCache.end()) {
        it->lastAccess = now;
        it->accessCount++;
        return *it;
    }

    return std::nullopt;
}

void RegistryFacade::CacheValue(HKEY root, const std::wstring& subKeyPath,
                               const std::wstring& valueName, REGSAM sam,
                               const std::vector<unsigned char>& data, const DWORD type) const
{
    if (!m_cacheConfig.enabled) {
        return;
    }

    std::lock_guard lock(m_cacheMutex);

    m_valueCache.erase(std::ranges::remove_if(m_valueCache,
                                              [&](const CachedValue& cached) {
                                                  return cached.root == root &&
                                                         cached.subKeyPath == subKeyPath &&
                                                         cached.valueName == valueName &&
                                                         cached.sam == sam;
                                              }).begin(), m_valueCache.end());

    const auto now = std::chrono::steady_clock::now();
    CachedValue newEntry{
        root, subKeyPath, valueName, sam, data, type,
        now, now + m_cacheConfig.valueCacheTTL, 1
    };

    m_valueCache.push_back(std::move(newEntry));
    EnforceCacheSizeLimits();
}

void RegistryFacade::InvalidateKeyCache(HKEY root, const std::wstring& subKeyPath) const
{
    std::lock_guard lock(m_cacheMutex);

    m_keyCache.erase(std::ranges::remove_if(m_keyCache,
                                            [&](const CachedKey& cached) {
                                                return cached.root == root &&
                                                       (subKeyPath.empty() || cached.subKeyPath == subKeyPath);
                                            }).begin(), m_keyCache.end());
}

void RegistryFacade::InvalidateValueCache(HKEY root, const std::wstring& subKeyPath, const std::wstring& valueName)
{
    std::lock_guard lock(m_cacheMutex);

    std::erase_if(m_valueCache,
                  [&](const CachedValue& cached) {
                      const bool matchesRoot = cached.root == root;
                      const bool matchesPath = subKeyPath.empty() || cached.subKeyPath == subKeyPath;
                      const bool matchesValue = valueName.empty() || cached.valueName == valueName;
                      return matchesRoot && matchesPath && matchesValue;
                  });
}

void RegistryFacade::CleanupExpiredCache() const
{
    const auto now = std::chrono::steady_clock::now();

    std::erase_if(m_keyCache,
                  [&](const CachedKey& cached) {
                      return cached.expiryTime <= now;
                  });

    std::erase_if(m_valueCache,
                  [&](const CachedValue& cached) {
                      return cached.expiryTime <= now;
                  });
}

void RegistryFacade::EnforceCacheSizeLimits() const
{
    const size_t totalSize = m_keyCache.size() + m_valueCache.size();
    if (totalSize <= m_cacheConfig.maxCacheSize)
    {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    struct CacheItem
    {
        enum Type { Key, Value };

        Type type;
        std::chrono::steady_clock::time_point lastAccess;
        size_t index;
    };

    std::vector<CacheItem> allItems;

    for (size_t i = 0; i < m_keyCache.size(); ++i)
    {
        allItems.push_back({CacheItem::Key, m_keyCache[i].lastAccess, i});
    }

    for (size_t i = 0; i < m_valueCache.size(); ++i)
    {
        allItems.push_back({CacheItem::Value, m_valueCache[i].lastAccess, i});
    }

    std::ranges::sort(allItems,
                      [](const CacheItem &a, const CacheItem &b)
                      {
                          return a.lastAccess < b.lastAccess;
                      });

    const size_t itemsToRemove = totalSize - m_cacheConfig.maxCacheSize;
    for (size_t i = 0; i < itemsToRemove && i < allItems.size(); ++i)
    {
        const auto &item = allItems[i];
        if (item.type == CacheItem::Key)
        {
            if (item.index < m_keyCache.size())
            {
                m_keyCache[item.index] = std::move(m_keyCache.back());
                m_keyCache.pop_back();
            }
        } else
        {
            if (item.index < m_valueCache.size())
            {
                m_valueCache[item.index] = std::move(m_valueCache.back());
                m_valueCache.pop_back();
            }
        }
    }
}

RegistryKey RegistryFacade::OpenKeyInternal(HKEY root,
                                           std::wstring const& subKeyPath,
                                           const REGSAM sam,
                                           const bool createIfMissing,
                                           const bool forceRefresh) const
{
    auto startTime = std::chrono::steady_clock::now();

    ValidateRootKey(root);
    ValidateSamDesired(sam, createIfMissing);

    if (!forceRefresh && m_cacheConfig.enabled && !createIfMissing)
    {
        std::optional<CachedKey> cached = FindCachedKey(root, subKeyPath, sam);
        if (cached)
        {
            RecordCacheHit(true);
            RecordKeyOpened();
            RecordOperationTime(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime));
            return std::move(cached->key);
        }
    }

    RecordCacheHit(false);

    RegistryKey key = OpenKeyUncached(root, subKeyPath, sam, createIfMissing);

    if (m_cacheConfig.enabled && !createIfMissing && !subKeyPath.empty() && !(sam & KEY_WRITE))
    {
        CacheKey(root, subKeyPath, sam, std::move(key));
    }

    RecordKeyOpened();
    RecordOperationTime(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime));

    return key;
}

RegistryKey RegistryFacade::OpenKeyUncached(HKEY root,
                                            std::wstring const& subKeyPath,
                                            const REGSAM sam,
                                            const bool createIfMissing)
{
    if (subKeyPath.empty())
    {
        return RegistryKey(root, false);
    }

    if (createIfMissing)
    {
        try
        {
            return RegistryKey::Open(root, subKeyPath, sam);
        }
        catch (const RegException &)
        {
            return RegistryKey::Create(root, subKeyPath, sam);
        }
    }

    return RegistryKey::Open(root, subKeyPath, sam);
}

std::vector<std::wstring> RegistryFacade::ListSubKeys(HKEY root, std::wstring const& subKeyPath, REGSAM sam, ListOptions options) const
{
    auto startTime = std::chrono::steady_clock::now();

    RegistryKey key = OpenKeyInternal(root, subKeyPath, sam, false, options.forceRefresh);
    std::vector<std::wstring> result = EnumerateSubKeys(key);

    if (options.offset > 0 || options.maxItems > 0) {
        if (options.offset >= result.size()) {
            result.clear();
        } else {
            auto startIt = result.begin();
            std::advance(startIt, static_cast<std::ptrdiff_t>(options.offset));

            std::vector<std::wstring>::iterator endIt;
            if (options.maxItems > 0)
            {
                size_t remaining = 0;
                if (result.size() > options.offset)
                {
                    remaining = result.size() - options.offset;
                }
                size_t take = (options.maxItems < remaining ? options.maxItems : remaining);
                endIt = startIt;
                std::advance(endIt, static_cast<std::ptrdiff_t>(take));
            }
            else
            {
                endIt = result.end();
            }
        }
    }

    RecordOperationTime(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime));

    return result;
}

    std::vector<RegValueRecord> RegistryFacade::ListValues(HKEY root, std::wstring const& subKeyPath, REGSAM sam, ListOptions options) const
{
    const auto startTime = std::chrono::steady_clock::now();

    RegistryKey key = OpenKeyInternal(root, subKeyPath, sam, false, options.forceRefresh);
    std::vector<RegValueRecord> result = EnumerateValues(key);

    if (options.offset > 0 || options.maxItems > 0)
    {
        const auto diff_offset = static_cast<std::ptrdiff_t>(options.offset);
        const auto diff_size = static_cast<std::ptrdiff_t>(result.size());

        if (diff_offset >= diff_size)
        {
            result.clear();
        }
        else
        {
            const auto startIt = result.begin() + diff_offset;

            if (options.maxItems > 0)
            {
                const auto diff_maxItems = static_cast<std::ptrdiff_t>(options.maxItems);
                const auto itemsToTake = std::min(diff_maxItems, diff_size - diff_offset);
                const auto endIt = startIt + itemsToTake;
                result = std::vector(startIt, endIt);
            }
            else
            {
                result = std::vector(startIt, result.end());
            }
        }
    }

    RecordOperationTime(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime));

    return result;
}

// Чтение значений с кэшированием
std::wstring RegistryFacade::GetStringValue(HKEY root,
    std::wstring const& subKeyPath,
    std::wstring const& valueName, const REGSAM sam,
    const GetValueOptions &options) const
{
    const auto startTime = std::chrono::steady_clock::now();

    if (options.cacheResult && m_cacheConfig.enabled)
    {
        const std::optional<CachedValue> cached = FindCachedValue(root, subKeyPath, valueName, sam);
        if (cached && cached->type == REG_SZ)
        {
            RecordCacheHit(true);
            RecordValueRead();
            RecordOperationTime(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime));

            const auto *strData = reinterpret_cast<const wchar_t *>(cached->data.data());
            size_t len = (cached->data.size() / sizeof(wchar_t)) - 1; // Без null-terminator
            return {strData, len};
        }
    }

    RecordCacheHit(false);

    const RegistryKey key = OpenKeyInternal(root, subKeyPath, sam, false);
    std::wstring result = ReadStringValue(key, valueName);

    if (options.cacheResult && m_cacheConfig.enabled && !result.empty())
    {
        std::vector<unsigned char> data((result.size() + 1) * sizeof(wchar_t));
        memcpy(data.data(), result.c_str(), data.size());
        CacheValue(root, subKeyPath, valueName, sam, data, REG_SZ);
    }

    RecordValueRead();
    RecordOperationTime(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime));

    return result;
}
} // namespace core::registry