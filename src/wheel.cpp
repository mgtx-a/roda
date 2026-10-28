#include "wheel.h"

#include "dragdrop.h"

namespace rd {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Geometry, in DIPs.
constexpr double kCentre = 46;                      // the centre disc: file count, and Cancel
constexpr double kRing0In = 52, kRing0Out = 148;    // up to 8 options
constexpr double kRing1In = 154, kRing1Out = 226;   // the rest
constexpr double kGrow = 6;                         // a hovered segment reaches out this far
constexpr double kMargin = 30;                      // room around the disc for the shadow and the hover
constexpr int kPerRing = 8;

struct Palette {
    uint32_t ring, sep, edge, hot, text, textHot, centre, centreHot, centreText, dim;
    float shadow;
};

const Palette& Colours()
{
    // Our own colours: a deep teal accent on a pale (or dark) glass disc.
    static const Palette light{ 0xF2FAFBFC, 0x1A10202A, 0x2610202A, 0xFF1F7A74, 0xFF1D2430, 0xFFFFFFFF, 0xFFE8EEF0, 0xFFF3D9D9, 0xFF27303C, 0xFF6A7480, 0.28f };
    static const Palette dark{ 0xF0252A30, 0x26FFFFFF, 0x30FFFFFF, 0xFF2E9E96, 0xFFE8EBEE, 0xFFFFFFFF, 0xFF1B1F24, 0xFF4A2A2C, 0xFFE8EBEE, 0xFF9AA3AD, 0.50f };
    return DarkMode() ? dark : light;
}

uint32_t WithAlpha(uint32_t argb, double factor)
{
    uint32_t a = uint32_t(std::clamp(((argb >> 24) & 255) * factor, 0.0, 255.0) + 0.5);
    return (argb & 0x00FFFFFF) | (a << 24);
}

D2D1_POINT_2F Polar(double cx, double cy, double r, double a)
{
    return D2D1::Point2F(float(cx + r * std::sin(a)), float(cy - r * std::cos(a)));   // clockwise from the top
}

Com<ID2D1PathGeometry> Sector(double cx, double cy, double rIn, double rOut, double a0, double a1)
{
    Com<ID2D1PathGeometry> geo;
    Com<ID2D1GeometrySink> sink;
    if (FAILED(D2D()->CreatePathGeometry(geo.Put())) || FAILED(geo->Open(sink.Put()))) return {};
    D2D1_ARC_SIZE big = (a1 - a0) > kPi ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
    sink->BeginFigure(Polar(cx, cy, rOut, a0), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddArc(D2D1::ArcSegment(Polar(cx, cy, rOut, a1), D2D1::SizeF(float(rOut), float(rOut)), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, big));
    sink->AddLine(Polar(cx, cy, rIn, a1));
    sink->AddArc(D2D1::ArcSegment(Polar(cx, cy, rIn, a0), D2D1::SizeF(float(rIn), float(rIn)), 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, big));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    return geo;
}

/// Phase 1 placeholders: the real lists come from the converter registry in Phase 2.
std::vector<const wchar_t*> PlaceholderOptions(WheelKind k)
{
    switch (k) {
        case WheelKind::Convert: return { L"WEBP", L"PNG", L"JPG", L"PDF", L"TIFF", L"AVIF", L"BMP", L"ICO" };
        case WheelKind::Tools: return { L"Compress", L"Resize", L"Crop", L"Rotate", L"Flip", L"Strip info", L"Merge PDF" };
        default: return { L"Copy to", L"Move to", L"Copy path", L"Terminal", L"Float", L"Share" };
    }
}

}  // namespace

// ---------------------------------------------------------------- drop target

class WheelDropTarget : public DropTarget {
public:
    Wheel* wheel = nullptr;
    STDMETHODIMP DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override
    {
        hasFiles_ = HasFiles(data);
        *effect = hasFiles_ && wheel ? wheel->OnDragOver(pt, *effect) : DROPEFFECT_NONE;
        return S_OK;
    }
    STDMETHODIMP DragOver(DWORD, POINTL pt, DWORD* effect) override
    {
        *effect = hasFiles_ && wheel ? wheel->OnDragOver(pt, *effect) : DROPEFFECT_NONE;
        return S_OK;
    }
    STDMETHODIMP DragLeave() override
    {
        if (wheel) wheel->OnDragLeave();
        return S_OK;
    }
    STDMETHODIMP Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override
    {
        *effect = hasFiles_ && wheel ? wheel->OnDrop(data, pt, *effect) : DROPEFFECT_NONE;
        return S_OK;
    }
private:
    bool hasFiles_ = false;
};

// ---------------------------------------------------------------- window

Wheel::Wheel() {}

Wheel::~Wheel()
{
    if (hwnd_) {
        RevokeDragDrop(hwnd_);
        DestroyWindow(hwnd_);
    }
    if (target_) { target_->wheel = nullptr; target_->Release(); }
}

bool Wheel::Create(HINSTANCE inst)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"RodaWheel";
    RegisterClassW(&wc);
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"Roda",
                            WS_POPUP, -10000, -10000, 1, 1, nullptr, nullptr, inst, this);
    if (!hwnd_) return false;
    target_ = new WheelDropTarget();
    target_->wheel = this;
    HRESULT hr = RegisterDragDrop(hwnd_, target_);
    if (FAILED(hr)) Log(L"wheel: could not accept drops (0x%08lx)", hr);
    // Load the font now: the first wheel would otherwise wait ~15 ms for DirectWrite.
    for (float size : { 10.5f, 11.0f, 12.0f, 13.0f })
        for (auto wt : { DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_WEIGHT_SEMI_BOLD }) {
            Com<IDWriteTextLayout> layout;
            if (auto* fmt = TextFormat(size, wt)) DWrite()->CreateTextLayout(L"WEBP 0", 6, fmt, 100, 20, layout.Put());
        }
    return SUCCEEDED(hr);
}

