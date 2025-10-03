// RegistryFacade.h
#pragma once

#include "RegistryHelpers.h" // contains RegistryKey, RegValueRecord, helpers
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <mutex>
#include <memory>

namespace core::registry
{

class RegistryFacade
{
public:
    struct ListOptions {
        size_t maxItems = 0;
        size_t offset = 0;
        bool includeSecurityInfo = false;
        bool forceRefresh = false;
        ListOptions() = default;
    };

    struct GetValueOptions {
        std::wstring defaultValue;
        bool cacheResult = true;
        std::chrono::seconds cacheTTL = std::chrono::seconds(60);
        GetValueOptions() = default;
    };

    struct CacheConfig {
        std::chrono::seconds keyCacheTTL = std::chrono::seconds(30);
        std::chrono::seconds valueCacheTTL = std::chrono::seconds(60);
        size_t maxCacheSize = 1000;
        bool enabled = true;
    };

    struct WatchConfig {
        bool enabled = false;
        DWORD notifyFilter = REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET;
        bool watchSubtree = false;
        std::chrono::milliseconds pollInterval = std::chrono::milliseconds(1000);
    };

    RegistryFacade() noexcept;
    explicit RegistryFacade(CacheConfig cacheConfig) noexcept;
    ~RegistryFacade() noexcept;

    RegistryFacade(const RegistryFacade&) = delete;
    RegistryFacade& operator=(const RegistryFacade&) = delete;

    RegistryFacade(RegistryFacade&& other) noexcept;
    RegistryFacade& operator=(RegistryFacade&& other) noexcept;

    std::vector<std::wstring> ListSubKeys(HKEY root,
                                         std::wstring const& subKeyPath,
                                         REGSAM sam,
                                         ListOptions options);

    std::vector<RegValueRecord> ListValues(HKEY root,
                                          std::wstring const& subKeyPath,
                                          REGSAM sam,
                                          ListOptions options);

    std::wstring GetStringValue(HKEY root,
                               std::wstring const& subKeyPath,
                               std::wstring const& valueName,
                               REGSAM sam,
                               const GetValueOptions &options ) const;

    DWORD GetDwordValue(HKEY root,
                       std::wstring const& subKeyPath,
                       std::wstring const& valueName,
                       REGSAM sam,
                       GetValueOptions options);

    unsigned long long GetQwordValue(HKEY root,
                                    std::wstring const& subKeyPath,
                                    std::wstring const& valueName,
                                    REGSAM sam,
                                    GetValueOptions options);

    std::vector<unsigned char> GetBinaryValue(HKEY root,
                                             std::wstring const& subKeyPath,
                                             std::wstring const& valueName,
                                             REGSAM sam,
                                             GetValueOptions options);

    void SetStringValue(HKEY root,
                       std::wstring const& subKeyPath,
                       std::wstring const& valueName,
                       std::wstring const& data,
                       DWORD regType = REG_SZ,
                       REGSAM sam = KEY_WRITE);

    void SetDwordValue(HKEY root,
                      std::wstring const& subKeyPath,
                      std::wstring const& valueName,
                      DWORD data,
                      REGSAM sam = KEY_WRITE);

    void SetQwordValue(HKEY root,
                      std::wstring const& subKeyPath,
                      std::wstring const& valueName,
                      unsigned long long data,
                      REGSAM sam = KEY_WRITE);

    void SetBinaryValue(HKEY root,
                       std::wstring const& subKeyPath,
                       std::wstring const& valueName,
                       std::vector<unsigned char> const& data,
                       REGSAM sam = KEY_WRITE);

    void CreateKey(HKEY root,
                  std::wstring const& subKeyPath,
                  REGSAM sam = KEY_READ | KEY_WRITE);

    bool KeyExists(HKEY root,
                  std::wstring const& subKeyPath,
                  REGSAM sam = KEY_READ);

    void DeleteValue(HKEY root,
                    std::wstring const& subKeyPath,
                    std::wstring const& valueName,
                    REGSAM sam = KEY_WRITE);

