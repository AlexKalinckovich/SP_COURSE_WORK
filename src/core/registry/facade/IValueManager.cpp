// ValueManager.cpp
#include "IValueManager.h"
#include <algorithm>
#include <sstream>

namespace core::registry
{

ValueManagerImpl::ValueManagerImpl(std::unique_ptr<IKeyManager> keyManager)
    : m_keyManager(std::move(keyManager)) {}

std::wstring ValueManagerImpl::GetStringValue(HKEY root,
                                            const std::wstring& subKeyPath,
                                            const std::wstring& valueName,
                                            REGSAM sam,
                                            const GetValueOptions& options)
{
    const ValueInfo rawValue = GetRawValue(root, subKeyPath, valueName, sam, options);

    if (rawValue.type != REG_SZ && rawValue.type != REG_EXPAND_SZ)
    {
        throw RegException(ERROR_INVALID_DATATYPE,
                           "Expected string type for value: " +
                           std::string(valueName.begin(), valueName.end()) +
                           ", got type: " + std::to_string(rawValue.type));
    }

    return BinaryToString(rawValue.data, rawValue.type);
}

DWORD ValueManagerImpl::GetDwordValue(HKEY root,
                                     const std::wstring& subKeyPath,
                                     const std::wstring& valueName,
                                     REGSAM sam,
                                     const GetValueOptions options)
{
    ValueInfo rawValue = GetRawValue(root, subKeyPath, valueName, sam, options);

    if (rawValue.type != REG_DWORD && rawValue.type != REG_DWORD_LITTLE_ENDIAN)
    {
        throw RegException(ERROR_INVALID_DATATYPE,
                           "Expected DWORD type for value: " +
                           std::string(valueName.begin(), valueName.end()));
    }

    return BinaryToDword(rawValue.data, rawValue.type);
}

uint64_t ValueManagerImpl::GetQwordValue(HKEY root,
                                         const std::wstring& subKeyPath,
                                         const std::wstring& valueName,
                                         REGSAM sam,
                                         const GetValueOptions options)
{
    ValueInfo rawValue = GetRawValue(root, subKeyPath, valueName, sam, options);

    if (rawValue.type != REG_QWORD && rawValue.type != REG_QWORD_LITTLE_ENDIAN)
    {
        throw RegException(ERROR_INVALID_DATATYPE,
                           "Expected QWORD type for value: " +
                           std::string(valueName.begin(), valueName.end()));
    }

    return BinaryToQword(rawValue.data, rawValue.type);
}

std::vector<unsigned char> ValueManagerImpl::GetBinaryValue(HKEY root,
                                                            const std::wstring& subKeyPath,
                                                            const std::wstring& valueName,
                                                            REGSAM sam,
                                                            const GetValueOptions options)
{
    ValueInfo rawValue = GetRawValue(root, subKeyPath, valueName, sam, options);

    if (rawValue.type != REG_BINARY &&
        rawValue.type != REG_NONE &&
        rawValue.type != REG_SZ &&
        rawValue.type != REG_DWORD &&
        rawValue.type != REG_QWORD)
    {
        throw RegException(ERROR_INVALID_DATATYPE,
                           "Expected binary-compatible type for value: " +
                           std::string(valueName.begin(), valueName.end()));
    }

    return rawValue.data;
}


void ValueManagerImpl::SetStringValue(HKEY root,
                                      const std::wstring& subKeyPath,
                                      const std::wstring& valueName,
                                      const std::wstring& data,
                                      const DWORD regType,
                                      const REGSAM sam)
{
    if (regType != REG_SZ && regType != REG_EXPAND_SZ)
    {
        throw RegException(ERROR_INVALID_PARAMETER,
                           "Invalid registry type for string value");
    }

    std::vector<unsigned char> binaryData = StringToBinary(data, regType);
    SetRawValue(root, subKeyPath, valueName, regType, binaryData, sam);
}

void ValueManagerImpl::SetDwordValue(HKEY root,
                                     const std::wstring& subKeyPath,
                                     const std::wstring& valueName,
                                     const DWORD data,
                                     const REGSAM sam)
{
    const std::vector<unsigned char> binaryData = DwordToBinary(data);
    SetRawValue(root, subKeyPath, valueName, REG_DWORD, binaryData, sam);
}

void ValueManagerImpl::SetQwordValue(HKEY root,
                                     const std::wstring& subKeyPath,
                                     const std::wstring& valueName,
                                     const uint64_t data,
                                     const REGSAM sam)
{
    const std::vector<unsigned char> binaryData = QwordToBinary(data);
    SetRawValue(root, subKeyPath, valueName, REG_QWORD, binaryData, sam);
}

void ValueManagerImpl::SetBinaryValue(HKEY root,
                                      const std::wstring& subKeyPath,
                                      const std::wstring& valueName,
                                      const std::vector<unsigned char>& data,
                                      const REGSAM sam)
{
    SetRawValue(root, subKeyPath, valueName, REG_BINARY, data, sam);
}

std::vector<RegValueRecord> ValueManagerImpl::ListValues(HKEY root,
                                                         const std::wstring& subKeyPath,
                                                         const REGSAM sam,
                                                         const ListOptions options) const
{
    const RegistryKey key = m_keyManager->OpenKey(root, subKeyPath, sam, false);
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
                result = std::vector<RegValueRecord>(startIt, endIt);
            }
            else
            {
                result = std::vector<RegValueRecord>(startIt, result.end());
            }
        }
    }

    return result;
}

