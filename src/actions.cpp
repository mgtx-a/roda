#include "actions.h"

#include "cards.h"

#include <exdisp.h>
#include <shobjidl.h>
#include <thread>

namespace rd {

namespace {

wstring Plural(size_t n, const wchar_t* one, const wchar_t* many) { return std::to_wstring(n) + L" " + (n == 1 ? one : many); }

/// The folder chosen in a picker, or empty.
wstring PickFolder(const wchar_t* title)
{
    Com<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.Put())))) return {};
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(title);
    BeginModal();   // the picker runs its own loop; keep Roda's animations going
    HRESULT hr = dlg->Show(nullptr);
    EndModal();
    if (FAILED(hr)) return {};
    Com<IShellItem> item;
    PWSTR p = nullptr;
    wstring out;
    if (SUCCEEDED(dlg->GetResult(item.Put())) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

void Remember(Settings& s, const wstring& folder)
{
    auto& r = s.recentFolders;
    r.erase(std::remove_if(r.begin(), r.end(), [&](const wstring& f) { return SamePath(f, folder); }), r.end());
    r.insert(r.begin(), folder);
    if (r.size() > 3) r.resize(3);
    s.Save();
}

Com<IShellItemArray> ItemsFor(const std::vector<wstring>& paths)
{
    std::vector<PIDLIST_ABSOLUTE> ids;
    for (auto& p : paths) if (auto id = ILCreateFromPathW(p.c_str())) ids.push_back(id);
    Com<IShellItemArray> arr;
    if (!ids.empty()) SHCreateShellItemArrayFromIDLists(UINT(ids.size()), (LPCITEMIDLIST*)ids.data(), arr.Put());
    for (auto id : ids) ILFree(id);
    return arr;
}

}  // namespace

void CopyOrMove(const std::vector<wstring>& paths, wstring folder, bool move, Settings& settings, Cards& cards)
{
    if (folder.empty()) folder = PickFolder(move ? L"Move to" : L"Copy to");
    if (folder.empty()) { Log(L"actions: %s cancelled in the folder picker", move ? L"move" : L"copy"); return; }
    if (!DirExists(folder)) { cards.Note(move ? L"Move to" : L"Copy to", L"That folder is gone: " + folder, true); return; }
    Remember(settings, folder);
    Log(L"actions: %s %d item(s) to %s", move ? L"moving" : L"copying", int(paths.size()), folder.c_str());
    // IFileOperation shows Explorer's progress and conflict dialogs and blocks while it works,
    // so it runs on a thread of its own (an STA, as it needs).
    std::thread([paths, folder, move, &cards] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        HRESULT hr = E_FAIL;
        BOOL aborted = FALSE;
        {
            Com<IFileOperation> op;
            Com<IShellItem> dest;
            Com<IShellItemArray> items = ItemsFor(paths);
            if (items && SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(op.Put()))) &&
                SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(dest.Put())))) {
                op->SetOperationFlags(FOF_NOCONFIRMMKDIR | FOFX_ADDUNDORECORD | FOFX_NOMINIMIZEBOX);
                hr = move ? op->MoveItems(items.Get(), dest.Get()) : op->CopyItems(items.Get(), dest.Get());
                if (SUCCEEDED(hr)) hr = op->PerformOperations();
                op->GetAnyOperationsAborted(&aborted);
            }
        }
        CoUninitialize();
        PostToUi([&cards, hr, aborted, move, folder, n = paths.size()] {
            wstring title = (move ? L"Moved " : L"Copied ") + Plural(n, L"item", L"items");
            if (FAILED(hr)) cards.Note(move ? L"Move to" : L"Copy to", L"Could not finish: some items may not have been " + wstring(move ? L"moved." : L"copied."), true);
            else if (aborted) cards.Note(move ? L"Move to" : L"Copy to", L"Stopped before the end.", false);
            else cards.Note(title, L"to " + folder, false);
        });
    }).detach();
}

void CopyPaths(const std::vector<wstring>& paths, Cards& cards)
{
    wstring text;
    for (auto& p : paths) {
        bool quote = p.find(L' ') != wstring::npos;
        text += (quote ? L"\"" + p + L"\"" : p) + L"\r\n";
    }
    if (paths.size() == 1) text.resize(text.size() - 2);   // one path: no line break after it
    bool ok = false;
    if (OpenClipboard(MessageWindow())) {
        EmptyClipboard();
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            memcpy(GlobalLock(g), text.c_str(), bytes);
            GlobalUnlock(g);
            ok = SetClipboardData(CF_UNICODETEXT, g) != nullptr;
            if (!ok) GlobalFree(g);
        }
        CloseClipboard();
    }
    if (ok) cards.Note(L"Copied " + Plural(paths.size(), L"path", L"paths"), paths.size() == 1 ? paths[0] : L"One per line, ready to paste");
    else cards.Note(L"Copy path", L"The clipboard is busy; try again.", true);
}

void OpenTerminal(const std::vector<wstring>& paths, Cards& cards)
{
    if (paths.empty()) return;
    wstring dir = DirExists(paths[0]) ? paths[0] : ParentDir(paths[0]);
    // wt.exe is an app alias; it exists only when Windows Terminal is installed.
    wstring wt = JoinPath(EnvVar(L"LOCALAPPDATA"), L"Microsoft\\WindowsApps\\wt.exe");
    wstring arg = dir;
    if (!arg.empty() && arg.back() == L'\\') arg += L'.';   // "C:\" would escape the closing quote
    STARTUPINFOW si{ sizeof si };
    PROCESS_INFORMATION pi{};
    BOOL ok;
    if (GetFileAttributesW(wt.c_str()) != INVALID_FILE_ATTRIBUTES) {
        wstring cmd = L"\"" + wt + L"\" -d \"" + arg + L"\"";
        ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi);
    } else {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        wstring cmd = L"\"" + JoinPath(sys, L"cmd.exe") + L"\"";
        ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, dir.c_str(), &si, &pi);
    }
    if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); Log(L"actions: terminal opened in %s", dir.c_str()); }
    else cards.Note(L"Terminal", L"Could not open a terminal in " + dir, true);
}

std::vector<wstring> ExplorerSelection()
{
    std::vector<wstring> out;
    HWND fg = GetForegroundWindow();
    wstring cls = ClassOf(fg);
    bool desktop = cls == L"Progman" || cls == L"WorkerW";
    if (!desktop && cls != L"CabinetWClass" && cls != L"ExploreWClass") return out;

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
