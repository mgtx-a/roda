// The wheel: a ring of options under the pointer that dragged files are dropped on. Also opens from
// the hotkey for the files selected in Explorer, and is then driven with the keyboard.
#pragma once
#include "anim.h"
#include "common.h"
#include "gfx.h"
#include "registry.h"

namespace rd {

class WheelDropTarget;
class CatcherDropTarget;

class Wheel {
public:
    Wheel(const Settings& settings);
    ~Wheel();
    bool Create(HINSTANCE inst);

    /// Opens centred on the pointer (kept on screen). `keySeenAt` is when the trigger key
    /// was noticed, so the time to the first frame can be measured.
    void Show(WheelKind kind, const std::vector<wstring>& paths, double keySeenAt);
    /// The keyboard route: same wheel, but it takes the focus. Arrows, Tab, Enter, Esc.
    void ShowForKeyboard(const std::vector<wstring>& paths);
    /// The hotkey with nothing selected: an empty wheel at the pointer that waits for files to be dragged
    /// onto it. With files over it, it dims and invites the drop; dropped, it fills with their choices and
    /// waits for a click. It stays while Explorer is used; a click on its centre closes it. `at`: its
    /// centre (screen) instead of the pointer.
    void ShowWaiting(const POINT* at = nullptr);
    /// The floating button: files dropped on it, their choices around `at`, waiting for a click.
    void ShowFor(const std::vector<wstring>& paths, POINT at);
    void Switch(WheelKind kind);
    /// The next wheel (or the one before) that has something for these files.
    void Next(bool back = false);
    void Hide(bool animated = true);
    /// The mouse button went up. A drop on the wheel arrives about now from the drag source;
    /// if none does, the wheel stays open for a click.
    void ButtonReleased();
    /// A mouse button went down somewhere: outside a wheel that waits for clicks, that closes it.
    void ClickedAt(POINT screen);

    bool shown() const { return shown_; }
    bool open() const { return shown_ && !closing_; }   // shown and not on its way out
    WheelKind kind() const { return kind_; }
    POINT centre() const { return centre_; }
    int opens() const { return opens_; }      // wheels opened so far
    bool dirty() const { return dirty_; }
    void Draw();

    // Called by the drop target.
    void OnDragEnter(IDataObject* data);   // a waiting wheel lights up for the files coming
    DWORD OnDragOver(POINTL pt, DWORD allowed);
    void OnDragLeave();
    DWORD OnDrop(IDataObject* data, POINTL pt, DWORD allowed);

    /// An option was chosen, by a drop or by the keyboard.
    std::function<void(const WheelOption& option, const std::vector<wstring>& paths, POINT at)> onChoose;
    /// More… was chosen: the choices that didn't fit, to offer as a list.
    std::function<void(const std::vector<WheelOption>& rest, const std::vector<wstring>& paths, POINT at)> onMore;

private:
    struct Segment {
        WheelOption opt;
        int ring = 0;               // 0 inner, 1 outer
        double a0 = 0, a1 = 0;      // radians, clockwise from the top
        std::unique_ptr<Anim> hover;
    };

    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);
    bool Build();                      // false when this wheel has nothing for these files
    void StayForClicks();              // the drag ended off the options: keep the wheel, now for clicks
    void ShowCatcher(bool on);
    void Open(double keySeenAt, bool keepCentre = false);
    void Fit();                        // the window sized for the screen under centre_, the disc whole on it
    int HitTest(POINT screen) const;   // segment index, -1 centre, -2 outside, -10 - i tab i
    void SetHot(int i);
    void Choose(int i);
    void Present();
    wstring CentreText() const;
    bool Mixed() const;

    const Settings& settings_;
    HWND hwnd_ = nullptr;
    WheelDropTarget* target_ = nullptr;
    // An invisible window over every screen while a dragged wheel is open: a drop anywhere off the wheel
    // lands on Roda (nothing done) instead of in a folder, where Windows would move or copy the file.
    HWND catcher_ = nullptr;
    CatcherDropTarget* catcherTarget_ = nullptr;
    Surface surf_;
    WheelKind kind_ = WheelKind::Convert;
    std::vector<wstring> paths_;
    std::vector<Kind> kinds_;
    std::vector<Segment> segs_;
    std::vector<WheelOption> more_;  // the choices behind More…
    int hot_ = -2;
    bool avail_[3]{};               // Convert, Tools, Files: has something for these files
    double tabA_[3][2]{};           // the tabs along the rim: their start and end angles (clockwise from the top)
    double closeA_ = 0, tabR_ = 0;  // the close cross after them; the radius they sit on, DIPs
    bool shown_ = false, closing_ = false, dropped_ = false, dirty_ = false, keyboard_ = false;
    bool waiting_ = false;          // open with no files yet (ShowWaiting)
    bool moving_ = false;           // a waiting wheel being dragged to another place
    bool caught_ = false;           // its files came by a drop on a waiting wheel
    Anim invite_;                   // a waiting wheel with files over it: 0 at rest, 1 dimmed, inviting the drop
    bool Fill(const std::vector<wstring>& paths);   // a waiting wheel takes dropped files: their choices, Tools first
    POINT centre_{};                // screen pixels
    RECT frame_{};                  // window, screen pixels
    double scale_ = 1;              // DIPs to pixels on this monitor
    double sizeDip_ = 0;            // window side, DIPs
    double keySeenAt_ = -1;
    POINT lastMouse_{};             // keyboard mode: only a real pointer move takes the highlight
    int chosen_ = -1;
    int opens_ = 0;
    uint32_t token_ = 0;
    Anim open_, fade_, pulse_;
};

}  // namespace rd