void ValueManagerImpl::DeleteValue(HKEY root,
                                  const std::wstring& subKeyPath,
                                  const std::wstring& valueName,
                                  REGSAM sam)
{
    const RegistryKey key = m_keyManager->OpenKey(root, subKeyPath, sam, false);
    const LSTATUS status = RegDeleteValueW(key.Handle(), valueName.c_str());
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
    {
        throw RegException(status, "Failed to delete value: " +
                                   std::string(valueName.begin(), valueName.end()));
    }
}

ValueInfo ValueManagerImpl::GetRawValue(HKEY root,
                                       const std::wstring& subKeyPath,
                                       const std::wstring& valueName,
                                       REGSAM sam,
                                       const GetValueOptions& options)
{
    const RegistryKey key = m_keyManager->OpenKey(root, subKeyPath, sam, false);

    DWORD type = 0;
    DWORD dataSize = 0;

    LSTATUS status = RegGetValueW(
        key.Handle(),
        nullptr,
        valueName.empty() ? nullptr : valueName.c_str(),
        RRF_RT_ANY,
        &type,
        nullptr,
        &dataSize
    );

    if (status != ERROR_SUCCESS)
    {
        if (status == ERROR_FILE_NOT_FOUND && !options.defaultValue.empty())
        {
            ValueInfo defaultInfo;
            defaultInfo.name = valueName;
            defaultInfo.type = REG_SZ;
            defaultInfo.data = StringToBinary(options.defaultValue, REG_SZ);
            return defaultInfo;
        }
        throw RegException(status, "Failed to read value: " +
                                   std::string(valueName.begin(), valueName.end()));
    }

    std::vector<unsigned char> data(dataSize);
    status = RegGetValueW(
        key.Handle(),
        nullptr,
        valueName.empty() ? nullptr : valueName.c_str(),
        RRF_RT_ANY,
        &type,
        data.data(),
        &dataSize
    );

    if (status != ERROR_SUCCESS)
    {
        throw RegException(status, "Failed to read value data: " +
                                   std::string(valueName.begin(), valueName.end()));
    }

    data.resize(dataSize);

    ValueInfo result;
    result.name = valueName;
    result.type = type;
    result.data = std::move(data);

    return result;
}

void ValueManagerImpl::SetRawValue(HKEY root,
                                   const std::wstring& subKeyPath,
                                   const std::wstring& valueName,
                                   const DWORD type,
                                   const std::vector<unsigned char>& data,
                                   const REGSAM sam)
{
    const RegistryKey key = m_keyManager->OpenKey(root, subKeyPath, sam, true);
    const LSTATUS status = RegSetValueExW(
        key.Handle(),
        valueName.c_str(),
        0,
        type,
        data.data(),
        data.size()
    );

    if (status != ERROR_SUCCESS)
    {
        throw RegException(status, "Failed to set value: " +
                                   std::string(valueName.begin(), valueName.end()));
    }
}

