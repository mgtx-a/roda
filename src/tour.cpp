#include "tour.h"

#include "dragdrop.h"
#include "ui.h"

namespace rd {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr UINT kMsgDragDone = WM_APP + 1;   // from the practice picture's drag thread
constexpr int kSteps = 4;

struct Step {
    const wchar_t* title;
    const wchar_t* line;
};

// Steps 0 and 1 move on by themselves as they're done; 2 and 3 wait for Next / Done.
const Step kStep[kSteps] = {
    { L"Pick up the picture", L"Press and hold on the picture at the left, then move the mouse a little. Keep holding." },
    { L"Drop it on Rotate", L"Keep holding. Move onto Rotate (it glows) and let go." },
    { L"Here's your new picture", L"Saved next to the original, as a new file. The card at the bottom right has Open and Show in folder." },
    { L"Two wheels: Tools and Convert", L"Tools changes the file: rotate, crop, resize… Convert changes its type: PNG, PDF, ZIP… Space or Tab switches." },
};

// The card, DIPs.
constexpr double kW = 680, kH = 216, kPad = 22, kShadow = 24;
constexpr double kTileW = 108, kTileH = 80;                  // the practice picture
constexpr double kStageW = 188, kStageH = 136;               // the demo, top right
constexpr double kBtnH = 32;

double Ease(double t) { t = std::clamp(t, 0.0, 1.0); return t * t * (3 - 2 * t); }
double Span(double t, double a, double b) { return std::clamp((t - a) / (b - a), 0.0, 1.0); }
double Lerp(double a, double b, double t) { return a + (b - a) * t; }

}  // namespace

Tour::Tour() {}
Tour::~Tour() { if (hwnd_) DestroyWindow(hwnd_); }

void Tour::Start(TourSignals signals)
{
    sig_ = std::move(signals);
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
    Go(0);
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    Draw();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    token_++;
    if (onGuide) onGuide(L"tool.rotate");   // no options panel: the result comes at once
    Poll();
    Log(L"tour: started");
}

void Tour::End()
{
    if (!hwnd_) return;
    token_++;
    ShowWindow(hwnd_, SW_HIDE);
    surf_.Release();
    if (onGuide) onGuide(nullptr);
    Log(L"tour: finished at step %d", step_ + 1);
    if (onFinished) onFinished();
}

void Tour::Go(int step)
{
    if (step >= kSteps) { End(); return; }
    if (step != step_ || step == 0) Log(L"tour: step %d", step + 1);
    step_ = step;
    doneAt_ = -1;
    stepAt_ = Now();
    closedPolls_ = 0;
    made_ = false;
    if (hint_ != Again) hint_ = Plain;
    startWheels_ = sig_.wheels ? sig_.wheels() : 0;
    startChoices_ = sig_.choices ? sig_.choices() : 0;
    if (step <= 1) startResults_ = sig_.results ? sig_.results() : 0;   // counted from before the drop: a quick job may finish before "Here it is"
    dirty_ = true;
    RequestFrame();
}