LRESULT CALLBACK Wheel::WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_DPICHANGED: return 0;   // the wheel sizes itself for the monitor it opens on
    }
    return DefWindowProcW(h, m, w, l);
}

bool Wheel::MixedSelection() const
{
    if (paths_.empty()) return true;
    wstring first = Extension(paths_[0]);
    for (auto& p : paths_) {
        if (DirExists(p) || Extension(p).empty() || Extension(p) != first) return true;
    }
    return false;
}

wstring Wheel::CentreText() const
{
    size_t n = paths_.size();
    if (n == 1 && DirExists(paths_[0])) return L"1 folder";
    if (MixedSelection()) return std::to_wstring(n) + (n == 1 ? L" item" : L" items");
    wstring ext = Extension(paths_[0]).substr(1);
    for (auto& c : ext) c = wchar_t(towupper(c));
    return std::to_wstring(n) + L" " + ext + (n == 1 ? L" file" : L" files");
}

void Wheel::Build()
{
    auto opts = PlaceholderOptions(kind_);
    segs_.clear();
    int n = int(opts.size());
    int inner = std::min(n, kPerRing), outer = n - inner;
    for (int i = 0; i < n; i++) {
        Segment s;
        s.label = opts[size_t(i)];
        s.ring = i < inner ? 0 : 1;
        int count = s.ring == 0 ? inner : outer, idx = s.ring == 0 ? i : i - inner;
        double step = 2 * kPi / count;
        s.a0 = idx * step - step / 2;
        s.a1 = s.a0 + (count == 1 ? step - 1e-3 : step);
        s.hover = std::make_unique<Anim>();
        segs_.push_back(std::move(s));
    }
    hot_ = -2;
}

