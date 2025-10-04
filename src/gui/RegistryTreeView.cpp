
#include "RegistryTreeView.h"
#include <cassert>
#include <commctrl.h>
#include <new>
#include <sstream>
#include <windows.h>
#include "IThreadManager.h"   // thread pool interface (assumed enqueue(std::function<void()>))
#include "../core/registry/RegistryFacade.h"

static constexpr LPARAM DUMMY_CHILD_LPARAM = static_cast<LPARAM>(1);

static bool
IsTreeItemDummy(HWND treeHwnd, HTREEITEM item);

static void
SetTreeItemParam(HWND treeHwnd, HTREEITEM item, LPARAM param);

static LPARAM
GetTreeItemParam(HWND treeHwnd, HTREEITEM item);

// -------------------- Construction / Destruction --------------------

RegistryTreeView::RegistryTreeView() noexcept
    : m_parentWnd(nullptr)
    , m_hwnd(nullptr)
    , m_instance(nullptr)
    , m_threadManager(nullptr)
    , m_facade(nullptr)
{
}

RegistryTreeView::~RegistryTreeView()
{
    if (m_hwnd != nullptr)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}


bool
RegistryTreeView::Initialize(HWND parentWnd, HINSTANCE instance, IThreadManager* threadManager, core::registry::RegistryFacade* facade)
{
    m_parentWnd = parentWnd;
    m_instance = instance;
    m_threadManager = threadManager;
    m_facade = facade;

    RECT rcClient;
    int width;
    int height;
    if (GetClientRect(m_parentWnd, &rcClient) != 0)
    {
        width = rcClient.right - rcClient.left;
        height = rcClient.bottom - rcClient.top;

    }
    else
    {
        width = 800;
        height = 600;
    }

    HWND tree = CreateWindowExW(
        0,
        WC_TREEVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN
            | WS_VSCROLL | WS_HSCROLL
            | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS,
        0,
        0,
        width,
        height,
        m_parentWnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(1001)),
        m_instance,
        nullptr);

    if (tree == nullptr)
    {
        const DWORD err = GetLastError();
        (void)err;
        return false;
    }

    m_hwnd = tree;

    const LONG style = GetWindowLong(m_hwnd, GWL_STYLE);
    SetWindowLong(m_hwnd, GWL_STYLE, style | WS_HSCROLL);

    UpdateColumnWidth();

    return true;
}


void RegistryTreeView::UpdateColumnWidth() const
{
    if (m_hwnd == nullptr) return;

    const int maxWidth = CalculateMaxItemWidth();

    TreeView_SetScrollTime(m_hwnd, maxWidth);

    TreeView_SetItemHeight(m_hwnd, 20);

    InvalidateRect(m_hwnd, nullptr, TRUE);
    UpdateWindow(m_hwnd);
}

int RegistryTreeView::CalculateMaxItemWidth() const
{
    if (m_hwnd == nullptr) return 800;

    HDC hdc = GetDC(m_hwnd);
    if (hdc == nullptr) return 800;

    const auto hFont = reinterpret_cast<HFONT>(SendMessage(m_hwnd, WM_GETFONT, 0, 0));
    const auto hOldFont = static_cast<HFONT>(SelectObject(hdc, hFont));

    int maxWidth = 0;

    std::lock_guard guard(m_mapMutex);
    for (const auto& pair : m_itemPathMap) {
        HTREEITEM item = pair.first;

        wchar_t buffer[256] = {0};
        TVITEMW tvi;
        ZeroMemory(&tvi, sizeof(TVITEMW));
        tvi.mask = TVIF_TEXT;
        tvi.hItem = item;
        tvi.pszText = buffer;
        tvi.cchTextMax = 255;

        if (TreeView_GetItem(m_hwnd, &tvi)) {
            SIZE size = {0};
            if (GetTextExtentPoint32W(hdc, buffer, wcslen(buffer), &size)) {
                maxWidth = std::max(maxWidth, static_cast<int>(size.cx));
            }
        }
    }

    SelectObject(hdc, hOldFont);
    ReleaseDC(m_hwnd, hdc);

    return maxWidth + 100;
}


HWND RegistryTreeView::Handle() const noexcept
{
    return m_hwnd;
}


