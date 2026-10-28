// Test helper (not shipped): plays a real OLE drag the way Explorer does, with injected mouse and
// key input, over a window of its own, so the trigger can be checked without a person.
//
//   dragtest.exe <scenario> <file> [<file2> <folder>]
//   scenarios: file-shift  file-nokey  text-shift  select-shift  mixed-ctrl  file-esc  switch
//
// Prints what the helper's own window received; Roda's log says what Roda did.
#ifndef UNICODE
#define UNICODE
#endif
#define NOMINMAX
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <shobjidl.h>

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
    int ww = std::min(1000, W - 100), wh = std::min(760, H - 100);
    int wx = (W - ww) / 2, wy = (H - wh) / 2;
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
            WORD key = sc == L"mixed-ctrl" ? VK_CONTROL : VK_SHIFT;
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
                Key(VK_CONTROL, true);         // Ctrl switches to the Tools wheel
                Sleep(80);
                Glide(px, py - 3, px, py - 100, 12, 15);
                Key(VK_CONTROL, false);
            } else {
                Glide(px, py - 3, px, py - 100, 12, 15);   // up onto the top segment
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