std::wstring ValueManagerImpl::BinaryToString(const std::vector<unsigned char>& data, DWORD type)
{
    if (data.empty()) {
        return L"";
    }

    const auto stringData = reinterpret_cast<const wchar_t*>(data.data());
    size_t charCount = data.size() / sizeof(wchar_t);

    if (charCount > 0 && stringData[charCount - 1] == L'\0') {
        charCount--;
    }

    return {stringData, charCount};
}

DWORD ValueManagerImpl::BinaryToDword(const std::vector<unsigned char>& data, DWORD type)
{
    if (data.size() < sizeof(DWORD)) {
        throw RegException(ERROR_INVALID_DATA, "Insufficient data for DWORD conversion");
    }

    return *reinterpret_cast<const DWORD*>(data.data());
}

uint64_t ValueManagerImpl::BinaryToQword(const std::vector<unsigned char>& data, DWORD type)
{
    if (data.size() < sizeof(uint64_t)) {
        throw RegException(ERROR_INVALID_DATA, "Insufficient data for QWORD conversion");
    }

    return *reinterpret_cast<const uint64_t*>(data.data());
}

std::vector<unsigned char> ValueManagerImpl::StringToBinary(const std::wstring& str, DWORD type)
{
    std::vector<unsigned char> data;

    const size_t byteSize = (str.length() + 1) * sizeof(wchar_t);
    data.resize(byteSize);

    memcpy(data.data(), str.c_str(), byteSize);

    return data;
}

std::vector<unsigned char> ValueManagerImpl::DwordToBinary(const DWORD value)
{
    std::vector<unsigned char> data(sizeof(DWORD));
    memcpy(data.data(), &value, sizeof(DWORD));
    return data;
}

std::vector<unsigned char> ValueManagerImpl::QwordToBinary(const uint64_t value)
{
    std::vector<unsigned char> data(sizeof(uint64_t));
    memcpy(data.data(), &value, sizeof(uint64_t));
    return data;
}

// Validation
void ValueManagerImpl::ValidateValueType(const DWORD actualType, const DWORD expectedType, const std::wstring& valueName)
{
    if (actualType != expectedType)
    {
        std::stringstream ss;
        ss << "Type mismatch for value: " << std::string(valueName.begin(), valueName.end())
                << ". Expected: " << expectedType << ", Got: " << actualType;
        throw RegException(ERROR_INVALID_DATATYPE, ss.str());
    }
}

std::vector<std::wstring> ValueManagerImpl::ParseMultiString(const std::vector<unsigned char>& data)
{
    std::vector<std::wstring> result;

    if (data.empty())
    {
        return result;
    }

    const auto *stringData = reinterpret_cast<const wchar_t *>(data.data());
    const size_t totalChars = data.size() / sizeof(wchar_t);

    const wchar_t *current = stringData;
    while (current < stringData + totalChars && *current != L'\0')
    {
        size_t len = wcslen(current);
        if (len > 0)
        {
            result.emplace_back(current, len);
        }
        current += len + 1;
    }

    return result;
}

std::vector<unsigned char> ValueManagerImpl::SerializeMultiString(const std::vector<std::wstring>& strings)
{
    size_t totalSize = 0;
    for (const std::wstring &str: strings)
    {
        totalSize += (str.length() + 1) * sizeof(wchar_t);
    }
    totalSize += sizeof(wchar_t);

    std::vector<unsigned char> data(totalSize, 0);
    auto writePtr = reinterpret_cast<wchar_t *>(data.data());

    for (const std::wstring &str: strings)
    {
        if (!str.empty())
        {
            memcpy(writePtr, str.c_str(), str.length() * sizeof(wchar_t));
            writePtr += str.length();
        }
        *writePtr++ = L'\0';
    }
    *writePtr = L'\0';

    return data;
}
} // namespace core::registry