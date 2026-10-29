#include "common.h"

#include <cstdarg>

namespace rd {

double Now()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(freq.QuadPart);
}

wstring Widen(std::string_view s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string Narrow(std::wstring_view w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool FileExists(const wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

wstring FileName(const wstring& p)
{
    size_t i = p.find_last_of(L"\\/");
    return i == wstring::npos ? p : p.substr(i + 1);
}

wstring Extension(const wstring& p)
{
    wstring n = FileName(p);
    size_t i = n.find_last_of(L'.');
    if (i == wstring::npos) return {};
    wstring e = n.substr(i);
    for (auto& c : e) c = wchar_t(towlower(c));
    return e;
}

wstring JoinPath(const wstring& a, const wstring& b)
{
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

wstring ParentDir(const wstring& p)
{
    size_t i = p.find_last_of(L"\\/");
    return i == wstring::npos ? wstring() : p.substr(0, i);
}

bool SamePath(const wstring& a, const wstring& b)
{
    return CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
}

void EnsureDir(const wstring& p)
{
    if (p.empty() || DirExists(p)) return;
    EnsureDir(ParentDir(p));
    CreateDirectoryW(p.c_str(), nullptr);
}

void DeleteTree(const wstring& dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wstring p = JoinPath(dir, fd.cFileName);
            // A junction is removed, never followed: only Roda's own files are deleted.
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) DeleteTree(p);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p.c_str());
            else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

bool ReadAllBytes(const wstring& path, std::vector<uint8_t>& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    out.resize(size_t(size.QuadPart));
    DWORD got = 0;
    bool ok = out.empty() || ReadFile(h, out.data(), DWORD(out.size()), &got, nullptr);
    CloseHandle(h);
    return ok && got == out.size();
}

bool WriteAllBytes(const wstring& path, const void* data, size_t size)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, data, DWORD(size), &put, nullptr) && put == size;
    CloseHandle(h);
    return ok;
}

wstring EnvVar(const wchar_t* name)
{
    wchar_t buf[2048];
    DWORD n = GetEnvironmentVariableW(name, buf, 2048);
    return n > 0 && n < 2048 ? wstring(buf, n) : wstring();
}

bool TestMode() { static bool on = !EnvVar(L"RODA_DATA").empty(); return on; }

static wstring KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) r = p;
    CoTaskMemFree(p);
    return r;
}

wstring ExeDir()
{
    static wstring d = [] {
        wstring buf(32768, L'\0');
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
        buf.resize(n);
        return ParentDir(buf);
    }();
    return d;
}