void RegistryTreeView::PopulateHives()
{
    InsertNode(nullptr, L"HKEY_CLASSES_ROOT", std::wstring(), HKEY_CLASSES_ROOT, true);
    InsertNode(nullptr, L"HKEY_CURRENT_USER", std::wstring(), HKEY_CURRENT_USER, true);
    InsertNode(nullptr, L"HKEY_LOCAL_MACHINE", std::wstring(), HKEY_LOCAL_MACHINE, true);
    InsertNode(nullptr, L"HKEY_USERS", std::wstring(), HKEY_USERS, true);
    InsertNode(nullptr, L"HKEY_CURRENT_CONFIG", std::wstring(), HKEY_CURRENT_CONFIG, true);
}

HTREEITEM
RegistryTreeView::InsertNode(HTREEITEM parent, std::wstring const& name, std::wstring const& fullPath, HKEY hiveRoot, bool hasChildren)
{
    TVINSERTSTRUCTW tvins;
    ZeroMemory(&tvins, sizeof(TVINSERTSTRUCTW));
    tvins.hParent = parent;
    tvins.hInsertAfter = TVI_LAST;

    TVITEMW item;
    ZeroMemory(&item, sizeof(TVITEMW));
    item.mask = TVIF_TEXT | TVIF_PARAM;
    item.pszText = const_cast<LPWSTR>(name.c_str());
    item.lParam = 0; // default param; we may override for dummy child
    item.cchTextMax = 255;
    tvins.item = item;

    auto inserted = reinterpret_cast<HTREEITEM>(SendMessageW(m_hwnd, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&tvins)));

    if (inserted == nullptr)
    {
        return nullptr;
    }

    wchar_t buf[512];
    ZeroMemory(buf, sizeof(buf));
    TVITEMW tvi;
    ZeroMemory(&tvi, sizeof(tvi));
    tvi.mask = TVIF_TEXT;
    tvi.hItem = inserted;
    tvi.pszText = buf;
    tvi.cchTextMax = static_cast<int>(sizeof(buf) / sizeof(wchar_t)) - 1;

    if (TreeView_GetItem(m_hwnd, &tvi))
    {
        wchar_t dbg[600];
        swprintf_s(dbg, L"[InsertNode] inserted item %p text = \"%s\" fullPath = \"%s\"\n",
                   inserted, buf, fullPath.c_str());
        OutputDebugStringW(dbg);
    }

    {
        std::lock_guard guard(m_mapMutex);
        m_itemPathMap.insert(std::make_pair(inserted, fullPath));
        m_itemHiveMap.insert(std::make_pair(inserted, hiveRoot));
        PostMessage(m_parentWnd, WM_APP_UPDATE_COLUMN_WIDTH, 0, 0);
    }

    if (hasChildren)
    {
        AddDummyChild(inserted);
    }

    return inserted;
}

void
RegistryTreeView::RequestExpand(HTREEITEM item) const
{
    if (item == nullptr)
    {
        return;
    }

    std::wstring parentPath;
    HKEY hiveRoot = nullptr;

    {
        std::lock_guard guard(m_mapMutex);
        const auto itPath = m_itemPathMap.find(item);
        if (itPath != m_itemPathMap.end())
        {
            parentPath = itPath->second;
        }
        else
        {
            parentPath = std::wstring(); // empty => root of hive
        }

        auto itHive = m_itemHiveMap.find(item);
        if (itHive != m_itemHiveMap.end())
        {
            hiveRoot = itHive->second;
        }
        else
        {
            hiveRoot = HKEY_CURRENT_USER;
        }
    }

    if (m_threadManager == nullptr || m_facade == nullptr)
    {
        return;
    }

    if (!HasDummyChild(item))
    {
        return;
    }

    HWND uiWnd = m_parentWnd;
    HKEY rootCopy = hiveRoot;
    std::wstring pathCopy = parentPath;
    HTREEITEM parentCopy = item;
    core::registry::RegistryFacade* facadeCopy = m_facade;

    ExpandResult* res = nullptr;
    try
    {
        std::vector<std::wstring> names = facadeCopy->ListSubKeys(rootCopy, pathCopy, KEY_READ, core::registry::RegistryFacade::ListOptions{});

        // Allocate result on heap for PostMessage handoff; UI thread will delete.
        res = new (std::nothrow) ExpandResult();
        if (res == nullptr)
        {
            // allocation failed; nothing we can do but bail.
            return;
        }

        res->parentItem = parentCopy;
        res->hiveRoot = rootCopy;
        res->parentFullPath = pathCopy;
        res->children = std::move(names);
        res->errorCode = ERROR_SUCCESS;

        BOOL posted = PostMessageW(uiWnd, WM_APP_TREE_EXPAND_RESULT, reinterpret_cast<WPARAM>(res), 0);
        if (posted == FALSE)
        {
            delete res;
            res = nullptr;
        }
    }
    catch (const std::exception& ex)
    {
        std::wstring* err = new (std::nothrow) std::wstring();
        if (err != nullptr)
        {
            std::wstring msg = std::wstring(L"Failed to list subkeys: ") + std::wstring(ex.what(), ex.what() + strlen(ex.what()));
            *err = msg;
            BOOL postedErr = PostMessageW(uiWnd, WM_APP_TREE_OP_ERROR, reinterpret_cast<WPARAM>(err), 0);
            if (postedErr == FALSE)
            {
                delete err;
            }
        }
    }
    // m_threadManager->enqueue([uiWnd, rootCopy, pathCopy, parentCopy, facadeCopy]()
    // {
    //     // This lambda runs on a worker thread.
    // });

}

