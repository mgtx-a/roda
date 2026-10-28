// The wheel: a ring of options under the pointer that dragged files are dropped on. Also opens from
// the hotkey for the files selected in Explorer, and is then driven with the keyboard.
#pragma once
#include "anim.h"
#include "common.h"
#include "gfx.h"
#include "registry.h"

namespace rd {

class WheelDropTarget;

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
    void Hide(bool animated = true);
    /// The mouse button went up. A drop on the wheel arrives about now from the drag source;
    /// if none does, the wheel closes.
    void ButtonReleased();

    bool shown() const { return shown_; }
    bool open() const { return shown_ && !closing_; }   // shown and not on its way out
    WheelKind kind() const { return kind_; }
    POINT centre() const { return centre_; }
    bool dirty() const { return dirty_; }
    void Draw();

    // Called by the drop target.
    DWORD OnDragOver(POINTL pt, DWORD allowed);
    void OnDragLeave();
    DWORD OnDrop(IDataObject* data, POINTL pt, DWORD allowed);

    /// An option was chosen, by a drop or by the keyboard.
    std::function<void(const WheelOption& option, const std::vector<wstring>& paths, POINT at)> onChoose;

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
    void Open(double keySeenAt);
    int HitTest(POINT screen) const;   // segment index, -1 centre, -2 outside
    void SetHot(int i);
    void Choose(int i);
    void Present();
    wstring CentreText() const;
    bool Mixed() const;

    const Settings& settings_;
    HWND hwnd_ = nullptr;
    WheelDropTarget* target_ = nullptr;
    Surface surf_;
    WheelKind kind_ = WheelKind::Convert;
    std::vector<wstring> paths_;
    std::vector<Kind> kinds_;
    std::vector<Segment> segs_;
    int hot_ = -2;
    bool shown_ = false, closing_ = false, dropped_ = false, dirty_ = false, keyboard_ = false;
    POINT centre_{};                // screen pixels
    RECT frame_{};                  // window, screen pixels
    double scale_ = 1;              // DIPs to pixels on this monitor
    double sizeDip_ = 0;            // window side, DIPs
    double keySeenAt_ = -1;
    int chosen_ = -1;
    uint32_t token_ = 0;
    Anim open_, fade_, pulse_;
};

}  // namespace rd
