#include "RegistryValuesView.h"
#include <commctrl.h>

#include "registry/RegistryHelpers.h"

RegistryValuesView::RegistryValuesView() noexcept
    : m_hwndList(nullptr)
{
}

RegistryValuesView::~RegistryValuesView()
{
    if (m_hwndList != nullptr)
    {
        DestroyWindow(m_hwndList);
        m_hwndList = nullptr;
    }
}

bool RegistryValuesView::Initialize(HWND parentWnd, HINSTANCE hinst, UINT controlId)
{
    // Create a ListView in report mode
    HWND lv = CreateWindowExW(
        0,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | WS_VSCROLL | WS_HSCROLL,
        0, 0, 0, 0,
        parentWnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
        hinst,
        nullptr);
    if (lv == nullptr)
    {
        return false;
    }
    m_hwndList = lv;

    // Insert columns: "Name", "Type", "Data"
    LVCOLUMNW col = {0};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 150;
    col.pszText = const_cast<LPWSTR>(L"Name");
    ListView_InsertColumn(m_hwndList, 0, &col);

    col.cx = 100;
    col.pszText = const_cast<LPWSTR>(L"Type");
    ListView_InsertColumn(m_hwndList, 1, &col);

    col.cx = 300;
    col.pszText = const_cast<LPWSTR>(L"Data");
    ListView_InsertColumn(m_hwndList, 2, &col);

    return true;
}

void RegistryValuesView::HandleValuesResult(ValuesResult* vr) const
{
    if (m_hwndList == nullptr || vr == nullptr)
    {
        return;
    }

    ListView_DeleteAllItems(m_hwndList);

    for (size_t i = 0; i < vr->values.size(); ++i)
    {
        const core::registry::RegValueRecord& rec = vr->values[i];
        LVITEMW item = {0};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);

        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(rec.name.c_str());
        ListView_InsertItem(m_hwndList, &item);

        wchar_t bufType[64] = {0};
        swprintf_s(bufType, L"%u", rec.type);
        ListView_SetItemText(m_hwndList, static_cast<int>(i), 1, bufType);

        std::wstring dataStr;
        if (rec.type == REG_SZ || rec.type == REG_EXPAND_SZ)
        {
            dataStr.assign(reinterpret_cast<const wchar_t*>(rec.data.data()));
        }
        else if (rec.type == REG_DWORD && rec.data.size() >= sizeof(DWORD))
        {
            DWORD dw = 0;
            memcpy(&dw, rec.data.data(), sizeof(DWORD));
            wchar_t buf[32];
            swprintf_s(buf, L"%u", dw);
            dataStr = buf;
        }
        else
        {
            wchar_t bufHex[3];
            for (const unsigned char byte : rec.data)
            {
                swprintf_s(bufHex, L"%02X", byte);
                dataStr += bufHex;
            }
        }

        ListView_SetItemText(m_hwndList, static_cast<int>(i), 2, const_cast<LPWSTR>(dataStr.c_str()));
    }
}

HWND RegistryValuesView::Handle() const noexcept
{
    return m_hwndList;
}
