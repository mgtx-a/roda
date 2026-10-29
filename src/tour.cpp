#include "tour.h"

#include "ui.h"

namespace rd {

namespace {

struct Step {
    const wchar_t* title;
    const wchar_t* line1;
    const wchar_t* line2;
    int waitFor;   // 0 a wheel opening, 1 an option chosen, 2 the button
};

const Step kSteps[] = {
    { L"Drag a file, then press Space", L"Start dragging a picture or a document. While the mouse button is still down, press Space.",
      L"A wheel of formats opens under the pointer. Space or Tab again: tools, then actions.", 0 },
    { L"Drop it on a format", L"Move onto a segment and let go. The result is saved next to the original, under a new name.",
      L"A card in the corner shows it, with Open and Show in folder. Esc or the centre cancels.", 1 },
    { L"You're all set", L"No dragging? Select files and right-click them (Shift + right-click on Windows 11), or press Ctrl + Alt + R.",
      L"The tray icon has Settings, Pause and \"Take the tour\" to see this again.", 2 },
};
constexpr int kSteps_ = int(sizeof(kSteps) / sizeof(kSteps[0]));
constexpr double kW = 580, kH = 168, kPad = 24, kShadow = 24;

}  // namespace

Tour::Tour() {}
Tour::~Tour() { if (hwnd_) DestroyWindow(hwnd_); }

void Tour::Start(std::function<int()> wheels, std::function<int()> choices)
{
    wheels_ = std::move(wheels);
    choices_ = std::move(choices);
    step_ = 0;
    doneAt_ = -1;
    startWheels_ = wheels_();
    startChoices_ = choices_();
    if (!hwnd_) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"RodaTour";
        RegisterClassW(&wc);
        // Topmost and never focused, so the drags it asks for work as usual underneath.
        hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"Roda tour", WS_POPUP,
                                0, 0, 1, 1, nullptr, nullptr, wc.hInstance, this);
        if (!hwnd_) return;
    }
    POINT at = CursorPos();
    scale_ = ScaleAt(at);
    surf_.SetScale(scale_);
    Mon mon = MonitorOf(at);
    int w = int(std::lround((kW + 2 * kShadow) * scale_)), h = int(std::lround((kH + 2 * kShadow) * scale_));
    int x = mon.work.left + (mon.work.right - mon.work.left - w) / 2, y = mon.work.bottom - h - int(8 * scale_);
    frame_ = { x, y, x + w, y + h };
    surf_.Resize(w, h);
    if (ReducedMotion()) fade_.Set(1); else { fade_.Set(0); fade_.Ease(1, 0.25, Curve::EaseOut); }
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    Draw();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    token_++;
    Poll();
    Log(L"tour: started");
}

void Tour::End()
{
    if (!hwnd_) return;
    token_++;
    ShowWindow(hwnd_, SW_HIDE);
    surf_.Release();
    Log(L"tour: finished at step %d", step_ + 1);
    if (onFinished) onFinished();
}

void Tour::Next()
{
    step_++;
    doneAt_ = -1;
    if (step_ >= kSteps_) { End(); return; }
    startWheels_ = wheels_();
    startChoices_ = choices_();
    dirty_ = true;
    RequestFrame();
}

void Tour::Poll()
{
    if (!active()) return;
    // Checked a few times a second, only while the card is up.
    if (doneAt_ >= 0) { if (Now() - doneAt_ > 0.9) Next(); }
    else {
        int wait = kSteps[step_].waitFor;
        if ((wait == 0 && wheels_() > startWheels_) || (wait == 1 && choices_() > startChoices_)) { doneAt_ = Now(); dirty_ = true; RequestFrame(); }
    }
    uint32_t t = token_;
    After(0.2, [this, t] { if (t == token_) Poll(); });
}

