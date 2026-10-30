#include "bubble.h"

#include "dragdrop.h"
#include "ui.h"

namespace rd {

namespace {

constexpr double kSize = 52;     // the button, DIPs across
constexpr double kPad = 16;      // room for the shadow
constexpr double kEdge = 10;     // from the side of the screen once settled
constexpr double kPi = 3.14159265358979323846;

}  // namespace

/// Files over the button light it up; dropped, they go to the wheel. Copy is all it ever answers, so the
/// files stay where they are.
class BubbleDropTarget : public DropTarget {
public:
    Bubble* bubble = nullptr;
    STDMETHODIMP DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override
    {
        ok_ = bubble && HasFiles(data);
        if (ok_) { bubble->invited_ = true; bubble->Lift(true); }
        *effect = ok_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    STDMETHODIMP DragOver(DWORD, POINTL, DWORD* effect) override
    {
        *effect = ok_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    STDMETHODIMP DragLeave() override
    {
        if (bubble) { bubble->invited_ = false; bubble->Lift(false); }
        return S_OK;
    }
    STDMETHODIMP Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override
    {
        std::vector<wstring> paths;
        wstring err;
        // From a ZIP or an email the files are copied out now, while the source still offers them.
        bool got = ok_ && (HasVirtualFiles(data) && !HasFilesOnDisk(data) ? ExtractVirtualFiles(data, paths, err) : ReadDroppedPaths(data, paths));
        if (!got && !err.empty()) Log(L"bubble: %s", err.c_str());
        DragLeave();
        if (got && !paths.empty() && bubble && bubble->onDrop) {
            // The drag source waits inside the drop until it returns: the wheel opens just after.
            POINT c = bubble->centre();
            auto fn = bubble->onDrop;
            After(0, [fn, paths, c] { fn(paths, c); });
        }
        *effect = ok_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
private:
    bool ok_ = false;
};

Bubble::~Bubble()
{
    if (hwnd_) { RevokeDragDrop(hwnd_); DestroyWindow(hwnd_); }
    if (drop_) { drop_->bubble = nullptr; drop_->Release(); }
}

bool Bubble::Create(HINSTANCE inst, POINT at)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = L"RodaBubble";
    RegisterClassW(&wc);
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"Roda", WS_POPUP,
                            0, 0, 1, 1, nullptr, nullptr, inst, this);
    if (!hwnd_) return false;
    drop_ = new BubbleDropTarget();
    drop_->bubble = this;
    if (FAILED(RegisterDragDrop(hwnd_, drop_))) Log(L"bubble: could not take dropped files");
    // Off every screen (first start, or a screen since unplugged): the main screen's right side, below the middle.
    if (!MonitorFromPoint(at, MONITOR_DEFAULTTONULL)) {
        Mon m = MonitorOf({ 0, 0 });
        at = { m.work.right, m.work.top + (m.work.bottom - m.work.top) * 3 / 5 };
    }
    lift_.Set(0);
    Place(at);
    Settle();
    Show(true);
    return true;
}

LRESULT CALLBACK Bubble::WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<Bubble*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self && self->hwnd_ == h) return self->Handle(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

void Bubble::Show(bool on)
{
    if (!hwnd_ || on == shown_) return;
    shown_ = on;
    if (on) { dirty_ = true; Draw(); ShowWindow(hwnd_, SW_SHOWNOACTIVATE); }
    else { ShowWindow(hwnd_, SW_HIDE); surf_.Release(); }
}

void Bubble::Place(POINT c)
{
    scale_ = ScaleAt(c);
    surf_.SetScale(scale_);
    int px = int(std::lround((kSize + 2 * kPad) * scale_)), half = px / 2;
    Mon mon = MonitorOf(c);
    c.x = std::clamp<LONG>(c.x, mon.work.left + half, std::max<LONG>(mon.work.left + half, mon.work.right - half));
    c.y = std::clamp<LONG>(c.y, mon.work.top + half, std::max<LONG>(mon.work.top + half, mon.work.bottom - half));
    frame_ = { c.x - half, c.y - half, c.x - half + px, c.y - half + px };
    surf_.Resize(px, px);
    SetWindowPos(hwnd_, HWND_TOPMOST, frame_.left, frame_.top, px, px, SWP_NOACTIVATE);
    dirty_ = true;
    RequestFrame();
}

void Bubble::Settle()
{
    // To the nearer side, its shadow's room past the edge so the button itself sits kEdge in.
    POINT c = centre();
    Mon mon = MonitorOf(c);
    int px = frame_.right - frame_.left, inset = int(std::lround((kEdge - kPad) * scale_));
    int x = c.x < (mon.work.left + mon.work.right) / 2 ? mon.work.left + inset : mon.work.right - px - inset;
    if (ReducedMotion()) { slide_.Set(x); OffsetRect(&frame_, x - frame_.left, 0); SetWindowPos(hwnd_, nullptr, frame_.left, frame_.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); }
    else { slide_.Set(frame_.left); slide_.Ease(x, 0.28, Curve::EaseOut); }
    if (onMoved) onMoved({ x + px / 2, c.y });
    dirty_ = true;
    RequestFrame();
}

void Bubble::Lift(bool on)
{
    if (ReducedMotion()) lift_.Set(on ? 1 : 0); else lift_.Spring(on ? 1 : 0, 0.22, 0.75);
    dirty_ = true;
    RequestFrame();
}

LRESULT Bubble::Handle(UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;   // a click never takes the focus from the app in front
        case WM_LBUTTONDOWN:
            pressed_ = true;
            moved_ = false;
            downAt_ = CursorPos();
            grab_ = { downAt_.x - frame_.left, downAt_.y - frame_.top };
            slide_.Set(frame_.left);   // a press stops a glide where it is
            SetCapture(hwnd_);
            return 0;
        case WM_MOUSEMOVE: {
            POINT p = CursorPos();
            if (pressed_) {
                // Past a few pixels it's a drag: the button follows the pointer.
                if (std::abs(p.x - downAt_.x) > GetSystemMetrics(SM_CXDRAG) || std::abs(p.y - downAt_.y) > GetSystemMetrics(SM_CYDRAG)) moved_ = true;
                if (moved_) {
                    OffsetRect(&frame_, p.x - grab_.x - frame_.left, p.y - grab_.y - frame_.top);
                    slide_.Set(frame_.left);
                    SetWindowPos(hwnd_, nullptr, frame_.left, frame_.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
            if (!tracking_) {
                TRACKMOUSEEVENT t{ sizeof t, TME_LEAVE, hwnd_, 0 };
                tracking_ = TrackMouseEvent(&t) != FALSE;
                Lift(true);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            if (!pressed_ && !invited_) Lift(false);
            return 0;
        case WM_LBUTTONUP: {
            if (!pressed_) return 0;
            pressed_ = false;
            ReleaseCapture();
            if (moved_) {
                // Let go somewhere: onto the screen it's over (its scale may differ), then to the nearer side.
                Place(centre());
                Settle();
            } else if (onClick) onClick(centre());
            return 0;
        }
        case WM_RBUTTONUP:
            if (onMenu) onMenu();
            return 0;
        case WM_DPICHANGED: return 0;   // it sizes itself for the screen it's let go on
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

void Bubble::Draw()
{
    dirty_ = false;
    if (!shown_ || !hwnd_) return;
    if (slide_.Active() || std::lround(slide_.value) != frame_.left) {   // settling: the window follows the glide
        if (slide_.Active()) {
            OffsetRect(&frame_, int(std::lround(slide_.value)) - frame_.left, 0);
            SetWindowPos(hwnd_, nullptr, frame_.left, frame_.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            dirty_ = true;
        }
    }
    ID2D1DCRenderTarget* rt = surf_.Begin();
    if (!rt) return;
    const Theme& th = CurrentTheme();
    float lift = float(std::clamp(lift_.value, 0.0, 1.2)), c = float(kPad + kSize / 2), r = float(kSize / 2) * (1 + 0.08f * lift);
    surf_.DrawShadow(RectF(c - r, c - r, 2 * r, 2 * r), r, 14, 0, 4, th.shadow);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), r, r), surf_.Solid(invited_ ? th.accent : th.glass | 0xFF000000));
    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), r - 0.5f, r - 0.5f), surf_.Solid(th.edge), 1);
    // A wheel: a ring and its spokes, in the accent (white on the accent while files are over it).
    uint32_t ink = invited_ ? th.accentText : th.accent;
    float wr = r * 0.46f;
    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), wr, wr), surf_.Solid(ink), 3);
    for (int k = 0; k < 4; k++) {
        double a = kPi / 4 + k * kPi / 2;
        rt->DrawLine(D2D1::Point2F(c + float(std::sin(a)) * 3.5f, c - float(std::cos(a)) * 3.5f),
                     D2D1::Point2F(c + float(std::sin(a)) * wr, c - float(std::cos(a)) * wr), surf_.Solid(ink), 2);
    }
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), 3, 3), surf_.Solid(ink));
    surf_.End();
    // Half see-through at rest, so it stays out of the way; solid when the pointer or files come.
    surf_.Present(hwnd_, frame_.left, frame_.top, BYTE((0.55 + 0.45 * std::min(1.0f, lift)) * 255));
    if (lift_.Active() || slide_.Active()) dirty_ = true;
}

}  // namespace rd
