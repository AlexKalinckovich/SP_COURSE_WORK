#pragma once
#include <windows.h>

#include "RegistryTreeView.h"

class RegistryValuesView
{
public:
    RegistryValuesView() noexcept;
    ~RegistryValuesView();

    bool Initialize(HWND parentWnd, HINSTANCE hinst, UINT controlId);
    void HandleValuesResult(ValuesResult* vr) const;

    [[nodiscard]] HWND Handle() const noexcept;

private:
    HWND m_hwndList;  // e.g. a ListView control
};
