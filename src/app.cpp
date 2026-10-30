// The application: single instance, tray icon and menu, and the main loop.
// Nothing runs at idle: the loop sleeps until a message, a scheduled job or a frame is due.
#include "actions.h"
#include "anim.h"
#include "cards.h"
#include "dragdrop.h"
#include "bubble.h"
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
constexpr int HOTKEY_MERGE = 2;   // Ctrl+Alt+M: the files selected in Explorer, merged into one PDF

enum Cmd { CmdOutput = 1, CmdPause, CmdStartup, CmdSettings, CmdTour, CmdLog, CmdQuit, CmdChoose, CmdBubble, CmdRecent = 100 };   // CmdRecent: 4 per entry

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
// "Merge into one PDF", on PDFs and pictures only: straight to the order of the files.
const wchar_t* kMergeTypes[] = { L".pdf", L".jpg", L".jpeg", L".png", L".webp", L".gif", L".bmp", L".tif", L".tiff", L".heic", L".avif", L".svg", L".ico" };

bool ExplorerMenuOn()
{
    wchar_t cmd[1024] = L"";
    DWORD size = sizeof cmd;
    if (RegGetValueW(HKEY_CURRENT_USER, (wstring(kVerbKeys[0]) + L"\\command").c_str(), nullptr, RRF_RT_REG_SZ, nullptr, cmd, &size) != ERROR_SUCCESS) return false;
    return wcsstr(cmd, ExePath().c_str()) != nullptr;   // another copy's entry is not ours
}