void Wheel::Show(WheelKind kind, const std::vector<wstring>& paths, double keySeenAt)
{
    if (!hwnd_) return;
    paths_ = paths;
    kind_ = kind;
    if (kind_ != WheelKind::Actions && MixedSelection()) {
        Log(L"wheel: folders or mixed file types, so the Actions wheel opens instead of %s", WheelName(kind));
        kind_ = WheelKind::Actions;
    }
    Build();
    token_++;
    shown_ = true; closing_ = false; dropped_ = false; chosen_ = -1;
    keySeenAt_ = keySeenAt;

    centre_ = CursorPos();
    scale_ = ScaleAt(centre_);
    surf_.SetScale(scale_);
    bool twoRings = segs_.size() > size_t(kPerRing);
    double outer = twoRings ? kRing1Out : kRing0Out;
    sizeDip_ = 2 * (outer + kGrow + kMargin);
    int px = int(std::lround(sizeDip_ * scale_));

    // The disc stays whole on the screen under the pointer; only the shadow may spill over.
    Mon mon = MonitorOf(centre_);
    int disc = int(std::lround((outer + kGrow) * scale_));
    POINT c = centre_;
    c.x = std::clamp<LONG>(c.x, mon.work.left + disc, std::max<LONG>(mon.work.left + disc, mon.work.right - disc));
    c.y = std::clamp<LONG>(c.y, mon.work.top + disc, std::max<LONG>(mon.work.top + disc, mon.work.bottom - disc));
    centre_ = c;
    frame_ = { c.x - px / 2, c.y - px / 2, c.x - px / 2 + px, c.y - px / 2 + px };
    surf_.Resize(px, px);

    if (ReducedMotion()) { open_.Set(1); fade_.Set(1); }
    else {
        open_.Set(0.85); open_.Spring(1, 0.22, 0.78);
        fade_.Set(0.35); fade_.Ease(1, 0.15, Curve::EaseOut);
    }
    pulse_.Set(0);
    SetWindowPos(hwnd_, HWND_TOPMOST, frame_.left, frame_.top, px, px, SWP_NOACTIVATE);
    Draw();   // the first frame now, not on the next pass of the loop
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    Log(L"wheel: %s opened for %s at %.0f%% scale", WheelName(kind_), CentreText().c_str(), scale_ * 100);
}

void Wheel::Switch(WheelKind kind)
{
    if (!shown_ || closing_ || kind == kind_) return;
    if (kind != WheelKind::Actions && MixedSelection()) return;
    size_t before = segs_.size();
    kind_ = kind;
    Build();
    bool sizeChanged = (before > size_t(kPerRing)) != (segs_.size() > size_t(kPerRing));
    if (sizeChanged) {
        // The other wheel needs another window size: re-open it under the pointer.
        auto paths = paths_;
        Show(kind, paths, -1);
        return;
    }
    if (!ReducedMotion()) { open_.Set(0.94); open_.Spring(1, 0.2, 0.8); }
    dirty_ = true;
    RequestFrame();
    Log(L"wheel: switched to %s", WheelName(kind_));
}

void Wheel::Hide(bool animated)
{
    if (!shown_ || closing_) return;
    closing_ = true;
    if (!animated || ReducedMotion()) fade_.Set(0);
    else { fade_.Ease(0, 0.12, Curve::EaseIn); open_.Ease(0.92, 0.12, Curve::EaseIn); }
    dirty_ = true;
    RequestFrame();
}

void Wheel::ButtonReleased()
{
    if (!shown_ || closing_ || dropped_) return;
    // The drop is delivered by the drag source a moment after the button goes up.
    uint32_t t = token_;
    After(0.35, [this, t] {
        if (t == token_ && shown_ && !dropped_) { Log(L"wheel: released away from the wheel, nothing done"); Hide(); }
    });
}

