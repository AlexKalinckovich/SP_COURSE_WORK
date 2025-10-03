//
// Created by brota on 27.09.2025.
//

#include "RegistryKey.h"
#include "RegistryHelpers.h"
#include <vector>
#include <limits>
#include <memory>
#include <sstream>
namespace core::registry
{
    // -------------------- RegistryKey implementation --------------------
    RegistryKey::RegistryKey() noexcept
        : m_hKey(nullptr)
        , m_owns(false)
    {
    }

    RegistryKey::RegistryKey(HKEY hKey, bool owns) noexcept
        : m_hKey(hKey)
        , m_owns(owns)
    {
    }

    RegistryKey::RegistryKey(RegistryKey&& other) noexcept
            : m_hKey(nullptr)
            , m_owns(false)
    {
        std::swap(m_hKey, other.m_hKey);
        std::swap(m_owns, other.m_owns);
    }

    RegistryKey& RegistryKey::operator=(RegistryKey&& other) noexcept
    {
        if (this != &other)
        {
            RegistryKey temp(std::move(*this));

            m_hKey = other.m_hKey;
            m_owns = other.m_owns;

            other.m_hKey = nullptr;
            other.m_owns = false;
        }
        return *this;
    }

    RegistryKey::~RegistryKey()
    {
        if (m_owns && m_hKey != INVALID_HANDLE_VALUE)
        {
            RegCloseKey(m_hKey);
            m_hKey = nullptr;
        }
    }

    bool IsValidRootKey(HKEY root)
    {
        const HKEY validRoots[] = {
            HKEY_CLASSES_ROOT, HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE,
            HKEY_USERS, HKEY_CURRENT_CONFIG, HKEY_PERFORMANCE_DATA
        };

        for (HKEY validRoot: validRoots)
        {
            if (root == validRoot)
            {
                return true;
            }
        }

        return (root != nullptr && root != INVALID_HANDLE_VALUE);
    }

    RegistryKey RegistryKey::Open(HKEY root, std::wstring const& subKey, REGSAM samDesired)
    {
        if (!IsValidRootKey(root))
        {
            throw RegException(ERROR_INVALID_HANDLE, "Invalid root key specified");
        }

        if (subKey.empty())
        {
            throw RegException(ERROR_INVALID_PARAMETER, "SubKey cannot be empty");
        }

        HKEY hRes = nullptr;
        const LSTATUS status = RegOpenKeyExW(root, subKey.c_str(), 0, samDesired, &hRes);

        if (status != ERROR_SUCCESS)
        {
            std::string message = "Failed to open registry key: ";
            message += FormatWinErrorMessage(status);
            throw RegException(status, message);
        }

        return RegistryKey(hRes, true);
    }

    RegistryKey RegistryKey::Create(HKEY root,std::wstring const& subKey,const REGSAM samDesired,
                                    const DWORD options,DWORD* disposition)
    {
        if (!IsValidRootKey(root))
        {
            throw RegException(ERROR_INVALID_HANDLE, "Invalid root key specified");
        }

        if (subKey.empty())
        {
            throw RegException(ERROR_INVALID_PARAMETER, "SubKey cannot be empty");
        }

        HKEY hRes = nullptr;
        DWORD disp = 0;

        const LSTATUS status = RegCreateKeyExW(root,
                                               subKey.c_str(),
                                               0,
                                               nullptr,
                                               options,
                                               samDesired,
                                               nullptr,
                                               &hRes,
                                               &disp);

        if (status != ERROR_SUCCESS)
        {
            std::string message = "Failed to create registry key: ";
            message += FormatWinErrorMessage(status);
            throw RegException(status, message);
        }

        if (disposition != nullptr)
        {
            *disposition = disp;
        }

        return RegistryKey(hRes, true);
    }

    void
    RegistryKey::Close()
    {
        if (m_owns && m_hKey != INVALID_HANDLE_VALUE)
        {
            RegCloseKey(m_hKey);
            m_hKey = nullptr;
            m_owns = false;
        }
    }

    HKEY
    RegistryKey::Detach() noexcept
    {
        HKEY tmp = m_hKey;
        m_hKey = nullptr;
        m_owns = false;
        return tmp;
    }

    HKEY
    RegistryKey::Handle() const noexcept
    {
        return m_hKey;
    }

    bool
    RegistryKey::IsValid() const noexcept
    {
        return (m_hKey != INVALID_HANDLE_VALUE) && (m_hKey != nullptr);
    }
}