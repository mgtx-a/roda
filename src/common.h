// Roda: shared helpers. No framework: plain Win32 + Direct2D. (Base taken from Ampaian.)
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <objbase.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace rd {

using std::wstring;

/// Minimal COM smart pointer.
template <class T>
class Com {
    T* p_ = nullptr;
public:
    Com() = default;
    Com(T* p) : p_(p) {}
    Com(const Com& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~Com() { Reset(); }
    Com& operator=(Com o) { std::swap(p_, o.p_); return *this; }
    T* operator->() const { return p_; }
    T* Get() const { return p_; }
    T** Put() { Reset(); return &p_; }
    void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    explicit operator bool() const { return p_ != nullptr; }
};

// ---- time, strings, files
double Now();                                   // seconds, monotonic
wstring Widen(std::string_view utf8);
std::string Narrow(std::wstring_view wide);
bool FileExists(const wstring& path);
bool DirExists(const wstring& path);
wstring FileName(const wstring& path);
wstring Extension(const wstring& path);         // lower case, with the dot
wstring JoinPath(const wstring& a, const wstring& b);
wstring ParentDir(const wstring& path);
bool SamePath(const wstring& a, const wstring& b);   // case-insensitive
void EnsureDir(const wstring& path);
void DeleteTree(const wstring& path);           // a folder and everything in it
bool ReadAllBytes(const wstring& path, std::vector<uint8_t>& out);
bool WriteAllBytes(const wstring& path, const void* data, size_t size);

// ---- log, environment, folders
void Log(const wchar_t* fmt, ...);
wstring EnvVar(const wchar_t* name);
bool TestMode();                                // RODA_DATA is set: isolated copy
wstring ExeDir();                               // the folder Roda.exe runs from
wstring DataDir();                              // <exe>\data when writable, else %APPDATA%\Roda (or RODA_DATA)
bool PortableData();                            // DataDir() is next to the exe
wstring TempDir();                              // %TEMP%\Roda\<pid>, made on first use
void CleanStaleTemp();                          // folders left by Roda processes that are gone

// ---- screens, DPI, appearance
double Scale();                                 // system DPI / 96
double ScaleAt(POINT p);                        // DPI of the monitor at p / 96
bool DarkMode();                                // apps use the dark theme (read-only registry check)
bool ReducedMotion();                           // "Show animations in Windows" is off

struct Mon {
    HMONITOR handle = nullptr;
    RECT bounds{}, work{};
};
std::vector<Mon> Monitors();
POINT CursorPos();
const Mon* MonitorAt(const std::vector<Mon>& all, POINT p);
Mon MonitorOf(POINT p);

// ---- settings.json
struct Settings {
    // Trigger keys, as virtual-key codes (VK_SHIFT, VK_CONTROL, VK_MENU).
    int keyConvert = VK_SHIFT, keyTools = VK_CONTROL, keyActions = VK_MENU;
    int hotkeyMods = MOD_CONTROL | MOD_ALT, hotkeyKey = 'R';
    wstring outputFolder;      // empty: next to the source file
    bool keepMetadata = true;
    bool paused = false;
    bool welcomed = false;
    static Settings Load();
    void Save() const;
};

// ---- application services used across modules (defined in app.cpp)
HWND MessageWindow();        // the hidden window that owns menus, the tray icon and raw input
void BeginModal();           // keep animating inside menus and drags (their loops bypass ours)
void EndModal();
void RequestFrame();         // something changed on screen: draw on the next pass of the loop

// ---- run something on the UI thread from any thread
void PostToUi(std::function<void()> fn);

// ---- a tiny scheduler: do something later, on the UI thread
void After(double seconds, std::function<void()> fn);
double NextDueIn();          // seconds until the next scheduled job, or -1
void RunDue();               // run what is due (call from the message loop)

}  // namespace rd
