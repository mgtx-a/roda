// Test helper (not shipped): plays a real OLE drag the way Explorer does, with injected mouse and
// key input, over a window of its own, so the trigger can be checked without a person.
//
//   dragtest.exe <scenario> <file> [<file2> <folder>]
//   scenarios: file-shift  file-nokey  text-shift  select-shift  mixed-ctrl  file-esc  switch  file-fullscreen
//
// Or with a real app as the drag source (found with UI Automation):
//   dragtest.exe ext <source> <item|-> <shift|ctrl|alt|none> [selectall|textdrag|select|titlebar]
//   source: explorer:<folder> (opened, then closed)  desktop  title:<part of a window title>
//   item:   name of the list item to press on (Explorer, desktop, a downloads page), or -
//   selectall  Ctrl+A in the source first (many files)
//   textdrag   Ctrl+A, then press on the highlighted text and drag it (an OLE text drag)
//   select     press inside the text area and drag to select, key pressed mid-way
//   titlebar   press on the title bar and move the window, key pressed mid-way
//
// The key is pressed only once the pointer is over the helper's window, so if the wheel fails
// to open the drop lands there (and is reported), never on a folder in the source.
// Prints what the helper's own window received; Roda's log says what Roda did.
#ifndef UNICODE
#define UNICODE
#endif
#define NOMINMAX
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uiautomation.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

static HWND g_win;
static HANDLE g_startDrag;
static bool g_gotDrop = false;
static int W, H;

static void MoveAbs(int x, int y)
{
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = LONG(x * 65535.0 / (W - 1));
    in.mi.dy = LONG(y * 65535.0 / (H - 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &in, sizeof in);
}

static void Button(bool down)
{
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof in);
}

static void Key(WORD vk, bool down)
{
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof in);
}

/// Glides from (x0,y0) to (x1,y1) in steps, like a hand.
static void Glide(int x0, int y0, int x1, int y1, int steps, int msEach)
{
    for (int i = 1; i <= steps; i++) {
        MoveAbs(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
        Sleep(msEach);
    }
}

class Src : public IDropSource {
    LONG ref_ = 1;
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IDropSource) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ULONG(InterlockedIncrement(&ref_)); }
    STDMETHODIMP_(ULONG) Release() override { return ULONG(InterlockedDecrement(&ref_)); }
    STDMETHODIMP QueryContinueDrag(BOOL esc, DWORD keys) override
    {
        if (esc) return DRAGDROP_S_CANCEL;
        return (keys & MK_LBUTTON) ? S_OK : DRAGDROP_S_DROP;
    }
    STDMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

/// The helper's window accepts drops, standing in for "wherever the user really meant to drop".
class Tgt : public IDropTarget {
    LONG ref_ = 1;
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ULONG(InterlockedIncrement(&ref_)); }
    STDMETHODIMP_(ULONG) Release() override { return ULONG(InterlockedDecrement(&ref_)); }
    STDMETHODIMP DragEnter(IDataObject*, DWORD, POINTL, DWORD* e) override { *e &= DROPEFFECT_COPY; return S_OK; }
    STDMETHODIMP DragOver(DWORD, POINTL, DWORD* e) override { *e &= DROPEFFECT_COPY; return S_OK; }
    STDMETHODIMP DragLeave() override { return S_OK; }
    STDMETHODIMP Drop(IDataObject*, DWORD, POINTL, DWORD* e) override { g_gotDrop = true; *e &= DROPEFFECT_COPY; return S_OK; }
};

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    // Like a text editor: the window captures the mouse while the button is held.
    if (m == WM_LBUTTONDOWN) SetCapture(h);
    if (m == WM_LBUTTONUP) ReleaseCapture();
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- real apps as the drag source

static const CLSID kCUIAutomation = { 0xff48dba4, 0x60ef, 0x4201, { 0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e } };

static HWND g_src;
static std::wstring g_part;

static BOOL CALLBACK FindTitle(HWND h, LPARAM)
{
    wchar_t t[512];
    if (!IsWindowVisible(h) || !GetWindowTextW(h, t, 512)) return TRUE;
    if (wcsstr(t, g_part.c_str()) && h != g_win) { g_src = h; return FALSE; }
    return TRUE;
}

