#include "popup.h"

#include "ui.h"

namespace rd {

namespace {

constexpr double kPad = 24;            // room for the shadow around the card
constexpr double kHandle = 9;          // crop handles: how close counts
constexpr double kBtnW = 92, kBtnH = 30;
const wchar_t* kAspects[] = { L"Free", L"1:1", L"4:3", L"16:9" };
const double kRatios[] = { 0, 1.0, 4.0 / 3.0, 16.0 / 9.0 };

bool In(double x, double y, double l, double t, double w, double h) { return x >= l && x < l + w && y >= t && y < t + h; }

}  // namespace

OptionsPopup::OptionsPopup() {}
OptionsPopup::~OptionsPopup() { if (hwnd_) DestroyWindow(hwnd_); }

bool OptionsPopup::Create(HINSTANCE inst)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"RodaOptions";
    RegisterClassW(&wc);
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"Roda options", WS_POPUP,
                            -10000, -10000, 1, 1, nullptr, nullptr, inst, this);
    return hwnd_ != nullptr;
}

LRESULT CALLBACK OptionsPopup::WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<OptionsPopup*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self && self->hwnd_ == h) return self->Handle(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

void OptionsPopup::Show(Popup kind, const wstring& title, POINT at, int quality, const wstring& preview, int origW, int origH)
{
    kind_ = kind;
    title_ = title;
    quality_ = std::clamp(quality, 1, 100);
    width_.clear();
    height_.clear();
    field_ = 0;
    drag_ = hover_ = Part::None;
    aspect_ = 0;
    cx_ = cy_ = 0.1;
    cw_ = ch_ = 0.8;
    pic_ = {};
    origW_ = origW;
    origH_ = origH;
    scale_ = ScaleAt(at);
    surf_.SetScale(scale_);
    if (kind == Popup::Crop) {
        pic_ = LoadMips(preview, 2048);
        DeleteFileW(preview.c_str());   // loaded; the temp file isn't needed any more
        if (pic_.empty() || origW <= 0 || origH <= 0) { Log(L"popup: no preview to crop on"); return; }
        double k = std::min(560.0 / origW, 420.0 / origH);
        double pw = std::max(160.0, origW * k), ph = std::max(120.0, origH * k);
        k = std::min(pw / origW, ph / origH);
        w_ = origW * k + 40;
        h_ = origH * k + 46 + 12 + 30 + 18;
        w_ = std::max(w_, 380.0);
    } else {
        w_ = 320;
        h_ = 172;
    }
    int pw = int(std::lround((w_ + 2 * kPad) * scale_)), ph = int(std::lround((h_ + 2 * kPad) * scale_));
    Mon mon = MonitorOf(at);
    int x = std::clamp<int>(at.x - pw / 2, mon.work.left, std::max<int>(mon.work.left, mon.work.right - pw));
    int y = std::clamp<int>(at.y - ph / 2, mon.work.top, std::max<int>(mon.work.top, mon.work.bottom - ph));
    frame_ = { x, y, x + pw, y + ph };
    surf_.Resize(pw, ph);
    if (ReducedMotion()) fade_.Set(1); else { fade_.Set(0); fade_.Ease(1, 0.14, Curve::EaseOut); }
    shown_ = true;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, pw, ph, 0);
    Draw();
    ShowWindow(hwnd_, SW_SHOW);
    // Keys need the focus. Windows may refuse right after a drag (the source app is in front);
    // then a click on the popup gives it.
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    Log(L"popup: %s", title.c_str());
}

void OptionsPopup::Hide()
{
    if (!shown_) return;
    shown_ = false;
    ShowWindow(hwnd_, SW_HIDE);
    surf_.Release();
    pic_ = {};
}

D2D1_RECT_F OptionsPopup::PictureRect() const
{
    double k = std::min((w_ - 40) / origW_, (h_ - 46 - 12 - 30 - 18) / origH_);
    double pw = origW_ * k, ph = origH_ * k;
    return RectF(kPad + (w_ - pw) / 2, kPad + 46, pw, ph);
}

