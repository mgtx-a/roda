// The application: single instance, tray icon and menu, and the main loop.
// Nothing runs at idle: the loop sleeps until a message, a scheduled job or a frame is due.
#include "anim.h"
#include "dragdrop.h"
#include "gfx.h"
#include "input.h"
#include "wheel.h"

#include <mutex>

namespace rd {

namespace {

constexpr UINT WM_RUN = WM_APP + 1;
constexpr UINT WM_TRAY = WM_APP + 2;
constexpr UINT_PTR TIMER_MODAL = 99;

enum Cmd { CmdOutput = 1, CmdPause, CmdStartup, CmdSettings, CmdLog, CmdQuit };

HWND g_msg = nullptr;
std::mutex g_uiMutex;
std::vector<std::function<void()>> g_uiQueue;
int g_modalDepth = 0;
bool g_frameWanted = false;

// ---------------------------------------------------------------- Start with Windows: a shortcut, no registry

wstring StartupShortcut()
{
    PWSTR p = nullptr;
    wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &p)) && p) r = JoinPath(p, L"Roda.lnk");
    CoTaskMemFree(p);
    return r;
}

wstring ExePath() { return JoinPath(ExeDir(), L"Roda.exe"); }

/// On only when the shortcut starts this copy: another copy's shortcut (or one left from a moved folder) is not ours.
bool StartupOn()
{
    wstring lnk = StartupShortcut();
    if (lnk.empty() || !FileExists(lnk)) return false;
    Com<IShellLinkW> link;
    Com<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.Put()))) ||
        FAILED(link->QueryInterface(IID_PPV_ARGS(file.Put()))) || FAILED(file->Load(lnk.c_str(), STGM_READ))) return false;
    wchar_t target[MAX_PATH * 2] = L"";
    link->GetPath(target, MAX_PATH * 2, nullptr, SLGP_RAWPATH);
    wchar_t self[MAX_PATH * 2] = L"";
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    return SamePath(target, self);
}

void SetStartup(bool on)
{
    wstring lnk = StartupShortcut();
    if (lnk.empty()) return;
    if (!on) { DeleteFileW(lnk.c_str()); Log(L"start with Windows: off (shortcut removed)"); return; }
    wchar_t self[MAX_PATH * 2] = L"";
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    Com<IShellLinkW> link;
    Com<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.Put())))) return;
    link->SetPath(self);
    link->SetWorkingDirectory(ExeDir().c_str());
    link->SetDescription(L"Roda");
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(file.Put()))) && SUCCEEDED(file->Save(lnk.c_str(), TRUE)))
        Log(L"start with Windows: on (%s)", lnk.c_str());
    else
        Log(L"start with Windows: could not write %s", lnk.c_str());
}

// ---------------------------------------------------------------- the app

struct App {
    Settings settings;
    Wheel wheel;
    Trigger trigger;
    HINSTANCE inst = nullptr;
    NOTIFYICONDATAW nid{};
    bool trayAdded = false;
    double nextFrame = 0, lastFrame = 0;
    UINT taskbarCreated = 0;

    App() : trigger(settings, wheel) {}

    bool Start(HINSTANCE hInst)
    {
        inst = hInst;
        settings = Settings::Load();
        CleanStaleTemp();
        Log(L"started: Roda, data in %s (%s), scale %.2f, %d screen(s)", DataDir().c_str(),
            TestMode() ? L"test" : PortableData() ? L"portable, next to the exe" : L"%APPDATA%, the exe folder is read-only",
            Scale(), int(Monitors().size()));

        if (!wheel.Create(inst) || !trigger.Create(inst)) Log(L"could not create the wheel or the probe; dragging will not open the wheel");
        wheel.onChoose = [](WheelKind k, const wstring& label, const std::vector<wstring>& paths) {
            Log(L"chosen: %s > %s, on %d item(s):", WheelName(k), label.c_str(), int(paths.size()));
            for (size_t i = 0; i < paths.size() && i < 50; i++) Log(L"  %s", paths[i].c_str());
        };
        if (!settings.paused) trigger.Enable(true);

        taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        SetUpTray();
        if (TestMode()) WatchMemory();
        return true;
    }

    void Shutdown()
    {
        trigger.Enable(false);
        if (trayAdded) Shell_NotifyIconW(NIM_DELETE, &nid);
        DeleteTree(TempDir());
    }

    void WatchMemory()
    {
        // Test mode only: a memory line in the log every few seconds.
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
        Log(L"mem private=%llu KB ws=%llu KB", (unsigned long long)(pmc.PrivateUsage >> 10), (unsigned long long)(pmc.WorkingSetSize >> 10));
        After(5, [this] { WatchMemory(); });
    }

    // ------------------------------------------------------------ tray

    void SetUpTray()
    {
        nid.cbSize = sizeof nid;
        nid.hWnd = g_msg;
        nid.uID = 1;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_TRAY;
        nid.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        UpdateTip();
        trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    }