static HWND WindowTitled(const std::wstring& part, int waitMs)
{
    g_part = part;
    for (int t = 0; t < waitMs; t += 100) {
        g_src = nullptr;
        EnumWindows(FindTitle, 0);
        if (g_src) return g_src;
        Sleep(100);
    }
    return nullptr;
}

static BOOL CALLBACK FindDefView(HWND h, LPARAM lp)
{
    HWND v = FindWindowExW(h, nullptr, L"SHELLDLL_DefView", nullptr);
    if (v) { *reinterpret_cast<HWND*>(lp) = FindWindowExW(v, nullptr, L"SysListView32", nullptr); return FALSE; }
    return TRUE;
}

static HWND DesktopList()
{
    HWND list = nullptr;
    EnumWindows(FindDefView, reinterpret_cast<LPARAM>(&list));   // under Progman, or a WorkerW after a wallpaper change
    return list;
}

static std::wstring StripExt(const std::wstring& n)
{
    size_t dot = n.find_last_of(L'.');
    return dot == std::wstring::npos ? n : n.substr(0, dot);
}

/// Screen rectangle of the element named `name` (or the name without its extension, as Explorer
/// shows it when extensions are hidden) inside `root`. Browsers build their tree on demand, so retry.
static bool FindItem(HWND root, const std::wstring& name, RECT& out)
{
    IUIAutomation* ua = nullptr;
    if (FAILED(CoCreateInstance(kCUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ua)))) return false;
    bool found = false;
    for (int tries = 0; tries < 40 && !found; tries++) {
        IUIAutomationElement* top = nullptr;
        if (SUCCEEDED(ua->ElementFromHandle(root, &top)) && top) {
            for (const std::wstring& n : { name, StripExt(name) }) {
                VARIANT v;
                v.vt = VT_BSTR;
                v.bstrVal = SysAllocString(n.c_str());
                IUIAutomationCondition* c = nullptr;
                ua->CreatePropertyCondition(UIA_NamePropertyId, v, &c);
                IUIAutomationElement* e = nullptr;
                top->FindFirst(TreeScope_Descendants, c, &e);
                if (e) {
                    RECT r{};
                    if (SUCCEEDED(e->get_CurrentBoundingRectangle(&r)) && r.right > r.left) { out = r; found = true; }
                    e->Release();
                }
                c->Release();
                VariantClear(&v);
                if (found) break;
            }
            top->Release();
        }
        if (!found) Sleep(250);
    }
    ua->Release();
    return found;
}

static BOOL CALLBACK Biggest(HWND h, LPARAM lp)
{
    auto* best = reinterpret_cast<std::pair<HWND, long>*>(lp);
    RECT r;
    if (IsWindowVisible(h) && GetWindowRect(h, &r)) {
        long a = (r.right - r.left) * (r.bottom - r.top);
        if (a > best->second) *best = { h, a };
    }
    return TRUE;
}

/// The text area: the biggest visible child window (Edit, RICHEDIT50W, _WwG, a browser's page).
static RECT TextArea(HWND top)
{
    std::pair<HWND, long> best{ nullptr, 0 };
    EnumChildWindows(top, Biggest, reinterpret_cast<LPARAM>(&best));
    RECT r{};
    GetWindowRect(best.first ? best.first : top, &r);
    return r;
}

/// A point on highlighted (selected) text: blue-ish pixels with blue all around them.
static bool FindHighlight(RECT area, POINT& out)
{
    int w = area.right - area.left, h = area.bottom - area.top;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, area.left, area.top, SRCCOPY);
    auto px = static_cast<const uint8_t*>(bits);
    auto blue = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= w || y >= h) return false;
        const uint8_t* p = px + (size_t(y) * w + x) * 4;   // B G R
        return p[0] > 170 && p[0] > p[2] + 50 && p[1] > p[2];   // dark (0,120,215) or light (153,201,239) selection blue
    };
    bool found = false;
    for (int y = 0; y < h && !found; y += 2)
        for (int x = 0; x < w && !found; x += 2)
            if (blue(x, y) && blue(x + 8, y) && blue(x, y + 8) && blue(x + 8, y + 8)) { out = { area.left + x + 4, area.top + y + 4 }; found = true; }
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return found;
}