OptionsPopup::Part OptionsPopup::HitTest(double x, double y, int* aspect) const
{
    double L = kPad, T = kPad;
    double by = T + h_ - 16 - kBtnH;
    if (In(x, y, L + w_ - 20 - kBtnW, by, kBtnW, kBtnH)) return Part::Apply;
    if (In(x, y, L + w_ - 20 - 2 * kBtnW - 8, by, kBtnW, kBtnH)) return Part::Cancel;
    if (kind_ == Popup::Quality && In(x, y, L + 12, T + 60, w_ - 24, 30)) return Part::Slider;
    if (kind_ == Popup::Size || kind_ == Popup::Trim) {
        if (In(x, y, L + 20, T + 64, 124, 34)) return Part::Width;
        if (In(x, y, L + w_ - 20 - 124, T + 64, 124, 34)) return Part::Height;
    }
    if (kind_ == Popup::Crop) {
        for (int i = 0; i < 4; i++)
            if (In(x, y, L + 20 + i * 52, by, 48, kBtnH)) { *aspect = i; return Part::Aspect; }
        D2D1_RECT_F pr = PictureRect();
        double l = pr.left + cx_ * (pr.right - pr.left), t = pr.top + cy_ * (pr.bottom - pr.top);
        double r = l + cw_ * (pr.right - pr.left), b = t + ch_ * (pr.bottom - pr.top);
        bool nearL = std::fabs(x - l) < kHandle, nearR = std::fabs(x - r) < kHandle, nearT = std::fabs(y - t) < kHandle, nearB = std::fabs(y - b) < kHandle;
        bool inX = x > l - kHandle && x < r + kHandle, inY = y > t - kHandle && y < b + kHandle;
        if (nearT && nearL) return Part::CropNW;
        if (nearT && nearR) return Part::CropNE;
        if (nearB && nearL) return Part::CropSW;
        if (nearB && nearR) return Part::CropSE;
        if (nearT && inX) return Part::CropN;
        if (nearB && inX) return Part::CropS;
        if (nearL && inY) return Part::CropW;
        if (nearR && inY) return Part::CropE;
        if (x > l && x < r && y > t && y < b) return Part::CropMove;
    }
    return Part::None;
}

void OptionsPopup::SetAspect(int i)
{
    aspect_ = i;
    if (i == 0 || origW_ <= 0) return;
    // The biggest rectangle of that shape, centred on the current one.
    double r = kRatios[i] * origH_ / origW_;   // wanted cw/ch in normalised units
    double mx = cx_ + cw_ / 2, my = cy_ + ch_ / 2;
    double w = std::min(1.0, r), h = w / r;
    if (h > 1) { h = 1; w = r; }
    cx_ = std::clamp(mx - w / 2, 0.0, 1 - w);
    cy_ = std::clamp(my - h / 2, 0.0, 1 - h);
    cw_ = w;
    ch_ = h;
}

