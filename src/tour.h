// The first-run tour: a small card at the bottom of the screen that walks through Roda, moving on by
// itself as you do each thing. Never takes the focus. Also in the tray menu ("Take the tour").
#pragma once
#include "anim.h"
#include "gfx.h"

namespace rd {

class Tour {
public:
    Tour();
    ~Tour();
    /// `wheels` and `choices` count wheels opened and options chosen so far: a step is done when they grow.
    void Start(std::function<int()> wheels, std::function<int()> choices);
    void End();
    bool active() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_); }
    std::function<void()> onFinished;   // the last card was closed (or the tour skipped)
    std::function<void()> onPractice;   // "Practice file": a picture to try it on

    bool dirty() const { return dirty_; }
    void Draw();

private:
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);
    void Poll();
    void Next();
    int HitTest(double x, double y) const;   // 1 end, 2 next, 3 practice file

    HWND hwnd_ = nullptr;
    Surface surf_;
    std::function<int()> wheels_, choices_;
    int step_ = 0, startWheels_ = 0, startChoices_ = 0, hover_ = 0;
    double doneAt_ = -1;
    double scale_ = 1;
    RECT frame_{};
    bool dirty_ = false, tracking_ = false;
    uint32_t token_ = 0;
    Anim fade_;
};

}  // namespace rd