int Wheel::HitTest(POINT p) const
{
    double k = std::max(0.5, open_.value) * scale_;
    double dx = (p.x - centre_.x) / k, dy = (p.y - centre_.y) / k;
    double d = std::sqrt(dx * dx + dy * dy);
    if (d < kRing0In) return -1;
    bool twoRings = segs_.size() > size_t(kPerRing);
    int ring;
    if (d <= kRing0Out + (twoRings ? 3 : kGrow)) ring = 0;
    else if (twoRings && d <= kRing1Out + kGrow) ring = 1;
    else return -2;
    double a = std::atan2(dx, -dy);
    if (a < 0) a += 2 * kPi;
    for (size_t i = 0; i < segs_.size(); i++) {
        const auto& s = segs_[i];
        if (s.ring != ring) continue;
        double x = a;
        if (x < s.a0) x += 2 * kPi;   // the first segment straddles the top
        if (x >= s.a0 && x < s.a1) return int(i);
    }
    return -2;
}

void Wheel::SetHot(int i)
{
    if (i == hot_) return;
    bool still = ReducedMotion();
    if (hot_ >= 0 && hot_ < int(segs_.size())) { auto& h = *segs_[size_t(hot_)].hover; still ? h.Set(0) : h.Spring(0, 0.2, 0.9); }
    hot_ = i;
    if (hot_ >= 0) { auto& h = *segs_[size_t(hot_)].hover; still ? h.Set(1) : h.Spring(1, 0.16, 0.8); }
    dirty_ = true;
    RequestFrame();
}

DWORD Wheel::OnDragOver(POINTL pt, DWORD allowed)
{
    if (!shown_ || closing_) return DROPEFFECT_NONE;
    int h = HitTest({ pt.x, pt.y });
    SetHot(h);
    return h >= 0 ? CopyEffect(allowed) : DROPEFFECT_NONE;
}

void Wheel::OnDragLeave()
{
    SetHot(-2);
    // Leaving with the pointer still over the disc means the drag itself ended: Esc, or the source gave up.
    if (shown_ && !closing_ && !dropped_ && HitTest(CursorPos()) != -2) {
        Log(L"wheel: the drag was cancelled");
        Hide();
    }
}

DWORD Wheel::OnDrop(IDataObject* data, POINTL pt, DWORD allowed)
{
    if (!shown_ || closing_) return DROPEFFECT_NONE;
    int h = HitTest({ pt.x, pt.y });
    if (h < 0) {
        Log(h == -1 ? L"wheel: dropped on the centre, cancelled" : L"wheel: dropped outside the options, nothing done");
        dropped_ = true;
        Hide();
        return DROPEFFECT_NONE;
    }
    std::vector<wstring> paths;
    if (!ReadDroppedPaths(data, paths)) paths = paths_;
    dropped_ = true;
    chosen_ = h;
    SetHot(h);
    if (onChoose) onChoose(kind_, segs_[size_t(h)].label, paths);

    if (ReducedMotion()) Hide(false);
    else {
        pulse_.Set(0);
        pulse_.Ease(1, 0.14, Curve::EaseOut);
        uint32_t t = token_;
        After(0.14, [this, t] { if (t == token_) { pulse_.Ease(0, 0.1, Curve::EaseIn); Hide(); } });
    }
    return CopyEffect(allowed);
}

// ---------------------------------------------------------------- drawing

void Wheel::Present()
{
    double f = std::clamp(fade_.value, 0.0, 1.0);
    // While open, every pixel of the disc keeps some alpha: alpha-0 pixels let drops fall through.
    int alpha = closing_ ? int(f * 255) : std::max(8, int(f * 255));
    surf_.Present(hwnd_, frame_.left, frame_.top, BYTE(alpha));
}