void OptionsPopup::Drag(double x, double y)
{
    if (drag_ == Part::Slider) {
        double f = (x - (kPad + 20)) / (w_ - 40);
        quality_ = std::clamp(int(std::lround(f * 100)), 1, 100);
        return;
    }
    D2D1_RECT_F pr = PictureRect();
    double dx = (x - dragX_) / (pr.right - pr.left), dy = (y - dragY_) / (pr.bottom - pr.top);
    const double minS = 0.03;
    double l = startX_, t = startY_, r = startX_ + startW_, b = startY_ + startH_;
    switch (drag_) {
        case Part::CropMove:
            cx_ = std::clamp(startX_ + dx, 0.0, 1 - cw_);
            cy_ = std::clamp(startY_ + dy, 0.0, 1 - ch_);
            return;
        case Part::CropN: t += dy; break;
        case Part::CropS: b += dy; break;
        case Part::CropW: l += dx; break;
        case Part::CropE: r += dx; break;
        case Part::CropNW: t += dy; l += dx; break;
        case Part::CropNE: t += dy; r += dx; break;
        case Part::CropSW: b += dy; l += dx; break;
        case Part::CropSE: b += dy; r += dx; break;
        default: return;
    }
    l = std::clamp(l, 0.0, r - minS);
    r = std::clamp(r, l + minS, 1.0);
    t = std::clamp(t, 0.0, b - minS);
    b = std::clamp(b, t + minS, 1.0);
    if (aspect_ > 0) {
        // Keep the shape: the dragged edge leads, the other side follows.
        double ratio = kRatios[aspect_] * origH_ / origW_;
        bool vertical = drag_ == Part::CropN || drag_ == Part::CropS;
        if (vertical) { double w = (b - t) * ratio; r = std::min(1.0, l + w); b = t + (r - l) / ratio; }
        else {
            double h = (r - l) / ratio;
            if (drag_ == Part::CropNW || drag_ == Part::CropNE) { t = std::max(0.0, b - h); l = drag_ == Part::CropNW ? r - (b - t) * ratio : l; r = drag_ == Part::CropNE ? l + (b - t) * ratio : r; }
            else { b = std::min(1.0, t + h); if (drag_ == Part::CropW || drag_ == Part::CropSW) l = r - (b - t) * ratio; else r = l + (b - t) * ratio; }
        }
    }
    cx_ = l; cy_ = t; cw_ = r - l; ch_ = b - t;
}

void OptionsPopup::Apply()
{
    Json opts = Json::Object();
    if (kind_ == Popup::Quality) opts.Set(L"q", quality_);
    else if (kind_ == Popup::Size) {
        if (width_.empty() && height_.empty()) { MessageBeep(MB_OK); return; }
        if (!width_.empty()) opts.Set(L"w", _wtoi(width_.c_str()));
        if (!height_.empty()) opts.Set(L"h", _wtoi(height_.c_str()));
    } else if (kind_ == Popup::Trim) {
        // "1:30", "0:01:30" or "90" (seconds, decimals allowed).
        auto seconds = [](const wstring& t) {
            double v = 0, part = 0;
            for (size_t i = 0, start = 0; i <= t.size(); i++)
                if (i == t.size() || t[i] == L':') { part = _wtof(t.substr(start, i - start).c_str()); v = v * 60 + part; start = i + 1; }
            return v;
        };
        double a = width_.empty() ? 0 : seconds(width_), b = height_.empty() ? 0 : seconds(height_);
        if (b > 0 && b <= a) { MessageBeep(MB_OK); return; }
        opts.Set(L"start", a);
        if (b > 0) opts.Set(L"end", b);
    } else if (kind_ == Popup::Crop) {
        int x = int(std::lround(cx_ * origW_)), y = int(std::lround(cy_ * origH_));
        int w = std::max(1, int(std::lround(cw_ * origW_))), h = std::max(1, int(std::lround(ch_ * origH_)));
        opts.Set(L"crop", std::to_wstring(x) + L"," + std::to_wstring(y) + L"," + std::to_wstring(w) + L"," + std::to_wstring(h));
    }
    auto fn = onApply;
    Hide();
    if (fn) fn(opts);
}