/// Portable when a file can really be written next to the exe (a USB stick, a folder in Documents).
/// Program Files or a read-only share fall back to %APPDATA%.
static bool CanWriteNextToExe(const wstring& dataDir)
{
    EnsureDir(dataDir);
    wstring probe = JoinPath(dataDir, L".write-test");
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

static bool g_portable = false;

wstring DataDir()
{
    static wstring d = [] {
        wstring t = EnvVar(L"RODA_DATA");
        if (!t.empty()) return t;
        wstring local = JoinPath(ExeDir(), L"data");
        if (CanWriteNextToExe(local)) { g_portable = true; return local; }
        return JoinPath(KnownFolder(FOLDERID_RoamingAppData), L"Roda");
    }();
    return d;
}

bool PortableData() { DataDir(); return g_portable; }

static wstring TempRoot()
{
    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    return JoinPath(wstring(buf, n), L"Roda");
}

wstring TempDir()
{
    static wstring d = [] {
        wstring p = JoinPath(TempRoot(), std::to_wstring(GetCurrentProcessId()));
        EnsureDir(p);
        return p;
    }();
    return d;
}

void CleanStaleTemp()
{
    wstring root = TempRoot();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(JoinPath(root, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        DWORD pid = wcstoul(fd.cFileName, nullptr, 10);
        if (!pid || pid == GetCurrentProcessId()) continue;
        // A folder whose process still runs belongs to another Roda (or a worker): leave it.
        if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            DWORD code = 0;
            bool alive = GetExitCodeProcess(p, &code) && code == STILL_ACTIVE;
            CloseHandle(p);
            if (alive) continue;
        }
        DeleteTree(JoinPath(root, fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---------------------------------------------------------------- screens, DPI, appearance

double Scale()
{
    static double s = [] {
        using Fn = UINT(WINAPI*)();
        auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"));
        return fn ? fn() / 96.0 : 1.0;
    }();
    return s;
}

double ScaleAt(POINT p)
{
    // shcore.dll is on every Windows 10/11; looked up once, by full system path.
    using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static Fn fn = [] {
        HMODULE m = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return m ? reinterpret_cast<Fn>(GetProcAddress(m, "GetDpiForMonitor")) : nullptr;
    }();
    UINT x = 96, y = 96;
    if (fn && SUCCEEDED(fn(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), 0 /* MDT_EFFECTIVE_DPI */, &x, &y))) return x / 96.0;
    return Scale();
}

static int g_theme = 0;
void SetThemeOverride(int theme) { g_theme = theme; }

bool DarkMode()
{
    if (g_theme == 1) return false;
    if (g_theme == 2) return true;
    DWORD v = 1, size = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                 RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v == 0;
}

bool ReducedMotion()
{
    BOOL on = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
    return !on;
}

std::vector<Mon> Monitors()
{
    std::vector<Mon> out;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR h, HDC, LPRECT, LPARAM lp) -> BOOL {
            MONITORINFO mi{ sizeof(mi) };
            if (GetMonitorInfoW(h, &mi)) {
                Mon m;
                m.handle = h; m.bounds = mi.rcMonitor; m.work = mi.rcWork;
                reinterpret_cast<std::vector<Mon>*>(lp)->push_back(m);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&out));
    return out;
}

POINT CursorPos()
{
    POINT p{};
    GetCursorPos(&p);
    return p;
}

const Mon* MonitorAt(const std::vector<Mon>& all, POINT p)
{
    for (auto& m : all)
        if (p.x >= m.bounds.left && p.x < m.bounds.right && p.y >= m.bounds.top && p.y < m.bounds.bottom) return &m;
    return nullptr;
}

Mon MonitorOf(POINT p)
{
    Mon m;
    m.handle = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfoW(m.handle, &mi)) { m.bounds = mi.rcMonitor; m.work = mi.rcWork; }
    return m;
}

wstring ProcessNameOf(HWND w)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"?";
    wchar_t path[MAX_PATH] = L"";
    DWORD n = MAX_PATH;
    if (!QueryFullProcessImageNameW(p, 0, path, &n)) path[0] = 0;
    CloseHandle(p);
    wstring name = FileName(path);
    for (auto& c : name) c = wchar_t(towlower(c));
    return name;
}

wstring ClassOf(HWND w)
{
    wchar_t cls[96] = L"";
    if (w) GetClassNameW(w, cls, 96);
    return cls;
}

/// Screenshot overlays cover the whole screen while you snip, and on Windows 10 the
/// snipping overlay is often only cloaked afterwards. None of that is a full screen app.
/// (From Ampaian's full screen watcher.)
static bool IsCaptureOverlay(HWND w)
{
    BOOL cloaked = FALSE;
    if (!IsWindowVisible(w) || IsIconic(w)) return true;
    if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) return true;
    static const wchar_t* overlays[] = { L"screenclippinghost.exe", L"snippingtool.exe", L"screensketch.exe", L"shellexperiencehost.exe" };
    wstring name = ProcessNameOf(w);
    for (auto* o : overlays) if (name == o) return true;
    return false;
}

HWND FullScreenAppAt(POINT p)
{
    // Ampaian polls this four times a second on a thread; Roda only needs it when a drag
    // starts, so it is asked then and costs nothing while idle.
    int st = 0;
    bool busy = SHQueryUserNotificationState(reinterpret_cast<QUERY_USER_NOTIFICATION_STATE*>(&st)) == S_OK && (st == 2 || st == 3 || st == 4);   // busy, D3D full screen, presentation
    if (!busy) return nullptr;
    HWND fg = GetForegroundWindow();
    wstring cls = ClassOf(fg);
    if (!fg || cls == L"Progman" || cls == L"WorkerW" || cls == L"Shell_TrayWnd" || IsCaptureOverlay(fg)) return nullptr;
    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    if (mon != MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST)) return nullptr;
    // The app in front must really cover the whole screen: Windows reports "busy" in other situations too.
    MONITORINFO mi{ sizeof mi };
    RECT wr{};
    if (!GetMonitorInfoW(mon, &mi) || !GetWindowRect(fg, &wr)) return nullptr;
    const RECT& b = mi.rcMonitor;
    bool covers = wr.left <= b.left + 1 && wr.top <= b.top + 1 && wr.right >= b.right - 1 && wr.bottom >= b.bottom - 1;
    return covers ? fg : nullptr;
}

