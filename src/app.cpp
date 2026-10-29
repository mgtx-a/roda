// The application: single instance, tray icon and menu, and the main loop.
// Nothing runs at idle: the loop sleeps until a message, a scheduled job or a frame is due.
#include "actions.h"
#include "anim.h"
#include "cards.h"
#include "dragdrop.h"
#include "floatcard.h"
#include "gfx.h"
#include "input.h"
#include "jobs.h"
#include "popup.h"
#include "place.h"
#include "settings_ui.h"
#include "tour.h"
#include "wheel.h"
#include "worker.h"

#include <map>
#include <mutex>

namespace rd {

namespace {

constexpr UINT WM_RUN = WM_APP + 1;
constexpr UINT WM_TRAY = WM_APP + 2;
constexpr UINT WM_SHOW_SETTINGS = WM_APP + 3;   // from a second start of Roda.exe
constexpr UINT WM_SHOW_TOUR = WM_APP + 4;
constexpr UINT_PTR TIMER_MODAL = 99;
constexpr int HOTKEY_WHEEL = 1;

enum Cmd { CmdOutput = 1, CmdPause, CmdStartup, CmdSettings, CmdTour, CmdLog, CmdQuit };

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

// ---------------------------------------------------------------- the optional Explorer entry
// The one thing Roda writes to the registry, and only when asked (Settings): a "Roda…" verb for files
// and folders under HKEY_CURRENT_USER (no admin rights), removed again when switched off.

const wchar_t* kVerbKeys[] = { L"Software\\Classes\\*\\shell\\Roda", L"Software\\Classes\\Directory\\shell\\Roda" };

bool ExplorerMenuOn()
{
    wchar_t cmd[1024] = L"";
    DWORD size = sizeof cmd;
    if (RegGetValueW(HKEY_CURRENT_USER, (wstring(kVerbKeys[0]) + L"\\command").c_str(), nullptr, RRF_RT_REG_SZ, nullptr, cmd, &size) != ERROR_SUCCESS) return false;
    return wcsstr(cmd, ExePath().c_str()) != nullptr;   // another copy's entry is not ours
}

void SetExplorerMenu(bool on)
{
    for (auto* key : kVerbKeys) {
        if (!on) { RegDeleteTreeW(HKEY_CURRENT_USER, key); continue; }
        HKEY k = nullptr, c = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) continue;
        auto set = [](HKEY h, const wchar_t* name, const wstring& v) { RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * 2)); };
        set(k, L"MUIVerb", L"Roda…");
        set(k, L"Icon", L"\"" + ExePath() + L"\",0");
        set(k, L"MultiSelectModel", L"Player");   // a selection of many files is one wheel, not one per file
        if (RegCreateKeyExW(k, L"command", 0, nullptr, 0, KEY_WRITE, nullptr, &c, nullptr) == ERROR_SUCCESS) {
            set(c, nullptr, L"\"" + ExePath() + L"\" --open \"%1\"");
            RegCloseKey(c);
        }
        RegCloseKey(k);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    Log(on ? L"explorer menu: added (HKCU\\Software\\Classes\\*\\shell\\Roda and Directory\\shell\\Roda)" : L"explorer menu: removed");
}

// ---------------------------------------------------------------- the app

struct App {
    Settings settings;
    Wheel wheel;
    Trigger trigger;
    Jobs jobs;
    Cards cards;
    OptionsPopup popup;
    SettingsWindow settingsUi{ settings };
    PlaceWindow placeUi;
    Tour tour;
    int choices = 0;   // options chosen so far (the tour watches it)
    std::vector<std::unique_ptr<FloatCard>> floats;
    std::map<int, std::function<void(const JobState&)>> waiting;   // quiet jobs (previews): who wants the result
    HINSTANCE inst = nullptr;
    NOTIFYICONDATAW nid{};
    bool trayAdded = false, hotkey = false, fastTimer = false;
    double nextFrame = 0, lastFrame = 0;
    UINT taskbarCreated = 0;

    App() : wheel(settings), trigger(settings, wheel) {}

