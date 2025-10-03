// MainWindow.h
#pragma once

#include <windows.h>
#include <commctrl.h>
#include <memory>
#include "Messages.h"

class IThreadManager;
namespace core::registry { class RegistryFacade; }
class RegistryTreeView;

class MainWindow
{
public:
    // Construct with non-owning pointers to thread manager and facade.
    // Must be created on the UI thread (where the window will live).
    MainWindow(HINSTANCE hInstance, IThreadManager* threadManager, core::registry::RegistryFacade* facade);

    // Non-copyable
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    ~MainWindow();

    // Initialize the MainWindow and child UI skeleton. Must be called on UI thread.
    // Returns true on success. This registers the window class and creates the main HWND.
    bool
    Initialize(int nCmdShow);

    // Return the main window HWND (UI thread).
    [[nodiscard]] HWND Handle() const noexcept;

    // Run the standard Windows message loop. This call blocks until WM_QUIT.
    // Must be called on the UI thread after Initialize.
    int RunMessageLoop();

    // WM_NOTIFY handler: forward notifications from child common controls
    // (e.g., TreeView TVN_ITEMEXPANDING) into our instance methods.
    // This should be invoked by the static WndProc when a WM_NOTIFY arrives.
    LRESULT
    HandleNotify(LPNMHDR pnmh);

    // Forward worker-posted custom messages (WM_APP_*) to child components.
    // Should be invoked from WndProc for WM_APP ... messages.
    LRESULT
    HandleAppMessage(UINT msg, WPARAM wParam, LPARAM lParam) const;

    // Return pointer to the internal RegistryTreeView instance (non-owning).
    [[nodiscard]] RegistryTreeView* GetTreeView() const noexcept;

private:
    HINSTANCE m_hInstance;
    HWND m_hwnd;                      // main window handle
    std::unique_ptr<RegistryTreeView> m_tree; // owned child control wrapper

    IThreadManager* m_threadManager;  // non-owning
    core::registry::RegistryFacade* m_facade; // non-owning

    // Register and unregister: RegisterClassExW / UnregisterClassW
    [[nodiscard]] bool
    RegisterWindowClass() const;

    // Create child controls (tree + placeholder list). Called from OnCreate.
    bool
    CreateChildControls();

    // Layout child controls on WM_SIZE (simple split).
    void
    LayoutChildren(int width, int height);

    // Static window procedure - routes messages to instance.
    static LRESULT CALLBACK
    StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // Instance WndProc implementations (UI thread).
    LRESULT
    OnCreate(HWND hwnd, LPCREATESTRUCTW createStruct);

    LRESULT
    OnSize(int width, int height);

    LRESULT
    OnDestroy();

    // Helper to set/get 'this' pointer into HWND userdata (GWLP_USERDATA)
    static void
    SetThisPtr(HWND hwnd, MainWindow* self);

    static MainWindow*
    GetThisPtr(HWND hwnd) noexcept;
};