    void UpdateTip()
    {
        wcscpy(nid.szTip, settings.paused ? L"Roda (paused)" : L"Roda: drag a file, then hold Shift, Ctrl or Alt");
        if (trayAdded) Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void SetPaused(bool on)
    {
        settings.paused = on;
        settings.Save();
        trigger.Enable(!on);
        if (on) wheel.Hide(false);
        UpdateTip();
        Log(on ? L"paused" : L"resumed");
    }

    void TrayMenu()
    {
        bool fixedOutput = !settings.outputFolder.empty();
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING | (fixedOutput ? 0 : MF_GRAYED), CmdOutput, fixedOutput ? L"Open output folder" : L"Output: next to each file");
        AppendMenuW(m, MF_STRING | (settings.paused ? MF_CHECKED : 0), CmdPause, L"Pause Roda");
        AppendMenuW(m, MF_STRING | (StartupOn() ? MF_CHECKED : 0), CmdStartup, L"Start with Windows");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        // The settings window comes in a later phase; until then the file is edited directly.
        AppendMenuW(m, MF_STRING, CmdSettings, L"Settings (edit settings.json)");
        AppendMenuW(m, MF_STRING, CmdLog, L"Open log");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, CmdQuit, L"Quit Roda");
        POINT p = CursorPos();
        BeginModal();
        SetForegroundWindow(g_msg);
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_BOTTOMALIGN, p.x, p.y, 0, g_msg, nullptr);
        EndModal();
        DestroyMenu(m);
        switch (cmd) {
            case CmdOutput: EnsureDir(settings.outputFolder); ShellExecuteW(nullptr, L"open", settings.outputFolder.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
            case CmdPause: SetPaused(!settings.paused); break;
            case CmdStartup: SetStartup(!StartupOn()); break;
            case CmdSettings: {
                wstring f = JoinPath(DataDir(), L"settings.json");
                if (!FileExists(f)) settings.Save();
                ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + f + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
                break;
            }
            case CmdLog: ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + JoinPath(DataDir(), L"log.txt") + L"\"").c_str(), nullptr, SW_SHOWNORMAL); break;
            case CmdQuit: PostQuitMessage(0); break;
        }
    }

    // ------------------------------------------------------------ frames and the loop

    bool NeedFrame() const { return AnyAnimating() || wheel.dirty() || g_frameWanted; }

    void Frame()
    {
        double now = Now();
        double dt = lastFrame > 0 ? std::min(0.05, now - lastFrame) : 1.0 / 60;
        lastFrame = now;
        g_frameWanted = false;
        StepAll(dt);
        wheel.Draw();
    }

    void Pump()
    {
        RunDue();
        double now = Now();
        if (trigger.NeedsTick()) trigger.Tick();
        if (NeedFrame()) {
            if (now >= nextFrame) {
                nextFrame = now + 1.0 / 62;
                Frame();
            }
        } else {
            lastFrame = 0;
        }
    }

    int Run()
    {
        timeBeginPeriod(1);
        MSG msg;
        for (;;) {
            double timeout = -1;
            double now = Now();
            auto want = [&](double secs) { timeout = timeout < 0 ? secs : std::min(timeout, secs); };
            if (NeedFrame()) want(std::max(0.0, nextFrame - now));
            if (trigger.NeedsTick()) want(trigger.NextTickIn());
            double due = NextDueIn();
            if (due >= 0) want(due);

            DWORD ms = timeout < 0 ? INFINITE : DWORD(std::ceil(timeout * 1000));
            MsgWaitForMultipleObjectsEx(0, nullptr, ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { timeEndPeriod(1); return int(msg.wParam); }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Pump();
        }
    }
};

App* g_app = nullptr;

LRESULT CALLBACK MsgProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
        case WM_RUN: {
            std::vector<std::function<void()>> q;
            { std::lock_guard<std::mutex> lock(g_uiMutex); q.swap(g_uiQueue); }
            for (auto& f : q) f();
            return 0;
        }
        case WM_INPUT:
            if (g_app) g_app->trigger.OnRawInput(reinterpret_cast<HRAWINPUT>(l));
            return DefWindowProcW(h, m, w, l);   // lets Windows free the input
        case WM_TRAY:
            if (g_app && (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_LBUTTONUP)) g_app->TrayMenu();
            return 0;
        case WM_TIMER:
            if (w == TIMER_MODAL && g_app) g_app->Pump();   // menus run their own loops
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    // Explorer restarted: put the tray icon back.
    if (g_app && m == g_app->taskbarCreated && m) { g_app->trayAdded = false; g_app->SetUpTray(); return 0; }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

HWND MessageWindow() { return g_msg; }

void BeginModal()
{
    if (g_modalDepth++ == 0) SetTimer(g_msg, TIMER_MODAL, 8, nullptr);
}

void EndModal()
{
    if (--g_modalDepth == 0) KillTimer(g_msg, TIMER_MODAL);
}

void RequestFrame() { g_frameWanted = true; }

void PostToUi(std::function<void()> fn)
{
    { std::lock_guard<std::mutex> lock(g_uiMutex); g_uiQueue.push_back(std::move(fn)); }
    if (g_msg) PostMessageW(g_msg, WM_RUN, 0, 0);
}

}  // namespace rd

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int)
{
    using namespace rd;
    // Engine DLLs are only ever loaded by full path; this keeps the current folder out of the search too.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS);

    wstring lockName = L"Roda.SingleInstance";
    if (TestMode()) lockName += L".Test." + std::to_wstring(std::hash<wstring>{}(DataDir()) % 1000000);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, lockName.c_str());
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    OleInitialize(nullptr);   // apartment-threaded COM: OLE drag and drop needs it
    if (!GfxInit()) { MessageBoxW(nullptr, L"Roda could not start its graphics.", L"Roda", MB_ICONERROR); return 1; }

    WNDCLASSW wc{};
    wc.lpfnWndProc = MsgProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"RodaMessages";
    RegisterClassW(&wc);
    g_msg = CreateWindowExW(0, wc.lpszClassName, L"Roda", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);

    int rc;
    {
        App app;
        g_app = &app;
        app.Start(inst);
        rc = app.Run();
        app.Shutdown();
        Log(L"quit");
        g_app = nullptr;
    }
    DestroyWindow(g_msg);
    OleUninitialize();
    return rc;
}
