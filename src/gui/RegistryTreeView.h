// RegistryTreeView.h
#pragma once

// Minimal UI wrapper for a TreeView control used to browse the Windows Registry.
// Header only; implementation will be in RegistryTreeView.cpp.
//
// Design notes (summary):
//  - All Win32 control calls must run on the UI thread.
//  - TV item -> full path mapping is kept in an internal map (HTREEITEM -> std::wstring).
//  - When a node expands, the handler enqueues a background task on IThreadManager.
//    The worker calls RegistryFacade::ListSubKeys(...) and posts an ExpandResult* to the
//    main window using PostMessage. The main window MUST forward that pointer to
//    RegistryTreeView::HandleExpandResult(ExpandResult*). UI thread will free ExpandResult.
//  - The component does NOT own the RegistryFacade or IThreadManager pointers; those are non-owning.

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <optional>
#include "Messages.h"
namespace core::registry
{
    struct RegValueRecord;
    class RegistryFacade;
}

class IThreadManager; // forward (your threadpool interface)

struct ValuesResult
{
    HKEY hiveRoot;
    std::wstring fullPath;             // path of the key whose values are listed
    std::vector<core::registry::RegValueRecord> values;
    LONG errorCode;                     // ERROR_SUCCESS or error
};


struct ExpandResult
{
    HTREEITEM parentItem;               // which tree item to populate (UI thread's HTREEITEM)
    HKEY       hiveRoot;                // HKEY_CURRENT_USER etc. - useful for re-querying or further ops
    std::wstring parentFullPath;        // full path to parent (e.g. L"Software\\MyApp")
    std::vector<std::wstring> children; // child subkey names (just names, not full paths)
    LSTATUS    errorCode;               // ERROR_SUCCESS on success, otherwise set
};

// RegistryTreeView: manages a TreeView control and lazy-loading of children via RegistryFacade.
// Implementation will rely on Win32 TreeView notifications (TVN_ITEMEXPANDING) and TreeView_InsertItem.
// See: TVN_ITEMEXPANDING docs and TreeView_InsertItem docs in the Win32 API. :contentReference[oaicite:5]{index=5}
class RegistryTreeView
{
public:
    RegistryTreeView() noexcept;

    RegistryTreeView(const RegistryTreeView&) = delete;
    RegistryTreeView& operator=(const RegistryTreeView&) = delete;

    ~RegistryTreeView();

    // Initialize the TreeView control as a child of parentWnd.
    // - parentWnd: HWND of the main window (UI thread)
    // - instance: HINSTANCE of the module (used for CreateWindowEx)
    // - threadManager: non-owning pointer to your IThreadManager (StdThreadPool) for background tasks
    // - facade: non-owning pointer to RegistryFacade for ListSubKeys() calls
    //
    // Returns true on success. Call from UI thread only.
    bool Initialize(HWND parentWnd, HINSTANCE instance, IThreadManager* threadManager, core::registry::RegistryFacade* facade);

    // Return HWND of the TreeView (UI thread).
    HWND Handle() const noexcept;

    // Insert top-level registry hive nodes (HKEY_CLASSES_ROOT, HKEY_CURRENT_USER, etc.)
    // This is a convenience; you can also insert arbitrary root nodes via InsertNode().
    // Call on UI thread.
    void PopulateHives();

    // Insert a node under 'parent'. name is the displayed label; fullPath is used for lookups (e.g. "Software\\MyApp").
    // If hasChildren==true, the implementation will add a single dummy child to show an expand glyph.
    // Returns the HTREEITEM inserted (or nullptr on failure). UI thread.
    HTREEITEM InsertNode(HTREEITEM parent, std::wstring const& name, std::wstring const& fullPath, HKEY hiveRoot, bool hasChildren);

    // Ask the tree wrapper to expand 'item' by enqueuing a background task which will
    // call RegistryFacade::ListSubKeys(hiveRoot, fullPath, ...) and post an ExpandResult.
    // This method is intended to be called on the UI thread in response to TVN_ITEMEXPANDING.
    void RequestExpand(HTREEITEM item) const;

    // Called by the UI thread when the main window receives WM_APP_TREE_EXPAND_RESULT and
    // has ownership of the ExpandResult pointer (wParam). This method must run on UI thread.
    // It inserts children under ExpandResult->parentItem and deletes the ExpandResult object.
    void HandleExpandResult(ExpandResult* result);

    // Optional: called by UI thread when an expand operation failed; result may be nullptr.
    // You can show an error message. The worker may PostMessage WM_APP_TREE_OP_ERROR with heap 'std::wstring*' (the message).
    void HandleOperationError(std::wstring* errorText) const;

    // Find the full registry path for an HTREEITEM. Returns std::nullopt if item unknown.
    // UI thread only.
    std::optional<std::wstring>GetItemPath(HTREEITEM item) const;

    // Cleanup all items and mappings. UI thread.
    void Clear() const;

    // Helper: forward WM_NOTIFY from parent to this control wrapper.
    // Should be called from MainWindow's WM_NOTIFY handler when pnmh->hwndFrom == Handle().
    // Implementation will inspect TVN_ITEMEXPANDING and call RequestExpand(...) as required.
    LRESULT HandleNotify(LPNMHDR pnmh) const;
    void UpdateColumnWidth() const;

private:
    HWND m_parentWnd;   // main window (for PostMessage targets and layout)
    HWND m_hwnd;        // tree-view control handle (SysTreeView32)
    HINSTANCE m_instance;
    IThreadManager* m_threadManager;                     // non-owning
    core::registry::RegistryFacade* m_facade;            // non-owning

    // Map to keep full path and hive for each HTREEITEM. Only accessed on UI thread.
    mutable std::unordered_map<HTREEITEM, std::wstring> m_itemPathMap;
    mutable std::unordered_map<HTREEITEM, HKEY> m_itemHiveMap;

    // If you want to be extra defensive (in debug), you can protect map with this mutex. UI thread access is expected.
    mutable std::mutex m_mapMutex;


    int CalculateMaxItemWidth() const;

    // Internal helpers (implementation private)
    void
    AddDummyChild(HTREEITEM parent);

    bool
    HasDummyChild(HTREEITEM parent) const;

    void
    RemoveDummyChild(HTREEITEM parent) const;

    // Insert item (UI thread) low-level helper that also registers item path/hive in maps.
    HTREEITEM
    InsertItemInternal(TVINSERTSTRUCTW const &tvins, std::wstring const &fullPath, HKEY hiveRoot) const;
};
