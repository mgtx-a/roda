#include "floatcard.h"

#include "ui.h"

namespace rd {

namespace {

constexpr double kStart = 420;      // DIPs, the longest side when it opens
constexpr double kMin = 120, kRadius = 12, kPad = 22;   // kPad: room for the shadow
constexpr double kGrip = 10;        // the resizing edge

}  // namespace

FloatCard::FloatCard(const wstring& original, const wstring& preview, int origW, int origH, POINT at)
    : original_(original), preview_(preview)
{
    mips_ = LoadMips(preview, 2048);
    if (mips_.empty()) return;
    aspect_ = origH > 0 ? double(origW) / origH : double(mips_.width()) / std::max(1, mips_.height());
    scale_ = ScaleAt(at);
    surf_.SetScale(scale_);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    wc.lpszClassName = L"RodaFloat";
    wc.style = CS_DBLCLKS;
    RegisterClassW(&wc);
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, FileName(original).c_str(), WS_POPUP,
                            0, 0, 1, 1, nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_) return;

    // Opens around the pointer, the picture's longest side kStart DIPs, inside the screen.
    double w = aspect_ >= 1 ? kStart : kStart * aspect_, h = aspect_ >= 1 ? kStart / aspect_ : kStart;
    int pw = int(std::lround((w + 2 * kPad) * scale_)), ph = int(std::lround((h + 2 * kPad) * scale_));
    Mon mon = MonitorOf(at);
    int x = std::clamp<int>(at.x - pw / 2, mon.work.left, std::max<int>(mon.work.left, mon.work.right - pw));
    int y = std::clamp<int>(at.y - ph / 2, mon.work.top, std::max<int>(mon.work.top, mon.work.bottom - ph));
    frame_ = { x, y, x + pw, y + ph };
    surf_.Resize(pw, ph);
    if (ReducedMotion()) fade_.Set(1); else { fade_.Set(0); fade_.Ease(1, 0.16, Curve::EaseOut); }
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, pw, ph, SWP_NOACTIVATE);
    Draw();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    Log(L"float: %s", original.c_str());
}

FloatCard::~FloatCard()
{
    if (hwnd_) DestroyWindow(hwnd_);
    if (!preview_.empty()) DeleteFileW(preview_.c_str());
}

LRESULT CALLBACK FloatCard::WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<FloatCard*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self && self->hwnd_ == h) return self->Handle(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

void FloatCard::Close()
{
    if (closed_) return;
    closed_ = true;
    ShowWindow(hwnd_, SW_HIDE);
    surf_.Release();
}

void FloatCard::ResizeTo(int w, int h, POINT keep)
{
    // Keeps the point `keep` (screen) where it is, in proportion.
    RECT r = frame_;
    double fx = double(keep.x - r.left) / std::max(1L, r.right - r.left), fy = double(keep.y - r.top) / std::max(1L, r.bottom - r.top);
    int x = keep.x - int(fx * w), y = keep.y - int(fy * h);
    frame_ = { x, y, x + w, y + h };
    surf_.Resize(w, h);
    SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOACTIVATE | SWP_NOZORDER);
    dirty_ = true;
    RequestFrame();
}

