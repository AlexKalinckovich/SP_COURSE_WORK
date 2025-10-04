// IKeyManager.h
#pragma once

#include <string>
#include <vector>
#include <memory>

#include "registry/RegistryHelpers.h"

namespace core::registry
{

struct KeyInfo
{
    std::wstring path;
    DWORD lastWriteTime;
    DWORD subKeyCount;
    DWORD valueCount;
    std::wstring className;
};

struct ListOptions
{
    size_t maxItems = 0;
    size_t offset = 0;
    bool includeSecurityInfo = false;
    bool forceRefresh = false;
    ListOptions() = default;
};

class IKeyManager
{
public:
    virtual ~IKeyManager() = default;

    virtual RegistryKey OpenKey(HKEY root,
                               const std::wstring& subKeyPath,
                               REGSAM sam,
                               bool createIfMissing) = 0;

    virtual bool KeyExists(HKEY root,
                          const std::wstring& subKeyPath,
                          REGSAM sam) const = 0;

    virtual KeyInfo GetKeyInfo(HKEY root,
                              const std::wstring& subKeyPath,
                              REGSAM sam) const = 0;

    virtual std::vector<std::wstring> ListSubKeys(HKEY root,
                                                 const std::wstring& subKeyPath,
                                                 REGSAM sam,
                                                 ListOptions options) const = 0;

    virtual void CreateKey(HKEY root,
                          const std::wstring& subKeyPath,
                          REGSAM sam) = 0;

    virtual void DeleteKey(HKEY root,
                          const std::wstring& subKeyPath,
                          REGSAM sam) = 0;

    virtual bool CopyKey(HKEY sourceRoot,
                        const std::wstring& sourcePath,
                        HKEY targetRoot,
                        const std::wstring& targetPath,
                        REGSAM sam) = 0;

    virtual bool MoveKey(HKEY sourceRoot,
                        const std::wstring& sourcePath,
                        HKEY targetRoot,
                        const std::wstring& targetPath,
                        REGSAM sam) = 0;

    virtual void ValidateRootKey(HKEY root) const = 0;
    virtual void ValidateSamDesired(REGSAM sam, bool forWrite) const = 0;
};

class KeyManagerImpl final : public IKeyManager
{
public:
    KeyManagerImpl() = default;

    RegistryKey OpenKey(HKEY root,
                       const std::wstring& subKeyPath,
                       REGSAM sam,
                       bool createIfMissing) override;

    bool KeyExists(HKEY root,
                  const std::wstring& subKeyPath,
                  REGSAM sam) const override;

    KeyInfo GetKeyInfo(HKEY root,
                      const std::wstring& subKeyPath,
                      REGSAM sam) const override;

    std::vector<std::wstring> ListSubKeys(HKEY root,
                                         const std::wstring& subKeyPath,
                                         REGSAM sam,
                                         ListOptions options) const override;
    void CreateKey(HKEY root,
                  const std::wstring& subKeyPath,
                  REGSAM sam) override;

    void DeleteKey(HKEY root,
                  const std::wstring& subKeyPath,
                  REGSAM sam) override;

    bool CopyKey(HKEY sourceRoot,
                const std::wstring& sourcePath,
                HKEY targetRoot,
                const std::wstring& targetPath,
                REGSAM sam) override;

    bool MoveKey(HKEY sourceRoot,
                const std::wstring& sourcePath,
                HKEY targetRoot,
                const std::wstring& targetPath,
                REGSAM sam) override;

    void ValidateRootKey(HKEY root) const override;
    void ValidateSamDesired(REGSAM sam, bool forWrite) const override;

private:
    static RegistryKey OpenKeyUncached(HKEY root,
                                       const std::wstring& subKeyPath,
                                       REGSAM sam,
                                       bool createIfMissing);
};

} // namespace core::registry