void Tour::Poll()
{
    if (!active()) return;
    // A few times a second, only while the card is up: the hints answer what's happening.
    auto setHint = [&](Hint h) { if (h != hint_) { hint_ = h; hintAt_ = Now(); dirty_ = true; RequestFrame(); } };
    bool dragging = sig_.dragging && sig_.dragging(), open = sig_.wheelOpen && sig_.wheelOpen();
    if (doneAt_ >= 0) {
        if (Now() - doneAt_ > (step_ == 0 ? 0.2 : 0.9)) { fromWheel_ = step_ == 0; Go(step_ + 1); }
    } else if (step_ == 0) {
        if (sig_.wheels() > startWheels_) { doneAt_ = Now(); hint_ = Plain; dirty_ = true; RequestFrame(); }
        else if (dragging) setHint(Holding);
        else if (GetAsyncKeyState(VK_SPACE) & 0x8000) setHint(SpaceTooSoon);   // read only while this step asks for Space
        else if (hint_ == Holding) setHint(Plain);   // let go before the wheel
    } else if (step_ == 1) {
        if (sig_.choices() > startChoices_) { doneAt_ = Now(); dirty_ = true; RequestFrame(); }
        else if (sig_.stranded && sig_.stranded()) setHint(Stranded);
        else if (fromWheel_ && !open && ++closedPolls_ >= 2) { hint_ = Again; Go(0); }   // closed with nothing chosen: try again
        else if (open) closedPolls_ = 0;
    } else if (step_ == 2) {
        if (!made_ && sig_.results() > startResults_) { made_ = true; dirty_ = true; RequestFrame(); }
    }
    if (step_ != 0 && hint_ == Again) hint_ = Plain;
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
    if (HasPicture() && x >= kPad && x <= kPad + kTileW && y >= kPad + 30 && y <= kPad + 30 + kTileH) return 4;
    if (y < kH - kPad - kBtnH || y > kH - kPad) return 0;
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
            if (pressTile_) {
                POINT p = CursorPos();
                if (std::abs(p.x - pressAt_.x) > GetSystemMetrics(SM_CXDRAG) || std::abs(p.y - pressAt_.y) > GetSystemMetrics(SM_CYDRAG)) {
                    // The picture leaves the card as a real file drag, like one from Explorer.
                    pressTile_ = false;
                    ReleaseCapture();
                    wstring png = practiceFile ? practiceFile() : L"";
                    if (!png.empty()) { draggingTile_ = true; DragFileAsync(png, hwnd_, kMsgDragDone); Log(L"tour: practice picture picked up"); }
                }
                return 0;
            }
            int h = HitTest(GET_X_LPARAM(l) / scale_, GET_Y_LPARAM(l) / scale_);
            if (h != hover_) { hover_ = h; dirty_ = true; RequestFrame(); }
            return 0;
        }
        case WM_MOUSELEAVE: tracking_ = false; if (!pressTile_) { hover_ = 0; dirty_ = true; RequestFrame(); } return 0;
        case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, hover_ == 4 ? IDC_SIZEALL : hover_ ? IDC_HAND : IDC_ARROW)); return TRUE;
        case WM_LBUTTONDOWN:
            if (HitTest(GET_X_LPARAM(l) / scale_, GET_Y_LPARAM(l) / scale_) == 4) { pressTile_ = true; pressAt_ = CursorPos(); SetCapture(hwnd_); }
            return 0;
        case WM_LBUTTONUP: {
            if (pressTile_) {   // a click, not a drag: the picture in Explorer, to drag from there
                pressTile_ = false;
                ReleaseCapture();
                if (onPractice) onPractice();
                return 0;
            }
            int h = HitTest(GET_X_LPARAM(l) / scale_, GET_Y_LPARAM(l) / scale_);
            if (h == 1) End();
            else if (h == 2) { fromWheel_ = false; Go(step_ <= 1 ? kSteps - 1 : step_ + 1); }   // skipping the practice: on to the two wheels
            return 0;
        }
        case WM_CAPTURECHANGED: pressTile_ = false; return 0;
        case kMsgDragDone:
            draggingTile_ = false;
            Log(L"tour: practice drag ended (0x%08lx, effect %ld)", long(w), long(l));
            dirty_ = true;
            RequestFrame();
            return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

wstring Tour::HintText() const
{
    switch (hint_) {
        case Holding: return L"Good, you're holding it. Now press Space.";
        case SpaceTooSoon: return L"Hold the mouse button down on the picture first, then press Space.";
        case Again: return L"No problem. Pick the picture up again, then press Space.";
        case Stranded: return L"No need to drag any more: just click Rotate on the wheel.";
        default: break;
    }
    switch (step_) {
        case 0: return L"Then press Space. The demo on the right shows how.";
        case 1: return L"Space again switches to Convert, for other formats.";
        case 2: return made_ ? L"✓ Done. Missed the card? Right-click the Roda icon near the clock → Recent." : L"Making it…";
        default: return L"No dragging? Right-click files → Roda…, press Ctrl + Alt + R, or Choose files… in the tray menu.";
    }
}

