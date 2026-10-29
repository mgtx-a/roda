// The Settings window: trigger keys, the hotkey, where results go, quality, photo info, theme and
// Start with Windows. Every change applies at once and is saved; there is no OK button to forget.
#pragma once
#include "anim.h"
#include "gfx.h"

namespace rd {

class SettingsWindow {
public:
    explicit SettingsWindow(Settings& s);
    ~SettingsWindow();
    void Show();                 // opens (or brings to the front)
    bool shown() const { return shown_; }

    std::function<void()> onKeysChanged;       // trigger keys or hotkey: re-register
    std::function<bool()> startupOn;           // Start with Windows: is it on, and switch it
    std::function<void(bool)> setStartup;
    std::function<bool()> explorerMenuOn;      // the optional "Roda…" entry in Explorer's right-click menu
    std::function<void(bool)> setExplorerMenu;
    std::function<void()> onThemeChanged;

    bool dirty() const { return dirty_; }
    void Draw();

private:
    struct Hit { int row = -1, choice = -1; };
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);
    Hit HitTest(double x, double y) const;
    void Click(const Hit& h, double x);
    void Slide(int row, double x);
    void Capture(UINT vk);
    void PickFolder();

    Settings& s_;
    HWND hwnd_ = nullptr;
    Surface surf_;
    bool shown_ = false, dirty_ = false, capturing_ = false;
    int dragging_ = -1;
    Hit hover_;
    double scale_ = 1;
    RECT frame_{};
    Anim fade_;
};

}  // namespace rd
