// main.cpp (example)
#include <windows.h>
#include "../gui/MainWindow.h"
#include "StdThreadPool.h"      // your IThreadManager implementation
#include "../core/registry/RegistryFacade.h"

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrev*/, LPWSTR /*lpCmdLine*/, const int nCmdShow)
{
    // Create your thread pool (adjust constructor to your implementation)
    StdThreadPool pool(4); // assumes a constructor that takes thread count

    // Create registry facade (default config)
    core::registry::RegistryFacade facade;

    // Create and initialize the main window (UI thread).
    MainWindow wnd(hInstance, &pool, &facade);
    if (!wnd.Initialize(nCmdShow))
    {
        return -1;
    }

    // Run message loop (blocks here until WM_QUIT)
    return wnd.RunMessageLoop();
}
extern "C" int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    return wWinMain(hInstance, hPrevInstance, ::GetCommandLineW(), nCmdShow);
}