void RegistryTreeView::HandleExpandResult(ExpandResult* result)
{
    if (result == nullptr)
    {
        return;
    }

    HTREEITEM parent = result->parentItem;

    if (HasDummyChild(parent))
    {
        RemoveDummyChild(parent);
    }

    for (std::size_t i = 0; i < result->children.size(); ++i)
    {
        std::wstring const& childName = result->children[i];

        std::wstring childFullPath;
        if (result->parentFullPath.empty())
        {
            childFullPath = childName;
        }
        else
        {
            childFullPath = result->parentFullPath + L"\\" + childName;
        }

        HTREEITEM newItem = InsertNode(parent, childName, childFullPath, result->hiveRoot, true);
        (void)newItem;
    }

    delete result;
}

void RegistryTreeView::HandleOperationError(std::wstring* errorText) const
{
    if (errorText == nullptr)
    {
        return;
    }

    MessageBoxW(m_parentWnd, errorText->c_str(), L"Registry operation error", MB_OK | MB_ICONERROR);

    delete errorText;
}

std::optional<std::wstring>
RegistryTreeView::GetItemPath(HTREEITEM item) const
{
    if (item == nullptr)
    {
        return {};
    }

    std::lock_guard guard(m_mapMutex);
    auto it = m_itemPathMap.find(item);
    if (it == m_itemPathMap.end())
    {
        return {};
    }
    return {it->second};
}

void RegistryTreeView::Clear() const
{
    if (m_hwnd != nullptr)
    {
        TreeView_DeleteAllItems(m_hwnd);
    }

    std::lock_guard guard(m_mapMutex);
    m_itemPathMap.clear();
    m_itemHiveMap.clear();
}

