// RegistryHelpers.h
#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <stdexcept>
#include <optional>

namespace core::registry
{

/**
 * RegException - простая обёртка для ошибок WinAPI в реестре.
 * Хранит LSTATUS (обычно LONG) код ошибки и текстовое сообщение.
 */
class RegException : public std::runtime_error
{
public:
    explicit RegException(LSTATUS code, std::string const& message)
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
 * RegistryKey - RAII-обёртка над HKEY.
 *
 * - Автоматически закрывает ключ в деструкторе, если владеет хендлом.
 * - Предоставляет фабрики Open/Create для удобства (Open - не создаёт ключ).
 *
 * Примечания по функциям WinAPI:
 *  - RegOpenKeyExW: открытие ключа. Документация: https://learn.microsoft.com/.../RegOpenKeyExW. :contentReference[oaicite:5]{index=5}
 *  - RegCreateKeyExW: создание/открытие ключа. :contentReference[oaicite:6]{index=6}
 *  - RegCloseKey: закрытие. :contentReference[oaicite:7]{index=7}
 */
class RegistryKey
{
public:
    RegistryKey() noexcept;
    explicit RegistryKey(HKEY hKey, bool owns = true) noexcept;
    RegistryKey(RegistryKey&& other) noexcept;
    RegistryKey& operator=(RegistryKey&& other) noexcept;
    ~RegistryKey();

    // Non-copyable
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;

    // Open existing key (does not create)
    static RegistryKey
    Open(HKEY root, std::wstring const& subKey, REGSAM samDesired = KEY_READ);

    // Create or open (creates intermediate missing keys)
    static RegistryKey
    Create(HKEY root,
           std::wstring const& subKey,
           REGSAM samDesired = KEY_READ | KEY_WRITE,
           DWORD options = REG_OPTION_NON_VOLATILE,
           DWORD* disposition = nullptr);

    // Close handle explicitly
    void
    Close();

    // Detach handle (transfer ownership)
    HKEY
    Detach() noexcept;

    // Access underlying HKEY (may be INVALID_HANDLE_VALUE)
    HKEY
    Handle() const noexcept;

    bool
    IsValid() const noexcept;

private:
    HKEY m_hKey;
    bool m_owns;
};

/**
 * RegValueRecord - represents одно значение ключа (имя, тип, данные)
 */
struct RegValueRecord
{
    std::wstring name;
    DWORD type;
    std::vector<unsigned char> data;
};

/**
 * Read/Write helpers.
 *
 * Все функции бросают RegException в случае ошибки.
 *
 * Для строковых чтений используем RegGetValueW (более безопасно для строки).
 * Документация RegGetValueW: https://learn.microsoft.com/.../RegGetValueW. :contentReference[oaicite:8]{index=8}
 */

// Read helpers
std::wstring
ReadStringValue(RegistryKey const& key, std::wstring const& valueName);

DWORD
ReadDwordValue(RegistryKey const& key, std::wstring const& valueName);

unsigned long long
ReadQwordValue(RegistryKey const& key, std::wstring const& valueName);

std::vector<unsigned char>
ReadBinaryValue(RegistryKey const& key, std::wstring const& valueName);

// Write helpers
void
SetStringValue(RegistryKey const& key, std::wstring const& valueName, std::wstring const& data, DWORD regType = REG_SZ);

void
SetDwordValue(RegistryKey const& key, std::wstring const& valueName, DWORD data);

void
SetQwordValue(RegistryKey const& key, std::wstring const& valueName, unsigned long long data);

void
SetBinaryValue(RegistryKey const& key, std::wstring const& valueName, std::vector<unsigned char> const& data);

// Enumerate
std::vector<std::wstring>
EnumerateSubKeys(RegistryKey const& key);

std::vector<RegValueRecord>
EnumerateValues(RegistryKey const& key);

// Delete helpers
void
DeleteValue(RegistryKey const& key, std::wstring const& valueName);

// Delete subkey (uses RegDeleteKeyExW when available)
void
DeleteSubKey(HKEY root, std::wstring const& subKey, REGSAM samDesired = 0);

// Save / Restore (snapshots)
// Note: RegSaveKey / RegRestoreKey require special privileges (SE_BACKUP_NAME / SE_RESTORE_NAME).
// RegSaveKey doc: https://learn.microsoft.com/.../RegSaveKeyW . :contentReference[oaicite:9]{index=9}
// RegRestoreKey doc: https://learn.microsoft.com/.../RegRestoreKeyW . :contentReference[oaicite:10]{index=10}
void
SaveKeyToFile(RegistryKey const& key, std::wstring const& filePath);

void
RestoreKeyFromFile(RegistryKey const& key, std::wstring const& filePath, DWORD flags);

// Privilege helper: enable/disable a privilege in current process token.
// Use to enable SeBackupPrivilege / SeRestorePrivilege.
// Docs on enabling privileges: https://learn.microsoft.com/.../enabling-and-disabling-privileges-in-c-- . :contentReference[oaicite:11]{index=11}
bool
EnablePrivilege(std::wstring const& privilegeName, bool enable);

// Watch helper: create event and request notification with RegNotifyChangeKeyValue.
// Returns HANDLE to event (caller must CloseHandle). If fAsynchronous==TRUE, the event will be signaled by the system.
// RegNotifyChangeKeyValue doc: https://learn.microsoft.com/.../RegNotifyChangeKeyValue . :contentReference[oaicite:12]{index=12}
HANDLE
CreateRegistryChangeEvent(RegistryKey const& key, bool watchSubtree, DWORD notifyFilter, BOOL fAsynchronous);

} // namespace core::registry
