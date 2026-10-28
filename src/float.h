// Float: an always-on-top picture card you can move (drag), resize (wheel or corner) and close
// (Esc, or the cross). Double-click opens the file. The picture is a preview a worker prepared.
#pragma once
#include "anim.h"
#include "gfx.h"

namespace rd {

class FloatCard {
public:
    /// `preview`: a PNG in Roda's temp folder (deleted on close); `original`: what was dropped.
    FloatCard(const wstring& original, const wstring& preview, int origW, int origH, POINT at);
    ~FloatCard();
    bool ok() const { return hwnd_ != nullptr; }
    bool closed() const { return closed_; }
    bool dirty() const { return dirty_; }
    void Draw();

private:
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);
    void Close();
    void ResizeTo(int w, int h, POINT keep);

    HWND hwnd_ = nullptr;
    Surface surf_;
    Mips mips_;
    wstring original_, preview_;
    double aspect_ = 1, scale_ = 1;
    RECT frame_{};
    bool dirty_ = false, closed_ = false, hover_ = false, hoverClose_ = false, tracking_ = false;
    Anim fade_;
};

}  // namespace rd
