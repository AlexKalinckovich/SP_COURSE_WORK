// RegistryHelpers.h
#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include "RegistryKey.h"

namespace core::registry
{
struct RegValueRecord
{
    std::wstring name;
    DWORD type;
    std::vector<unsigned char> data;
};

std::wstring ReadStringValue(RegistryKey const& key, std::wstring const& valueName);

DWORD ReadDwordValue(RegistryKey const& key, std::wstring const& valueName);

unsigned long long ReadQwordValue(RegistryKey const& key, std::wstring const& valueName);

std::vector<unsigned char>ReadBinaryValue(RegistryKey const& key, std::wstring const& valueName);

void SetStringValue(RegistryKey const& key, std::wstring const& valueName, std::wstring const& data, DWORD regType = REG_SZ);

void SetDwordValue(RegistryKey const& key, std::wstring const& valueName, DWORD data);

void SetQwordValue(RegistryKey const& key, std::wstring const& valueName, unsigned long long data);

void SetBinaryValue(RegistryKey const& key, std::wstring const& valueName, std::vector<unsigned char> const& data);

std::vector<std::wstring>EnumerateSubKeys(RegistryKey const& key);

std::vector<RegValueRecord>EnumerateValues(RegistryKey const& key);

void DeleteValue(RegistryKey const& key, std::wstring const& valueName);

void DeleteSubKey(HKEY root, std::wstring const& subKey, REGSAM samDesired = 0);

bool IsValidRootKey(HKEY root);

void SaveKeyToFile(RegistryKey const& key, std::wstring const& filePath);

void RestoreKeyFromFile(RegistryKey const& key, std::wstring const& filePath, DWORD flags);

bool EnablePrivilege(std::wstring const& privilegeName, bool enable);

HANDLE CreateRegistryChangeEvent(RegistryKey const& key, bool watchSubtree, DWORD notifyFilter, BOOL fAsynchronous);

std::string FormatWinErrorMessage(LSTATUS code);


} // namespace core::registry
