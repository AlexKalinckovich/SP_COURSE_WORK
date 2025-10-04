// ICacheManager.h
#pragma once

#include "registry/RegistryHelpers.h"
#include <memory>
#include <chrono>
#include <shared_mutex>
#include <unordered_map>
#include <list>
#include <optional>

namespace core::registry
{

struct CacheConfig {
    std::chrono::seconds keyCacheTTL = std::chrono::seconds(30);
    std::chrono::seconds valueCacheTTL = std::chrono::seconds(60);
    size_t maxCacheSize = 1000;
    bool enabled = true;
};

struct CachedKey
{
    HKEY root;
    std::wstring subKeyPath;
    REGSAM sam;
    std::shared_ptr<RegistryKey> key;
    std::chrono::steady_clock::time_point lastAccess;
    std::chrono::steady_clock::time_point expiryTime;
    size_t accessCount = 0;

    [[nodiscard]] bool IsExpired() const
    {
        return std::chrono::steady_clock::now() > expiryTime;
    }
};

struct CachedValue
{
    HKEY root;
    std::wstring subKeyPath;
    std::wstring valueName;
    REGSAM sam;
    std::vector<unsigned char> data;
    DWORD type = 0;
    std::chrono::steady_clock::time_point lastAccess;
    std::chrono::steady_clock::time_point expiryTime;
    size_t accessCount = 0;

    [[nodiscard]] bool IsExpired() const
    {
        return std::chrono::steady_clock::now() > expiryTime;
    }
};

class ICacheManager {
public:
    virtual ~ICacheManager() = default;

    [[nodiscard]] virtual CacheConfig GetCacheConfig() const = 0;
    virtual void SetCacheConfig(const CacheConfig& config) = 0;

    virtual void ClearCache() = 0;
    virtual void ClearKeyCache() = 0;
    virtual void ClearValueCache() = 0;
    [[nodiscard]] virtual size_t GetCacheSize() const = 0;

    virtual std::shared_ptr<RegistryKey> TryGetCachedKey(HKEY root,
                                                        const std::wstring& subKeyPath,
                                                        REGSAM sam) = 0;

    virtual void CacheKey(HKEY root,
                         const std::wstring& subKeyPath,
                         REGSAM sam,
                         std::shared_ptr<RegistryKey> key) = 0;

    virtual std::optional<CachedValue> TryGetCachedValue(HKEY root,
                                                        const std::wstring& subKeyPath,
                                                        const std::wstring& valueName,
                                                        REGSAM sam) = 0;

    virtual void CacheValue(HKEY root,
                           const std::wstring& subKeyPath,
                           const std::wstring& valueName,
                           REGSAM sam,
                           const std::vector<unsigned char>& data,
                           DWORD type) = 0;

    virtual void InvalidateKey(HKEY root,
                              const std::wstring& subKeyPath) = 0;

    virtual void InvalidateValue(HKEY root,
                                const std::wstring& subKeyPath,
                                const std::wstring& valueName) = 0;

    virtual void CleanupExpiredEntries() = 0;
    virtual void EnforceSizeLimits() = 0;
};

class CacheManagerImpl final : public ICacheManager {
public:
    explicit CacheManagerImpl(const CacheConfig &config = CacheConfig{});
    ~CacheManagerImpl() override = default;

    CacheConfig GetCacheConfig() const override;
    void SetCacheConfig(const CacheConfig& config) override;

    void ClearCache() override;
    void ClearKeyCache() override;
    void ClearValueCache() override;
    size_t GetCacheSize() const override;

    std::shared_ptr<RegistryKey> TryGetCachedKey(HKEY root,
                                                const std::wstring& subKeyPath,
                                                REGSAM sam) override;

    void CacheKey(HKEY root,
                 const std::wstring& subKeyPath,
                 REGSAM sam,
                 std::shared_ptr<RegistryKey> registryKey) override;

    std::optional<CachedValue> TryGetCachedValue(HKEY root,
                                                const std::wstring& subKeyPath,
                                                const std::wstring& valueName,
                                                REGSAM sam) override;

    void CacheValue(HKEY root,
                   const std::wstring& subKeyPath,
                   const std::wstring& valueName,
                   REGSAM sam,
                   const std::vector<unsigned char>& data,
                   DWORD type) override;

    void InvalidateKey(HKEY root,
                      const std::wstring& subKeyPath ) override;

    void InvalidateValue(HKEY root,
                        const std::wstring& subKeyPath,
                        const std::wstring& valueName ) override;

    void CleanupExpiredEntries() override;
    void EnforceSizeLimits() override;

private:
    struct KeyCacheKey
    {
        HKEY root;
        std::wstring subKeyPath;
        REGSAM sam;

        bool operator==(const KeyCacheKey& other) const {
            return root == other.root &&
                   subKeyPath == other.subKeyPath &&
                   sam == other.sam;
        }

        struct Hash {
            size_t operator()(const KeyCacheKey& k) const {
                return std::hash<void*>{}(k.root) ^
                       std::hash<std::wstring>{}(k.subKeyPath) ^
                       std::hash<REGSAM>{}(k.sam);
            }
        };
    };

    struct ValueCacheKey
    {
        HKEY root;
        std::wstring subKeyPath;
        std::wstring valueName;
        REGSAM sam;

        bool operator==(const ValueCacheKey& other) const {
            return root == other.root &&
                   subKeyPath == other.subKeyPath &&
                   valueName == other.valueName &&
                   sam == other.sam;
        }

        struct Hash {
            size_t operator()(const ValueCacheKey& k) const {
                return std::hash<void*>{}(k.root) ^
                       std::hash<std::wstring>{}(k.subKeyPath) ^
                       std::hash<std::wstring>{}(k.valueName) ^
                       std::hash<REGSAM>{}(k.sam);
            }
        };
    };

    struct KeyLRUNode
    {
        KeyCacheKey key;
        std::shared_ptr<CachedKey> data;
    };

    struct ValueLRUNode
    {
        ValueCacheKey key;
        std::shared_ptr<CachedValue> data;
    };

    mutable std::shared_mutex m_configMutex;
    CacheConfig m_config;

    std::unordered_map<KeyCacheKey,std::list<KeyLRUNode>::iterator,KeyCacheKey::Hash> m_keyCacheMap;
    std::list<KeyLRUNode> m_keyCacheLRU;
    mutable std::shared_mutex m_keyCacheMutex;

    std::unordered_map<ValueCacheKey,std::list<ValueLRUNode>::iterator,ValueCacheKey::Hash> m_valueCacheMap;
    std::list<ValueLRUNode> m_valueCacheLRU;
    mutable std::shared_mutex m_valueCacheMutex;

    void RemoveKeyFromCache(const KeyCacheKey& key);
    void RemoveValueFromCache(const ValueCacheKey& key);
    void TouchKeyLRU(std::list<KeyLRUNode>::iterator it);
    void TouchValueLRU(std::list<ValueLRUNode>::iterator it);
    void EvictKeysIfNeeded();
    void EvictValuesIfNeeded();
};

} // namespace core::registry