void Wheel::Draw()
{
    dirty_ = false;
    if (!shown_) return;
    if (closing_ && !fade_.Active() && fade_.value <= 0.001) {
        ShowWindow(hwnd_, SW_HIDE);
        SetWindowPos(hwnd_, nullptr, -10000, -10000, 1, 1, SWP_NOACTIVATE | SWP_NOZORDER);
        surf_.Release();   // a hidden wheel holds no pixels
        shown_ = closing_ = false;
        paths_.clear();
        segs_.clear();
        uint32_t t = ++token_;
        // Drop the renderer's memory once the wheel has stayed closed for a moment.
        After(1.5, [this, t] { if (t == token_ && !shown_) GfxTrim(); });
        return;
    }
    ID2D1DCRenderTarget* rt = surf_.Begin();
    if (!rt) return;
    const Palette& pal = Colours();
    float s = float(scale_), k = float(open_.value);
    float c = float(sizeDip_ / 2);
    rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F(c, c)) * D2D1::Matrix3x2F::Scale(s, s));

    bool twoRings = segs_.size() > size_t(kPerRing);
    float R = float(twoRings ? kRing1Out : kRing0Out);
    surf_.DrawShadow(RectF(c - R, c - R, 2 * R, 2 * R), R, 20, 0, 6, pal.shadow);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), R, R), surf_.Solid(pal.ring));
    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), R - 0.5f, R - 0.5f), surf_.Solid(pal.edge), 1.0f);   // under the segments: a hovered one reaches past it

    for (size_t i = 0; i < segs_.size(); i++) {
        const auto& sg = segs_[i];
        double rIn = sg.ring == 0 ? kRing0In : kRing1In, rOut = sg.ring == 0 ? kRing0Out : kRing1Out;
        double hv = std::clamp(sg.hover->value, 0.0, 1.2);
        double pulse = int(i) == chosen_ ? pulse_.value : 0;
        if (hv > 0.01) {
            auto geo = Sector(c, c, rIn + 2, rOut + kGrow * hv + 4 * pulse, sg.a0 + 0.012, sg.a1 - 0.012);
            if (geo) rt->FillGeometry(geo.Get(), surf_.Solid(WithAlpha(pal.hot, std::min(1.0, hv))));
        }
        if (segs_.size() > 1) rt->DrawLine(Polar(c, c, rIn + 4, sg.a0), Polar(c, c, rOut - 4, sg.a0), surf_.Solid(pal.sep), 1.0f);

        float size = sg.ring == 0 ? 13.0f : 12.0f;
        DWRITE_FONT_WEIGHT wt = DWRITE_FONT_WEIGHT_SEMI_BOLD;
        D2D1_POINT_2F mid = Polar(c, c, (rIn + rOut) / 2 + kGrow * hv / 2, (sg.a0 + sg.a1) / 2);
        float w = surf_.MeasureText(sg.label.c_str(), size, wt);
        uint32_t col = hv > 0.5 ? pal.textHot : pal.text;
        surf_.DrawText(sg.label.c_str(), size, wt, D2D1::RectF(mid.x - w / 2, mid.y - size * 0.68f, mid.x + w / 2 + 2, mid.y + size), col);
    }
    if (twoRings) rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), float(kRing0Out + 3), float(kRing0Out + 3)), surf_.Solid(pal.sep), 1.0f);

    // The centre: what is being dragged, and Cancel.
    bool cancelHot = hot_ == -1;
    float cr = float(kCentre);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), cr, cr), surf_.Solid(cancelHot ? pal.centreHot : pal.centre));
    wstring top = cancelHot ? L"Cancel" : CentreText();
    const wchar_t* bottom = cancelHot ? L"" : WheelName(kind_);
    float ts = top.size() > 12 ? 10.5f : 12.0f;
    float tw = surf_.MeasureText(top.c_str(), ts, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float ty = cancelHot ? c - ts * 0.68f : c - ts - 2;
    surf_.DrawText(top.c_str(), ts, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(c - tw / 2, ty, c + tw / 2 + 2, ty + ts * 1.6f), pal.centreText);
    if (*bottom) {
        float bw = surf_.MeasureText(bottom, 11, DWRITE_FONT_WEIGHT_NORMAL);
        surf_.DrawText(bottom, 11, DWRITE_FONT_WEIGHT_NORMAL, D2D1::RectF(c - bw / 2, c + 3, c + bw / 2 + 2, c + 20), pal.dim);
    }

    surf_.End();
    Present();
    if (keySeenAt_ >= 0) {
        Log(L"wheel: first frame on screen %.1f ms after the key was seen", (Now() - keySeenAt_) * 1000);
        keySeenAt_ = -1;
    }
}

}  // namespace rd
