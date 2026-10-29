#include "cards.h"

#include "ui.h"

namespace rd {

namespace {

// DIPs.
constexpr double kW = 330, kH = 74, kGap = 8, kMargin = 16, kPad = 26;   // kPad: room for the shadow
constexpr int kMaxCards = 6;
constexpr double kDoneFor = 6.0, kFailedFor = 9.0, kNoteFor = 2.6;

double WindowW() { return kW + 2 * kPad; }
double WindowH() { return kMaxCards * (kH + kGap) + 2 * kPad; }

}  // namespace

Cards::Cards() {}

Cards::~Cards()
{
    if (hwnd_) DestroyWindow(hwnd_);
}

bool Cards::Create(HINSTANCE inst)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"RodaCards";
    RegisterClassW(&wc);
    // Layered with per-pixel alpha: the transparent parts let clicks through to whatever is below.
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"Roda",
                            WS_POPUP, -10000, -10000, 1, 1, nullptr, nullptr, inst, this);
    return hwnd_ != nullptr;
}

LRESULT CALLBACK Cards::WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<Cards*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self && self->hwnd_ == h) return self->Handle(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT Cards::Handle(UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_MOUSEMOVE: {
            if (!tracking_) {
                TRACKMOUSEEVENT t{ sizeof t, TME_LEAVE, hwnd_, 0 };
                tracking_ = TrackMouseEvent(&t) != FALSE;
            }
            int idx = -1;
            Hit hit = HitTest({ GET_X_LPARAM(l), GET_Y_LPARAM(l) }, &idx);
            if (hit != hoverHit_ || idx != hoverIndex_) { hoverHit_ = hit; hoverIndex_ = idx; dirty_ = true; RequestFrame(); }
            return 0;
        }
        case WM_MOUSELEAVE: {
            tracking_ = false;
            hoverHit_ = Hit::None;
            hoverIndex_ = -1;
            // Fading was paused while the pointer was on the cards: give the finished ones a moment more.
            for (auto& c : cards_) if (c.dieAt > 0) Arm(c, 2.0);
            dirty_ = true;
            RequestFrame();
            return 0;
        }
        case WM_SETCURSOR: {
            bool link = hoverHit_ == Hit::Show || hoverHit_ == Hit::Open || hoverHit_ == Hit::Cancel || hoverHit_ == Hit::Close ||
                        (hoverHit_ == Hit::Body && hoverIndex_ >= 0 && !cards_[size_t(hoverIndex_)].out.empty());
            SetCursor(LoadCursorW(nullptr, link ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        case WM_LBUTTONUP: {
            int idx = -1;
            Hit hit = HitTest({ GET_X_LPARAM(l), GET_Y_LPARAM(l) }, &idx);
            Click(hit, idx);
            return 0;
        }
        case WM_DPICHANGED: return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

Cards::Card* Cards::Find(int id)
{
    for (auto& c : cards_) if (c.id == id && !c.leaving) return &c;
    return nullptr;
}

void Cards::Arm(Card& c, double seconds)
{
    c.dieAt = Now() + seconds;
    uint32_t t = token_;
    After(seconds + 0.02, [this, t] { if (t == token_) { dirty_ = true; RequestFrame(); } });
}

void Cards::Update(const JobState& s)
{
    Card* c = Find(s.id);
    if (!c) {
        if (s.phase == JobState::Cancelled) return;
        Card n;
        n.id = s.id;
        n.y = std::make_unique<Anim>();
        n.fade = std::make_unique<Anim>();
        n.bar = std::make_unique<Anim>();
        n.y->Set(-1);    // placed by Layout
        if (ReducedMotion()) n.fade->Set(1); else { n.fade->Set(0); n.fade->Ease(1, 0.18, Curve::EaseOut); }
        cards_.push_back(std::move(n));
        c = &cards_.back();
        if (cards_.size() == 1 || !IsWindowVisible(hwnd_)) {
            // A new stack: on the screen where the files were dropped.
            scale_ = ScaleAt(anchor_);
            surf_.SetScale(scale_);
            Mon mon = MonitorOf(anchor_);
            int w = int(std::lround(WindowW() * scale_)), h = int(std::lround(WindowH() * scale_));
            int x = mon.work.right - w - int(std::lround((kMargin - kPad) * scale_));
            int y = mon.work.bottom - h - int(std::lround((kMargin - kPad) * scale_));
            frame_ = { x, y, x + w, y + h };
            surf_.Resize(w, h);
            SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
        }
    }
    c->title = s.title;
    c->what = s.what;
    c->phase = s.phase;
    c->progress = s.progress;
    if (ReducedMotion()) c->bar->Set(s.progress); else c->bar->Ease(s.progress, 0.25, Curve::EaseOut);
    switch (s.phase) {
        case JobState::Queued: c->text = L"Waiting…"; break;
        case JobState::Running: c->text = s.what + L"…"; break;
        case JobState::Done: {
            c->out = s.out;
            c->text = L"Saved as " + FileName(s.out);
            if (s.before && s.after && s.before != s.after) c->text += L" · " + HumanSize(s.before) + L" → " + HumanSize(s.after);
            Arm(*c, kDoneFor);
            break;
        }
        case JobState::Failed: c->text = s.error; Arm(*c, kFailedFor); break;
        case JobState::Cancelled: c->leaving = true; c->fade->Ease(0, 0.15, Curve::EaseIn); break;
    }
    Layout();
}

void Cards::Note(const wstring& title, const wstring& text, bool failed)
{
    JobState s;
    s.id = nextNote_--;
    s.title = title;
    s.phase = failed ? JobState::Failed : JobState::Done;
    s.error = text;
    Update(s);
    if (Card* c = Find(s.id)) {
        c->text = text;
        c->out.clear();
        Arm(*c, failed ? kFailedFor : kNoteFor);
    }
}

void Cards::Layout()
{
    // Newest at the bottom; old ones beyond the limit leave.
    int visible = 0;
    for (auto it = cards_.rbegin(); it != cards_.rend(); ++it) {
        if (it->leaving) continue;
        if (visible >= kMaxCards) { it->leaving = true; it->fade->Ease(0, 0.15, Curve::EaseIn); continue; }
        double target = WindowH() - kPad - (visible + 1) * kH - visible * kGap;
        if (it->y->value < 0 || ReducedMotion()) it->y->Set(target + (it->y->value < 0 && !ReducedMotion() ? 18 : 0));
        if (!ReducedMotion()) it->y->Spring(target, 0.3, 0.86);
        visible++;
    }
    dirty_ = true;
    RequestFrame();
}

void Cards::Expire()
{
    double now = Now();
    bool hovering = hoverIndex_ >= 0;
    bool changed = false;
    for (auto& c : cards_) {
        if (!c.leaving && c.dieAt > 0 && now >= c.dieAt && !hovering) {
            c.leaving = true;
            if (ReducedMotion()) c.fade->Set(0); else c.fade->Ease(0, 0.3, Curve::EaseIn);
            changed = true;
        }
    }
    size_t before = cards_.size();
    cards_.erase(std::remove_if(cards_.begin(), cards_.end(), [](const Card& c) { return c.leaving && !c.fade->Active() && c.fade->value <= 0.01; }),
                 cards_.end());
    if (changed || cards_.size() != before) Layout();
}

Cards::Hit Cards::HitTest(POINT p, int* index) const
{
    double x = p.x / scale_, y = p.y / scale_;
    for (size_t i = 0; i < cards_.size(); i++) {
        const Card& c = cards_[i];
        if (c.leaving) continue;
        double top = c.y->value, left = kPad;
        if (x < left || x > left + kW || y < top || y > top + kH) continue;
        *index = int(i);
        double cx = x - left, cy = y - top;
        if (cx > kW - 34 && cy < 30) return c.phase == JobState::Running || c.phase == JobState::Queued ? Hit::Cancel : Hit::Close;
        if (c.phase == JobState::Done && !c.out.empty() && cy > 48) {
            if (cx >= 64 && cx < 104) return Hit::Open;
            if (cx >= 112 && cx < 214) return Hit::Show;
        }
        return Hit::Body;
    }
    *index = -1;
    return Hit::None;
}

void Cards::Click(Hit hit, int index)
{
    if (index < 0 || index >= int(cards_.size())) return;
    Card& c = cards_[size_t(index)];
    switch (hit) {
        case Hit::Cancel: if (onCancel && c.id > 0) onCancel(c.id); break;
        case Hit::Close: c.dieAt = Now(); c.leaving = true; c.fade->Ease(0, 0.15, Curve::EaseIn); Layout(); break;
        case Hit::Show: {
            // Explorer opens with the result selected.
            PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(c.out.c_str());
            if (pidl) { SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0); ILFree(pidl); }
            break;
        }
        case Hit::Open: case Hit::Body:
            if (!c.out.empty()) ShellExecuteW(nullptr, L"open", c.out.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        default: break;
    }
}

void Cards::Draw()
{
    dirty_ = false;
    Expire();
    if (cards_.empty()) {
        if (IsWindowVisible(hwnd_)) {
            ShowWindow(hwnd_, SW_HIDE);
            surf_.Release();
            token_++;
            After(1.5, [] { GfxTrim(); });   // only acts when nothing at all is on screen
        }
        return;
    }
    if (!surf_.ready()) surf_.Resize(frame_.right - frame_.left, frame_.bottom - frame_.top);
    ID2D1DCRenderTarget* rt = surf_.Begin();
    if (!rt) return;
    const Theme& th = CurrentTheme();
    for (size_t i = 0; i < cards_.size(); i++) {
        const Card& c = cards_[i];
        float f = float(std::clamp(c.fade->value, 0.0, 1.0));
        if (f <= 0.01) continue;
        float x = float(kPad), y = float(c.y->value), w = float(kW), h = float(kH);
        bool hover = int(i) == hoverIndex_;
        surf_.DrawShadow(RectF(x, y, w, h), 14, 16, 0, 5, th.shadow * f * 0.8f);
        rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(x, y, w, h), 14, 14), surf_.Solid(WithAlpha(th.glass, f)));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(RectF(x + 0.5, y + 0.5, w - 1, h - 1), 13.5f, 13.5f), surf_.Solid(WithAlpha(th.edge, f)), 1);

        // The badge: what was done, or a cross for a failure.
        bool failed = c.phase == JobState::Failed;
        uint32_t badge = failed ? th.error : th.accent;
        rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(x + 14, y + 16, 40, 40), 10, 10), surf_.Solid(WithAlpha(badge, f * 0.14)));
        wstring tag = failed ? L"!" : c.what.size() <= 5 ? c.what : c.what.substr(0, 1);
        float ts = tag.size() > 3 ? 10.5f : 12.5f;
        float tw = surf_.MeasureText(tag.c_str(), ts, DWRITE_FONT_WEIGHT_BOLD);
        surf_.DrawText(tag.c_str(), ts, DWRITE_FONT_WEIGHT_BOLD, RectF(x + 34 - tw / 2, y + 36 - ts * 0.68, tw + 4, ts * 1.6), WithAlpha(badge, f));

        // Title and status line.
        float textX = x + 64, textW = w - 64 - 38;
        wstring title = Ellipsize(surf_, c.title, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, textW);
        surf_.DrawText(title.c_str(), 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(textX, y + 12, textW + 4, 20), WithAlpha(th.text, f));
        if (failed) {
            surf_.DrawText(c.text.c_str(), 11.5f, DWRITE_FONT_WEIGHT_NORMAL, RectF(textX, y + 32, textW + 26, 36), WithAlpha(th.error, f));
        } else {
            wstring line = Ellipsize(surf_, c.text, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, textW + 26);
            surf_.DrawText(line.c_str(), 11.5f, DWRITE_FONT_WEIGHT_NORMAL, RectF(textX, y + 31, textW + 30, 18), WithAlpha(th.dim, f));
        }
        if (c.phase == JobState::Running || c.phase == JobState::Queued) {
            float bw = w - 64 - 18;
            rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(textX, y + 54, bw, 4), 2, 2), surf_.Solid(WithAlpha(th.sep, f)));
            float p = float(std::clamp(c.bar->value, 0.0, 1.0));
            if (p > 0.005) rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(textX, y + 54, std::max(4.0f, bw * p), 4), 2, 2), surf_.Solid(WithAlpha(th.accent, f)));
        }
        if (c.phase == JobState::Done && !c.out.empty()) {
            uint32_t openCol = hover && hoverHit_ == Hit::Open ? th.text : th.accent;
            uint32_t showCol = hover && hoverHit_ == Hit::Show ? th.text : th.accent;
            surf_.DrawText(L"Open", 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(textX, y + 51, 40, 18), WithAlpha(openCol, f));
            surf_.DrawText(L"Show in folder", 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(textX + 48, y + 51, 110, 18), WithAlpha(showCol, f));
        }
        // Cancel while working; close on hover otherwise.
        bool working = c.phase == JobState::Running || c.phase == JobState::Queued;
        if (working || hover) {
            bool hot = hover && (hoverHit_ == Hit::Cancel || hoverHit_ == Hit::Close);
            D2D1_POINT_2F cc = D2D1::Point2F(x + w - 20, y + 18);
            if (hot) rt->FillEllipse(D2D1::Ellipse(cc, 11, 11), surf_.Solid(WithAlpha(th.button, f)));
            uint32_t col = WithAlpha(hot ? th.text : th.dim, f);
            rt->DrawLine(D2D1::Point2F(cc.x - 4, cc.y - 4), D2D1::Point2F(cc.x + 4, cc.y + 4), surf_.Solid(col), 1.4f);
            rt->DrawLine(D2D1::Point2F(cc.x - 4, cc.y + 4), D2D1::Point2F(cc.x + 4, cc.y - 4), surf_.Solid(col), 1.4f);
        }
    }
    surf_.End();
    surf_.Present(hwnd_, frame_.left, frame_.top);
    if (!IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    for (auto& c : cards_) if (c.y->Active() || c.fade->Active() || c.bar->Active()) { dirty_ = true; break; }
}

}  // namespace rd