    bool Start(HINSTANCE hInst)
    {
        inst = hInst;
        settings = Settings::Load();
        CleanStaleTemp();
        Log(L"started: Roda, data in %s (%s), scale %.2f, %d screen(s)", DataDir().c_str(),
            TestMode() ? L"test" : PortableData() ? L"portable, next to the exe" : L"%APPDATA%, the exe folder is read-only",
            Scale(), int(Monitors().size()));

        if (!wheel.Create(inst) || !trigger.Create(inst)) Log(L"could not create the wheel or the probe; dragging will not open the wheel");
        if (!cards.Create(inst) || !popup.Create(inst)) Log(L"could not create the cards or the options popup");
        SetThemeOverride(settings.theme);
        settingsUi.onKeysChanged = [this] {
            if (hotkey) UnregisterHotKey(g_msg, HOTKEY_WHEEL);
            RegisterWheelHotkey();
        };
        settingsUi.startupOn = [] { return StartupOn(); };
        settingsUi.setStartup = [](bool on) { if (on != StartupOn()) SetStartup(on); };
        settingsUi.explorerMenuOn = [] { return ExplorerMenuOn(); };
        settingsUi.setExplorerMenu = [](bool on) { if (on != ExplorerMenuOn()) SetExplorerMenu(on); };
        tour.onFinished = [this] { if (!settings.welcomed) { settings.welcomed = true; settings.Save(); } };
        // First run: the tour, once things have settled.
        if (!settings.welcomed && !TestMode()) After(1.5, [this] { StartTour(); });
        wheel.onChoose = [this](const WheelOption& o, const std::vector<wstring>& paths, POINT at) { Chosen(o, paths, at); };
        jobs.onChange = [this](const JobState& s) {
            if (!s.quiet) { cards.Update(s); return; }
            if (s.phase != JobState::Done && s.phase != JobState::Failed && s.phase != JobState::Cancelled) return;
            auto it = waiting.find(s.id);
            if (it == waiting.end()) return;
            auto fn = std::move(it->second);
            waiting.erase(it);
            fn(s);
        };
        cards.onCancel = [this](int id) { jobs.Cancel(id); };
        if (!settings.paused) trigger.Enable(true);
        RegisterWheelHotkey();

        taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        SetUpTray();
        if (TestMode()) WatchMemory();
        return true;
    }