// ---------------------------------------------------------------- log

void Log(const wchar_t* fmt, ...)
{
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(msg, 1023, fmt, ap);
    va_end(ap);
    msg[1023] = 0;

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[1200];
    _snwprintf(line, 1199, L"%04d-%02d-%02dT%02d:%02d:%02d.%03d %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    line[1199] = 0;
    std::string utf8 = Narrow(line);

    EnsureDir(DataDir());
    wstring file = JoinPath(DataDir(), L"log.txt");
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 200000) DeleteFileW(file.c_str());   // keep it small
    HANDLE h = CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD put;
    WriteFile(h, utf8.data(), DWORD(utf8.size()), &put, nullptr);
    CloseHandle(h);
}

// ---------------------------------------------------------------- scheduler

namespace {
struct Job { double due; uint64_t seq; std::function<void()> fn; };
std::vector<Job>& Jobs() { static std::vector<Job> j; return j; }
uint64_t g_seq = 0;
}

void After(double seconds, std::function<void()> fn)
{
    Jobs().push_back({ Now() + seconds, g_seq++, std::move(fn) });
}

double NextDueIn()
{
    auto& j = Jobs();
    if (j.empty()) return -1;
    double best = j[0].due;
    for (auto& x : j) best = std::min(best, x.due);
    return std::max(0.0, best - Now());
}

void RunDue()
{
    auto& j = Jobs();
    for (;;) {
        double now = Now();
        size_t pick = j.size();
        for (size_t i = 0; i < j.size(); i++)
            if (j[i].due <= now && (pick == j.size() || j[i].due < j[pick].due || (j[i].due == j[pick].due && j[i].seq < j[pick].seq))) pick = i;
        if (pick == j.size()) return;
        auto fn = std::move(j[pick].fn);
        j.erase(j.begin() + pick);
        fn();   // may schedule more
    }
}

// ---------------------------------------------------------------- settings.json

static void AppendJsonString(std::string& out, const wstring& s)
{
    out += '"';
    for (wchar_t c : s) {
        if (c == L'\\') out += "\\\\";
        else if (c == L'"') out += "\\\"";
        else if (c < 0x20 || c > 0x7e) { char b[8]; snprintf(b, sizeof b, "\\u%04x", unsigned(c)); out += b; }
        else out += char(c);
    }
    out += '"';
}

/// The text right after `"key":`, or npos.
static size_t ValueAt(const std::string& j, const char* key)
{
    size_t k = j.find(std::string("\"") + key + "\"");
    if (k == std::string::npos) return k;
    size_t colon = j.find(':', k);
    if (colon == std::string::npos) return colon;
    return j.find_first_not_of(" \t\r\n", colon + 1);
}

static bool ParseBool(const std::string& j, const char* key, bool fallback)
{
    size_t v = ValueAt(j, key);
    if (v == std::string::npos) return fallback;
    return j.compare(v, 4, "true") == 0 ? true : (j.compare(v, 5, "false") == 0 ? false : fallback);
}

static int ParseInt(const std::string& j, const char* key, int fallback)
{
    size_t v = ValueAt(j, key);
    if (v == std::string::npos || !(isdigit(uint8_t(j[v])) || j[v] == '-')) return fallback;
    return int(strtol(j.c_str() + v, nullptr, 10));
}