LRESULT OptionsPopup::Handle(UINT m, WPARAM w, LPARAM l)
{
    auto local = [&](LPARAM lp) { return std::pair<double, double>{ GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_ }; };
    switch (m) {
        case WM_LBUTTONDOWN: {
            auto [x, y] = local(l);
            int a = -1;
            Part p = HitTest(x, y, &a);
            if (p == Part::Width || p == Part::Height) { field_ = p == Part::Width ? 0 : 1; dirty_ = true; RequestFrame(); return 0; }
            if (p == Part::Aspect) { SetAspect(a); dirty_ = true; RequestFrame(); return 0; }
            if (p == Part::Slider || (p >= Part::CropMove && p <= Part::CropSW)) {
                drag_ = p;
                dragX_ = x; dragY_ = y;
                startX_ = cx_; startY_ = cy_; startW_ = cw_; startH_ = ch_;
                SetCapture(hwnd_);
                Drag(x, y);
                dirty_ = true;
                RequestFrame();
                return 0;
            }
            if (p == Part::None) {
                // Empty parts of the card move the popup.
                ReleaseCapture();
                SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                GetWindowRect(hwnd_, &frame_);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            auto [x, y] = local(l);
            if (drag_ != Part::None) { Drag(x, y); dirty_ = true; RequestFrame(); return 0; }
            int a;
            Part p = HitTest(x, y, &a);
            if (p != hover_) { hover_ = p; dirty_ = true; RequestFrame(); }
            return 0;
        }
        case WM_LBUTTONUP: {
            auto [x, y] = local(l);
            if (drag_ != Part::None) { drag_ = Part::None; ReleaseCapture(); return 0; }
            int a;
            Part p = HitTest(x, y, &a);
            if (p == Part::Apply) Apply();
            else if (p == Part::Cancel) { Log(L"popup: cancelled"); Hide(); }
            return 0;
        }
        case WM_SETCURSOR: {
            LPCWSTR c = IDC_ARROW;
            switch (hover_) {
                case Part::CropMove: c = IDC_SIZEALL; break;
                case Part::CropN: case Part::CropS: c = IDC_SIZENS; break;
                case Part::CropE: case Part::CropW: c = IDC_SIZEWE; break;
                case Part::CropNW: case Part::CropSE: c = IDC_SIZENWSE; break;
                case Part::CropNE: case Part::CropSW: c = IDC_SIZENESW; break;
                case Part::Width: case Part::Height: c = IDC_IBEAM; break;
                case Part::Apply: case Part::Cancel: case Part::Aspect: case Part::Slider: c = IDC_HAND; break;
                default: break;
            }
            SetCursor(LoadCursorW(nullptr, c));
            return TRUE;
        }
        case WM_KEYDOWN: {
            if (w == VK_ESCAPE) { Log(L"popup: cancelled"); Hide(); return 0; }
            if (w == VK_RETURN) { Apply(); return 0; }
            if (kind_ == Popup::Quality && (w == VK_LEFT || w == VK_RIGHT)) {
                int step = (GetKeyState(VK_CONTROL) & 0x8000) ? 1 : 5;
                quality_ = std::clamp(quality_ + (w == VK_RIGHT ? step : -step), 1, 100);
            }
            if ((kind_ == Popup::Size || kind_ == Popup::Trim) && w == VK_TAB) field_ ^= 1;
            if ((kind_ == Popup::Size || kind_ == Popup::Trim) && w == VK_BACK) { wstring& f = field_ ? height_ : width_; if (!f.empty()) f.pop_back(); }
            if (kind_ == Popup::Crop && w >= '1' && w <= '4') SetAspect(int(w - '1'));
            dirty_ = true;
            RequestFrame();
            return 0;
        }
        case WM_CHAR:
            if (kind_ == Popup::Size && w >= L'0' && w <= L'9') {
                wstring& f = field_ ? height_ : width_;
                if (f.size() < 5 && !(f.empty() && w == L'0')) f += wchar_t(w);
                dirty_ = true;
                RequestFrame();
            }
            if (kind_ == Popup::Trim && ((w >= L'0' && w <= L'9') || w == L':' || w == L'.')) {
                wstring& f = field_ ? height_ : width_;   // start, end
                if (f.size() < 10) f += wchar_t(w);
                dirty_ = true;
                RequestFrame();
            }
            return 0;
        case WM_ACTIVATE:
            // Clicking elsewhere cancels, like a menu.
            if (LOWORD(w) == WA_INACTIVE && shown_ && drag_ == Part::None) { Log(L"popup: closed (clicked elsewhere)"); Hide(); }
            return 0;
        case WM_MOVE: GetWindowRect(hwnd_, &frame_); return 0;
        case WM_DPICHANGED: return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

void OptionsPopup::Draw()
{
    dirty_ = false;
    if (!shown_) return;
    ID2D1DCRenderTarget* rt = surf_.Begin();
    if (!rt) return;
    const Theme& th = CurrentTheme();
    float L = float(kPad), T = float(kPad), W = float(w_), H = float(h_);
    surf_.DrawShadow(RectF(L, T, W, H), 16, 22, 0, 8, th.shadow);
    rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(L, T, W, H), 16, 16), surf_.Solid(th.glass | 0xFF000000));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(RectF(L + 0.5, T + 0.5, W - 1, H - 1), 15.5f, 15.5f), surf_.Solid(th.edge), 1);
    surf_.DrawText(title_.c_str(), 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(L + 20, T + 16, W - 40, 22), th.text);

    auto button = [&](float x, float y, const wchar_t* label, bool primary, bool hot) {
        uint32_t bg = primary ? th.accent : th.button;
        if (hot) bg = primary ? WithAlpha(th.accent, 0.85) : WithAlpha(th.button, 0.7);
        rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(x, y, kBtnW, kBtnH), 8, 8), surf_.Solid(bg));
        float tw = surf_.MeasureText(label, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        surf_.DrawText(label, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(x + (kBtnW - tw) / 2, y + 6.5, tw + 4, 18), primary ? th.accentText : th.text);
    };
    float by = T + H - 16 - float(kBtnH);
    const wchar_t* verb = kind_ == Popup::Quality ? L"Compress" : kind_ == Popup::Size ? L"Resize" : kind_ == Popup::Trim ? L"Trim" : L"Crop";
    button(L + W - 20 - float(kBtnW), by, verb, true, hover_ == Part::Apply);
    button(L + W - 20 - 2 * float(kBtnW) - 8, by, L"Cancel", false, hover_ == Part::Cancel);

    if (kind_ == Popup::Quality) {
        wstring v = L"Quality " + std::to_wstring(quality_);
        float vw = surf_.MeasureText(v.c_str(), 12.5f, DWRITE_FONT_WEIGHT_NORMAL);
        surf_.DrawText(v.c_str(), 12.5f, DWRITE_FONT_WEIGHT_NORMAL, RectF(L + W - 20 - vw, T + 18, vw + 4, 20), th.dim);
        float x0 = L + 20, x1 = L + W - 20, y = T + 75;
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, y - 2, x1, y + 2), 2, 2), surf_.Solid(th.sep));
        float kx = x0 + (x1 - x0) * float(quality_) / 100.0f;
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, y - 2, kx, y + 2), 2, 2), surf_.Solid(th.accent));
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, y), 9, 9), surf_.Solid(th.accent));
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, y), 4, 4), surf_.Solid(th.accentText));
        surf_.DrawText(L"Smaller file", 11, DWRITE_FONT_WEIGHT_NORMAL, RectF(x0, y + 16, 120, 16), th.dim);
        float rw = surf_.MeasureText(L"Better picture", 11, DWRITE_FONT_WEIGHT_NORMAL);
        surf_.DrawText(L"Better picture", 11, DWRITE_FONT_WEIGHT_NORMAL, RectF(x1 - rw, y + 16, rw + 4, 16), th.dim);
    } else if (kind_ == Popup::Size || kind_ == Popup::Trim) {
        bool trim = kind_ == Popup::Trim;
        auto field = [&](float x, const wchar_t* caption, const wstring& value, bool active) {
            surf_.DrawText(caption, 11, DWRITE_FONT_WEIGHT_NORMAL, RectF(x, T + 46, 124, 16), th.dim);
            rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(x, T + 64, 124, 34), 8, 8), surf_.Solid(th.button));
            if (active) rt->DrawRoundedRectangle(D2D1::RoundedRect(RectF(x + 0.75, T + 64.75, 122.5, 32.5), 7.5f, 7.5f), surf_.Solid(th.accent), 1.5f);
            wstring shown = value.empty() ? (trim ? (caption[0] == L'S' ? L"0:00" : L"the end") : L"auto") : trim ? value : value + L" px";
            surf_.DrawText(shown.c_str(), 13, DWRITE_FONT_WEIGHT_NORMAL, RectF(x + 12, T + 72, 100, 20), value.empty() ? th.dim : th.text);
            if (active) {
                float cw = value.empty() ? 0 : surf_.MeasureText(value.c_str(), 13, DWRITE_FONT_WEIGHT_NORMAL);
                rt->DrawLine(D2D1::Point2F(x + 12 + cw + 1, T + 73), D2D1::Point2F(x + 12 + cw + 1, T + 89), surf_.Solid(th.text), 1);
            }
        };
        field(L + 20, trim ? L"Start" : L"Width", width_, field_ == 0);
        field(L + W - 20 - 124, trim ? L"End" : L"Height", height_, field_ == 1);
        surf_.DrawText(trim ? L"–" : L"×", 14, DWRITE_FONT_WEIGHT_NORMAL, RectF(L + W / 2 - 5, T + 71, 12, 20), th.dim);
        surf_.DrawText(trim ? L"Minutes:seconds, like 1:30" : L"Leave one on auto to keep the proportions", 11, DWRITE_FONT_WEIGHT_NORMAL,
                       RectF(L + 20, T + 104, W - 40, 16), th.dim);
    } else if (kind_ == Popup::Crop) {
        wstring dims = std::to_wstring(int(std::lround(cw_ * origW_))) + L" × " + std::to_wstring(int(std::lround(ch_ * origH_)));
        float dw = surf_.MeasureText(dims.c_str(), 12, DWRITE_FONT_WEIGHT_NORMAL);
        surf_.DrawText(dims.c_str(), 12, DWRITE_FONT_WEIGHT_NORMAL, RectF(L + W - 20 - dw, T + 18, dw + 4, 18), th.dim);
        D2D1_RECT_F pr = PictureRect();
        surf_.DrawMips(pic_, pr);
        float pw = pr.right - pr.left, ph = pr.bottom - pr.top;
        float l = pr.left + float(cx_) * pw, t = pr.top + float(cy_) * ph, r = l + float(cw_) * pw, b = t + float(ch_) * ph;
        auto shade = surf_.Solid(0x88000000);
        rt->FillRectangle(D2D1::RectF(pr.left, pr.top, pr.right, t), shade);
        rt->FillRectangle(D2D1::RectF(pr.left, b, pr.right, pr.bottom), shade);
        rt->FillRectangle(D2D1::RectF(pr.left, t, l, b), shade);
        rt->FillRectangle(D2D1::RectF(r, t, pr.right, b), shade);
        rt->DrawRectangle(D2D1::RectF(l, t, r, b), surf_.Solid(0xFFFFFFFF), 1.5f);
        for (int i = 1; i < 3; i++) {   // thirds, faint
            rt->DrawLine(D2D1::Point2F(l + (r - l) * i / 3, t), D2D1::Point2F(l + (r - l) * i / 3, b), surf_.Solid(0x55FFFFFF), 1);
            rt->DrawLine(D2D1::Point2F(l, t + (b - t) * i / 3), D2D1::Point2F(r, t + (b - t) * i / 3), surf_.Solid(0x55FFFFFF), 1);
        }
        for (float hx : { l, r }) for (float hy : { t, b }) rt->FillRectangle(D2D1::RectF(hx - 4, hy - 4, hx + 4, hy + 4), surf_.Solid(0xFFFFFFFF));
        for (int i = 0; i < 4; i++) {
            float x = L + 20 + i * 52;
            bool on = aspect_ == i;
            rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(x, by, 48, kBtnH), 8, 8), surf_.Solid(on ? WithAlpha(th.accent, 0.18) : th.button));
            float tw = surf_.MeasureText(kAspects[i], 12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            surf_.DrawText(kAspects[i], 12, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(x + (48 - tw) / 2, by + 7, tw + 4, 18), on ? th.accent : th.text);
        }
    }
    surf_.End();
    surf_.Present(hwnd_, frame_.left, frame_.top, BYTE(std::clamp(fade_.value, 0.0, 1.0) * 255));
    if (fade_.Active()) dirty_ = true;
}

}  // namespace rd