void SetExplorerMenu(bool on)
{
    auto verb = [on](const wstring& key, const wchar_t* label, const wchar_t* option) {
        if (!on) { RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str()); return; }
        HKEY k = nullptr, c = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
        auto set = [](HKEY h, const wchar_t* name, const wstring& v) { RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * 2)); };
        set(k, L"MUIVerb", label);
        set(k, L"Icon", L"\"" + ExePath() + L"\",0");
        set(k, L"MultiSelectModel", L"Player");   // a selection of many files is one wheel, not one per file
        if (RegCreateKeyExW(k, L"command", 0, nullptr, 0, KEY_WRITE, nullptr, &c, nullptr) == ERROR_SUCCESS) {
            set(c, nullptr, L"\"" + ExePath() + L"\" " + option + L" \"%1\"");
            RegCloseKey(c);
        }
        RegCloseKey(k);
    };
    for (auto* key : kVerbKeys) verb(key, L"Roda…", L"--open");
    for (auto* type : kMergeTypes) verb(wstring(L"Software\\Classes\\SystemFileAssociations\\") + type + L"\\shell\\RodaMerge", L"Merge into one PDF", L"--merge");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    Log(on ? L"explorer menu: added (Roda… on files and folders, Merge into one PDF on PDFs and pictures)" : L"explorer menu: removed");
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
    std::unique_ptr<Bubble> bubble;    // the floating button, while it's switched on
    std::map<int, std::function<void(const JobState&)>> waiting;   // quiet jobs (previews): who wants the result
    struct Finished { wstring title, what, out, error; bool ok = false; };
    std::vector<Finished> recent;    // the tray's Recent menu, newest first: results outlive their card
    HINSTANCE inst = nullptr;
    NOTIFYICONDATAW nid{};
    bool trayAdded = false, hotkey = false, mergeHotkey = false, fastTimer = false;
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
            if (mergeHotkey) UnregisterHotKey(g_msg, HOTKEY_MERGE);
            RegisterWheelHotkey();
        };
        // The Explorer entries are on from the first start; after that they're the user's to switch off.
        // An entry from an older Roda gains the ones added since (Merge into one PDF).
        if (!TestMode() && !settings.explorerMenuSet) { SetExplorerMenu(true); settings.explorerMenuSet = true; settings.Save(); }
        else if (ExplorerMenuOn()) SetExplorerMenu(true);
        settingsUi.startupOn = [] { return StartupOn(); };
        settingsUi.setStartup = [](bool on) { if (on != StartupOn()) SetStartup(on); };
        settingsUi.explorerMenuOn = [] { return ExplorerMenuOn(); };
        settingsUi.setExplorerMenu = [](bool on) { if (on != ExplorerMenuOn()) SetExplorerMenu(on); };
        tour.onPractice = [this] { Practice(); };
        tour.onFinished = [this] { if (!settings.welcomed) { settings.welcomed = true; settings.Save(); } };
        // First run: the tour, once things have settled.
        if (!settings.welcomed && !TestMode()) After(1.5, [this] { StartTour(); });
        wheel.onChoose = [this](const WheelOption& o, const std::vector<wstring>& paths, POINT at) { Chosen(o, paths, at); };
        wheel.onMore = [this](const std::vector<WheelOption>& rest, const std::vector<wstring>& paths, POINT at) { ChooseMore(rest, paths, at); };
        jobs.onChange = [this](const JobState& s) {
            if (!s.quiet) {
                cards.Update(s);
                if (s.phase == JobState::Done || s.phase == JobState::Failed) {
                    recent.insert(recent.begin(), { s.title, s.what, s.out, s.error, s.phase == JobState::Done });
                    if (recent.size() > 10) recent.pop_back();
                }
                return;
            }
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
        if (settings.bubble && !TestMode()) SetBubble(true);
        if (TestMode()) WatchMemory();
        return true;
    }

    /// The floating button on or off (remembered).
    void SetBubble(bool on)
    {
        if (on && !bubble) {
            auto b = std::make_unique<Bubble>();
            b->onClick = [this](POINT c) { if (!settings.paused && !wheel.open()) wheel.ShowWaiting(&c); };
            b->onDrop = [this](const std::vector<wstring>& paths, POINT c) {
                if (settings.paused) return;
                if (wheel.open()) wheel.Hide(false);
                Log(L"bubble: %d file(s) dropped", int(paths.size()));
                wheel.ShowFor(paths, c);
            };
            b->onMenu = [this] { TrayMenu(); };
            b->onMoved = [this](POINT c) { settings.bubbleX = c.x; settings.bubbleY = c.y; settings.Save(); };
            if (!b->Create(GetModuleHandleW(nullptr), { settings.bubbleX, settings.bubbleY })) { Log(L"bubble: could not create it"); return; }
            bubble = std::move(b);
            WatchFullScreen();
        } else if (!on) bubble.reset();
        if (settings.bubble != on) { settings.bubble = on; settings.Save(); }
        Log(on ? L"bubble: on" : L"bubble: off");
    }

    /// The floating button steps aside while a game or a full-screen video is in front of it, as the wheel does.
    void WatchFullScreen()
    {
        if (!bubble) return;
        bubble->Show(!FullScreenAppAt(bubble->centre()));
        After(1.5, [this] { WatchFullScreen(); });
    }

    void Shutdown()
    {
        jobs.CancelAll();
        bubble.reset();   // its drop target goes before OLE does
        if (hotkey) UnregisterHotKey(g_msg, HOTKEY_WHEEL);
        if (mergeHotkey) UnregisterHotKey(g_msg, HOTKEY_MERGE);
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

    /// "photo.jpg", or "3 pictures" / "3 videos" / "3 files".
    static wstring Named(const std::vector<wstring>& paths)
    {
        if (paths.size() == 1) return FileName(paths[0]);
        bool images = true, videos = true;
        for (auto& p : paths) { Kind k = KindFromExtension(p); images = images && IsImage(k); videos = videos && (Bit(k) & kVideo) != 0; }
        return std::to_wstring(paths.size()) + (images ? L" pictures" : videos ? L" videos" : L" files");
    }

    /// The popup's main button: what happens, to how many ("Resize 3 pictures").
    static wstring ApplyLabel(Popup kind, const std::vector<wstring>& paths)
    {
        const wchar_t* verb = kind == Popup::Quality ? L"Compress" : kind == Popup::Size ? L"Resize" : kind == Popup::Trim ? L"Trim" :
                              kind == Popup::Pages ? L"Remove pages" : kind == Popup::Order ? L"Merge into one PDF" : L"Crop";
        if (paths.size() < 2 || kind == Popup::Order || kind == Popup::Pages) return verb;
        return wstring(verb) + L" " + Named(paths);
    }

    /// Where the results will be, said before they're made (the same rule as MakeJob).
    wstring WhereText(const std::vector<wstring>& paths, bool oneResult) const
    {
        if (!settings.outputFolder.empty()) return L"Saved in " + settings.outputFolder;
        if (!paths.empty() && paths[0].rfind(TempDir(), 0) == 0) return L"Saved in your Downloads folder";
        wstring folder = paths.empty() ? L"" : FileName(ParentDir(paths[0]));
        if (oneResult) return L"Saved next to the first file, in " + folder;
        return paths.size() == 1 ? L"Saved next to the original, in " + folder : L"Saved next to each original";
    }

    std::vector<wstring> opened;     // paths from "Roda…" in Explorer, gathered for a moment
    std::vector<wstring> lastOpened; // the last batch shown, in case more of the same selection arrives
    double lastOpenedAt = -10;
    uint32_t openToken = 0;
    bool openMerge = false;          // they came from "Merge into one PDF", not "Roda…"

    void Open(const wstring& path, bool merge = false)
    {
        if (path.empty()) return;
        opened.push_back(path);
        openMerge = merge;
        uint32_t t = ++openToken;
        // Explorer starts the verb once per selected file, each a moment apart: wait for the rest, then
        // one wheel for all. Stragglers after that join the wheel already open.
        After(1.0, [this, t] {
            if (t != openToken || opened.empty() || settings.paused) return;
            std::vector<wstring> paths = std::move(opened);
            opened.clear();
            if (openMerge) { Log(L"explorer menu: merge %d item(s)", int(paths.size())); ShowMerge(paths); return; }
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

    /// More… on the wheel: the rest of the choices as a list where the wheel was.
    void ChooseMore(const std::vector<WheelOption>& rest, const std::vector<wstring>& paths, POINT at)
    {
        if (rest.empty()) return;
        HMENU m = CreatePopupMenu();
        for (size_t i = 0; i < rest.size(); i++) {
            wstring label = rest[i].label;
            for (size_t k = 0; (k = label.find(L'&', k)) != wstring::npos; k += 2) label.insert(k, 1, L'&');
            AppendMenuW(m, MF_STRING, UINT_PTR(i + 1), label.c_str());
        }
        BeginModal();
        SetForegroundWindow(g_msg);   // so a click elsewhere closes the list
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_CENTERALIGN | TPM_VCENTERALIGN, at.x, at.y, 0, g_msg, nullptr);
        EndModal();
        DestroyMenu(m);
        if (cmd > 0 && size_t(cmd) <= rest.size()) Chosen(rest[size_t(cmd - 1)], paths, at);
        else Log(L"wheel: More… closed, nothing chosen");
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
        if (id == L"pdf.mergewith") {
            // One PDF: the files to merge it with are picked now (from its folder), then put in order as usual.
            auto more = PickFiles(L"Merge " + FileName(paths[0]) + L" with", L"Merge", ParentDir(paths[0]), true);
            if (more.empty()) return;
            std::vector<wstring> all = paths;
            for (auto& m : more) if (m != paths[0]) all.push_back(m);
            Log(L"merge with: %d more file(s) picked", int(all.size() - 1));
            ShowMerge(all);
            return;
        }
        if (c->popup == Popup::None) { Submit(o, paths, Json::Object()); return; }
        if (c->popup == Popup::Place) {
            OpenPlace(o, paths[0], id == L"pdf.sign" ? PlaceWindow::Mode::Sign : id == L"pdf.fill" ? PlaceWindow::Mode::Fill : PlaceWindow::Mode::Picture);
            return;
        }

        // Options first: the popup opens where the wheel was. Its button says what and to how many; above it,
        // where the results will go.
        popup.SetSummary(ApplyLabel(c->popup, paths), WhereText(paths, c->mode == Converter::AllInOne));
        popup.onApply = [this, o, paths](const Json& opts) {
            if (opts.has(L"op")) {   // one of Resize's sizes: that size's own tool
                if (const Converter* sized = FindConverter(opts[L"op"].Str())) Submit({ sized, sized->label, L"", o.icon }, paths, Json::Object());
                return;
            }
            if (opts.has(L"q")) { settings.compressQuality = int(opts[L"q"].Num()); settings.Save(); }
            // Merge: the files the basket ended with, in its order (some may have been added or taken out).
            std::vector<wstring> files;
            if (opts.has(L"files")) for (auto& f : opts[L"files"].arr) files.push_back(f.Str());
            Submit(o, files.empty() ? paths : files, opts);
        };
        wstring title = paths.size() == 1 ? o.label + L" " + FileName(paths[0]) : o.label + L" " + std::to_wstring(paths.size()) + L" pictures";
        if (c->popup == Popup::Quality) popup.Show(Popup::Quality, L"Compress", at, settings.compressQuality);
        else if (c->popup == Popup::Size) {
            // The usual sizes in one click; a picture can also have exact pixels.
            if (id == L"media.resize") popup.SetPresets({ { L"1080p", L"media.1080" }, { L"720p", L"media.720" }, { L"480p", L"media.480" } }, false);
            else popup.SetPresets({ { L"50%", L"tool.resize50" }, { L"1920 px", L"tool.resize1920" }, { L"1080 px", L"tool.resize1080" } }, true);
            popup.Show(Popup::Size, L"Resize " + Named(paths), at, 0);
        }
        else if (c->popup == Popup::Pages) {
            // Pictures of the pages, small, each from a quiet job as it scrolls into view.
            wstring pdf = paths[0];
            popup.onNeedPage = [this, pdf](int token, int page) {
                Job j = MakeJob(L"prep.preview", { pdf }, Json::Object().Set(L"max", 420).Set(L"page", page));
                static int n = 0;
                j.outPath = JoinPath(TempDir(), L"thumb-" + std::to_wstring(++n) + L".png");
                j.outDir.clear();
                int jid = jobs.Submit(j, FileName(pdf), L"Page", true);
                waiting[jid] = [this, token, page, pdf](const JobState& s) {
                    if (s.phase == JobState::Done) { popup.SetPage(token, page, s.out, int(s.info[L"info"][L"pages"].Num(0))); return; }
                    // The first page can't be drawn: the PDF can't be opened (a password, damage). Say so.
                    if (page == 0 && popup.shown() && popup.token() == token) {
                        popup.Hide();
                        cards.Note(FileName(pdf), s.error.empty() ? L"Could not open this PDF." : s.error, true);
                    }
                };
            };
            popup.ShowPages(L"Remove pages from " + FileName(pdf), at);
        }
        else if (c->popup == Popup::Trim) popup.Show(Popup::Trim, L"Trim " + (paths.size() == 1 ? FileName(paths[0]) : L""), at, 0);
        else if (c->popup == Popup::Order) {
            popup.whereFor = [this](const wstring& first) { return WhereText({ first }, true); };
            popup.onAddFiles = [this, paths] {
                wstring from = paths.empty() ? L"" : ParentDir(paths[0]);
                popup.AddFiles(PickFiles(L"Add files to merge", L"Add", from, true));
            };
            popup.ShowOrder(paths, at);
        }
        else if (c->popup == Popup::Crop) {
            wstring apply = ApplyLabel(c->popup, paths), where = WhereText(paths, false);
            Preview(paths[0], 1200, [this, at, title, apply, where](const wstring& png, int w, int h) {
                popup.SetSummary(apply, where);   // the preview took a moment: say it again, another popup may have cleared it
                popup.Show(Popup::Crop, title, at, 0, png, w, h);
            });
        }
    }

    // ------------------------------------------------------------ keyboard route

    void RegisterWheelHotkey()
    {
        hotkey = RegisterHotKey(g_msg, HOTKEY_WHEEL, UINT(settings.hotkeyMods) | MOD_NOREPEAT, UINT(settings.hotkeyKey)) != FALSE;
        if (!hotkey) Log(L"hotkey: could not register it (another app uses it); change hotkeyMods/hotkeyKey in settings.json");
        mergeHotkey = RegisterHotKey(g_msg, HOTKEY_MERGE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'M') != FALSE;
        if (!mergeHotkey) Log(L"hotkey: could not register Ctrl+Alt+M (another app uses it)");
    }

    /// Merge PDF straight away (the Explorer entry, Ctrl+Alt+M): the order of the PDFs and pictures among `paths`.
    void ShowMerge(std::vector<wstring> paths)
    {
        const Converter* c = FindConverter(L"tool.mergepdf");
        if (!c) return;
        std::erase_if(paths, [&](const wstring& p) { return (Bit(KindOfPath(p)) & c->inputs) == 0; });
        POINT at = CursorPos();
        cards.AnchorAt(at);
        Chosen({ c, c->label, L"", c->icon }, paths, at);   // fewer than two: the basket opens to add the rest
    }

    void OnMergeHotkey()
    {
        auto paths = ExplorerSelection();
        if (paths.empty() && ExplorerInFront()) {
            // Explorer or the desktop, nothing selected: the wheel, waiting for files to be dragged onto it.
            if (settings.paused || wheel.open() || popup.shown()) return;
            Log(L"hotkey: merge, nothing selected: the wheel waits for files");
            wheel.ShowWaiting();
            return;
        }
        if (paths.empty()) {
            // Another app in front: the keys are its (Word's Ctrl+Alt+M is a new comment). Let go of them,
            // send them on, and take them back once they've arrived.
            UnregisterHotKey(g_msg, HOTKEY_MERGE);
            mergeHotkey = false;
            INPUT in[2]{};
            in[0].type = in[1].type = INPUT_KEYBOARD;
            in[0].ki.wVk = in[1].ki.wVk = 'M';
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, in, sizeof(INPUT));
            After(0.3, [this] { if (!mergeHotkey) mergeHotkey = RegisterHotKey(g_msg, HOTKEY_MERGE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'M') != FALSE; });
            return;
        }
        if (settings.paused || wheel.open() || popup.shown()) return;
        Log(L"hotkey: merge %d item(s) selected in Explorer", int(paths.size()));
        ShowMerge(paths);
    }

    void OnHotkey()
    {
        if (settings.paused || wheel.open()) return;
        auto paths = ExplorerSelection();
        if (paths.empty()) {   // nothing selected: the wheel waits for files to be dragged onto it
            Log(L"hotkey: nothing selected, the wheel waits for files");
            wheel.ShowWaiting();
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

    /// Files picked in Windows' own dialog, several at once; empty when cancelled. It starts in `folder`
    /// when given, and with `mergeable` shows PDFs and pictures first.
    std::vector<wstring> PickFiles(const wstring& title, const wchar_t* ok, const wstring& folder = L"", bool mergeable = false)
    {
        std::vector<wstring> paths;
        Com<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.Put())))) return paths;
        DWORD o = 0;
        dlg->GetOptions(&o);
        dlg->SetOptions(o | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
        dlg->SetTitle(title.c_str());
        dlg->SetOkButtonLabel(ok);
        if (mergeable) {
            COMDLG_FILTERSPEC types[] = { { L"PDFs and pictures", L"*.pdf;*.jpg;*.jpeg;*.png;*.webp;*.gif;*.bmp;*.tif;*.tiff;*.heic;*.avif;*.svg;*.ico" }, { L"All files", L"*.*" } };
            dlg->SetFileTypes(2, types);
        }
        Com<IShellItem> start;
        if (!folder.empty() && SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(start.Put())))) dlg->SetFolder(start.Get());
        BeginModal();
        HRESULT hr = dlg->Show(nullptr);
        EndModal();
        Com<IShellItemArray> items;
        if (FAILED(hr) || FAILED(dlg->GetResults(items.Put()))) return paths;
        DWORD n = 0;
        items->GetCount(&n);
        for (DWORD i = 0; i < n; i++) {
            Com<IShellItem> item;
            PWSTR p = nullptr;
            if (SUCCEEDED(items->GetItemAt(i, item.Put())) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) paths.push_back(p);
            CoTaskMemFree(p);
        }
        return paths;
    }

    /// Files picked in a dialog, then the wheel for them (driven by clicks or keys, like "Roda…" in Explorer).
    void ChooseFiles()
    {
        auto paths = PickFiles(L"Choose files for Roda", L"Choose");
        if (paths.empty() || settings.paused) return;
        Log(L"choose files: %d item(s)", int(paths.size()));
        wheel.ShowForKeyboard(paths);
    }

    /// The tour's practice file: a picture in Roda's own folder, shown selected in Explorer, ready to drag.
    void Practice()
    {
        wstring dir = JoinPath(DataDir(), L"Practice"), png = JoinPath(dir, L"Practice picture.png");
        EnsureDir(dir);
        if (!FileExists(png) && !DrawPractice(png)) { cards.AnchorAt(CursorPos()); cards.Note(L"Practice file", L"Could not make the practice picture.", true); return; }
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(png.c_str());
        if (pidl) { SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0); ILFree(pidl); }
        Log(L"tour: practice file at %s", png.c_str());
    }

    static bool DrawPractice(const wstring& png)
    {
        const UINT w = 1200, h = 800;
        Com<IWICBitmap> bmp;
        Com<ID2D1RenderTarget> rt;
        if (FAILED(Wic()->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bmp.Put()))) return false;
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(D2D()->CreateWicBitmapRenderTarget(bmp.Get(), props, rt.Put()))) return false;
        Com<ID2D1SolidColorBrush> sky, sun, hill, ink;
        rt->CreateSolidColorBrush(Argb(0xFF9FD3E6), sky.Put());
        rt->CreateSolidColorBrush(Argb(0xFFF6C453), sun.Put());
        rt->CreateSolidColorBrush(Argb(0xFF2E8B6E), hill.Put());
        rt->CreateSolidColorBrush(Argb(0xFF14302A), ink.Put());
        rt->BeginDraw();
        rt->Clear(Argb(0xFF9FD3E6));
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(930, 210), 110, 110), sun.Get());
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(300, 900), 700, 380), hill.Get());
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(1050, 950), 600, 360), hill.Get());
        const wchar_t* text = L"Drag me, then press Space";
        if (auto* fmt = TextFormat(64, DWRITE_FONT_WEIGHT_SEMI_BOLD)) rt->DrawText(text, UINT32(wcslen(text)), fmt, D2D1::RectF(80, 320, 1150, 420), ink.Get());
        if (FAILED(rt->EndDraw())) return false;
        Com<IWICStream> stream;
        Com<IWICBitmapEncoder> enc;
        Com<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        return SUCCEEDED(Wic()->CreateStream(stream.Put())) && SUCCEEDED(stream->InitializeFromFilename(png.c_str(), GENERIC_WRITE))
            && SUCCEEDED(Wic()->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.Put())) && SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))
            && SUCCEEDED(enc->CreateNewFrame(frame.Put(), nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h))
            && SUCCEEDED(frame->SetPixelFormat(&fmt)) && SUCCEEDED(frame->WriteSource(bmp.Get(), nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    }

    void UpdateTip()
    {
        wcscpy(nid.szTip, settings.paused ? L"Roda (paused)" : L"Roda: drag a file, then press Space");
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
        AppendMenuW(m, MF_STRING, CmdChoose, L"Choose files…");
        // Recent: each result with Open and Show in folder, each failure with why.
        HMENU rec = CreatePopupMenu();
        for (size_t i = 0; i < recent.size(); i++) {
            const Finished& d = recent[i];
            HMENU item = CreatePopupMenu();
            UINT base = CmdRecent + UINT(i) * 4;
            if (d.ok) { AppendMenuW(item, MF_STRING, base, L"Open"); AppendMenuW(item, MF_STRING, base + 1, L"Show in folder"); }
            else AppendMenuW(item, MF_STRING, base + 2, L"Why it failed");
            wstring label = d.ok ? FileName(d.out) + L"   from " + d.title : L"Failed: " + d.title + L" (" + d.what + L")";
            for (size_t k = 0; (k = label.find(L'&', k)) != wstring::npos; k += 2) label.insert(k, 1, L'&');   // a plain & in a menu
            AppendMenuW(rec, MF_POPUP, UINT_PTR(item), label.c_str());
        }
        AppendMenuW(m, MF_POPUP | (recent.empty() ? MF_GRAYED : 0), UINT_PTR(rec), L"Recent");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (fixedOutput ? 0 : MF_GRAYED), CmdOutput, fixedOutput ? L"Open output folder" : L"Output: next to each file");
        AppendMenuW(m, MF_STRING | (settings.paused ? MF_CHECKED : 0), CmdPause, L"Pause Roda");
        AppendMenuW(m, MF_STRING | (StartupOn() ? MF_CHECKED : 0), CmdStartup, L"Start with Windows");
        AppendMenuW(m, MF_STRING | (bubble ? MF_CHECKED : 0), CmdBubble, L"Floating button");
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
        if (cmd >= CmdRecent && size_t(cmd - CmdRecent) / 4 < recent.size()) {
            const Finished d = recent[size_t(cmd - CmdRecent) / 4];
            int what = (cmd - CmdRecent) % 4;
            cards.AnchorAt(CursorPos());
            if (what < 2 && !FileExists(d.out)) cards.Note(FileName(d.out), L"That file isn't there any more (moved or deleted).", true);
            else if (what == 0) ShellExecuteW(nullptr, L"open", d.out.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            else if (what == 1) {
                PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(d.out.c_str());
                if (pidl) { SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0); ILFree(pidl); }
            } else cards.Note(d.title, d.error.empty() ? L"It failed without saying why. The log has more." : d.error, true);
            return;
        }
        switch (cmd) {
            case CmdChoose: ChooseFiles(); break;
            case CmdOutput: EnsureDir(settings.outputFolder); ShellExecuteW(nullptr, L"open", settings.outputFolder.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
            case CmdPause: SetPaused(!settings.paused); break;
            case CmdStartup: SetStartup(!StartupOn()); break;
            case CmdBubble: SetBubble(!bubble); break;
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
        if (bubble && bubble->dirty()) return true;
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
        if (bubble && bubble->shown()) bubble->Draw();
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
            // 1: "Roda…", 2: "Merge into one PDF"
            if (g_app && cd && (cd->dwData == 1 || cd->dwData == 2) && cd->cbData >= 2)
                g_app->Open(wstring(static_cast<const wchar_t*>(cd->lpData), cd->cbData / 2 - 1), cd->dwData == 2);
            return TRUE;
        }
        case WM_SHOW_TOUR: if (g_app) g_app->StartTour(); return 0;
        case WM_HOTKEY:
            if (g_app && w == HOTKEY_WHEEL) g_app->OnHotkey();
            if (g_app && w == HOTKEY_MERGE) g_app->OnMergeHotkey();
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
    // "Roda.exe --open <path>" or "--merge <path>": from the Explorer entries.
    wstring openPath;
    bool merge = false;
    {
        int n = 0;
        wchar_t** a = CommandLineToArgvW(GetCommandLineW(), &n);
        for (int i = 1; i + 1 < n; i++)
            if (!wcscmp(a[i], L"--open") || !wcscmp(a[i], L"--merge")) { openPath = a[i + 1]; merge = !wcscmp(a[i], L"--merge"); }
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
                COPYDATASTRUCT cd{ ULONG_PTR(merge ? 2 : 1), DWORD((openPath.size() + 1) * 2), const_cast<wchar_t*>(openPath.c_str()) };
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
        if (!openPath.empty()) app.Open(openPath, merge);   // started by an Explorer entry
        rc = app.Run();
        app.Shutdown();
        Log(L"quit");
        g_app = nullptr;
    }
    DestroyWindow(g_msg);
    OleUninitialize();
    return rc;
}
