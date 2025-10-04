// KeyManager.cpp
#include "IKeyManager.h"
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace core::registry
{

namespace
{
    std::wstring SamToString(const REGSAM sam)
    {
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

    std::wstring HkeyToString(HKEY hkey)
    {
        if (hkey == HKEY_CLASSES_ROOT) return L"HKEY_CLASSES_ROOT";
        if (hkey == HKEY_CURRENT_USER) return L"HKEY_CURRENT_USER";
        if (hkey == HKEY_LOCAL_MACHINE) return L"HKEY_LOCAL_MACHINE";
        if (hkey == HKEY_USERS) return L"HKEY_USERS";
        if (hkey == HKEY_CURRENT_CONFIG) return L"HKEY_CURRENT_CONFIG";
        if (hkey == HKEY_PERFORMANCE_DATA) return L"HKEY_PERFORMANCE_DATA";
        return L"HKEY_UNKNOWN";
    }
}

RegistryKey KeyManagerImpl::OpenKey(HKEY root,
                                   const std::wstring& subKeyPath,
                                   REGSAM sam,
                                   const bool createIfMissing) {
    ValidateRootKey(root);
    ValidateSamDesired(sam, createIfMissing);

    return OpenKeyUncached(root, subKeyPath, sam, createIfMissing);
}

bool KeyManagerImpl::KeyExists(HKEY root,
                              const std::wstring& subKeyPath,
                              REGSAM sam) const
{
    bool result;
    try
    {
        const RegistryKey key = OpenKeyUncached(root, subKeyPath, sam, false);
        result = key.IsValid();
    } catch (const RegException &)
    {
        result = false;
    }
    return result;
}

KeyInfo KeyManagerImpl::GetKeyInfo(HKEY root,
                                   const std::wstring& subKeyPath,
                                   REGSAM sam) const {
    const RegistryKey key = OpenKeyUncached(root, subKeyPath, sam, false);

    KeyInfo info;
    info.path = subKeyPath;

    DWORD subKeys = 0, values = 0;
    LSTATUS status = RegQueryInfoKeyW(
        key.Handle(),
        nullptr, // class name
        nullptr, // class name size
        nullptr, // reserved
        &subKeys,
        nullptr, // max subkey length
        nullptr, // max class length
        &values,
        nullptr, // max value name length
        nullptr, // max value data length
        nullptr, // security descriptor
        reinterpret_cast<PFILETIME>(&info.lastWriteTime)
    );

    if (status != ERROR_SUCCESS) {
        throw RegException(status, "Failed to query key info for: " +
                          std::to_string(reinterpret_cast<uintptr_t>(root)) + "\\" +
                          std::string(subKeyPath.begin(), subKeyPath.end()));
    }

    info.subKeyCount = subKeys;
    info.valueCount = values;

    return info;
}

std::vector<std::wstring> KeyManagerImpl::ListSubKeys(HKEY root,
                                                     const std::wstring& subKeyPath,
                                                     REGSAM sam,
                                                     ListOptions options) const
{
    const RegistryKey key = OpenKeyUncached(root, subKeyPath, sam, false);
    std::vector<std::wstring> result = EnumerateSubKeys(key);

    if (options.offset > 0 || options.maxItems > 0)
    {
        if (options.offset >= result.size())
        {
            result.clear();
        }
        else
        {
            const auto startIt = result.begin() + static_cast<std::ptrdiff_t>(options.offset);

            if (options.maxItems > 0)
            {
                const size_t itemsToTake = std::min(options.maxItems, result.size() - options.offset);
                const auto endIt = startIt + static_cast<std::ptrdiff_t>(itemsToTake);
                result = std::vector(startIt, endIt);
            }
            else
            {
                result = std::vector(startIt, result.end());
            }
        }
    }

    return result;
}

void KeyManagerImpl::CreateKey(HKEY root,
                               const std::wstring& subKeyPath,
                               REGSAM sam) {
    ValidateRootKey(root);
    ValidateSamDesired(sam, true); // Write operation

    if (subKeyPath.empty()) {
        throw RegException(ERROR_INVALID_PARAMETER, "SubKey path cannot be empty for CreateKey");
    }

    try {
        RegistryKey::Create(root, subKeyPath, sam);
    } catch (const RegException& ex) {
        throw RegException(ex.code(),
                          "Failed to create key: " +
                          std::string(subKeyPath.begin(), subKeyPath.end()) +
                          " - " + ex.what());
    }
}

void KeyManagerImpl::DeleteKey(HKEY root,
                              const std::wstring& subKeyPath,
                              REGSAM sam) {
    ValidateRootKey(root);

    const LSTATUS status = RegDeleteKeyExW(root, subKeyPath.c_str(), sam, 0);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        throw RegException(status, "Failed to delete key: " +
                          std::string(subKeyPath.begin(), subKeyPath.end()));
    }
}

bool KeyManagerImpl::CopyKey(HKEY sourceRoot,
                            const std::wstring& sourcePath,
                            HKEY targetRoot,
                            const std::wstring& targetPath,
                            REGSAM sam) {
    ValidateRootKey(sourceRoot);
    ValidateRootKey(targetRoot);
    bool result = true;
    try {
        CreateKey(targetRoot, targetPath, sam);
    } catch (const RegException&) {
        result = false;
    }
    return result;
}

bool KeyManagerImpl::MoveKey(HKEY sourceRoot,
                            const std::wstring& sourcePath,
                            HKEY targetRoot,
                            const std::wstring& targetPath,
                            const REGSAM sam) {
    bool result = true;
    if (CopyKey(sourceRoot, sourcePath, targetRoot, targetPath, sam)) {
        try {
            DeleteKey(sourceRoot, sourcePath, sam);
        } catch (const RegException&) {
            result = false;
        }
    }
    return result;
}

// Validation
void KeyManagerImpl::ValidateRootKey(HKEY root) const {
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

    if (!isValid && root != nullptr && root != INVALID_HANDLE_VALUE) {
        isValid = true;
    }

    if (!isValid) {
        throw RegException(ERROR_INVALID_HANDLE,
                          "Invalid root key specified: " +
                          std::to_string(reinterpret_cast<uintptr_t>(root)));
    }
}

void KeyManagerImpl::ValidateSamDesired(const REGSAM sam, const bool forWrite) const {
    if (forWrite && !(sam & KEY_WRITE)) {
        throw RegException(ERROR_ACCESS_DENIED,
                          "Write operation requires KEY_WRITE access");
    }

    if (sam == 0) {
        throw RegException(ERROR_INVALID_PARAMETER, "REGSAM cannot be zero");
    }
}

RegistryKey KeyManagerImpl::OpenKeyUncached(HKEY root,
                                           const std::wstring& subKeyPath,
                                           const REGSAM sam,
                                           const bool createIfMissing)
{
    if (subKeyPath.empty()) {
        return RegistryKey(root, false);
    }

    if (createIfMissing) {
        try {
            return RegistryKey::Open(root, subKeyPath, sam);
        } catch (const RegException&) {
            return RegistryKey::Create(root, subKeyPath, sam);
        }
    }

    return RegistryKey::Open(root, subKeyPath, sam);
}

} // namespace core::registry