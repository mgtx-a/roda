// The first-run tour: a card at the bottom of the screen that teaches Roda by doing. A practice picture
// sits on the card to drag straight off it; a small animated demo shows each move first; the hints
// answer what you actually do (picked it up, pressed Space too early, let go off the wheel...); and the
// wheel makes the choice it asks for glow. Never takes the focus. Also in the tray menu ("Take the tour").
#pragma once
#include "anim.h"
#include "gfx.h"

namespace rd {

/// What the tour watches, asked a few times a second while it's up.
struct TourSignals {
    std::function<int()> wheels;        // wheels opened so far
    std::function<int()> choices;       // options chosen so far
    std::function<int()> results;       // files made so far
    std::function<bool()> dragging;     // files are being dragged right now
    std::function<bool()> wheelOpen;    // a wheel is open
    std::function<bool()> stranded;     // a wheel with files waits for a click (let go off the choices)
};

class Tour {
public:
    Tour();
    ~Tour();
    void Start(TourSignals signals);
    void End();
    bool active() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_); }
    std::function<void()> onFinished;               // the last card was closed (or the tour skipped)
    std::function<void()> onPractice;               // the practice picture clicked: show it in Explorer
    std::function<wstring()> practiceFile;          // the practice picture's path (made if needed); empty if it can't be
    std::function<void(const wchar_t* id)> onGuide; // the choice the wheel should make glow (nullptr: none)

    bool dirty() const { return dirty_; }
    void Draw();

private:
    enum Hint { Plain, Holding, SpaceTooSoon, Again, Stranded };

    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);
    void Poll();
    void Go(int step);
    int HitTest(double x, double y) const;   // 1 end, 2 next, 4 the practice picture
    bool HasPicture() const { return step_ <= 1; }
    void DrawStage(float x, float y, float w, float h, double t);
    wstring HintText() const;

    HWND hwnd_ = nullptr;
    Surface surf_;
    TourSignals sig_;
    int step_ = 0, startWheels_ = 0, startChoices_ = 0, startResults_ = 0, hover_ = 0;
    Hint hint_ = Plain;
    double doneAt_ = -1, hintAt_ = -1, stepAt_ = 0;
    int closedPolls_ = 0;                 // "Drop it": polls with the wheel closed and nothing chosen
    bool made_ = false;                   // "Here it is": the new file arrived
    bool pressTile_ = false, draggingTile_ = false;
    bool fromWheel_ = false;              // "Drop it" was reached by opening a wheel (not by Skip): closing it goes back
    POINT pressAt_{};
    double scale_ = 1;
    RECT frame_{};
    bool dirty_ = false, tracking_ = false;
    uint32_t token_ = 0;
    Anim fade_;
};

}  // namespace rd
