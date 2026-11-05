#include "actions.h"

#include "cards.h"

#include <exdisp.h>
#include <shobjidl.h>
#include <thread>

namespace rd {

bool ExplorerInFront()
{
    wstring cls = ClassOf(GetForegroundWindow());
    return cls == L"Progman" || cls == L"WorkerW" || cls == L"CabinetWClass" || cls == L"ExploreWClass";
}

std::vector<wstring> ExplorerSelection()
{
    std::vector<wstring> out;
    HWND fg = GetForegroundWindow();
    wstring cls = ClassOf(fg);
    bool desktop = cls == L"Progman" || cls == L"WorkerW";
    if (!ExplorerInFront()) return out;

    Com<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(windows.Put())))) return out;
    auto selectionOf = [&](IDispatch* disp) -> bool {
        Com<IServiceProvider> sp;
        Com<IShellBrowser> browser;
        Com<IShellView> view;
        Com<IFolderView2> fv;
        Com<IShellItemArray> sel;
        if (FAILED(disp->QueryInterface(IID_PPV_ARGS(sp.Put()))) ||
            FAILED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(browser.Put()))) ||
            FAILED(browser->QueryActiveShellView(view.Put())) || FAILED(view->QueryInterface(IID_PPV_ARGS(fv.Put())))) return false;
        // Windows 11 has tabs: one window, several views. The visible one is the tab in front.
        HWND vw = nullptr;
        if (!desktop && SUCCEEDED(view->GetWindow(&vw)) && !IsWindowVisible(vw)) return false;
        if (FAILED(fv->GetSelection(FALSE, sel.Put())) || !sel) return true;
        DWORD n = 0;
        sel->GetCount(&n);
        for (DWORD i = 0; i < n; i++) {
            Com<IShellItem> it;
            PWSTR p = nullptr;
            if (SUCCEEDED(sel->GetItemAt(i, it.Put())) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) out.push_back(p);
            CoTaskMemFree(p);
        }
        return true;
    };
    if (desktop) {
        VARIANT empty{};
        long hwnd = 0;
        Com<IDispatch> disp;
        if (SUCCEEDED(windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, disp.Put())) && disp) selectionOf(disp.Get());
        return out;
    }
    long count = 0;
    windows->get_Count(&count);
    for (long i = 0; i < count; i++) {
        VARIANT v{};
        v.vt = VT_I4;
        v.lVal = i;
        Com<IDispatch> disp;
        Com<IWebBrowserApp> app;
        SHANDLE_PTR h = 0;
        if (FAILED(windows->Item(v, disp.Put())) || !disp || FAILED(disp->QueryInterface(IID_PPV_ARGS(app.Put()))) ||
            FAILED(app->get_HWND(&h)) || HWND(h) != fg) continue;
        if (selectionOf(disp.Get())) break;
    }
    return out;
}

}  // namespace rd
