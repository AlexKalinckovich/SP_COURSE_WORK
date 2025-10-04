// RegistryHelpers.cpp
#include "RegistryHelpers.h"
#include <vector>
#include <limits>
#include <memory>
#include <sstream>

namespace core::registry
{

std::string FormatWinErrorMessage(const LSTATUS code)
{
    std::ostringstream oss;
    oss << "WinAPI registry error code: " << code;
    return oss.str();
}

std::wstring ReadStringValue(RegistryKey const& key, std::wstring const& valueName)
{
    if (!key.IsValid())
    {
        throw RegException(ERROR_INVALID_HANDLE, "Invalid registry key handle");
    }

    DWORD type = 0;
    DWORD cbData = 0;

    LSTATUS status = RegGetValueW(key.Handle(),
                                  nullptr,
                                  valueName.empty() ? nullptr : valueName.c_str(),
                                  RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                                  &type,
                                  nullptr,
                                  &cbData);

    if (status != ERROR_SUCCESS)
    {
        throw RegException(status, FormatWinErrorMessage(status));
    }

    if (cbData == 0)
    {
        return {};
    }

    std::wstring result;
    result.resize((cbData / sizeof(wchar_t)) - 1);

    DWORD actualSize = cbData;
    status = RegGetValueW(key.Handle(),
                         nullptr,
                         valueName.empty() ? nullptr : valueName.c_str(),
                         RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                         &type,
                         &result[0],
                         &actualSize);

    if (status != ERROR_SUCCESS)
    {
        throw RegException(status, FormatWinErrorMessage(status));
    }

    if (actualSize >= sizeof(wchar_t) && result[(actualSize / sizeof(wchar_t)) - 1] == L'\0')
    {
        result.resize((actualSize / sizeof(wchar_t)) - 1);
    }

    return result;
}

DWORD ReadDwordValue(RegistryKey const& key, std::wstring const& valueName)
{
    if (!key.IsValid())
    {
        throw RegException(ERROR_INVALID_HANDLE, "Invalid registry key handle");
    }

    DWORD type = 0;
    DWORD data = 0;
    DWORD cbData = sizeof(data);

    LSTATUS status = RegGetValueW(key.Handle(),
                                  nullptr,
                                  valueName.empty() ? nullptr : valueName.c_str(),
                                  RRF_RT_DWORD,
                                  &type,
                                  &data,
                                  &cbData);
    if (status != ERROR_SUCCESS)
    {
        throw RegException(status, FormatWinErrorMessage(status));
    }
    return data;
}

unsigned long long ReadQwordValue(RegistryKey const& key, std::wstring const& valueName)
{
    if (!key.IsValid())
    {
        throw RegException(ERROR_INVALID_HANDLE, "Invalid registry key handle");
    }

    DWORD type = 0;
    unsigned long long data = 0ull;
    DWORD cbData = sizeof(unsigned long long);

    const LSTATUS status = RegGetValueW(key.Handle(),
                                        nullptr,
                                        valueName.empty() ? nullptr : valueName.c_str(),
                                        RRF_RT_QWORD,
                                        &type,
                                        &data,
                                        &cbData);
    if (status != ERROR_SUCCESS)
    {
        throw RegException(status, FormatWinErrorMessage(status));
    }
    return data;
}

std::vector<unsigned char> ReadBinaryValue(RegistryKey const& key, std::wstring const& valueName)
{
    if (!key.IsValid())
    {
        throw RegException(ERROR_INVALID_HANDLE, "Invalid registry key handle");
    }

    DWORD type = 0;
    DWORD cbData = 0;

    LSTATUS status = RegGetValueW(key.Handle(),
                                  nullptr,
                                  valueName.empty() ? nullptr : valueName.c_str(),
                                  RRF_RT_REG_BINARY,
                                  &type,
                                  nullptr,
                                  &cbData);

    if (status != ERROR_SUCCESS && status != ERROR_MORE_DATA)
    {
        throw RegException(status, FormatWinErrorMessage(status));
    }

    std::vector<unsigned char> buffer(cbData);

    if (cbData > 0)
    {
        DWORD actualSize = cbData;
        status = RegGetValueW(key.Handle(),
                              nullptr,
                              valueName.empty() ? nullptr : valueName.c_str(),
                              RRF_RT_REG_BINARY,
                              &type,
                              buffer.data(),
                              &actualSize);

        if (status != ERROR_SUCCESS)
        {
            throw RegException(status, FormatWinErrorMessage(status));
        }

        if (actualSize < cbData)
        {
            buffer.resize(actualSize);
        }
    }

    return buffer;
}

void SetStringValue(RegistryKey const& key, std::wstring const& valueName, std::wstring const& data, DWORD regType)
{
    if (!key.IsValid())
    {
        throw RegException(ERROR_INVALID_HANDLE, "Invalid registry key handle");
    }

    if (regType != REG_SZ && regType != REG_EXPAND_SZ)
    {
        throw RegException(ERROR_INVALID_PARAMETER,
                          "Invalid string type. Use REG_SZ or REG_EXPAND_SZ");
    }

    const DWORD cbData = (data.size() + 1) * sizeof(wchar_t);

    const LSTATUS status = RegSetValueExW(key.Handle(),
                                          valueName.empty() ? nullptr : valueName.c_str(),
                                          0,
                                          regType,
                                          reinterpret_cast<const BYTE*>(data.c_str()),
                                          cbData);

    if (status != ERROR_SUCCESS)
    {
        throw RegException(status, FormatWinErrorMessage(status));
    }
}

void
SetDwordValue(RegistryKey const& key, std::wstring const& valueName, DWORD data)
{
    if (!key.IsValid())
    {
        throw RegException(ERROR_INVALID_HANDLE, "Invalid registry key handle");
    }

    const LSTATUS status = RegSetValueExW(key.Handle(),
                                          valueName.empty() ? nullptr : valueName.c_str(),
                                          0,
                                          REG_DWORD,
                                          reinterpret_cast<const BYTE*>(&data),
                                          sizeof(data));
    if (status != ERROR_SUCCESS)
    {
        throw RegException(status, FormatWinErrorMessage(status));
    }
}

void SetQwordValue(RegistryKey const& key, std::wstring const& valueName, unsigned long long data)
{
    if (!key.IsValid())
    {
        throw RegException(ERROR_INVALID_HANDLE, "Invalid registry key handle");
    }

    LSTATUS status = RegSetValueExW(key.Handle(),
                                    valueName.empty() ? nullptr : valueName.c_str(),
                                    0,
                                    REG_QWORD,
                                    reinterpret_cast<const BYTE*>(&data),
                                    static_cast<DWORD>(sizeof(data)));
    if (status != ERROR_SUCCESS)
    {
        throw RegException(status, FormatWinErrorMessage(status));
    }
}

void SetBinaryValue(RegistryKey const& key, std::wstring const& valueName, std::vector<unsigned char> const& data)
{
    if (!key.IsValid())
    {
        throw RegException(ERROR_INVALID_HANDLE, "Invalid registry key handle");
    }

    const BYTE* ptr = (data.empty() ? nullptr : data.data());
    const DWORD cbData = data.size();

    const LSTATUS status = RegSetValueExW(key.Handle(),
                                          valueName.empty() ? nullptr : valueName.c_str(),
                                          0,
                                          REG_BINARY,
                                          ptr,
                                          cbData);
    if (status != ERROR_SUCCESS)
    {
        throw RegException(status, FormatWinErrorMessage(status));
    }
}

std::vector<std::wstring> EnumerateSubKeys(RegistryKey const& key)
{
    if (!key.IsValid())
    {
        throw RegException(ERROR_INVALID_HANDLE, "Invalid registry key handle");
    }

    std::vector<std::wstring> result;

    constexpr DWORD INITIAL_BUFFER_SIZE = 256;
    constexpr size_t MAX_ITERATION_COUNT = 10000;
    std::vector<wchar_t> nameBuffer(INITIAL_BUFFER_SIZE);

    DWORD index = 0;
    DWORD safetyCounter = 0;
    bool bufferResized = false;
    bool isAllDataRead = false;

    while(safetyCounter < MAX_ITERATION_COUNT && !isAllDataRead)
    {
        DWORD nameLen = nameBuffer.size();
        FILETIME lastWriteTime = {};

        const LSTATUS status = RegEnumKeyExW(key.Handle(),
                                             index,
                                             nameBuffer.data(),
                                             &nameLen,
                                             nullptr,
                                             nullptr,
                                             nullptr,
                                             &lastWriteTime);

        if (status == ERROR_SUCCESS)
        {
            result.emplace_back(nameBuffer.data(), nameLen);
            ++index;
            bufferResized = false;
        }
        else if (status == ERROR_MORE_DATA)
        {
            nameBuffer.resize(nameLen + 1);
            bufferResized = true;
        }
        else if (status == ERROR_NO_MORE_ITEMS)
        {
            isAllDataRead = true;
        }
        else
        {

            throw RegException(status, FormatWinErrorMessage(status));
        }

        if (bufferResized && safetyCounter > 1000)
        {
            throw RegException(ERROR_INTERNAL_ERROR,
                              "Too many buffer resizes during subkey enumeration");
        }
        safetyCounter++;
    }

    result.shrink_to_fit();
    return result;
}

std::vector<RegValueRecord> EnumerateValues(RegistryKey const& key)
{
    if (!key.IsValid())
    {
        throw RegException(ERROR_INVALID_HANDLE, "Invalid registry key handle");
    }

    DWORD valueCount = 0;
    DWORD maxValueNameLen = 0;
    DWORD maxValueDataLen = 0;

    LSTATUS status = RegQueryInfoKeyW(key.Handle(),
                                     nullptr, nullptr, nullptr,
                                     nullptr, nullptr, nullptr,
                                     &valueCount,
                                     &maxValueNameLen,
                                     &maxValueDataLen,
                                     nullptr, nullptr);
    if (status != ERROR_SUCCESS)
    {
        throw RegException(status, FormatWinErrorMessage(status));
    }

    std::vector<RegValueRecord> result;
    result.reserve(valueCount);

    std::vector<wchar_t> nameBuffer(maxValueNameLen + 2);
    std::vector<BYTE> dataBuffer(maxValueDataLen > 0 ? maxValueDataLen : 1);

    for (DWORD index = 0; index < valueCount; ++index)
    {
        DWORD nameLen = nameBuffer.size();
        DWORD type = 0;
        DWORD dataSize = dataBuffer.size();

        LSTATUS enumStatus = RegEnumValueW(key.Handle(),
                                          index,
                                          nameBuffer.data(),
                                          &nameLen,
                                          nullptr,
                                          &type,
                                          dataBuffer.data(),
                                          &dataSize);

        if (enumStatus == ERROR_MORE_DATA)
        {
            dataBuffer.resize(dataSize);
            dataSize = static_cast<DWORD>(dataBuffer.size());

            enumStatus = RegEnumValueW(key.Handle(),
                                      index,
                                      nameBuffer.data(),
                                      &nameLen,
                                      nullptr,
                                      &type,
                                      dataBuffer.data(),
                                      &dataSize);
        }

        if (enumStatus == ERROR_NO_MORE_ITEMS)
            break;

        if (enumStatus != ERROR_SUCCESS)
        {
            throw RegException(enumStatus, FormatWinErrorMessage(enumStatus));
        }

        std::wstring valueName;
        if (nameLen == 0)
        {
            valueName = L"<Default_Value>";
        }
        else
        {
            valueName = std::wstring(nameBuffer.data(), nameLen);
        }

        result.emplace_back(RegValueRecord{
            valueName,
            type,
            std::vector(dataBuffer.begin(), dataBuffer.begin() + dataSize)
        });
    }

    return result;
}