LRESULT RegistryTreeView::HandleNotify(LPNMHDR pnmh) const
{

    if (pnmh == nullptr)
    {
        return 0;
    }

    if (pnmh->hwndFrom != m_hwnd)
    {
        return 0;
    }



    switch (pnmh->code)
    {
        case TVN_SELCHANGEDA:
        case TVN_SELCHANGEDW:
        {
            LPNMTREEVIEWW pTree = reinterpret_cast<LPNMTREEVIEWW>(pnmh);
            if (pTree != nullptr)
            {
                HTREEITEM newItem = pTree->itemNew.hItem;
                if (newItem != nullptr)
                {
                    const std::optional<std::wstring> maybePath = GetItemPath(newItem);
                    if (maybePath.has_value())
                    {
                        HKEY hiveRoot = HKEY_CURRENT_USER;
                        {
                            std::lock_guard guard(m_mapMutex);
                            auto it = m_itemHiveMap.find(newItem);
                            if (it != m_itemHiveMap.end())
                            {
                                hiveRoot = it->second;
                            }
                        }

                        struct SelMsg { HKEY root; std::wstring path; };
                        SelMsg* msg = new (std::nothrow) SelMsg{ hiveRoot, maybePath.value() };
                        if (msg != nullptr)
                        {

                            PostMessageW(m_parentWnd,
                                         WM_APP_SELECTION_CHANGED,
                                         reinterpret_cast<WPARAM>(msg),
                                         0);
                        }
                    }
                }
            }
            return 0;
        }

        case TVN_ITEMEXPANDING:
        {
            LPNMTREEVIEWW pTree = reinterpret_cast<LPNMTREEVIEWW>(pnmh);
            if (pTree != nullptr)
            {
                const UINT action = pTree->action;
                if ((action & TVE_EXPAND) != 0)
                {
                    HTREEITEM itemToExpand = pTree->itemNew.hItem;
                    if (itemToExpand != nullptr)
                    {
                        RequestExpand(itemToExpand);
                    }
                }
            }
            return 0;
        }

        case TVN_ITEMEXPANDED:
        {
            LPNMTREEVIEWW pTree = reinterpret_cast<LPNMTREEVIEWW>(pnmh);
            if (pTree != nullptr)
            {
                if ((pTree->action & TVE_EXPAND) != 0)
                {
                    HTREEITEM item = pTree->itemNew.hItem;
                    if (item != nullptr)
                    {
                        RequestExpand(item);
                    }
                }
            }
            return 0;
        }

        default:
        {
            {
                wchar_t buf[128];
                wsprintfW(buf, L"[RegistryTreeView] Unknown notify code: %d (0x%X)\n", pnmh->code, pnmh->code);
                OutputDebugStringW(buf);
            }
            break;
        }
    }


    return 0;
}

void RegistryTreeView::AddDummyChild(HTREEITEM parent)
{
    TVINSERTSTRUCTW tvins;
    ZeroMemory(&tvins, sizeof(TVINSERTSTRUCTW));
    tvins.hParent = parent;
    tvins.hInsertAfter = TVI_LAST;

    TVITEMW item;
    ZeroMemory(&item, sizeof(TVITEMW));
    item.mask = TVIF_TEXT | TVIF_PARAM;
    item.pszText = const_cast<LPWSTR>(L"");
    item.lParam = DUMMY_CHILD_LPARAM;

    tvins.item = item;

    HTREEITEM child = TreeView_InsertItem(m_hwnd, &tvins);
    (void)child;
}

bool RegistryTreeView::HasDummyChild(HTREEITEM parent) const
{
    HTREEITEM firstChild = TreeView_GetChild(m_hwnd, parent);
    if (firstChild == nullptr)
    {
        return false;
    }

    TVITEMW tvi;
    ZeroMemory(&tvi, sizeof(TVITEMW));
    tvi.mask = TVIF_PARAM;
    tvi.hItem = firstChild;

    BOOL got = TreeView_GetItem(m_hwnd, &tvi);
    if (got == FALSE)
    {
        return false;
    }

    if (tvi.lParam == DUMMY_CHILD_LPARAM)
    {
        return true;
    }
    return false;
}

void RegistryTreeView::RemoveDummyChild(HTREEITEM parent) const
{
    HTREEITEM firstChild = TreeView_GetChild(m_hwnd, parent);
    if (firstChild == nullptr)
    {
        return;
    }

    TVITEMW tvi;
    ZeroMemory(&tvi, sizeof(TVITEMW));
    tvi.mask = TVIF_PARAM;
    tvi.hItem = firstChild;

    BOOL got = TreeView_GetItem(m_hwnd, &tvi);
    if (got == FALSE)
    {
        return;
    }

    if (tvi.lParam == DUMMY_CHILD_LPARAM)
    {
        TreeView_DeleteItem(m_hwnd, firstChild);
    }
}

HTREEITEM RegistryTreeView::InsertItemInternal(TVINSERTSTRUCTW const &tvins, std::wstring const &fullPath, HKEY hiveRoot) const
{
    TVINSERTSTRUCTW insCopy = tvins;
    HTREEITEM inserted = TreeView_InsertItem(m_hwnd, &insCopy);
    if (inserted != nullptr)
    {
        std::lock_guard guard(m_mapMutex);
        m_itemPathMap.insert(std::make_pair(inserted, fullPath));
        m_itemHiveMap.insert(std::make_pair(inserted, hiveRoot));
    }
    return inserted;
}