    void Shutdown()
    {
        jobs.CancelAll();
        if (hotkey) UnregisterHotKey(g_msg, HOTKEY_WHEEL);
        if (fastTimer) timeEndPeriod(1);
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

    // ------------------------------------------------------------ choosing an option

    /// Quality, metadata and output folder from the settings, for every job.
    Job MakeJob(const wstring& op, std::vector<wstring> inputs, const Json& extra)
    {
        Job j;
        j.op = op;
        j.inputs = std::move(inputs);
        j.outDir = settings.outputFolder;
        // Files that came out of an Outlook mail or a ZIP live in Roda's temp folder: results go to the
        // chosen output folder, or Downloads, never next to them.
        if (j.outDir.empty() && !j.inputs.empty() && j.inputs[0].rfind(TempDir(), 0) == 0) {
            PWSTR p = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p)) && p) j.outDir = p;
            CoTaskMemFree(p);
        }
        j.opts.Set(L"jpegQ", settings.jpegQuality).Set(L"webpQ", settings.webpQuality).Set(L"keepMeta", settings.keepMetadata).Set(L"dpi", settings.pdfDpi);
        for (auto& [k, v] : extra.obj) j.opts.Set(k.c_str(), v);
        return j;
    }

    void Submit(const WheelOption& o, const std::vector<wstring>& paths, const Json& extra)
    {
        const Converter* c = o.conv;
        if (c->mode == Converter::AllInOne) {
            wstring title = paths.size() == 1 ? FileName(paths[0]) : std::to_wstring(paths.size()) + L" items";
            jobs.Submit(MakeJob(c->id, paths, extra), title, o.label);
        } else {
            for (auto& p : paths) jobs.Submit(MakeJob(c->id, { p }, extra), FileName(p), o.label);
        }
    }

    /// A downscaled PNG of `path`, made by a worker, then `then(previewPath, origW, origH)`.
    void Preview(const wstring& path, int maxSide, std::function<void(const wstring&, int, int)> then)
    {
        Job j = MakeJob(L"prep.preview", { path }, Json::Object().Set(L"max", maxSide));
        static int n = 0;
        j.outPath = JoinPath(TempDir(), L"preview-" + std::to_wstring(++n) + L".png");
        j.outDir.clear();
        int id = jobs.Submit(j, FileName(path), L"Preview", true);
        waiting[id] = [this, then, path](const JobState& s) {
            if (s.phase != JobState::Done) { cards.Note(FileName(path), s.error.empty() ? L"Could not open this picture." : s.error, true); return; }
            then(s.out, int(s.info[L"info"][L"w"].Num()), int(s.info[L"info"][L"h"].Num()));
        };
    }

    /// The PDF editor (Sign, Add picture, Fill form): pages and fields come from quiet jobs, Save submits.
    void OpenPlace(const WheelOption& o, const wstring& pdf, PlaceWindow::Mode mode)
    {
        placeUi.onNeedPage = [this, pdf](int page) {
            Job j = MakeJob(L"prep.preview", { pdf }, Json::Object().Set(L"max", 1600).Set(L"page", page));
            static int n = 0;
            j.outPath = JoinPath(TempDir(), L"page-" + std::to_wstring(++n) + L".png");
            j.outDir.clear();
            int id = jobs.Submit(j, FileName(pdf), L"Page", true);
            waiting[id] = [this, page](const JobState& s) {
                if (s.phase != JobState::Done) { placeUi.PageFailed(s.error); return; }
                placeUi.SetPage(page, s.out, int(s.info[L"info"][L"pages"].Num(1)), s.info[L"info"][L"pth"].Num(0));
            };
        };
        placeUi.onNeedFields = [this, pdf] {
            Job j = MakeJob(L"prep.fields", { pdf }, Json::Object());
            j.outDir.clear();
            int id = jobs.Submit(j, FileName(pdf), L"Fields", true);
            waiting[id] = [this](const JobState& s) { if (s.phase == JobState::Done) placeUi.SetFields(s.info[L"info"][L"fields"]); };
        };
        placeUi.onSave = [this, o, pdf](const Json& opts) { Submit(o, { pdf }, opts); };
        placeUi.Show(pdf, mode);
    }

    std::vector<wstring> opened;     // paths from "Roda…" in Explorer, gathered for a moment
    std::vector<wstring> lastOpened; // the last batch shown, in case more of the same selection arrives
    double lastOpenedAt = -10;
    uint32_t openToken = 0;

    void Open(const wstring& path)
    {
        if (path.empty()) return;
        opened.push_back(path);
        uint32_t t = ++openToken;
        // Explorer starts the verb once per selected file, each a moment apart: wait for the rest, then
        // one wheel for all. Stragglers after that join the wheel already open.
        After(1.0, [this, t] {
            if (t != openToken || opened.empty() || settings.paused) return;
            std::vector<wstring> paths = std::move(opened);
            opened.clear();
            if (wheel.open() && Now() - lastOpenedAt < 3) {
                paths.insert(paths.begin(), lastOpened.begin(), lastOpened.end());
                wheel.Hide(false);
            } else if (wheel.open()) return;
            lastOpened = paths;
            lastOpenedAt = Now();
            Log(L"explorer menu: %d item(s)", int(paths.size()));
            wheel.ShowForKeyboard(paths);
        });
    }

    void StartTour()
    {
        tour.Start([this] { return wheel.opens(); }, [this] { return choices; });
    }

    void Chosen(const WheelOption& o, const std::vector<wstring>& paths, POINT at)
    {
        const Converter* c = o.conv;
        choices++;
        Log(L"chosen: %s (%s) on %d item(s)", o.label.c_str(), c->id, int(paths.size()));
        cards.AnchorAt(at);
        wstring id = c->id;
        if (c->mode == Converter::Action) {
            if (id == L"act.copyto") CopyOrMove(paths, L"", false, settings, cards);
            else if (id == L"act.moveto") CopyOrMove(paths, L"", true, settings, cards);
            else if (id == L"act.copyrecent") CopyOrMove(paths, o.arg, false, settings, cards);
            else if (id == L"act.copypath") CopyPaths(paths, cards);
            else if (id == L"act.terminal") OpenTerminal(paths, cards);
            else if (id == L"act.float") {
                wstring original = paths[0];
                Preview(original, 2048, [this, original, at](const wstring& png, int w, int h) {
                    auto f = std::make_unique<FloatCard>(original, png, w, h, at);
                    if (f->ok()) floats.push_back(std::move(f));
                });
            }
            return;
        }
        if (c->popup == Popup::None) { Submit(o, paths, Json::Object()); return; }
        if (c->popup == Popup::Place) {
            OpenPlace(o, paths[0], id == L"pdf.sign" ? PlaceWindow::Mode::Sign : id == L"pdf.fill" ? PlaceWindow::Mode::Fill : PlaceWindow::Mode::Picture);
            return;
        }

        // Options first: the popup opens where the wheel was.
        popup.onApply = [this, o, paths](const Json& opts) {
            if (opts.has(L"q")) { settings.compressQuality = int(opts[L"q"].Num()); settings.Save(); }
            Submit(o, paths, opts);
        };
        wstring title = paths.size() == 1 ? o.label + L" " + FileName(paths[0]) : o.label + L" " + std::to_wstring(paths.size()) + L" pictures";
        if (c->popup == Popup::Quality) popup.Show(Popup::Quality, L"Compress", at, settings.compressQuality);
        else if (c->popup == Popup::Size) popup.Show(Popup::Size, L"Resize to", at, 0);
        else if (c->popup == Popup::Pages) popup.Show(Popup::Pages, L"Remove pages from " + FileName(paths[0]), at, 0);
        else if (c->popup == Popup::Trim) popup.Show(Popup::Trim, L"Trim " + (paths.size() == 1 ? FileName(paths[0]) : L""), at, 0);
        else if (c->popup == Popup::Crop) {
            Preview(paths[0], 1200, [this, at, title](const wstring& png, int w, int h) { popup.Show(Popup::Crop, title, at, 0, png, w, h); });
        }
    }

    // ------------------------------------------------------------ keyboard route

    void RegisterWheelHotkey()
    {
        hotkey = RegisterHotKey(g_msg, HOTKEY_WHEEL, UINT(settings.hotkeyMods) | MOD_NOREPEAT, UINT(settings.hotkeyKey)) != FALSE;
        if (!hotkey) Log(L"hotkey: could not register it (another app uses it); change hotkeyMods/hotkeyKey in settings.json");
    }

    void OnHotkey()
    {
        if (settings.paused || wheel.open()) return;
        auto paths = ExplorerSelection();
        if (paths.empty()) {
            cards.AnchorAt(CursorPos());
            cards.Note(L"Nothing selected", L"Select files in Explorer or on the desktop, then press the hotkey.");
            return;
        }
        Log(L"hotkey: %d item(s) selected in Explorer", int(paths.size()));
        wheel.ShowForKeyboard(paths);
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
        AppendMenuW(m, MF_STRING, CmdSettings, L"Settings…");
        AppendMenuW(m, MF_STRING, CmdTour, L"Take the tour");
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
            case CmdSettings: settingsUi.Show(); break;
            case CmdTour: StartTour(); break;
            case CmdLog: ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + JoinPath(DataDir(), L"log.txt") + L"\"").c_str(), nullptr, SW_SHOWNORMAL); break;
            case CmdQuit: PostQuitMessage(0); break;
        }
    }

    // ------------------------------------------------------------ frames and the loop

    bool NeedFrame() const
    {
        if (AnyAnimating() || wheel.dirty() || cards.dirty() || popup.dirty() || settingsUi.dirty() || placeUi.dirty() || tour.dirty() || g_frameWanted) return true;
        for (auto& f : floats) if (f->dirty()) return true;
        return false;
    }

    void Frame()
    {
        double now = Now();
        double dt = lastFrame > 0 ? std::min(0.05, now - lastFrame) : 1.0 / 60;
        lastFrame = now;
        g_frameWanted = false;
        StepAll(dt);
        wheel.Draw();
        cards.Draw();
        if (popup.shown()) popup.Draw();
        if (settingsUi.shown()) settingsUi.Draw();
        if (placeUi.shown()) placeUi.Draw();
        if (tour.active()) tour.Draw();
        for (auto& f : floats) f->Draw();
        floats.erase(std::remove_if(floats.begin(), floats.end(), [](auto& f) { return f->closed(); }), floats.end());
    }

    /// 1 ms timer resolution only while something moves or a drag is watched: it costs battery system-wide.
    void SetFastTimer(bool on)
    {
        if (on == fastTimer) return;
        fastTimer = on;
        if (on) timeBeginPeriod(1); else timeEndPeriod(1);
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
        MSG msg;
        for (;;) {
            double timeout = -1;
            double now = Now();
            auto want = [&](double secs) { timeout = timeout < 0 ? secs : std::min(timeout, secs); };
            if (NeedFrame()) want(std::max(0.0, nextFrame - now));
            if (trigger.NeedsTick()) want(trigger.NextTickIn());
            double due = NextDueIn();
            if (due >= 0) want(due);

            SetFastTimer(NeedFrame() || trigger.NeedsTick());
            DWORD ms = timeout < 0 ? INFINITE : DWORD(std::ceil(timeout * 1000));
            MsgWaitForMultipleObjectsEx(0, nullptr, ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) return int(msg.wParam);
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
        case WM_SHOW_SETTINGS: if (g_app) g_app->settingsUi.Show(); return 0;
        case WM_COPYDATA: {
            auto* cd = reinterpret_cast<COPYDATASTRUCT*>(l);
            if (g_app && cd && cd->dwData == 1 && cd->cbData >= 2) g_app->Open(wstring(static_cast<const wchar_t*>(cd->lpData), cd->cbData / 2 - 1));
            return TRUE;
        }
        case WM_SHOW_TOUR: if (g_app) g_app->StartTour(); return 0;
        case WM_HOTKEY:
            if (g_app && w == HOTKEY_WHEEL) g_app->OnHotkey();
            return 0;
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

    // Worker, CLI and info modes: no tray, no single-instance lock.
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool handled = false;
    int other = RunOtherMode(argc, argv, handled);
    LocalFree(argv);
    if (handled) return other;

    wstring lockName = L"Roda.SingleInstance";
    if (TestMode()) lockName += L".Test." + std::to_wstring(std::hash<wstring>{}(DataDir()) % 1000000);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, lockName.c_str());
    bool another = !mutex || GetLastError() == ERROR_ALREADY_EXISTS;   // read now: later calls overwrite the error
    // "Roda.exe --open <path>": from the Explorer entry.
    wstring openPath;
    {
        int n = 0;
        wchar_t** a = CommandLineToArgvW(GetCommandLineW(), &n);
        for (int i = 1; i + 1 < n; i++) if (!wcscmp(a[i], L"--open")) openPath = a[i + 1];
        LocalFree(a);
    }
    if (another) {
        // Roda is already running: hand it the path, or open its Settings (or the tour, with --tour).
        // It may be starting this very moment (several files opened at once): wait for its window.
        HWND other = nullptr;
        for (int i = 0; i < 30 && !other; i++) { other = FindWindowW(L"RodaMessages", lockName.c_str()); if (!other) Sleep(100); }
        if (other) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            if (!openPath.empty()) {
                COPYDATASTRUCT cd{ 1, DWORD((openPath.size() + 1) * 2), const_cast<wchar_t*>(openPath.c_str()) };
                SendMessageTimeoutW(other, WM_COPYDATA, 0, LPARAM(&cd), SMTO_ABORTIFHUNG, 3000, nullptr);
            } else {
                PostMessageW(other, wcsstr(GetCommandLineW(), L"--tour") ? WM_SHOW_TOUR : WM_SHOW_SETTINGS, 0, 0);
            }
        }
        return 0;
    }

    OleInitialize(nullptr);   // apartment-threaded COM: OLE drag and drop needs it
    if (!GfxInit()) { MessageBoxW(nullptr, L"Roda could not start its graphics.", L"Roda", MB_ICONERROR); return 1; }

    WNDCLASSW wc{};
    wc.lpfnWndProc = MsgProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"RodaMessages";
    RegisterClassW(&wc);
    g_msg = CreateWindowExW(0, wc.lpszClassName, lockName.c_str(), WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);   // found by name by a second start

    int rc;
    {
        App app;
        g_app = &app;
        app.Start(inst);
        if (!openPath.empty()) app.Open(openPath);   // started by the Explorer entry
        rc = app.Run();
        app.Shutdown();
        Log(L"quit");
        g_app = nullptr;
    }
    DestroyWindow(g_msg);
    OleUninitialize();
    return rc;
}
