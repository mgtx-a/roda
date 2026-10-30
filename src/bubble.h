// The floating button: a small round Roda button that stays on top, where it was left. Drop files on it
// and the wheel opens for them; click it and the wheel opens empty, waiting for files; right-click it for
// Roda's menu. Drag it anywhere: let go and it settles against the nearer side of the screen.
#pragma once
#include "anim.h"
#include "gfx.h"

namespace rd {

class BubbleDropTarget;

class Bubble {
public:
    ~Bubble();
    /// Opens with its centre at `at` (screen pixels), kept on a screen; one off every screen goes to the
    /// main screen's right side.
    bool Create(HINSTANCE inst, POINT at);
    void Show(bool on);
    bool shown() const { return shown_; }
    POINT centre() const { return { (frame_.left + frame_.right) / 2, (frame_.top + frame_.bottom) / 2 }; }

    std::function<void(POINT centre)> onClick;
    std::function<void()> onMenu;
    std::function<void(const std::vector<wstring>& paths, POINT centre)> onDrop;   // after the drag has ended
    std::function<void(POINT centre)> onMoved;                                     // to remember where it was left

    bool dirty() const { return dirty_; }
    void Draw();

private:
    friend class BubbleDropTarget;
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);
    void Place(POINT centre);          // there, on a screen
    void Settle();                     // to the nearer side of its screen, gliding
    void Lift(bool on);                // hovered or with files over it: brighter and a little bigger

    HWND hwnd_ = nullptr;
    BubbleDropTarget* drop_ = nullptr;
    Surface surf_;
    RECT frame_{};
    double scale_ = 1;
    bool shown_ = false, dirty_ = false, tracking_ = false, pressed_ = false, moved_ = false, invited_ = false;
    POINT downAt_{}, grab_{};          // where the press was, and the pointer's offset in the window
    Anim lift_;                        // 0 at rest (half see-through), 1 hovered or with files over it
    Anim slide_;                       // the window's left edge while it settles
};

}  // namespace rd