// ---------------------------------------------------------------- drawing

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
    double now = Now();

    // The practice picture, on the card itself: drag it straight off. It bobs a little to say so.
    float tx = L + float(kPad);
    if (HasPicture()) {
        float bob = ReducedMotion() || draggingTile_ ? 0 : float(std::sin(now * 2.4) * 2);
        float py = T + float(kPad) + 30 + bob;
        D2D1_ROUNDED_RECT pic = D2D1::RoundedRect(RectF(tx, py, kTileW, kTileH), 8, 8);
        rt->FillRoundedRectangle(pic, surf_.Solid(draggingTile_ ? WithAlpha(0xFF9FD3E6, 0.4) : 0xFF9FD3E6));
        if (!draggingTile_) {
            rt->PushAxisAlignedClip(RectF(tx, py, kTileW, kTileH), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(tx + 80, py + 22), 11, 11), surf_.Solid(0xFFF6C453));
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(tx + 26, py + 92), 70, 38), surf_.Solid(0xFF2E8B6E));
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(tx + 96, py + 96), 58, 34), surf_.Solid(0xFF2E8B6E));
            rt->PopAxisAlignedClip();
        }
        rt->DrawRoundedRectangle(pic, surf_.Solid(hover_ == 4 ? th.accent : th.edge), hover_ == 4 ? 2.0f : 1.0f);
        const wchar_t* cap = draggingTile_ ? L"Dragging…" : L"Drag me";
        float cw = surf_.MeasureText(cap, 12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        surf_.DrawText(cap, 12, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(tx + (kTileW - cw) / 2, T + kPad + 30 + kTileH + 8, cw + 4, 18), th.accent);
        tx += float(kTileW) + 20;
    }

    // The words: title, what to do, and the hint that answers what you're doing.
    float sx = L + W - float(kPad + kStageW), textW = sx - 18 - tx;
    const Step& s = kStep[std::min(step_, kSteps - 1)];
    bool done = doneAt_ >= 0;
    wstring title = done ? wstring(L"✓  ") + s.title : s.title;
    surf_.DrawText(title.c_str(), 16, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(tx, T + kPad - 2, textW, 24), done ? th.accent : th.text);
    float used = surf_.DrawWrapped(s.line, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, RectF(tx, T + kPad + 30, textW, 54), th.text);
    wstring hint = HintText();
    bool live = hint_ != Plain || (step_ == 2 && made_);
    float hy = T + float(kPad) + 30 + std::max(used, 34.0f) + 10;
    if (live && !ReducedMotion() && now - hintAt_ < 0.4) hy += float((1 - (now - hintAt_) / 0.4) * 6);   // a new hint slides in
    surf_.DrawWrapped(hint.c_str(), 12, live ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, RectF(tx, hy, textW, 36),
                   hint_ == SpaceTooSoon ? th.error : live ? th.accent : th.dim);

    // The demo: the teacher shows the move, over and over, until it's done.
    DrawStage(sx, T + float(kPad), float(kStageW), float(kStageH), ReducedMotion() ? 2.8 : now - stepAt_);

    // End tour, the step count, Skip / Next / Done.
    float by = T + H - float(kPad + kBtnH);
    if (hover_ == 1) rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(L + kPad, by, 96, kBtnH), 8, 8), surf_.Solid(th.button));
    surf_.DrawText(L"End tour", 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(L + kPad + 22, by + 7, 70, 18), th.dim);
    wstring count = std::to_wstring(step_ + 1) + L" of " + std::to_wstring(kSteps);
    float cw = surf_.MeasureText(count.c_str(), 11.5f, DWRITE_FONT_WEIGHT_NORMAL);
    float nx = L + W - float(kPad) - 112;
    surf_.DrawText(count.c_str(), 11.5f, DWRITE_FONT_WEIGHT_NORMAL, RectF(nx - 14 - cw, by + 8, cw + 4, 16), th.dim);
    for (int i = 0; i < kSteps; i++)   // dots for the steps, the current one long
        rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(nx - 14 - cw - 16 - (kSteps - i) * 12 + (i > step_ ? 8 : 0), by + 13, i == step_ ? 14 : 6, 6), 3, 3),
                                 surf_.Solid(i <= step_ ? th.accent : th.sep));
    bool primary = step_ >= 2;   // the first two move on by themselves: their button only skips
    const wchar_t* next = step_ == kSteps - 1 ? L"Done" : step_ >= 2 ? L"Next" : L"Skip";
    uint32_t bg = primary ? th.accent : th.button;
    rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(nx, by, 112, kBtnH), 8, 8), surf_.Solid(hover_ == 2 ? WithAlpha(bg, 0.85) : bg));
    float tw = surf_.MeasureText(next, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    surf_.DrawText(next, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(nx + 56 - tw / 2, by + 7, tw + 4, 18), primary ? th.accentText : th.text);

    surf_.End();
    surf_.Present(hwnd_, frame_.left, frame_.top, BYTE(std::clamp(fade_.value, 0.0, 1.0) * 255));
    if (fade_.Active() || (active() && !ReducedMotion())) { dirty_ = true; RequestFrame(); }   // the demo and the picture move
}