LRESULT CALLBACK Tour::WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<Tour*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self && self->hwnd_ == h) return self->Handle(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

int Tour::HitTest(double x, double y) const
{
    x -= kShadow;
    y -= kShadow;
    if (y < kH - kPad - 32 || y > kH - kPad) return 0;
    if (x > kPad && x < kPad + 96) return 1;
    if (x > kW - kPad - 112 && x < kW - kPad) return 2;
    return 0;
}

LRESULT Tour::Handle(UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_MOUSEMOVE: {
            if (!tracking_) { TRACKMOUSEEVENT t{ sizeof t, TME_LEAVE, hwnd_, 0 }; tracking_ = TrackMouseEvent(&t) != FALSE; }
            int h = HitTest(GET_X_LPARAM(l) / scale_, GET_Y_LPARAM(l) / scale_);
            if (h != hover_) { hover_ = h; dirty_ = true; RequestFrame(); }
            return 0;
        }
        case WM_MOUSELEAVE: tracking_ = false; hover_ = 0; dirty_ = true; RequestFrame(); return 0;
        case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, hover_ ? IDC_HAND : IDC_ARROW)); return TRUE;
        case WM_LBUTTONUP: {
            int h = HitTest(GET_X_LPARAM(l) / scale_, GET_Y_LPARAM(l) / scale_);
            if (h == 1) End();
            else if (h == 2) Next();
            return 0;
        }
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

void Tour::Draw()
{
    dirty_ = false;
    if (!active() && !(hwnd_ && surf_.ready())) return;
    ID2D1DCRenderTarget* rt = surf_.Begin();
    if (!rt) return;
    const Theme& th = CurrentTheme();
    float L = float(kShadow), T = float(kShadow), W = float(kW), H = float(kH);
    surf_.DrawShadow(RectF(L, T, W, H), 16, 20, 0, 6, th.shadow);
    rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(L, T, W, H), 16, 16), surf_.Solid(th.glass | 0xFF000000));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(RectF(L + 0.5, T + 0.5, W - 1, H - 1), 15.5f, 15.5f), surf_.Solid(th.edge), 1);
    const Step& s = kSteps[std::min(step_, kSteps_ - 1)];
    wstring count = std::to_wstring(step_ + 1) + L" of " + std::to_wstring(kSteps_);
    float cw = surf_.MeasureText(count.c_str(), 11.5f, DWRITE_FONT_WEIGHT_NORMAL);
    surf_.DrawText(count.c_str(), 11.5f, DWRITE_FONT_WEIGHT_NORMAL, RectF(L + W - kPad - cw, T + 22, cw + 4, 16), th.dim);
    bool done = doneAt_ >= 0;
    wstring title = done ? wstring(L"✓  ") + s.title : s.title;
    surf_.DrawText(title.c_str(), 16, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(L + kPad, T + 18, W - 2 * kPad - cw - 8, 24), done ? th.accent : th.text);
    surf_.DrawText(s.line1, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, RectF(L + kPad, T + 50, W - 2 * kPad, 36), th.text);
    surf_.DrawText(s.line2, 12, DWRITE_FONT_WEIGHT_NORMAL, RectF(L + kPad, T + 86, W - 2 * kPad, 20), th.dim);
    // End tour, Next / Done.
    float by = T + H - float(kPad) - 32;
    if (hover_ == 1) rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(L + kPad, by, 96, 32), 8, 8), surf_.Solid(th.button));
    surf_.DrawText(L"End tour", 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(L + kPad + 22, by + 7, 70, 18), th.dim);
    const wchar_t* next = step_ == kSteps_ - 1 ? L"Done" : L"Next";
    float nx = L + W - float(kPad) - 112;
    rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(nx, by, 112, 32), 8, 8), surf_.Solid(hover_ == 2 ? WithAlpha(th.accent, 0.85) : th.accent));
    float tw = surf_.MeasureText(next, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    surf_.DrawText(next, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(nx + 56 - tw / 2, by + 7, tw + 4, 18), th.accentText);
    surf_.End();
    surf_.Present(hwnd_, frame_.left, frame_.top, BYTE(std::clamp(fade_.value, 0.0, 1.0) * 255));
    if (fade_.Active()) dirty_ = true;
}

}  // namespace rd