static void ClickAt(int x, int y)
{
    MoveAbs(x, y); Sleep(60);
    Button(true); Sleep(40); Button(false); Sleep(250);
}

static void Chord(WORD mod, WORD vk)
{
    Key(mod, true); Key(vk, true); Key(vk, false); Key(mod, false);
}

/// DRAGTEST_KEYS=<convert>,<tools>,<actions> (hex virtual keys): play other trigger keys, so a test copy
/// of Roda with those keys set can run while another copy (with the default keys) is in use.
static WORD Trigger(WORD vk)
{
    const wchar_t* e = _wgetenv(L"DRAGTEST_KEYS");
    if (!e) return vk;
    unsigned k[3] = { VK_SHIFT, VK_CONTROL, VK_MENU };
    swscanf(e, L"%x,%x,%x", &k[0], &k[1], &k[2]);
    return vk == VK_SHIFT ? WORD(k[0]) : vk == VK_CONTROL ? WORD(k[1]) : vk == VK_MENU ? WORD(k[2]) : vk;
}

static WORD KeyNamed(const std::wstring& k)
{
    if (k == L"shift") return VK_SHIFT;
    if (k == L"ctrl") return VK_CONTROL;
    if (k == L"alt") return VK_MENU;
    return 0;
}

static int RunExternal(const std::wstring& source, const std::wstring& item, const std::wstring& keyName, const std::wstring& opt)
{
    HWND src = nullptr, area = nullptr;
    bool desktop = source == L"desktop", opened = false;
    if (desktop) {
        Chord(VK_LWIN, 'D');   // show the desktop
        Sleep(800);
        src = area = DesktopList();
    } else if (source.rfind(L"explorer:", 0) == 0) {
        std::wstring folder = source.substr(9);
        ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"\"" + folder + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        src = WindowTitled(folder.substr(folder.find_last_of(L'\\') + 1), 6000);
        opened = true;
    } else if (source.rfind(L"title:", 0) == 0) {
        src = WindowTitled(source.substr(6), 6000);
    }
    if (!src) { wprintf(L"scenario=ext source=%ls result=source-window-not-found\n", source.c_str()); return 1; }
    if (!desktop) {
        ShowWindow(src, SW_RESTORE);
        SetWindowPos(src, HWND_TOP, 20, 20, 900, H - 140, SWP_SHOWWINDOW);
        Sleep(700);
        RECT wr;
        GetWindowRect(src, &wr);
        ClickAt((wr.left + wr.right) / 2, wr.top + 12);      // the title bar: brings the window to the front
        area = src;
    }

    // The helper's window, to the right of the source and never activated: the source keeps the focus.
    int hx = 1000, hy = 120, hw = std::max(300, W - 1040), hh = H - 300;
    g_win = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"RodaDragTest", L"Roda drag test", WS_POPUP, hx, hy, hw, hh, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Tgt tgt;
    RegisterDragDrop(g_win, &tgt);
    ShowWindow(g_win, SW_SHOWNOACTIVATE);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);

    if (opt == L"selectall" || opt == L"textdrag") {
        Chord(VK_CONTROL, 'A');
        Sleep(400);
    }

    POINT at{};
    RECT r{};
    if (opt == L"titlebar") {
        GetWindowRect(src, &r);
        at = { (r.left + r.right) / 2 + 60, r.top + 12 };
    } else if (opt == L"textdrag") {
        if (!FindHighlight(TextArea(src), at)) { wprintf(L"scenario=ext source=%ls result=no-highlighted-text\n", source.c_str()); return 1; }
    } else if (opt == L"select") {
        r = TextArea(src);
        at = { r.left + 30, r.top + std::max(30L, (r.bottom - r.top) * 15 / 100) };   // below a browser's tab strip
    } else {
        if (!FindItem(area, item, r)) { wprintf(L"scenario=ext source=%ls item=%ls result=item-not-found\n", source.c_str(), item.c_str()); return 1; }
        // DRAGTEST_AT=dx,dy: press there inside the item instead (a browser's download card).
        // A row (details view): press on the icon and name, not the empty columns.
        if (const wchar_t* off = _wgetenv(L"DRAGTEST_AT")) at = { r.left + _wtoi(off), r.top + _wtoi(wcschr(off, L',') + 1) };
        else at = (r.right - r.left) > 2 * (r.bottom - r.top) ? POINT{ r.left + 30, (r.top + r.bottom) / 2 } : POINT{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 - 8 };
    }

    WORD key = Trigger(KeyNamed(keyName));
    int tx = hx + hw / 2, ty = hy + hh / 2 + 100;   // where the key is pressed: over the helper's window
    bool inPlace = opt == L"select" || opt == L"titlebar";
    std::thread input([&] {
        MoveAbs(at.x, at.y);
        Sleep(120);
        Button(true);
        Sleep(80);
        Glide(at.x, at.y, at.x + 12, at.y + 4, 3, 12);   // past the drag threshold
        Sleep(60);
        if (inPlace) {
            // A selection or a window move: the key goes down part-way, as a person might do.
            Glide(at.x + 12, at.y + 4, at.x + 200, at.y + 60, 10, 15);
            if (key) Key(key, true);
            Sleep(80);
            Glide(at.x + 200, at.y + 60, at.x + 320, at.y + 90, 10, 15);
            Sleep(150);
        } else {
            Glide(at.x + 12, at.y + 4, tx, ty, 25, 15);
            Sleep(150);
            if (key) {
                Key(key, true);
                Sleep(60);
                Glide(tx, ty, tx, ty - 3, 2, 15);
                Sleep(80);
                Glide(tx, ty - 3, tx, ty - 100, 12, 15);   // up onto the top segment
                Sleep(150);
            }
        }
        if (key) Key(key, false);
        Sleep(120);
        Button(false);
        Sleep(600);
        PostMessageW(g_win, WM_CLOSE, 0, 0);
    });
    while (IsWindow(g_win) && GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    input.join();
    wprintf(L"scenario=ext source=%ls item=%ls key=%ls opt=%ls pressed-at=%ld,%ld helper-window-got-drop=%ls\n", source.c_str(), item.c_str(), keyName.c_str(),
            opt.empty() ? L"-" : opt.c_str(), at.x, at.y, g_gotDrop ? L"yes" : L"no");
    if (opened) PostMessageW(src, WM_CLOSE, 0, 0);
    if (desktop) Chord(VK_LWIN, 'D');   // and back
    return 0;
}