/// One looping demo per step, drawn small: a pointer, a picture, the Space and Tab keys, a little wheel.
void Tour::DrawStage(float x, float y, float w, float h, double t)
{
    ID2D1DCRenderTarget* rt = surf_.Target();
    const Theme& th = CurrentTheme();
    rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(x, y, w, h), 10, 10), surf_.Solid(WithAlpha(th.button, 0.6)));
    rt->PushAxisAlignedClip(RectF(x, y, w, h), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    auto picture = [&](float cx, float cy, float k, float alpha) {   // the practice picture, small
        float pw = 30 * k, ph = 22 * k;
        D2D1_RECT_F r = D2D1::RectF(cx - pw / 2, cy - ph / 2, cx + pw / 2, cy + ph / 2);
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, 3, 3), surf_.Solid(WithAlpha(0xFF9FD3E6, alpha)));
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx - 3 * k, cy + 9 * k), 14 * k, 6 * k), surf_.Solid(WithAlpha(0xFF2E8B6E, alpha)));
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 9 * k, cy - 5 * k), 3 * k, 3 * k), surf_.Solid(WithAlpha(0xFFF6C453, alpha)));
    };
    auto pointer = [&](float px, float py, bool pressed) {   // Windows' arrow, tip at (px, py)
        if (pressed) rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), 7, 7), surf_.Solid(WithAlpha(th.accent, 0.8)), 2);
        Com<ID2D1PathGeometry> geo;
        Com<ID2D1GeometrySink> sink;
        if (FAILED(D2D()->CreatePathGeometry(geo.Put())) || FAILED(geo->Open(sink.Put()))) return;
        const float pts[][2] = { { 0, 0 }, { 0, 16 }, { 4, 12.5f }, { 7, 19 }, { 9.5f, 18 }, { 6.5f, 11.5f }, { 11.5f, 11.5f } };
        sink->BeginFigure(D2D1::Point2F(px, py), D2D1_FIGURE_BEGIN_FILLED);
        for (int i = 1; i < 7; i++) sink->AddLine(D2D1::Point2F(px + pts[i][0], py + pts[i][1]));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        rt->FillGeometry(geo.Get(), surf_.Solid(0xFFFFFFFF));
        rt->DrawGeometry(geo.Get(), surf_.Solid(0xFF1D2430), 1.1f);
    };
    auto key = [&](float cx, float cy, const wchar_t* label, float kw, bool down) {
        D2D1_RECT_F r = D2D1::RectF(cx - kw / 2, cy - 10 + (down ? 1.5f : 0), cx + kw / 2, cy + 10 + (down ? 1.5f : 0));
        if (!down) rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left, r.top + 2, r.right, r.bottom + 2), 5, 5), surf_.Solid(WithAlpha(th.text, 0.25)));
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, 5, 5), surf_.Solid(down ? th.accent : th.glass | 0xFF000000));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(r, 5, 5), surf_.Solid(down ? th.accent : th.edge), 1);
        float lw = surf_.MeasureText(label, 10, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        surf_.DrawText(label, 10, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(cx - lw / 2, r.top + 4, cx + lw / 2 + 3, r.bottom), down ? th.accentText : th.text);
    };
    // A little wheel: six segments, one lit; with labels for the Tab demo.
    auto wheel = [&](float cx, float cy, float r, float k, int lit, const wchar_t* const* labels) {
        if (k <= 0.01f) return;
        r *= k;
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), surf_.Solid(0xF2252A30));
        for (int i = 0; i < 6; i++) {
            double a0 = (i - 0.5) * kPi / 3, a1 = a0 + kPi / 3;
            if (i == lit) {
                Com<ID2D1PathGeometry> geo;
                Com<ID2D1GeometrySink> sink;
                if (SUCCEEDED(D2D()->CreatePathGeometry(geo.Put())) && SUCCEEDED(geo->Open(sink.Put()))) {
                    float ri = r * 0.38f;
                    sink->BeginFigure(D2D1::Point2F(cx + ri * float(std::sin(a0)), cy - ri * float(std::cos(a0))), D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine(D2D1::Point2F(cx + r * float(std::sin(a0)), cy - r * float(std::cos(a0))));
                    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + r * float(std::sin(a1)), cy - r * float(std::cos(a1))), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                    sink->AddLine(D2D1::Point2F(cx + ri * float(std::sin(a1)), cy - ri * float(std::cos(a1))));
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();
                    rt->FillGeometry(geo.Get(), surf_.Solid(th.accent));
                }
            }
            rt->DrawLine(D2D1::Point2F(cx + r * 0.4f * float(std::sin(a0)), cy - r * 0.4f * float(std::cos(a0))),
                         D2D1::Point2F(cx + r * 0.95f * float(std::sin(a0)), cy - r * 0.95f * float(std::cos(a0))), surf_.Solid(0x30FFFFFF), 1);
            if (labels && k > 0.9f) {
                double am = (a0 + a1) / 2;
                float lx = cx + r * 0.7f * float(std::sin(am)), ly = cy - r * 0.7f * float(std::cos(am));
                float lw = surf_.MeasureText(labels[i], 7.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
                surf_.DrawText(labels[i], 7.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(lx - lw / 2, ly - 5.5f, lx + lw / 2 + 3, ly + 6), 0xFFE8EBEE);
            }
        }
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r * 0.34f, r * 0.34f), surf_.Solid(0xFF1B1F24));
    };
    auto label = [&](const wchar_t* text, float alpha) {   // the scene's caption, along the bottom
        float lw = surf_.MeasureText(text, 10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        surf_.DrawText(text, 10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(x + (w - lw) / 2, y + h - 18, x + (w + lw) / 2 + 3, y + h - 4), WithAlpha(th.dim, alpha));
    };

    if (step_ <= 1) {
        // Pick up, move, Space, the wheel opens round the pointer, onto the top choice, let go.
        double period = 5.2;
        double u = step_ == 0 ? std::fmod(t, period) : 2.9 + std::fmod(t, 2.3);   // once the real wheel is open: just the drop
        float tileX = x + 34, tileY = y + 58, wx = x + w / 2 + 10, wy = y + 52, keyY = y + h - 46;
        float px, py;
        bool pressed = u > 0.7 && u < 3.7;
        if (u < 0.7) { double e = Ease(u / 0.7); px = float(Lerp(x + w - 20, tileX, e)); py = float(Lerp(y + h - 30, tileY, e)); }
        else if (u < 1.0) { px = tileX; py = tileY; }
        else if (u < 2.0) { double e = Ease(Span(u, 1.0, 2.0)); px = float(Lerp(tileX, wx, e)); py = float(Lerp(tileY, wy, e)); }
        else if (u < 2.9) { px = wx; py = wy; }
        else if (u < 3.6) { double e = Ease(Span(u, 2.9, 3.6)); px = wx; py = float(Lerp(wy, wy - 27, e)); }
        else { px = wx; py = wy - 27; }
        float wk = u < 2.5 ? 0 : u < 2.9 ? float(Ease(Span(u, 2.5, 2.9))) : 1;
        if (u > 4.6 && step_ == 0) wk *= float(1 - Span(u, 4.6, 5.0));
        bool lit = u > 3.35 && u < 4.6;
        wheel(wx, wy, 38, wk, lit ? 0 : -1, nullptr);
        if (u < 1.0) picture(tileX + 2, tileY + 4, 1, 1);
        else if (u < 3.7) picture(px + 12, py + 14, 0.9f, 0.85f);                       // under the pointer, as Windows shows it
        else if (u < 4.2) picture(wx, wy - 27, float(0.9 - 0.6 * Span(u, 3.7, 4.2)), float(1 - Span(u, 3.7, 4.2)));   // into Rotate
        bool space = u > 2.0 && u < 2.6;
        key(x + 46, keyY + 6, L"Space", 74, space);
        if (u > 3.7 && u < 4.8) {   // done
            float a = float(Span(u, 3.7, 3.9) * (1 - Span(u, 4.5, 4.8)));
            float cw = surf_.MeasureText(L"✓", 16, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            surf_.DrawText(L"✓", 16, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(wx - cw / 2, wy - 11, wx + cw / 2 + 3, wy + 12), WithAlpha(th.accent, a));
        }
        pointer(px, py, pressed);
        label(u < 1.0 ? L"Press and hold" : u < 2.0 ? L"Move a little" : u < 2.9 ? L"Press Space" : u < 3.7 ? L"Onto Rotate" : L"Let go", 1);
    } else if (step_ == 2) {
        // The screen, small: the new file's card in the bottom-right corner, and the pointer clicking Open.
        float sx0 = x + 12, sy0 = y + 10, sw = w - 24, sh = h - 36;
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sx0, sy0, sx0 + sw, sy0 + sh), 6, 6), surf_.Solid(th.edge), 1.2f);
        rt->FillRectangle(D2D1::RectF(sx0 + 1, sy0 + sh - 9, sx0 + sw - 1, sy0 + sh - 1), surf_.Solid(WithAlpha(th.text, 0.12)));   // the taskbar
        double u = std::fmod(t, 3.6);
        float cx0 = sx0 + sw - 74, cy0 = sy0 + sh - 46;
        float beat = ReducedMotion() ? 1.0f : float(0.5 + 0.5 * std::sin(t * 4));
        D2D1_ROUNDED_RECT card = D2D1::RoundedRect(D2D1::RectF(cx0, cy0, cx0 + 66, cy0 + 32), 5, 5);
        rt->FillRoundedRectangle(card, surf_.Solid(th.glass | 0xFF000000));
        rt->DrawRoundedRectangle(card, surf_.Solid(WithAlpha(th.accent, 0.5 + 0.5 * beat)), 1.8f);
        surf_.DrawText(L"Saved", 8, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(cx0 + 6, cy0 + 3, cx0 + 60, cy0 + 14), th.text);
        bool click = u > 1.6 && u < 1.9;
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx0 + 6, cy0 + 16, cx0 + 32, cy0 + 28), 3, 3), surf_.Solid(click ? WithAlpha(th.accent, 0.7) : th.accent));
        surf_.DrawText(L"Open", 7.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(cx0 + 9.5f, cy0 + 17.5f, cx0 + 32, cy0 + 28), th.accentText);
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx0 + 35, cy0 + 16, cx0 + 61, cy0 + 28), 3, 3), surf_.Solid(th.button));
        surf_.DrawText(L"Folder", 7.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(cx0 + 37, cy0 + 17.5f, cx0 + 61, cy0 + 28), th.text);
        double e = Ease(Span(u, 0.2, 1.5));
        float px = float(Lerp(sx0 + 30, cx0 + 18, e)), py = float(Lerp(sy0 + 18, cy0 + 22, e));
        pointer(px, py, click);
        label(L"Bottom right: your new file", 1);
    } else {
        // Tools ⇄ Convert: the Tab key flips the wheel between them.
        static const wchar_t* const tools[6] = { L"Rotate", L"Resize", L"Crop", L"Flip", L"Strip", L"Shrink" };
        static const wchar_t* const conv[6] = { L"PNG", L"WEBP", L"PDF", L"TIFF", L"ICO", L"ZIP" };
        double u = std::fmod(t, 4.4);
        bool onConvert = u > 1.5 && u < 3.7;
        bool tab = (u > 1.2 && u < 1.5) || (u > 3.4 && u < 3.7);
        float wx = x + w / 2, wy = y + 66;
        // The two names above the wheel, the open one lit.
        auto pill = [&](float cx, const wchar_t* name, bool on) {
            float pw = surf_.MeasureText(name, 9, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 14;
            D2D1_ROUNDED_RECT r = D2D1::RoundedRect(D2D1::RectF(cx - pw / 2, y + 8, cx + pw / 2, y + 22), 7, 7);
            rt->FillRoundedRectangle(r, surf_.Solid(on ? th.accent : th.glass | 0xFF000000));
            if (!on) rt->DrawRoundedRectangle(r, surf_.Solid(th.edge), 1);
            surf_.DrawText(name, 9, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(cx - pw / 2 + 7, y + 9.5f, cx + pw / 2, y + 22), on ? th.accentText : th.text);
        };
        pill(wx - 22, L"Convert", onConvert);
        pill(wx + 46, L"Tools", !onConvert);
        float k = 1;
        if (!ReducedMotion() && ((u > 1.5 && u < 1.75) || (u > 3.7 && u < 3.95))) k = 0.94f + 0.06f * float(Ease(Span(std::fmod(u, 2.2), 1.5, 1.75)));
        wheel(wx + 12, wy + 6, 36, k, -1, onConvert ? conv : tools);
        key(x + 30, wy + 6, L"Tab", 40, tab);
        label(onConvert ? L"Convert: another type" : L"Tools: change the file", 1);
    }
    rt->PopAxisAlignedClip();
}

}  // namespace rd
