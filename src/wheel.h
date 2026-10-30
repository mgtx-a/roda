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
    D2D1_RECT_F tabs_[3]{};         // the tabs, DIPs from the centre
    bool shown_ = false, closing_ = false, dropped_ = false, dirty_ = false, keyboard_ = false;
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
