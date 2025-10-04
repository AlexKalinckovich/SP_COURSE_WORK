// IValueManager.h
#pragma once
#include "IKeyManager.h"
#include "registry/RegistryHelpers.h"
#include <chrono>
#include <string>
#include <vector>
#include <memory>


namespace core::registry
{

struct GetValueOptions {
    std::wstring defaultValue;
    bool cacheResult = true;
    std::chrono::seconds cacheTTL = std::chrono::seconds(60);
    GetValueOptions() = default;
};

struct ValueInfo {
    std::wstring name;
    DWORD type;
    std::vector<unsigned char> data;
    [[nodiscard]] size_t size() const { return data.size(); }
};

class IValueManager {
public:
    virtual ~IValueManager() = default;

    virtual std::wstring GetStringValue(HKEY root,
                                       const std::wstring& subKeyPath,
                                       const std::wstring& valueName,
                                       REGSAM sam,
                                       const GetValueOptions& options) = 0;

    virtual DWORD GetDwordValue(HKEY root,
                               const std::wstring& subKeyPath,
                               const std::wstring& valueName,
                               REGSAM sam,
                               GetValueOptions options) = 0;

    virtual uint64_t GetQwordValue(HKEY root,
                                  const std::wstring& subKeyPath,
                                  const std::wstring& valueName,
                                  REGSAM sam,
                                  GetValueOptions options) = 0;

    virtual std::vector<unsigned char> GetBinaryValue(HKEY root,
                                                     const std::wstring& subKeyPath,
                                                     const std::wstring& valueName,
                                                     REGSAM sam,
                                                     GetValueOptions options) = 0;

    template<typename T>
    T GetValue(HKEY root,
               const std::wstring& subKeyPath,
               const std::wstring& valueName,
               REGSAM sam,
               const GetValueOptions& options);

    virtual void SetStringValue(HKEY root,
                               const std::wstring& subKeyPath,
                               const std::wstring& valueName,
                               const std::wstring& data,
                               DWORD regType,
                               REGSAM sam) = 0;

    virtual void SetDwordValue(HKEY root,
                              const std::wstring& subKeyPath,
                              const std::wstring& valueName,
                              DWORD data,
                              REGSAM sam) = 0;

    virtual void SetQwordValue(HKEY root,
                              const std::wstring& subKeyPath,
                              const std::wstring& valueName,
                              uint64_t data,
                              REGSAM sam) = 0;

    virtual void SetBinaryValue(HKEY root,
                               const std::wstring& subKeyPath,
                               const std::wstring& valueName,
                               const std::vector<unsigned char>& data,
                               REGSAM sam) = 0;

    virtual std::vector<RegValueRecord> ListValues(HKEY root,
                                                  const std::wstring& subKeyPath,
                                                  REGSAM sam,
                                                  ListOptions options) const = 0;

    virtual void DeleteValue(HKEY root,
                            const std::wstring& subKeyPath,
                            const std::wstring& valueName,
                            REGSAM sam) = 0;

    virtual ValueInfo GetRawValue(HKEY root,
                                 const std::wstring& subKeyPath,
                                 const std::wstring& valueName,
                                 REGSAM sam,
                                 const GetValueOptions& options) = 0;

    virtual void SetRawValue(HKEY root,
                            const std::wstring& subKeyPath,
                            const std::wstring& valueName,
                            DWORD type,
                            const std::vector<unsigned char>& data,
                            REGSAM sam) = 0;
};

class ValueManagerImpl final : public IValueManager {
public:
    explicit ValueManagerImpl(std::unique_ptr<IKeyManager> keyManager);

    std::wstring GetStringValue(HKEY root,
                               const std::wstring& subKeyPath,
                               const std::wstring& valueName,
                               REGSAM sam,
                               const GetValueOptions& options) override;

    DWORD GetDwordValue(HKEY root,
                       const std::wstring& subKeyPath,
                       const std::wstring& valueName,
                       REGSAM sam,
                       GetValueOptions options) override;

    uint64_t GetQwordValue(HKEY root,
                          const std::wstring& subKeyPath,
                          const std::wstring& valueName,
                          REGSAM sam,
                          GetValueOptions options) override;

    std::vector<unsigned char> GetBinaryValue(HKEY root,
                                             const std::wstring& subKeyPath,
                                             const std::wstring& valueName,
                                             REGSAM sam,
                                             GetValueOptions options) override;

    void SetStringValue(HKEY root,
                       const std::wstring& subKeyPath,
                       const std::wstring& valueName,
                       const std::wstring& data,
                       DWORD regType,
                       REGSAM sam) override;

    void SetDwordValue(HKEY root,
                      const std::wstring& subKeyPath,
                      const std::wstring& valueName,
                      DWORD data,
                      REGSAM sam) override;

    void SetQwordValue(HKEY root,
                      const std::wstring& subKeyPath,
                      const std::wstring& valueName,
                      uint64_t data,
                      REGSAM sam) override;

    void SetBinaryValue(HKEY root,
                       const std::wstring& subKeyPath,
                       const std::wstring& valueName,
                       const std::vector<unsigned char>& data,
                       REGSAM sam) override;

    std::vector<RegValueRecord> ListValues(HKEY root,
                                          const std::wstring& subKeyPath,
                                          REGSAM sam,
                                          ListOptions options) const override;

    void DeleteValue(HKEY root,
                    const std::wstring& subKeyPath,
                    const std::wstring& valueName,
                    REGSAM sam) override;

    ValueInfo GetRawValue(HKEY root,
                         const std::wstring& subKeyPath,
                         const std::wstring& valueName,
                         REGSAM sam,
                         const GetValueOptions& options) override;

    void SetRawValue(HKEY root,
                    const std::wstring& subKeyPath,
                    const std::wstring& valueName,
                    DWORD type,
                    const std::vector<unsigned char>& data,
                    REGSAM sam) override;

private:
    std::unique_ptr<IKeyManager> m_keyManager;

    [[nodiscard]] static std::wstring BinaryToString(const std::vector<unsigned char>& data, DWORD type);
    [[nodiscard]] static DWORD BinaryToDword(const std::vector<unsigned char>& data, DWORD type);
    [[nodiscard]] static uint64_t BinaryToQword(const std::vector<unsigned char>& data, DWORD type);

    [[nodiscard]] static std::vector<unsigned char> StringToBinary(const std::wstring& str, DWORD type);
    [[nodiscard]] static std::vector<unsigned char> DwordToBinary(DWORD value);
    [[nodiscard]] static std::vector<unsigned char> QwordToBinary(uint64_t value);

    static void ValidateValueType(DWORD actualType, DWORD expectedType, const std::wstring& valueName);

    [[nodiscard]] static std::vector<std::wstring> ParseMultiString(const std::vector<unsigned char>& data);
    [[nodiscard]] static std::vector<unsigned char> SerializeMultiString(const std::vector<std::wstring>& strings);
};

} // namespace core::registry