static IDataObject* FilesObject(const std::vector<std::wstring>& paths)
{
    std::vector<PIDLIST_ABSOLUTE> abs;
    for (auto& p : paths) abs.push_back(ILCreateFromPathW(p.c_str()));
    std::wstring dir = paths[0].substr(0, paths[0].find_last_of(L'\\'));
    PIDLIST_ABSOLUTE parent = ILCreateFromPathW(dir.c_str());
    std::vector<PCUITEMID_CHILD> kids;
    for (auto a : abs) kids.push_back(ILFindLastID(a));
    IDataObject* d = nullptr;
    SHCreateDataObject(parent, UINT(kids.size()), kids.data(), nullptr, IID_PPV_ARGS(&d));
    return d;
}

static IDataObject* TextObject()
{
    IDataObject* d = nullptr;
    SHCreateDataObject(nullptr, 0, nullptr, nullptr, IID_PPV_ARGS(&d));
    const wchar_t text[] = L"some selected text";
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sizeof text);
    memcpy(GlobalLock(g), text, sizeof text);
    GlobalUnlock(g);
    FORMATETC f{ CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM s{};
    s.tymed = TYMED_HGLOBAL;
    s.hGlobal = g;
    d->SetData(&f, &s, TRUE);
    return d;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) { wprintf(L"usage: dragtest <scenario> <file> [file2 folder]\n"); return 2; }
    std::wstring sc = argv[1];
    std::vector<std::wstring> paths;
    for (int i = 2; i < argc; i++) paths.push_back(argv[i]);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    W = GetSystemMetrics(SM_CXSCREEN);
    H = GetSystemMetrics(SM_CYSCREEN);

    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = CreateSolidBrush(RGB(70, 70, 80));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"RodaDragTest";
    RegisterClassW(&wc);
    if (sc == L"ext") {
        if (argc < 5) { wprintf(L"usage: dragtest ext <source> <item|-> <key> [opt]\n"); return 2; }
        int rc = RunExternal(argv[2], argv[3], argv[4], argc > 5 ? argv[5] : L"");
        OleUninitialize();
        return rc;
    }
    int ww = std::min(1000, W - 100), wh = std::min(760, H - 100);
    int wx = (W - ww) / 2, wy = (H - wh) / 2;
    if (sc == L"file-fullscreen") { ww = W; wh = H; wx = wy = 0; }   // a full screen app in front: Roda must stay out
    g_win = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"Roda drag test", WS_POPUP | WS_VISIBLE, wx, wy, ww, wh, nullptr, nullptr, wc.hInstance, nullptr);
    Tgt tgt;
    RegisterDragDrop(g_win, &tgt);
    SetForegroundWindow(g_win);

    bool ole = sc != L"select-shift";
    IDataObject* data = sc == L"text-shift" ? TextObject() : (ole ? FilesObject(paths) : nullptr);
    g_startDrag = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    int cx = wx + ww / 2, cy = wy + wh / 2 + 120;

    std::thread input([&] {
        Sleep(400);
        MoveAbs(cx, cy);
        Sleep(60);
        Button(true);
        Sleep(60);
        Glide(cx, cy, cx + 12, cy, 3, 10);    // past the drag threshold
        SetEvent(g_startDrag);                 // the source starts DoDragDrop, as Explorer does
        Glide(cx + 12, cy, cx + 60, cy - 10, 10, 15);
        Sleep(120);
        int px = cx + 60, py = cy - 10;
        if (sc == L"file-nokey") {
            Glide(px, py, px - 200, py - 100, 10, 15);
        } else {
            WORD key = Trigger(sc == L"mixed-ctrl" ? VK_CONTROL : VK_SHIFT);
            Key(key, true);
            Sleep(60);
            Glide(px, py, px, py - 3, 2, 15);   // the next move reaches the wheel
            Sleep(80);
            if (sc == L"file-esc") {
                Key(VK_ESCAPE, true); Sleep(40); Key(VK_ESCAPE, false);
                Sleep(150);
            } else if (sc == L"switch") {
                Key(key, false);
                Sleep(60);
                Key(Trigger(VK_CONTROL), true);   // Ctrl switches to the Tools wheel
                Sleep(80);
                Glide(px, py - 3, px, py - 100, 12, 15);
                Key(Trigger(VK_CONTROL), false);
            } else {
                // DRAGTEST_ANGLE=<degrees clockwise from the top>: which segment to drop on (default: the top one).
                const wchar_t* ang = _wgetenv(L"DRAGTEST_ANGLE");
                double a = ang ? _wtof(ang) * 3.14159265358979 / 180 : 0;
                int r = ang && wcschr(ang, L'o') ? 190 : 100;   // "225o": the outer ring
                Glide(px, py - 3, px + int(r * sin(a)), py - 3 - int(r * cos(a)), 12, 15);
                // DRAGTEST_HOLD=<ms>: wait over the segment, so a screenshot can be taken.
                if (const wchar_t* hold = _wgetenv(L"DRAGTEST_HOLD")) Sleep(DWORD(_wtoi(hold)));
            }
            Key(key, false);
            Sleep(120);
        }
        Button(false);
        Sleep(200);
        PostMessageW(g_win, WM_CLOSE, 0, 0);
    });

    MSG msg;
    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(1, &g_startDrag, FALSE, 4000, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) break;
        if (r == WAIT_TIMEOUT) break;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    }
    HRESULT hr = S_OK;
    DWORD effect = 0;
    if (ole && data) {
        Src src;
        hr = DoDragDrop(data, &src, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
    }
    input.join();
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    wprintf(L"scenario=%ls dodragdrop=%ls effect=%lu helper-window-got-drop=%ls\n", sc.c_str(),
            !ole ? L"none" : hr == DRAGDROP_S_DROP ? L"dropped" : hr == DRAGDROP_S_CANCEL ? L"cancelled" : L"other", effect,
            g_gotDrop ? L"yes" : L"no");
    RevokeDragDrop(g_win);
    DestroyWindow(g_win);
    OleUninitialize();
    return 0;
}
