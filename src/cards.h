// Progress and result cards: a stack in the bottom-right corner of the screen the files were dropped
// on. Running: name, progress bar, cancel. Done: "Saved as ...", Show in folder, Open; fades after ~4 s.
// Failed: what went wrong, in plain words.
#pragma once
#include "anim.h"
#include "gfx.h"
#include "jobs.h"

namespace rd {

class Cards {
public:
    Cards();
    ~Cards();
    bool Create(HINSTANCE inst);

    /// A job changed (from Jobs::onChange).
    void Update(const JobState& s);
    /// A one-line note for things that aren't jobs ("Copied 3 paths").
    void Note(const wstring& title, const wstring& text, bool failed = false);
    /// Where new cards appear: the monitor under this point.
    void AnchorAt(POINT p) { anchor_ = p; }

    std::function<void(int jobId)> onCancel;

    bool dirty() const { return dirty_; }
    void Draw();

private:
    struct Card {
        int id = 0;                    // job id, or negative for notes
        wstring title, what, text, out;
        JobState::Phase phase = JobState::Queued;
        double progress = 0;
        double dieAt = 0;              // when it starts fading (0: not yet)
        std::unique_ptr<Anim> y, fade, bar;
        bool leaving = false;
    };
    enum class Hit { None, Body, Cancel, Show, Open, Close };

    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);
    Card* Find(int id);
    void Layout();
    void Expire();
    Hit HitTest(POINT client, int* index) const;
    void Click(Hit hit, int index);
    void Arm(Card& c, double seconds);

    HWND hwnd_ = nullptr;
    Surface surf_;
    std::vector<Card> cards_;
    POINT anchor_{};
    RECT frame_{};
    double scale_ = 1;
    bool dirty_ = false, tracking_ = false;
    int hoverIndex_ = -1;
    Hit hoverHit_ = Hit::None;
    int nextNote_ = -1;
    uint32_t token_ = 0;
};

}  // namespace rd