    void DeleteKey(HKEY root,
                  std::wstring const& subKeyPath,
                  REGSAM sam = 0);

    bool CopyKey(HKEY sourceRoot,
                std::wstring const& sourcePath,
                HKEY targetRoot,
                std::wstring const& targetPath,
                REGSAM sam = KEY_READ | KEY_WRITE);

    bool MoveKey(HKEY sourceRoot,
                std::wstring const& sourcePath,
                HKEY targetRoot,
                std::wstring const& targetPath,
                REGSAM sam = KEY_READ | KEY_WRITE);

    std::wstring GetKeyInfo(HKEY root,
                           std::wstring const& subKeyPath,
                           REGSAM sam = KEY_READ);

    void ClearCache() const;
    void ClearKeyCache() const;
    void ClearValueCache() const;
    size_t GetCacheSize() const;
    CacheConfig GetCacheConfig() const;
    void SetCacheConfig(CacheConfig config);

    struct PerformanceStats {
        size_t cacheHits = 0;
        size_t cacheMisses = 0;
        size_t keysOpened = 0;
        size_t valuesRead = 0;
        size_t valuesWritten = 0;
        std::chrono::milliseconds totalOperationTime{0};
    };

    PerformanceStats GetStats() const;
    void ResetStats() const;

private:
    struct CachedKey {
        HKEY root;
        std::wstring subKeyPath;
        REGSAM sam;
        RegistryKey key;
        std::chrono::steady_clock::time_point lastAccess;
        std::chrono::steady_clock::time_point expiryTime;
        size_t accessCount = 0;
    };

    struct CachedValue {
        HKEY root;
        std::wstring subKeyPath;
        std::wstring valueName;
        REGSAM sam;
        std::vector<unsigned char> data;
        DWORD type = 0;
        std::chrono::steady_clock::time_point lastAccess;
        std::chrono::steady_clock::time_point expiryTime;
        size_t accessCount = 0;
    };

    CacheConfig m_cacheConfig;
    mutable std::mutex m_configMutex;

    mutable std::vector<CachedKey> m_keyCache;
    mutable std::vector<CachedValue> m_valueCache;
    mutable std::mutex m_cacheMutex;

    mutable PerformanceStats m_stats;
    mutable std::mutex m_statsMutex;

    RegistryKey OpenKeyInternal(HKEY root,
                               std::wstring const& subKeyPath,
                               REGSAM sam,
                               bool createIfMissing,
                               bool forceRefresh = false) const;

    std::optional<CachedKey> FindCachedKey(HKEY root,
                                          const std::wstring& subKeyPath,
                                          REGSAM sam) const;

    void CacheKey(HKEY root,
                 const std::wstring& subKeyPath,
                 REGSAM sam,
                 RegistryKey key) const;

    std::optional<CachedValue> FindCachedValue(HKEY root,
                                              const std::wstring& subKeyPath,
                                              const std::wstring& valueName,
                                              REGSAM sam) const;

    void CacheValue(HKEY root,
                   const std::wstring& subKeyPath,
                   const std::wstring& valueName,
                   REGSAM sam,
                   const std::vector<unsigned char>& data,
                   DWORD type) const;

    void InvalidateKeyCache(HKEY root, const std::wstring& subKeyPath) const;
    void InvalidateValueCache(HKEY root, const std::wstring& subKeyPath, const std::wstring& valueName = L"");

    void CleanupExpiredCache() const;
    void EnforceCacheSizeLimits() const;

    static void ValidateRootKey(HKEY root);

    static void ValidateSamDesired(REGSAM sam, bool forWrite);

    void RecordCacheHit(bool hit) const;
    void RecordKeyOpened() const;
    void RecordValueRead() const;
    void RecordValueWritten() const;
    void RecordOperationTime(std::chrono::milliseconds duration) const;

    static RegistryKey OpenKeyUncached(HKEY root,
                                       std::wstring const& subKeyPath,
                                       REGSAM sam,
                                       bool createIfMissing);
};

} // namespace core::registry