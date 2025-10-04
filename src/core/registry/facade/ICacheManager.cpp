// CacheManager.cpp
#include "ICacheManager.h"
#include <algorithm>
#include <mutex>
#include <thread>

namespace core::registry
{

CacheManagerImpl::CacheManagerImpl(const CacheConfig &config)
    : m_config(config) {
}

CacheConfig CacheManagerImpl::GetCacheConfig() const
{
    std::shared_lock lock(m_configMutex);
    return m_config;
}

void CacheManagerImpl::SetCacheConfig(const CacheConfig& config)
{
    std::unique_lock configLock(m_configMutex);
    m_config = config;

    EnforceSizeLimits();
}

void CacheManagerImpl::ClearCache()
{
    ClearKeyCache();
    ClearValueCache();
}

void CacheManagerImpl::ClearKeyCache()
{
    std::unique_lock lock(m_keyCacheMutex);
    m_keyCacheMap.clear();
    m_keyCacheLRU.clear();
}

void CacheManagerImpl::ClearValueCache()
{
    std::unique_lock lock(m_valueCacheMutex);
    m_valueCacheMap.clear();
    m_valueCacheLRU.clear();
}

size_t CacheManagerImpl::GetCacheSize() const
{
    std::shared_lock keyLock(m_keyCacheMutex, std::defer_lock);
    std::shared_lock valueLock(m_valueCacheMutex, std::defer_lock);
    std::lock(keyLock, valueLock);

    return m_keyCacheMap.size() + m_valueCacheMap.size();
}

std::shared_ptr<RegistryKey> CacheManagerImpl::TryGetCachedKey(HKEY root,
                                                               const std::wstring& subKeyPath,
                                                               const REGSAM sam)
{
    if (!m_config.enabled)
    {
        return nullptr;
    }

    CleanupExpiredEntries();

    const KeyCacheKey key{root, subKeyPath, sam};

    std::unique_lock lock(m_keyCacheMutex);
    const auto mapIt = m_keyCacheMap.find(key);

    if (mapIt == m_keyCacheMap.end())
    {
        return nullptr;
    }

    const std::_List_iterator<KeyLRUNode> lruIt = mapIt->second;
    const std::shared_ptr<CachedKey> &cachedKey = lruIt->data;

    if (cachedKey->IsExpired())
    {
        RemoveKeyFromCache(key);
        return nullptr;
    }

    cachedKey->lastAccess = std::chrono::steady_clock::now();
    cachedKey->accessCount++;
    TouchKeyLRU(lruIt);

    return cachedKey->key;
}

void CacheManagerImpl::CacheKey(HKEY root,
                                const std::wstring& subKeyPath,
                                const REGSAM sam,
                                const std::shared_ptr<RegistryKey> registryKey) {
    if (!m_config.enabled || subKeyPath.empty() || (sam & KEY_WRITE)) {
        return;
    }

    if (!registryKey || !registryKey->IsValid()) {
        return;
    }

    const KeyCacheKey key{root, subKeyPath, sam};
    const std::chrono::time_point<std::chrono::steady_clock> now = std::chrono::steady_clock::now();

    const auto cachedKey = std::make_shared<CachedKey>();
    cachedKey->root = root;
    cachedKey->subKeyPath = subKeyPath;
    cachedKey->sam = sam;
    cachedKey->key = registryKey;
    cachedKey->lastAccess = now;
    cachedKey->expiryTime = now + m_config.keyCacheTTL;
    cachedKey->accessCount = 1;

    std::unique_lock lock(m_keyCacheMutex);

    RemoveKeyFromCache(key);

    m_keyCacheLRU.push_front(KeyLRUNode{key, cachedKey});
    m_keyCacheMap[key] = m_keyCacheLRU.begin();

    EvictKeysIfNeeded();
}

std::optional<CachedValue> CacheManagerImpl::TryGetCachedValue(HKEY root,
                                                              const std::wstring& subKeyPath,
                                                              const std::wstring& valueName,
                                                              const REGSAM sam)
{
    if (!m_config.enabled)
    {
        return std::nullopt;
    }

    CleanupExpiredEntries();

    const ValueCacheKey key{root, subKeyPath, valueName, sam};

    std::unique_lock lock(m_valueCacheMutex);
    const auto mapIt = m_valueCacheMap.find(key);

    if (mapIt == m_valueCacheMap.end())
    {
        return std::nullopt; // Cache miss
    }

    const auto lruIt = mapIt->second;
    const std::shared_ptr<CachedValue> &cachedValue = lruIt->data;

    if (cachedValue->IsExpired())
    {
        RemoveValueFromCache(key);
        return std::nullopt;
    }

    cachedValue->lastAccess = std::chrono::steady_clock::now();
    cachedValue->accessCount++;
    TouchValueLRU(lruIt);

    return *cachedValue;
}

void CacheManagerImpl::CacheValue(HKEY root,
                                  const std::wstring& subKeyPath,
                                  const std::wstring& valueName,
                                  const REGSAM sam,
                                  const std::vector<unsigned char>& data,
                                  const DWORD type)
{
    if (!m_config.enabled)
    {
        return;
    }

    const ValueCacheKey key{root, subKeyPath, valueName, sam};
    const std::chrono::time_point<std::chrono::steady_clock> now = std::chrono::steady_clock::now();

    const std::shared_ptr<CachedValue> cachedValue = std::make_shared<CachedValue>();
    cachedValue->root = root;
    cachedValue->subKeyPath = subKeyPath;
    cachedValue->valueName = valueName;
    cachedValue->sam = sam;
    cachedValue->data = data; // Copy the data
    cachedValue->type = type;
    cachedValue->lastAccess = now;
    cachedValue->expiryTime = now + m_config.valueCacheTTL;
    cachedValue->accessCount = 1;

    std::unique_lock lock(m_valueCacheMutex);

    RemoveValueFromCache(key);

    m_valueCacheLRU.push_front(ValueLRUNode{key, cachedValue});
    m_valueCacheMap[key] = m_valueCacheLRU.begin();

    EvictValuesIfNeeded();
}

void CacheManagerImpl::InvalidateKey(HKEY root,
                                     const std::wstring& subKeyPath)
{
    std::unique_lock lock(m_keyCacheMutex);

    if (subKeyPath.empty())
    {
        for (auto it = m_keyCacheMap.begin(); it != m_keyCacheMap.end();)
        {
            if (it->first.root == root)
            {
                m_keyCacheLRU.erase(it->second);
                it = m_keyCacheMap.erase(it);
            }
            else
            {
                ++it;
            }
        }
    } else
    {
        for (auto it = m_keyCacheMap.begin(); it != m_keyCacheMap.end();)
        {
            if (it->first.root == root && it->first.subKeyPath == subKeyPath)
            {
                m_keyCacheLRU.erase(it->second);
                it = m_keyCacheMap.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}

void CacheManagerImpl::InvalidateValue(HKEY root,
                                       const std::wstring& subKeyPath,
                                       const std::wstring& valueName) {
    std::unique_lock lock(m_valueCacheMutex);

    for (auto it = m_valueCacheMap.begin(); it != m_valueCacheMap.end(); ) {
        const ValueCacheKey &cacheKey = it->first;
        const bool matchesRoot = cacheKey.root == root;
        const bool matchesPath = subKeyPath.empty() || cacheKey.subKeyPath == subKeyPath;
        const bool matchesValue = valueName.empty() || cacheKey.valueName == valueName;

        if (matchesRoot && matchesPath && matchesValue) {
            m_valueCacheLRU.erase(it->second);
            it = m_valueCacheMap.erase(it);
        } else {
            ++it;
        }
    }
}

void CacheManagerImpl::CleanupExpiredEntries()
{
    {
        std::unique_lock lock(m_keyCacheMutex);
        for (auto it = m_keyCacheMap.begin(); it != m_keyCacheMap.end();)
        {
            if (it->second->data->IsExpired())
            {
                m_keyCacheLRU.erase(it->second);
                it = m_keyCacheMap.erase(it);
            } else
            {
                ++it;
            }
        }
    }

    {
        std::unique_lock lock(m_valueCacheMutex);
        for (auto it = m_valueCacheMap.begin(); it != m_valueCacheMap.end();)
        {
            if (it->second->data->IsExpired())
            {
                m_valueCacheLRU.erase(it->second);
                it = m_valueCacheMap.erase(it);
            } else
            {
                ++it;
            }
        }
    }
}

void CacheManagerImpl::EnforceSizeLimits()
{
    CacheConfig config; {
        std::shared_lock lock(m_configMutex);
        config = m_config;
    }

    if (!config.enabled)
    {
        return;
    }

    const size_t totalCurrentSize = GetCacheSize();
    if (totalCurrentSize <= config.maxCacheSize)
    {
        return;
    }

    const size_t targetTotalSize = config.maxCacheSize;

    std::shared_lock keyLock(m_keyCacheMutex, std::defer_lock);
    std::shared_lock valueLock(m_valueCacheMutex, std::defer_lock);
    std::lock(keyLock, valueLock);

    const size_t currentKeySize = m_keyCacheMap.size();
    const size_t currentValueSize = m_valueCacheMap.size();

    size_t targetKeySize, targetValueSize;
    if (currentKeySize > currentValueSize)
    {
        targetKeySize = std::min(currentKeySize, targetTotalSize * 2 / 3);
        targetValueSize = targetTotalSize - targetKeySize;
    }
    else
    {
        targetValueSize = std::min(currentValueSize, targetTotalSize * 2 / 3);
        targetKeySize = targetTotalSize - targetValueSize;
    }

    keyLock.unlock();
    valueLock.unlock();

    if (currentKeySize > targetKeySize)
    {
        std::unique_lock lock(m_keyCacheMutex);
        while (m_keyCacheMap.size() > targetKeySize && !m_keyCacheLRU.empty())
        {
            auto &lruKey = m_keyCacheLRU.back().key;
            RemoveKeyFromCache(lruKey);
        }
    }

    if (currentValueSize > targetValueSize)
    {
        std::unique_lock lock(m_valueCacheMutex);
        while (m_valueCacheMap.size() > targetValueSize && !m_valueCacheLRU.empty())
        {
            auto &lruKey = m_valueCacheLRU.back().key;
            RemoveValueFromCache(lruKey);
        }
    }
}

void CacheManagerImpl::RemoveKeyFromCache(const KeyCacheKey& key)
{
    const auto mapIt = m_keyCacheMap.find(key);
    if (mapIt != m_keyCacheMap.end())
    {
        m_keyCacheLRU.erase(mapIt->second);
        m_keyCacheMap.erase(mapIt);
    }
}

void CacheManagerImpl::RemoveValueFromCache(const ValueCacheKey& key)
{
    const auto mapIt = m_valueCacheMap.find(key);
    if (mapIt != m_valueCacheMap.end())
    {
        m_valueCacheLRU.erase(mapIt->second);
        m_valueCacheMap.erase(mapIt);
    }
}

void CacheManagerImpl::TouchKeyLRU(std::list<KeyLRUNode>::iterator it) {
    m_keyCacheLRU.splice(m_keyCacheLRU.begin(), m_keyCacheLRU, it);
}

void CacheManagerImpl::TouchValueLRU(std::list<ValueLRUNode>::iterator it) {
    m_valueCacheLRU.splice(m_valueCacheLRU.begin(), m_valueCacheLRU, it);
}

void CacheManagerImpl::EvictKeysIfNeeded()
{
    CacheConfig config; {
        std::shared_lock lock(m_configMutex);
        config = m_config;
    }

    if (!config.enabled)
    {
        return;
    }

    const size_t maxKeys = config.maxCacheSize / 2;

    std::unique_lock lock(m_keyCacheMutex);
    while (m_keyCacheMap.size() > maxKeys && !m_keyCacheLRU.empty())
    {
        auto &lruKey = m_keyCacheLRU.back().key;
        RemoveKeyFromCache(lruKey);
    }
}

void CacheManagerImpl::EvictValuesIfNeeded()
{
    CacheConfig config; {
        std::shared_lock lock(m_configMutex);
        config = m_config;
    }

    if (!config.enabled)
    {
        return;
    }

    const size_t maxValues = config.maxCacheSize / 2;

    std::unique_lock lock(m_valueCacheMutex);
    while (m_valueCacheMap.size() > maxValues && !m_valueCacheLRU.empty())
    {
        auto &lruKey = m_valueCacheLRU.back().key;
        RemoveValueFromCache(lruKey);
    }
}
} // namespace core::registry