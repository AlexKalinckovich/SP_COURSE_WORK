// MainWindow.cpp
// Minimal MainWindow implementation that owns a RegistryTreeView, accepts
// worker-posted messages (WM_APP_TREE_EXPAND_RESULT / WM_APP_OPERATION_ERROR),
// and runs the Windows message loop.
//
// Assumptions:
//  - RegistryTreeView class and its ExpandResult / error posting contract exist.
//  - IThreadManager has an enqueue(std::function<void()>) method (adjust if yours is named differently).
//  - RegistryFacade has ListSubKeys(...) and is usable from background threads.

#include "MainWindow.h"
#include "RegistryTreeView.h"
#include "IThreadManager.h"
#include "../core/registry/RegistryFacade.h"


#include <windows.h>
#include <commctrl.h>
#include <cassert>
#include <iostream>

// Window class name used for RegisterClassEx / CreateWindowEx
static const wchar_t MAIN_WNDCLASS_NAME[] = L"RegistryEditor.MainWindow";

// -------------------- Helpers: set/get 'this' pointer on HWND --------------------
void
MainWindow::SetThisPtr(HWND hwnd, MainWindow* self)
{
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
}

MainWindow*
MainWindow::GetThisPtr(HWND hwnd) noexcept
{
    LONG_PTR ptr = GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    return reinterpret_cast<MainWindow*>(ptr);
}

// -------------------- Register window class --------------------
bool
MainWindow::RegisterWindowClass() const
{
    WNDCLASSEXW wc;
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &MainWindow::StaticWndProc;
    wc.cbClsExtra    = 0;
    wc.cbWndExtra    = 0;
    wc.hInstance     = m_hInstance;
    wc.hIcon         = nullptr;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszMenuName  = nullptr;
    wc.lpszClassName = MAIN_WNDCLASS_NAME;
    wc.hIconSm       = nullptr;

    ATOM atom = RegisterClassExW(&wc);
    if (atom == 0)
    {
        DWORD err = GetLastError();
        std::cerr << "RegisterClassExW failed, error=" << err << "\n";
        return false;
    }
    return true;
}

// -------------------- Create child controls (tree only for minimal GUI) --------------------
bool
MainWindow::CreateChildControls()
{
    // Create RegistryTreeView and initialize it.
    m_tree = std::make_unique<RegistryTreeView>();

    bool ok = m_tree->Initialize(m_hwnd, m_hInstance, m_threadManager, m_facade);
    if (ok == false)
    {
        std::cerr << "RegistryTreeView::Initialize failed\n";
        m_tree.reset();
        return false;
    }

    // Populate top-level hive nodes (HKEY_CURRENT_USER, etc).
    m_tree->PopulateHives();

    return true;
}

// -------------------- Layout children (simple full-client tree) --------------------
void
MainWindow::LayoutChildren(int width, int height)
{
    if (m_tree != nullptr && m_tree->Handle() != nullptr)
    {
        MoveWindow(m_tree->Handle(), 0, 0, width, height, TRUE);
        m_tree->UpdateColumnWidth();
    }
}

// -------------------- Initialize / Create window --------------------
MainWindow::MainWindow(HINSTANCE hInstance, IThreadManager* threadManager, core::registry::RegistryFacade* facade)
    : m_hInstance(hInstance)
    , m_hwnd(nullptr)
    , m_tree(nullptr)
    , m_threadManager(threadManager)
    , m_facade(facade)
{
}

MainWindow::~MainWindow()
{
    // Ensure child wrapper is destroyed on UI thread (expected).
    m_tree.reset();
}

bool
MainWindow::Initialize(int nCmdShow)
{
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icc.dwICC  = ICC_TREEVIEW_CLASSES;
    BOOL icc_ok = InitCommonControlsEx(&icc);
    if (icc_ok == FALSE)
    {
        const DWORD err = GetLastError();
        std::cerr << "InitCommonControlsEx failed error=" << err << "\n";
    }

    if (!RegisterWindowClass())
    {
        return false;
    }

    m_hwnd = CreateWindowExW(0,
                             MAIN_WNDCLASS_NAME,
                             L"Registry Editor - Minimal",
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT,
                             CW_USEDEFAULT,
                             800,
                             600,
                             nullptr,
                             nullptr,
                             m_hInstance,
                             this); // lpParam

    if (m_hwnd == nullptr)
    {
        const DWORD err = GetLastError();
        std::cerr << "CreateWindowExW failed error=" << err << "\n";
        return false;
    }

    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);

    return true;
}

