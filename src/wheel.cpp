#include "wheel.h"

#include "dragdrop.h"
#include "ui.h"

namespace rd {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Geometry, in DIPs.
constexpr double kCentre = 48;                      // the centre disc: what is dragged, and Cancel
constexpr double kRing0In = 54, kRing0Out = 150;    // up to 8 options
constexpr double kRing1In = 156, kRing1Out = 230;   // the rest
constexpr double kGrow = 6;                         // a hovered segment reaches out this far
constexpr double kMargin = 30;                      // room around the disc for the shadow and the hover
constexpr double kTabH = 24, kTabGap = 10;        // the Convert · Tools · Files tabs above the disc
constexpr double kTabRoom = kTabH + kTabGap + 4;
constexpr int kTabHit = -10;                        // HitTest: kTabHit - i is tab i
constexpr int kPerRing = 8;
constexpr size_t kMagicLimit = 200;                 // beyond this many items, judge by extension (no file reads)

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

Wheel::Wheel(const Settings& settings) : settings_(settings) {}

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
    for (float size : { 10.5f, 11.0f, 11.5f, 12.0f, 12.5f, 14.0f })
        for (auto wt : { DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_WEIGHT_SEMI_BOLD }) {
            Com<IDWriteTextLayout> layout;
            if (auto* fmt = TextFormat(size, wt)) DWrite()->CreateTextLayout(L"WEBP 0", 6, fmt, 100, 20, layout.Put());
        }
    return SUCCEEDED(hr);
}

