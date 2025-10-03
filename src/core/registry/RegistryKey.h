// RegistryKey.h
#pragma once

#include <stdexcept>
#include <windows.h>
#include <string>

class RegException final : public std::runtime_error
{
public:
    explicit RegException(const LSTATUS code, std::string const& message)
        : std::runtime_error(message)
        , m_code(code)
    {
    }

    [[nodiscard]] LSTATUS code() const
    {
        return m_code;
    }

private:
    LSTATUS m_code;
};

/**
 * RegistryKey - RAII wrapper for HKEY handles.
 *
 * Responsibilities:
 *  - Owns an HKEY and closes it via RegCloseKey when destroyed (if it owns the handle).
 *  - Provides factory helpers to create or open keys (thin wrappers around RegOpenKeyExW / RegCreateKeyExW).
 *  - Supports move semantics, non-copyable.
 *
 * Notes:
 *  - When created with owns == false the RegistryKey will not close the HKEY (useful for predefined roots).
 *  - Do not call RegCloseKey on predefined root keys; RegistryKey constructed with owns=false avoids that.
 *
 * WinAPI references:
 *  - RegOpenKeyExW (open key). :contentReference[oaicite:2]{index=2}
 *  - RegCreateKeyExW (create/open key). :contentReference[oaicite:3]{index=3}
 *  - RegCloseKey (close handle). :contentReference[oaicite:4]{index=4}
 */
namespace core::registry
{

class RegistryKey
{
public:
    RegistryKey() noexcept;
    explicit RegistryKey(HKEY hKey, bool owns = true) noexcept;
    RegistryKey(RegistryKey&& other) noexcept;
    RegistryKey& operator=(RegistryKey&& other) noexcept;
    ~RegistryKey();

    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;


    static RegistryKey Open(HKEY root, std::wstring const& subKey, REGSAM samDesired = KEY_READ);

    static RegistryKey Create(HKEY root, std::wstring const& subKey, REGSAM samDesired = KEY_READ | KEY_WRITE, DWORD options = REG_OPTION_NON_VOLATILE, DWORD* disposition = nullptr);

    void Close();

    HKEY Detach() noexcept;

    [[nodiscard]] HKEY Handle() const noexcept;

    [[nodiscard]] bool IsValid() const noexcept;
private:
    HKEY m_hKey;
    bool m_owns;
};

} // namespace core::registry