LRESULT FloatCard::Handle(UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
        case WM_NCHITTEST: {
            POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            double x = (p.x - frame_.left) / scale_, y = (p.y - frame_.top) / scale_;
            double W = (frame_.right - frame_.left) / scale_, H = (frame_.bottom - frame_.top) / scale_;
            if (x > W - kPad - 34 && x < W - kPad && y > kPad && y < kPad + 34) return HTCLIENT;   // the close cross
            bool right = x > W - kPad - kGrip, bottom = y > H - kPad - kGrip, left = x < kPad + kGrip, top = y < kPad + kGrip;
            if (right && bottom) return HTBOTTOMRIGHT;
            if (left && bottom) return HTBOTTOMLEFT;
            if (right && top) return HTTOPRIGHT;
            if (left && top) return HTTOPLEFT;
            return HTCAPTION;   // anywhere else: drag to move (double-click still arrives, see below)
        }
        case WM_NCLBUTTONDBLCLK:
            ShellExecuteW(nullptr, L"open", original_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        case WM_NCMOUSEMOVE: case WM_MOUSEMOVE: {
            POINT p = CursorPos();
            double x = (p.x - frame_.left) / scale_, y = (p.y - frame_.top) / scale_;
            double W = (frame_.right - frame_.left) / scale_;
            bool hc = x > W - kPad - 34 && x < W - kPad && y > kPad && y < kPad + 34;
            if (!hover_ || hc != hoverClose_) { hover_ = true; hoverClose_ = hc; dirty_ = true; RequestFrame(); }
            if (!tracking_) {
                TRACKMOUSEEVENT t{ sizeof t, TME_LEAVE | (m == WM_NCMOUSEMOVE ? DWORD(TME_NONCLIENT) : 0), hwnd_, 0 };
                tracking_ = TrackMouseEvent(&t) != FALSE;
            }
            break;
        }
        case WM_MOUSELEAVE: case WM_NCMOUSELEAVE:
            tracking_ = false;
            if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) { hover_ = hoverClose_ = false; dirty_ = true; RequestFrame(); }
            break;
        case WM_LBUTTONUP:
            if (hoverClose_) Close();
            return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) Close();
            return 0;
        case WM_MOUSEWHEEL: {
            // The wheel resizes, around the pointer.
            double k = GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1.1 : 1 / 1.1;
            double iw = (frame_.right - frame_.left) / scale_ - 2 * kPad;
            double nw = std::clamp(iw * k, kMin, 4000.0);
            int pw = int(std::lround((nw + 2 * kPad) * scale_)), ph = int(std::lround((nw / aspect_ + 2 * kPad) * scale_));
            ResizeTo(pw, ph, CursorPos());
            return 0;
        }
        case WM_SIZING: {
            // Corner and edge drags keep the picture's proportions.
            RECT* r = reinterpret_cast<RECT*>(l);
            double pad = 2 * kPad * scale_;
            double iw = std::max(kMin * scale_, double(r->right - r->left) - pad);
            int ph = int(std::lround(iw / aspect_ + pad));
            if (w == WMSZ_TOPLEFT || w == WMSZ_TOPRIGHT) r->top = r->bottom - ph; else r->bottom = r->top + ph;
            return TRUE;
        }
        case WM_WINDOWPOSCHANGED: {
            auto* wp = reinterpret_cast<WINDOWPOS*>(l);
            if (!(wp->flags & SWP_NOMOVE) || !(wp->flags & SWP_NOSIZE)) {
                RECT r;
                GetWindowRect(hwnd_, &r);
                if (r.right - r.left != frame_.right - frame_.left || r.bottom - r.top != frame_.bottom - frame_.top) surf_.Resize(r.right - r.left, r.bottom - r.top);
                frame_ = r;
                dirty_ = true;
                Draw();   // during the system's move/size loop our own loop doesn't run
            }
            return 0;
        }
        case WM_DPICHANGED: return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

void FloatCard::Draw()
{
    dirty_ = false;
    if (closed_ || !hwnd_) return;
    ID2D1DCRenderTarget* rt = surf_.Begin();
    if (!rt) return;
    const Theme& th = CurrentTheme();
    double W = (frame_.right - frame_.left) / scale_, H = (frame_.bottom - frame_.top) / scale_;
    D2D1_RECT_F pic = RectF(kPad, kPad, W - 2 * kPad, H - 2 * kPad);
    surf_.DrawShadow(pic, float(kRadius), 18, 0, 6, th.shadow);
    surf_.FillMips(mips_, pic, float(kRadius), 1.0f, true);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(RectF(kPad + 0.5, kPad + 0.5, W - 2 * kPad - 1, H - 2 * kPad - 1), float(kRadius), float(kRadius)), surf_.Solid(th.edge), 1);
    if (hover_) {
        D2D1_POINT_2F c = D2D1::Point2F(float(W - kPad - 18), float(kPad + 18));
        rt->FillEllipse(D2D1::Ellipse(c, 12, 12), surf_.Solid(hoverClose_ ? 0xE0000000 : 0x99000000));
        rt->DrawLine(D2D1::Point2F(c.x - 4.5f, c.y - 4.5f), D2D1::Point2F(c.x + 4.5f, c.y + 4.5f), surf_.Solid(0xFFFFFFFF), 1.6f);
        rt->DrawLine(D2D1::Point2F(c.x - 4.5f, c.y + 4.5f), D2D1::Point2F(c.x + 4.5f, c.y - 4.5f), surf_.Solid(0xFFFFFFFF), 1.6f);
    }
    surf_.End();
    surf_.Present(hwnd_, frame_.left, frame_.top, BYTE(std::clamp(fade_.value, 0.0, 1.0) * 255));
    if (fade_.Active()) dirty_ = true;
}

}  // namespace rd