static wstring ParseString(const std::string& j, const char* key)
{
    size_t i = ValueAt(j, key);
    if (i == std::string::npos || j[i] != '"') return {};
    i++;
    wstring cur;
    while (i < j.size() && j[i] != '"') {
        if (j[i] == '\\' && i + 1 < j.size()) {
            char e = j[i + 1];
            if (e == 'u' && i + 5 < j.size()) { cur += wchar_t(strtoul(j.substr(i + 2, 4).c_str(), nullptr, 16)); i += 6; continue; }
            cur += wchar_t(e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e);
            i += 2;
        } else {
            size_t st = i;   // raw UTF-8 runs: collect and widen
            while (i < j.size() && j[i] != '"' && j[i] != '\\') i++;
            cur += Widen(std::string_view(j).substr(st, i - st));
        }
    }
    return cur;
}

void Settings::Save() const
{
    std::string j = "{\n";
    auto num = [&](const char* k, int v) { j += "  \""; j += k; j += "\": "; j += std::to_string(v); j += ",\n"; };
    auto flag = [&](const char* k, bool v) { j += "  \""; j += k; j += "\": "; j += v ? "true" : "false"; j += ",\n"; };
    num("keyConvert", keyConvert);
    num("keyTools", keyTools);
    num("keyActions", keyActions);
    num("hotkeyMods", hotkeyMods);
    num("hotkeyKey", hotkeyKey);
    j += "  \"outputFolder\": "; AppendJsonString(j, outputFolder); j += ",\n";
    flag("keepMetadata", keepMetadata);
    num("jpegQuality", jpegQuality);
    num("webpQuality", webpQuality);
    num("compressQuality", compressQuality);
    num("pdfDpi", pdfDpi);
    num("theme", theme);
    for (size_t i = 0; i < 3; i++) {
        std::string k = "recentFolder" + std::to_string(i + 1);
        j += "  \""; j += k; j += "\": "; AppendJsonString(j, i < recentFolders.size() ? recentFolders[i] : wstring()); j += ",\n";
    }
    flag("paused", paused);
    flag("explorerMenuSet", explorerMenuSet);
    j += "  \"welcomed\": "; j += welcomed ? "true" : "false"; j += "\n}\n";
    EnsureDir(DataDir());
    WriteAllBytes(JoinPath(DataDir(), L"settings.json"), j.data(), j.size());
}

Settings Settings::Load()
{
    Settings s;
    std::vector<uint8_t> bytes;
    if (!ReadAllBytes(JoinPath(DataDir(), L"settings.json"), bytes) || bytes.empty()) return s;
    size_t skip = bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF ? 3 : 0;   // BOM
    std::string j(reinterpret_cast<const char*>(bytes.data()) + skip, bytes.size() - skip);
    s.keyConvert = ParseInt(j, "keyConvert", s.keyConvert);
    s.keyTools = ParseInt(j, "keyTools", s.keyTools);
    s.keyActions = ParseInt(j, "keyActions", s.keyActions);
    s.hotkeyMods = ParseInt(j, "hotkeyMods", s.hotkeyMods);
    s.hotkeyKey = ParseInt(j, "hotkeyKey", s.hotkeyKey);
    s.outputFolder = ParseString(j, "outputFolder");
    s.keepMetadata = ParseBool(j, "keepMetadata", s.keepMetadata);
    s.jpegQuality = std::clamp(ParseInt(j, "jpegQuality", s.jpegQuality), 1, 100);
    s.webpQuality = std::clamp(ParseInt(j, "webpQuality", s.webpQuality), 1, 100);
    s.compressQuality = std::clamp(ParseInt(j, "compressQuality", s.compressQuality), 1, 100);
    s.pdfDpi = std::clamp(ParseInt(j, "pdfDpi", s.pdfDpi), 36, 1200);
    s.theme = std::clamp(ParseInt(j, "theme", s.theme), 0, 2);
    for (int i = 1; i <= 3; i++) {
        wstring f = ParseString(j, ("recentFolder" + std::to_string(i)).c_str());
        if (!f.empty()) s.recentFolders.push_back(f);
    }
    s.paused = ParseBool(j, "paused", s.paused);
    s.welcomed = ParseBool(j, "welcomed", s.welcomed);
    s.explorerMenuSet = ParseBool(j, "explorerMenuSet", s.explorerMenuSet);
    return s;
}

}  // namespace rd