    void DeleteValue(RegistryKey const& key, std::wstring const& valueName)
{
    if (!key.IsValid())
    {
        throw RegException(ERROR_INVALID_HANDLE, "Invalid registry key handle");
    }

    DWORD type = 0;
    DWORD dataSize = 0;
    const LSTATUS checkStatus = RegGetValueW(key.Handle(),
                                             nullptr,
                                             valueName.empty() ? nullptr : valueName.c_str(),
                                             RRF_RT_ANY,
                                             &type,
                                             nullptr,
                                             &dataSize);

    if (checkStatus != ERROR_FILE_NOT_FOUND)
    {
        const wchar_t* valueToDelete = valueName.empty() ? nullptr : valueName.c_str();
        const LSTATUS status = RegDeleteValueW(key.Handle(), valueToDelete);

        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        {
            throw RegException(status, FormatWinErrorMessage(status));
        }
    }
}

void DeleteSubKey(HKEY root, std::wstring const& subKey, REGSAM samDesired)
{
    const LSTATUS status = RegDeleteKeyExW(root, subKey.c_str(), samDesired, 0);
    if (status != ERROR_SUCCESS)
    {
        throw RegException(status, FormatWinErrorMessage(status));
    }
}


void SaveKeyToFile(RegistryKey const& key, std::wstring const& filePath)
{
    if (!key.IsValid())
    {
        throw RegException(ERROR_INVALID_HANDLE, "Invalid registry key handle");
    }

    const LSTATUS status = RegSaveKeyW(key.Handle(), filePath.c_str(), nullptr);
    if (status != ERROR_SUCCESS)
    {
        throw RegException(status, FormatWinErrorMessage(status));
    }
}

void RestoreKeyFromFile(RegistryKey const& key, std::wstring const& filePath, DWORD flags)
{
    if (!key.IsValid())
    {
        throw RegException(ERROR_INVALID_HANDLE, "Invalid registry key handle");
    }

    const LSTATUS status = RegRestoreKeyW(key.Handle(), filePath.c_str(), flags);
    if (status != ERROR_SUCCESS)
    {
        throw RegException(status, FormatWinErrorMessage(status));
    }
}

bool EnablePrivilege(std::wstring const& privilegeName, bool enable)
{
    HANDLE token = nullptr;

    struct TokenGuard {
        HANDLE handle;
        explicit TokenGuard(HANDLE h) : handle(h) {}
        ~TokenGuard()
        {
            if (handle)
            {
                CloseHandle(handle);
            }
        }
    };

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
    {
        return false;
    }

    TokenGuard tokenGuard(token);

    LUID luid;
    if (!LookupPrivilegeValueW(nullptr, privilegeName.c_str(), &luid))
    {
        return false;
    }

    TOKEN_PRIVILEGES tp;
    TOKEN_PRIVILEGES previousTp;
    DWORD previousSize = sizeof(TOKEN_PRIVILEGES);

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = enable ? SE_PRIVILEGE_ENABLED : 0;

    if (!AdjustTokenPrivileges(token,
                              FALSE,
                              &tp,
                              sizeof(TOKEN_PRIVILEGES),
                              &previousTp,
                              &previousSize))
    {
        return false;
    }

    return (GetLastError() == ERROR_SUCCESS);
}

HANDLE CreateRegistryChangeEvent(RegistryKey const& key,   const bool watchSubtree,
                                 const DWORD notifyFilter, const BOOL fAsynchronous)
{
    if (!key.IsValid())
    {
        throw RegException(ERROR_INVALID_HANDLE, "Invalid registry key handle");
    }

    constexpr DWORD validFilters = REG_NOTIFY_CHANGE_NAME |
                                   REG_NOTIFY_CHANGE_ATTRIBUTES |
                                   REG_NOTIFY_CHANGE_LAST_SET |
                                   REG_NOTIFY_CHANGE_SECURITY;

    if ((notifyFilter & ~validFilters) != 0)
    {
        throw RegException(ERROR_INVALID_PARAMETER, "Invalid notify filter specified");
    }

    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (hEvent == nullptr)
    {
        const DWORD err = GetLastError();
        throw RegException(static_cast<LSTATUS>(err),
                          "Failed to create event: " + FormatWinErrorMessage(static_cast<LSTATUS>(err)));
    }

    struct EventGuard {
        HANDLE handle;
        explicit EventGuard(HANDLE h) : handle(h) {}
        ~EventGuard()
        {
            if (handle)
            {
                CloseHandle(handle);
            }
        }
    } eventGuard(hEvent);

    const LSTATUS status = RegNotifyChangeKeyValue(key.Handle(),
                                                   watchSubtree ? TRUE : FALSE,
                                                   notifyFilter,
                                                   hEvent,
                                                   fAsynchronous);

    if (status != ERROR_SUCCESS)
    {
        throw RegException(status, "Failed to set registry notification: " + FormatWinErrorMessage(status));
    }

    eventGuard.handle = nullptr;
    return hEvent;
}

} // namespace core::registry