LRESULT CALLBACK Wheel::WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<Wheel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self && self->hwnd_ == h) return self->Handle(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT Wheel::Handle(UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
        case WM_MOUSEACTIVATE: return keyboard_ ? MA_ACTIVATE : MA_NOACTIVATE;
        case WM_DPICHANGED: return 0;   // the wheel sizes itself for the monitor it opens on
        case WM_MOUSEMOVE: {
            // Windows sends a move when the wheel appears under the still pointer; that must not steal the
            // keyboard's highlight.
            POINT p = CursorPos();
            if (keyboard_ && open() && (p.x != lastMouse_.x || p.y != lastMouse_.y)) { lastMouse_ = p; SetHot(HitTest(p)); }
            return 0;
        }
        case WM_LBUTTONUP:
            if (keyboard_ && open()) {
                int h = HitTest(CursorPos());
                if (h <= kTabHit) { Switch(static_cast<WheelKind>(kTabHit - h)); if (!segs_.empty()) SetHot(0); }
                else if (h >= 0) Choose(h);
                else if (h == -1) { Log(L"wheel: cancelled"); Hide(); }
            }
            return 0;
        case WM_KEYDOWN: {
            if (!keyboard_ || !open()) return 0;
            int n = int(segs_.size());
            if (w == VK_ESCAPE) { Log(L"wheel: Esc, cancelled"); Hide(); }
            else if (w == VK_RETURN) { if (hot_ >= 0) Choose(hot_); }
            else if (w == VK_RIGHT || w == VK_DOWN) SetHot(hot_ < 0 ? 0 : (hot_ + 1) % n);
            else if (w == VK_LEFT || w == VK_UP) SetHot(hot_ < 0 ? n - 1 : (hot_ + n - 1) % n);
            else if (w == VK_TAB) {
                Next((GetKeyState(VK_SHIFT) & 0x8000) != 0);
                if (!segs_.empty()) SetHot(0);
            }
            return 0;
        }
        case WM_ACTIVATE:
            if (keyboard_ && LOWORD(w) == WA_INACTIVE && open()) { Log(L"wheel: closed (another window took the focus)"); Hide(); }
            return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

bool Wheel::Mixed() const
{
    if (kinds_.empty()) return true;
    for (Kind k : kinds_) if (k == Kind::Folder || k == Kind::Unknown || k != kinds_[0]) return true;
    return false;
}

wstring Wheel::CentreText() const
{
    size_t n = paths_.size();
    if (n == 1 && !kinds_.empty() && kinds_[0] == Kind::Folder) return L"1 folder";
    if (Mixed()) return std::to_wstring(n) + (n == 1 ? L" item" : L" items");
    return std::to_wstring(n) + L" " + KindLabel(kinds_[0]) + (n == 1 ? L" file" : L" files");
}

bool Wheel::Build()
{
    auto opts = OptionsFor(kind_, kinds_, settings_);
    if (opts.empty()) return false;
    segs_.clear();
    int n = int(opts.size());
    int inner = std::min(n, kPerRing), outer = n - inner;
    for (int i = 0; i < n; i++) {
        Segment s;
        s.opt = opts[size_t(i)];
        s.ring = i < inner ? 0 : 1;
        int count = s.ring == 0 ? inner : outer, idx = s.ring == 0 ? i : i - inner;
        double step = 2 * kPi / count;
        s.a0 = idx * step - step / 2;
        s.a1 = s.a0 + (count == 1 ? step - 1e-3 : step);
        s.hover = std::make_unique<Anim>();
        segs_.push_back(std::move(s));
    }
    hot_ = -2;
    return true;
}

void Wheel::Show(WheelKind kind, const std::vector<wstring>& paths, double keySeenAt)
{
    if (!hwnd_) return;
    keyboard_ = false;
    paths_ = paths;
    kinds_.clear();
    // Magic bytes for the first items (so a mislabelled file still gets the right wheel), the name for the rest.
    for (size_t i = 0; i < paths_.size(); i++) kinds_.push_back(i < kMagicLimit ? KindOfPath(paths_[i]) : KindFromExtension(paths_[i]));
    for (int i = 0; i < 3; i++) avail_[i] = !OptionsFor(static_cast<WheelKind>(i), kinds_, settings_).empty();
    kind_ = kind;
    if (!Build()) {
        Log(L"wheel: %s has nothing for %s, so the Files wheel opens", WheelName(kind), CentreText().c_str());
        kind_ = WheelKind::Actions;
        if (!Build()) return;
    }
    Open(keySeenAt);
}

void Wheel::ShowForKeyboard(const std::vector<wstring>& paths)
{
    Show(WheelKind::Convert, paths, -1);
    if (!shown_) return;
    keyboard_ = true;
    lastMouse_ = CursorPos();
    // This wheel takes the focus, so the arrows and Enter reach it. The hotkey press lets Roda come to the front.
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & ~WS_EX_NOACTIVATE);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    SetHot(0);
}

void Wheel::Open(double keySeenAt, bool keepCentre)
{
    token_++;
    opens_++;
    shown_ = true; closing_ = false; dropped_ = false; chosen_ = -1;
    keySeenAt_ = keySeenAt;

    if (!keepCentre) centre_ = CursorPos();
    scale_ = ScaleAt(centre_);
    surf_.SetScale(scale_);
    bool twoRings = segs_.size() > size_t(kPerRing);
    double outer = twoRings ? kRing1Out : kRing0Out;
    sizeDip_ = 2 * (outer + kGrow + kMargin + kTabRoom);
    int px = int(std::lround(sizeDip_ * scale_));

    // The disc stays whole on the screen under the pointer; only the shadow may spill over.
    Mon mon = MonitorOf(centre_);
    int disc = int(std::lround((outer + kGrow) * scale_));
    POINT c = centre_;
    c.x = std::clamp<LONG>(c.x, mon.work.left + disc, std::max<LONG>(mon.work.left + disc, mon.work.right - disc));
    int top = disc + int(std::lround(kTabRoom * scale_));   // the tabs stay on the screen too
    c.y = std::clamp<LONG>(c.y, mon.work.top + top, std::max<LONG>(mon.work.top + top, mon.work.bottom - disc));
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
    Log(L"wheel: %s opened for %s at %.0f%% scale (%d options)", WheelName(kind_), CentreText().c_str(), scale_ * 100, int(segs_.size()));
}

void Wheel::Switch(WheelKind kind)
{
    if (!shown_ || closing_ || kind == kind_) return;
    WheelKind before = kind_;
    size_t count = segs_.size();
    kind_ = kind;
    if (!Build()) { kind_ = before; Build(); return; }   // nothing there for these files: stay
    bool sizeChanged = (count > size_t(kPerRing)) != (segs_.size() > size_t(kPerRing));
    if (sizeChanged) {
        // The other wheel needs another window size: re-open it where it was (the pointer may be on a tab).
        bool kb = keyboard_;
        Open(-1, true);
        keyboard_ = kb;
        Log(L"wheel: switched to %s", WheelName(kind_));
        return;
    }
    if (!ReducedMotion()) { open_.Set(0.94); open_.Spring(1, 0.2, 0.8); }
    dirty_ = true;
    RequestFrame();
    Log(L"wheel: switched to %s", WheelName(kind_));
}

void Wheel::Next(bool back)
{
    for (int step = 1; step <= 2; step++) {
        WheelKind before = kind_;
        Switch(static_cast<WheelKind>((int(kind_) + (back ? 3 - step : step)) % 3));
        if (kind_ != before) break;
    }
}

void Wheel::Hide(bool animated)
{
    if (!shown_ || closing_) return;
    closing_ = true;
    if (!animated || ReducedMotion()) fade_.Set(0);
    else { fade_.Ease(0, 0.12, Curve::EaseIn); open_.Ease(0.92, 0.12, Curve::EaseIn); }
    if (keyboard_) {
        keyboard_ = false;
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
    }
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
    for (int i = 0; i < 3; i++) {
        const auto& t = tabs_[i];
        if (avail_[i] && dx >= t.left - 3 && dx <= t.right + 3 && dy >= t.top - 3 && dy <= t.bottom + 3) return kTabHit - i;
    }
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

void Wheel::Choose(int h)
{
    dropped_ = true;
    chosen_ = h;
    SetHot(h);
    WheelOption opt = segs_[size_t(h)].opt;
    auto paths = paths_;
    POINT at = centre_;
    if (ReducedMotion()) Hide(false);
    else {
        // The chosen segment pulses, then the wheel shrinks away.
        pulse_.Set(0);
        pulse_.Ease(1, 0.14, Curve::EaseOut);
        uint32_t t = token_;
        After(0.14, [this, t] { if (t == token_) { pulse_.Ease(0, 0.1, Curve::EaseIn); Hide(); } });
    }
    // Run it after the drop has returned: the drag source (Explorer) waits inside DoDragDrop until then,
    // and a folder picker or a popup must not keep it frozen.
    if (onChoose) After(0, [this, opt, paths, at] { if (onChoose) onChoose(opt, paths, at); });
}

DWORD Wheel::OnDragOver(POINTL pt, DWORD allowed)
{
    if (!shown_ || closing_) return DROPEFFECT_NONE;
    int h = HitTest({ pt.x, pt.y });
    // Dragging onto a tab opens that wheel; the drop still has to land on an option.
    if (h <= kTabHit && static_cast<WheelKind>(kTabHit - h) != kind_) Switch(static_cast<WheelKind>(kTabHit - h));
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
    wstring err;
    if (HasVirtualFiles(data) && !HasFilesOnDisk(data)) {
        // Outlook attachments, files inside a ZIP: copied out now, while the source still offers them.
        if (ExtractVirtualFiles(data, paths, err)) paths_ = paths;
        else { Log(L"drop: %s", err.c_str()); dropped_ = true; Hide(); return DROPEFFECT_NONE; }
    } else if (ReadDroppedPaths(data, paths)) paths_ = paths;
    Choose(h);
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
        kinds_.clear();
        segs_.clear();
        uint32_t t = ++token_;
        // Drop the renderer's memory once nothing has been on screen for a moment.
        After(1.5, [this, t] { if (t == token_ && !shown_) GfxTrim(); });
        return;
    }
    ID2D1DCRenderTarget* rt = surf_.Begin();
    if (!rt) return;
    const Theme& th = CurrentTheme();
    float s = float(scale_), k = float(open_.value);
    float c = float(sizeDip_ / 2);
    rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F(c, c)) * D2D1::Matrix3x2F::Scale(s, s));

    bool twoRings = segs_.size() > size_t(kPerRing);
    float R = float(twoRings ? kRing1Out : kRing0Out);
    surf_.DrawShadow(RectF(c - R, c - R, 2 * R, 2 * R), R, 20, 0, 6, th.shadow);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), R, R), surf_.Solid(th.glass));
    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), R - 0.5f, R - 0.5f), surf_.Solid(th.edge), 1.0f);   // under the segments: a hovered one reaches past it

    for (size_t i = 0; i < segs_.size(); i++) {
        const auto& sg = segs_[i];
        double rIn = sg.ring == 0 ? kRing0In : kRing1In, rOut = sg.ring == 0 ? kRing0Out : kRing1Out;
        double hv = std::clamp(sg.hover->value, 0.0, 1.2);
        double pulse = int(i) == chosen_ ? pulse_.value : 0;
        if (hv > 0.01) {
            auto geo = Sector(c, c, rIn + 2, rOut + kGrow * hv + 4 * pulse, sg.a0 + 0.012, sg.a1 - 0.012);
            if (geo) rt->FillGeometry(geo.Get(), surf_.Solid(WithAlpha(th.accent, std::min(1.0, hv))));
        }
        if (segs_.size() > 1) rt->DrawLine(Polar(c, c, rIn + 4, sg.a0), Polar(c, c, rOut - 4, sg.a0), surf_.Solid(th.sep), 1.0f);

        // Icon above the label for tools and actions; formats are their own label, set a little larger.
        bool icon = sg.opt.icon != Icon::None;
        float size = icon ? (sg.ring == 0 ? 11.5f : 10.5f) : (sg.ring == 0 ? 14.0f : 12.5f);
        DWRITE_FONT_WEIGHT wt = icon ? DWRITE_FONT_WEIGHT_NORMAL : DWRITE_FONT_WEIGHT_SEMI_BOLD;
        D2D1_POINT_2F mid = Polar(c, c, (rIn + rOut) / 2 + kGrow * hv / 2, (sg.a0 + sg.a1) / 2);
        uint32_t col = hv > 0.5 ? th.accentText : th.text;
        float maxW = float(sg.ring == 0 ? 78 : 66);
        wstring label = Ellipsize(surf_, sg.opt.label, size, wt, maxW);
        float w = surf_.MeasureText(label.c_str(), size, wt);
        if (icon) {
            float is = sg.ring == 0 ? 20.0f : 17.0f;
            DrawIcon(surf_, sg.opt.icon, D2D1::Point2F(mid.x, mid.y - is * 0.45f), is, col);
            surf_.DrawText(label.c_str(), size, wt, D2D1::RectF(mid.x - w / 2, mid.y + is * 0.3f, mid.x + w / 2 + 2, mid.y + is * 0.3f + size * 1.5f), col);
        } else {
            surf_.DrawText(label.c_str(), size, wt, D2D1::RectF(mid.x - w / 2, mid.y - size * 0.68f, mid.x + w / 2 + 2, mid.y + size), col);
        }
    }
    if (twoRings) rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), float(kRing0Out + 3), float(kRing0Out + 3)), surf_.Solid(th.sep), 1.0f);

    // The tabs: every wheel by name, so nobody has to guess what Space or Tab does next.
    float tw[3], total = 0;
    for (int i = 0; i < 3; i++) { tw[i] = surf_.MeasureText(WheelName(WheelKind(i)), 12, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 22; total += tw[i]; }
    total += 2 * 6;
    float x = c - total / 2, ty = c - R - float(kTabGap + kTabH);
    for (int i = 0; i < 3; i++) {
        tabs_[i] = D2D1::RectF(x - c, ty - c, x + tw[i] - c, ty + float(kTabH) - c);   // relative to the centre, for HitTest
        D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(D2D1::RectF(x, ty, x + tw[i], ty + float(kTabH)), float(kTabH / 2), float(kTabH / 2));
        bool on = i == int(kind_), hover = hot_ == kTabHit - i;
        uint32_t ink = on ? th.accentText : avail_[i] ? th.text : WithAlpha(th.dim, 0.5);
        if (on) rt->FillRoundedRectangle(pill, surf_.Solid(th.accent));
        else {
            rt->FillRoundedRectangle(pill, surf_.Solid(hover ? WithAlpha(th.accent, 0.3) : th.glass));
            rt->DrawRoundedRectangle(pill, surf_.Solid(th.edge), 1.0f);
        }
        const wchar_t* name = WheelName(WheelKind(i));
        surf_.DrawText(name, 12, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(x + 11, ty + 4, x + tw[i], ty + float(kTabH)), ink);
        x += tw[i] + 6;
    }

    // The centre: what is being dragged, which wheel, and Cancel.
    bool cancelHot = hot_ == -1;
    float cr = float(kCentre);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), cr, cr), surf_.Solid(cancelHot ? th.hubHot : th.hub));
    if (cancelHot) {
        float tw = surf_.MeasureText(L"Cancel", 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        surf_.DrawText(L"Cancel", 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(c - tw / 2, c - 8.5f, c + tw / 2 + 2, c + 12), th.text);
    } else {
        wstring top = Ellipsize(surf_, CentreText(), 12, DWRITE_FONT_WEIGHT_SEMI_BOLD, 84);
        float tw = surf_.MeasureText(top.c_str(), 12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        surf_.DrawText(top.c_str(), 12, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(c - tw / 2, c - 17, c + tw / 2 + 2, c + 2), th.text);
        // How to reach the next wheel: Space while dragging, Tab from the keyboard.
        const wchar_t* hint = keyboard_ ? L"Tab: next" : L"Space: next";
        float bw = surf_.MeasureText(hint, 11, DWRITE_FONT_WEIGHT_NORMAL);
        surf_.DrawText(hint, 11, DWRITE_FONT_WEIGHT_NORMAL, D2D1::RectF(c - bw / 2, c + 1, c + bw / 2 + 2, c + 16), th.dim);
    }

    surf_.End();
    Present();
    if (keySeenAt_ >= 0) {
        Log(L"wheel: first frame on screen %.1f ms after the key was seen", (Now() - keySeenAt_) * 1000);
        keySeenAt_ = -1;
    }
}

}  // namespace rd