// -------------------- Message loop --------------------
int
MainWindow::RunMessageLoop()
{
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

// -------------------- WM_NOTIFY forwarder --------------------
LRESULT
MainWindow::HandleNotify(LPNMHDR pnmh)
{
    if (pnmh == nullptr)
    {
        return 0;
    }

    // If the notification originates from our tree control, forward to wrapper.
    if (m_tree != nullptr && pnmh->hwndFrom == m_tree->Handle())
    {
        return m_tree->HandleNotify(pnmh);
    }

    // No special handling; return 0.
    return 0;
}

// -------------------- Handle worker-posted WM_APP_* messages --------------------
LRESULT
MainWindow::HandleAppMessage(UINT msg, WPARAM wParam, LPARAM /*lParam*/) const
{
    switch (msg)
    {
        case WM_APP_UPDATE_COLUMN_WIDTH:
        {
            if (m_tree) {
                m_tree->UpdateColumnWidth();
            }
            return 0;
        }
        case WM_APP_TREE_EXPAND_RESULT:
        {
            ExpandResult* res = reinterpret_cast<ExpandResult*>(wParam);
            if (res != nullptr && m_tree != nullptr)
            {
                m_tree->HandleExpandResult(res); // UI thread; this deletes res
            }
            else
            {
                // Defensive cleanup if tree missing
                if (res != nullptr)
                {
                    delete res;
                }
            }
            return 0;
        }

        case WM_APP_OPERATION_ERROR:
        {
            auto* err = reinterpret_cast<std::wstring*>(wParam);
            if (err != nullptr)
            {
                if (m_tree != nullptr)
                {
                    m_tree->HandleOperationError(err);
                }
                else
                {
                    // Fallback: show message and free
                    MessageBoxW(m_hwnd, err->c_str(), L"Error", MB_OK | MB_ICONERROR);
                    delete err;
                }
            }
            return 0;
        }

        default:
            break;
    }

    return 0;
}

// -------------------- Instance WndProc handlers --------------------
LRESULT
MainWindow::OnCreate(HWND hwnd, LPCREATESTRUCTW /*createStruct*/)
{
    // Associate this HWND with the MainWindow instance
    SetThisPtr(hwnd, this);
    m_hwnd = hwnd;

    // Create child controls (RegistryTreeView)
    if (!CreateChildControls())
    {
        return -1; // abort window creation
    }

    return 0;
}

LRESULT
MainWindow::OnSize(int width, int height)
{
    LayoutChildren(width, height);
    return 0;
}

LRESULT
MainWindow::OnDestroy()
{
    PostQuitMessage(0);
    return 0;
}

// -------------------- Static WndProc --------------------
LRESULT CALLBACK
MainWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_CREATE)
    {
        const auto pcs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        const auto self = static_cast<MainWindow*>(pcs->lpCreateParams);
        if (self != nullptr)
        {
            SetThisPtr(hwnd, self);
            return self->OnCreate(hwnd, pcs);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    MainWindow* self = GetThisPtr(hwnd);
    if (self == nullptr)
    {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg)
    {
        case WM_SIZE:
        {
            const int width  = LOWORD(lParam);
            const int height = HIWORD(lParam);
            return self->OnSize(width, height);
        }

        case WM_NOTIFY:
        {
            LPNMHDR pnmh = reinterpret_cast<LPNMHDR>(lParam);
            return self->HandleNotify(pnmh);
        }

        case WM_APP_TREE_EXPAND_RESULT:
        case WM_APP_OPERATION_ERROR:
        {
            return self->HandleAppMessage(static_cast<UINT>(msg), wParam, lParam);
        }

        case WM_DESTROY:
        {
            return self->OnDestroy();
        }

        default:
            break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
