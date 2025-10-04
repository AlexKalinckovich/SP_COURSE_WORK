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
bool MainWindow::CreateChildControls()
{
    m_tree = std::make_unique<RegistryTreeView>();
    m_valuesView = std::make_unique<RegistryValuesView>();
    bool ok = m_tree->Initialize(m_hwnd, m_hInstance, m_threadManager, m_facade);
    if (ok == false)
    {
        std::cerr << "RegistryTreeView::Initialize failed\n";
        m_tree.reset();
        return false;
    }

    ok = m_valuesView->Initialize(m_hwnd, m_hInstance, 1002);
    m_tree->PopulateHives();

    return true;
}

// -------------------- Layout children (simple full-client tree) --------------------
void MainWindow::LayoutChildren(const int width, const int height) const
{
    const int half = width / 2;
    if (m_tree != nullptr && m_tree->Handle() != nullptr)
    {
        MoveWindow(m_tree->Handle(), 0, 0, half, height, TRUE);
    }
    if (m_valuesView != nullptr && m_valuesView->Handle() != nullptr)
    {
        MoveWindow(m_valuesView->Handle(), half, 0, width - half, height, TRUE);
    }
}


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

int MainWindow::RunMessageLoop()
{
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT MainWindow::HandleNotify(LPNMHDR pnmh) const
{
    if (pnmh == nullptr)
    {
        return 0;
    }

    if (m_tree != nullptr && pnmh->hwndFrom == m_tree->Handle())
    {
        return m_tree->HandleNotify(pnmh);
    }

    return 0;
}

LRESULT MainWindow::HandleAppMessage(const UINT msg, WPARAM wParam, LPARAM /*lParam*/) const
{
    switch (msg)
    {
        case WM_APP_SELECTION_CHANGED:
        {
            struct SelMsg { HKEY root; std::wstring path; };
            const auto sel = reinterpret_cast<SelMsg*>(wParam);
            if (sel != nullptr)
            {
                HKEY hiveRoot = sel->root;
                const std::wstring keyPath = sel->path;
                delete sel;

                IThreadManager* tm = m_threadManager;
                const core::registry::RegistryFacade* facade = m_facade;
                HWND uiWnd = m_hwnd;
                auto vr = new (std::nothrow) ValuesResult();
                if (vr == nullptr)
                {
                    return 1;
                }

                vr->hiveRoot = hiveRoot;
                vr->fullPath = keyPath;

                try
                {
                    vr->values = facade->ListValues(hiveRoot, keyPath, KEY_READ, core::registry::RegistryFacade::ListOptions{});
                    vr->errorCode = ERROR_SUCCESS;
                }
                catch (...)
                {
                    vr->errorCode = ERROR_ACCESS_DENIED;
                }

                PostMessageW(uiWnd,
                             WM_APP_LIST_VALUES_RESULT,
                             reinterpret_cast<WPARAM>(vr),
                             0);
                // tm->enqueue([uiWnd, hiveRoot, keyPath, facade]()
                // {
                //
                // });
            }
            return 0;
        }

        case WM_APP_LIST_VALUES_RESULT:
        {
            ValuesResult* vr = reinterpret_cast<ValuesResult*>(wParam);
            if (vr != nullptr)
            {
                if (m_valuesView != nullptr)
                {
                    m_valuesView->HandleValuesResult(vr);
                }
                delete vr;
            }
            return 0;
        }

        case WM_APP_UPDATE_COLUMN_WIDTH:
        {
            if (m_tree) {
                m_tree->UpdateColumnWidth();
            }
            return 0;
        }
        case WM_APP_TREE_EXPAND_RESULT:
        {
            const auto res = reinterpret_cast<ExpandResult*>(wParam);
            if (res != nullptr && m_tree != nullptr)
            {
                m_tree->HandleExpandResult(res);
            }
            else
            {
                delete res;
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
LRESULT MainWindow::OnCreate(HWND hwnd, LPCREATESTRUCTW /*createStruct*/)
{
    SetThisPtr(hwnd, this);
    m_hwnd = hwnd;

    if (!CreateChildControls())
    {
        return -1;
    }

    return 0;
}

LRESULT MainWindow::OnSize(const int width, const int height) const
{
    LayoutChildren(width, height);
    return 0;
}

LRESULT MainWindow::OnDestroy()
{
    PostQuitMessage(0);
    return 0;
}

LRESULT CALLBACK MainWindow::StaticWndProc(HWND hwnd, const UINT msg, const WPARAM wParam, const LPARAM lParam)
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
        case WM_APP_SELECTION_CHANGED:
        case WM_APP_LIST_VALUES_RESULT:
        {
            return self->HandleAppMessage(msg, wParam, lParam);
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
