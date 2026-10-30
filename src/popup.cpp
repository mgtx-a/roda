#include "popup.h"

#include "dragdrop.h"
#include "ui.h"

namespace rd {

namespace {

constexpr double kPad = 24;            // room for the shadow around the card
constexpr double kHandle = 9;          // crop handles: how close counts
constexpr double kBtnW = 92, kBtnH = 30;
constexpr double kListTop = 50;        // Merge: where the list of files starts, below the title
constexpr double kAddW = 100;          // Merge: the Add files… button
constexpr double kDropH = 30;          // Merge: the place below the rows to drop more on
constexpr double kStripTop = 50;       // Remove pages: the strip of pages, below the title
constexpr double kThumbH = 168, kGap = 14, kNumH = 24, kBarH = 14;   // a page's height, between pages, its label, the scroll bar
const wchar_t* kAspects[] = { L"Free", L"1:1", L"4:3", L"16:9" };
const double kRatios[] = { 0, 1.0, 4.0 / 3.0, 16.0 / 9.0 };

bool In(double x, double y, double l, double t, double w, double h) { return x >= l && x < l + w && y >= t && y < t + h; }

}  // namespace

/// Merge's basket: files dragged in from Explorer join the list. Copy is all it ever answers, so the
/// files stay where they are.
class PopupDropTarget : public DropTarget {
public:
    OptionsPopup* popup = nullptr;
    STDMETHODIMP DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override
    {
        ok_ = popup && popup->shown_ && popup->kind_ == Popup::Order && HasFilesOnDisk(data);
        if (ok_) { popup->dropHover_ = true; popup->dirty_ = true; RequestFrame(); }
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
        if (popup) { popup->dropHover_ = false; popup->dirty_ = true; RequestFrame(); }
        return S_OK;
    }
    STDMETHODIMP Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override
    {
        std::vector<wstring> paths;
        if (ok_ && popup && ReadDroppedPaths(data, paths)) popup->AddFiles(paths);
        DragLeave();
        *effect = ok_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
private:
    bool ok_ = false;
};

OptionsPopup::OptionsPopup() {}
OptionsPopup::~OptionsPopup()
{
    if (hwnd_) { RevokeDragDrop(hwnd_); DestroyWindow(hwnd_); }
    if (drop_) { drop_->popup = nullptr; drop_->Release(); }
}

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
    if (!hwnd_) return false;
    drop_ = new PopupDropTarget();
    drop_->popup = this;
    if (FAILED(RegisterDragDrop(hwnd_, drop_))) Log(L"popup: could not take dropped files");
    return true;
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
    whereH_ = where_.empty() ? 0 : 22;
    hoverPreset_ = -1;
    presetSel_ = kind == Popup::Size && !sizeFields_ ? 0 : -1;   // only sizes: the first has the keys
    // The main button grows to fit its words ("Resize 3 pictures").
    applyW_ = std::max(kBtnW, double(surf_.MeasureText(apply_.empty() ? L"Compress" : apply_.c_str(), 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 28);
    if (kind != Popup::Size) sizeFields_ = true;
    if (kind == Popup::Crop) {
        pic_ = LoadMips(preview, 2048);
        DeleteFileW(preview.c_str());   // loaded; the temp file isn't needed any more
        if (pic_.empty() || origW <= 0 || origH <= 0) { Log(L"popup: no preview to crop on"); return; }
        double k = std::min(560.0 / origW, 420.0 / origH);
        double pw = std::max(160.0, origW * k), ph = std::max(120.0, origH * k);
        k = std::min(pw / origW, ph / origH);
        w_ = origW * k + 40;
        h_ = origH * k + 46 + 12 + 30 + 18 + whereH_;
        w_ = std::max(w_, 380.0);
    } else if (kind == Popup::Order) {
        w_ = 420;
        whereH_ = 22;   // the line where the result goes, kept even while the basket is empty
        h_ = kListTop + OrderListH() + 10 + kBtnH + 16 + whereH_;
    } else if (kind == Popup::Pages) {
        // About five pages across; the button's words grow with the count ("Remove 12 pages").
        w_ = 640;
        h_ = kStripTop + kThumbH + kNumH + kBarH + 10 + whereH_ + kBtnH + 16;
        applyW_ = std::max(applyW_, double(surf_.MeasureText(L"Remove 999 pages", 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 28);
    } else if (kind == Popup::Size && !sizeFields_) {
        w_ = 320;
        h_ = 46 + 30 + 36 + kBtnH + 16 + whereH_;   // room for the keys' hint under the sizes
    } else {
        w_ = 320;
        h_ = 172 + (kind == Popup::Size ? FieldsTop() : 0) + whereH_;
    }
    w_ = std::max(w_, applyW_ + kBtnW + 48 + (kind == Popup::Order ? kAddW + 8 : 0));   // Merge: Add files… on the left
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

void OptionsPopup::SetPresets(std::vector<Preset> presets, bool fields)
{
    presets_ = std::move(presets);
    sizeFields_ = fields || presets_.empty();
}

void OptionsPopup::SetSummary(const wstring& apply, const wstring& where)
{
    apply_ = apply;
    where_ = where;
}

double OptionsPopup::CancelX() const
{
    return w_ - 20 - kBtnW - (sizeFields_ ? applyW_ + 8 : 0);
}

void OptionsPopup::ShowOrder(const std::vector<wstring>& paths, POINT at)
{
    // shortcut: no scrolling; past about 16 files the rows get cramped. Add a scrolling list when merges that big turn up.
    items_.clear();
    orderNote_.clear();
    selected_ = 0;
    hoverRow_ = -1;
    dropHover_ = false;
    for (auto& p : paths) if (std::find(items_.begin(), items_.end(), p) == items_.end()) items_.push_back(p);
    Show(Popup::Order, L"Merge into one PDF", at, 0);
}

double OptionsPopup::OrderListH() const
{
    // Empty: a big place to drop files on. Else the rows, and a slim one below for more.
    return items_.empty() ? 3 * 34.0 : items_.size() * RowH() + 4 + kDropH;
}

void OptionsPopup::Relayout()
{
    h_ = kListTop + OrderListH() + 10 + kBtnH + 16 + whereH_;
    int pw = int(std::lround((w_ + 2 * kPad) * scale_)), ph = int(std::lround((h_ + 2 * kPad) * scale_));
    // It grows downwards from where it is, moving up only as far as the screen's bottom needs.
    Mon mon = MonitorOf({ frame_.left + pw / 2, frame_.top + kPad });
    int y = std::max<int>(mon.work.top, std::min<int>(frame_.top, mon.work.bottom - ph));
    frame_ = { frame_.left, y, frame_.left + pw, y + ph };
    surf_.Resize(pw, ph);
    SetWindowPos(hwnd_, nullptr, frame_.left, y, pw, ph, SWP_NOZORDER | SWP_NOACTIVATE);
    dirty_ = true;
    RequestFrame();
}

void OptionsPopup::RemoveRow(int row)
{
    if (row < 0 || row >= int(items_.size())) return;
    items_.erase(items_.begin() + row);
    selected_ = std::clamp(selected_, 0, std::max(0, int(items_.size()) - 1));
    hoverRow_ = -1;
    orderNote_.clear();
    Relayout();
}

void OptionsPopup::AddFiles(const std::vector<wstring>& paths)
{
    if (!shown_ || kind_ != Popup::Order) return;
    size_t before = items_.size(), skipped = 0;
    for (auto& p : paths) {
        if ((Bit(KindOfPath(p)) & (kImages | Bit(Kind::Pdf))) == 0) { skipped++; continue; }   // only these merge
        if (std::find(items_.begin(), items_.end(), p) == items_.end()) items_.push_back(p);
    }
    orderNote_ = skipped ? (skipped == 1 ? L"1 file left out: only PDFs and pictures merge" : std::to_wstring(skipped) + L" files left out: only PDFs and pictures merge") : L"";
    if (items_.size() > before) selected_ = int(items_.size()) - 1;
    Log(L"popup: merge basket has %d file(s)", int(items_.size()));
    Relayout();
}

void OptionsPopup::ShowPages(const wstring& title, POINT at)
{
    token_++;
    pages_ = 0;
    thumbs_.clear();
    asked_.clear();
    removed_.clear();
    scroll_ = 0;
    pageAspect_ = 0.707;
    focusPage_ = 0;
    hoverPage_ = pressPage_ = -1;
    pageKeys_ = false;
    Show(Popup::Pages, title, at, 0);
    asked_.insert(0);
    if (onNeedPage) onNeedPage(token_, 0);   // its answer also says how many pages there are
}

void OptionsPopup::SetPage(int token, int page, const wstring& png, int pages)
{
    if (token != token_ || !shown_ || kind_ != Popup::Pages) { DeleteFileW(png.c_str()); return; }
    Mips m = LoadMips(png, 1024);
    DeleteFileW(png.c_str());   // loaded; the temp file isn't needed any more
    if (pages_ == 0 && pages > 0) {
        pages_ = pages;
        if (!m.empty()) pageAspect_ = std::clamp(double(m.width()) / std::max(1, m.height()), 0.4, 1.8);
    }
    if (!m.empty()) thumbs_[page] = std::move(m);
    AskVisible();
    dirty_ = true;
    RequestFrame();
}

double OptionsPopup::PageW() const { return kThumbH * pageAspect_; }

double OptionsPopup::MaxScroll() const { return std::max(0.0, pages_ * (PageW() + kGap) - kGap - (w_ - 40)); }

void OptionsPopup::ScrollBy(double dx)
{
    // From where a glide is heading, so quick wheel turns add up instead of starting over.
    ScrollTo((glide_.Active() ? glide_.Target() : scroll_) + dx);
}

void OptionsPopup::ScrollTo(double x)
{
    if (!glide_.Active()) glide_.Set(scroll_);
    double to = std::clamp(x, 0.0, MaxScroll());
    if (ReducedMotion()) { glide_.Set(to); scroll_ = to; }
    else glide_.Spring(to, 0.28, 1.0);   // no bounce: it settles on the spot
    AskVisible();
    dirty_ = true;
    RequestFrame();
}

void OptionsPopup::ShowPage(int page)
{
    double left = page * (PageW() + kGap), view = w_ - 40, at = glide_.Active() ? glide_.Target() : scroll_;
    if (left < at) ScrollTo(left);
    else if (left + PageW() > at + view) ScrollTo(left + PageW() - view);
}

void OptionsPopup::AskVisible()
{
    if (pages_ <= 0 || !onNeedPage) return;
    // The pages in view and a few past either side, so a scroll finds them drawn.
    double slot = PageW() + kGap;
    int first = std::max(0, int(scroll_ / slot) - 2), last = std::min(pages_ - 1, int((scroll_ + w_ - 40) / slot) + 3);
    for (int p = first; p <= last; p++)
        if (asked_.insert(p).second) onNeedPage(token_, p);
}

void OptionsPopup::TogglePage(int page)
{
    if (page < 0 || page >= pages_) return;
    if (!removed_.erase(page)) removed_.insert(page);
    dirty_ = true;
    RequestFrame();
}

/// Merge: the picked row goes to row `to`, the rows between shift up or down.
void OptionsPopup::MoveRow(int to)
{
    to = std::clamp(to, 0, int(items_.size()) - 1);
    if (to == selected_ || selected_ < 0 || selected_ >= int(items_.size())) return;
    wstring moving = items_[size_t(selected_)];
    items_.erase(items_.begin() + selected_);
    items_.insert(items_.begin() + to, moving);
    selected_ = to;
}

void OptionsPopup::Hide()
{
    if (!shown_) return;
    shown_ = false;
    presets_.clear();
    sizeFields_ = true;
    apply_.clear();
    where_.clear();
    whereFor = nullptr;
    ShowWindow(hwnd_, SW_HIDE);
    surf_.Release();
    pic_ = {};
    thumbs_.clear();   // the pages' pictures; answers still on their way are dropped (the token moves on)
    token_++;
}

D2D1_RECT_F OptionsPopup::PictureRect() const
{
    double k = std::min((w_ - 40) / origW_, (h_ - 46 - 12 - 30 - 18 - whereH_) / origH_);
    double pw = origW_ * k, ph = origH_ * k;
    return RectF(kPad + (w_ - pw) / 2, kPad + 46, pw, ph);
}

OptionsPopup::Part OptionsPopup::HitTest(double x, double y, int* aspect) const
{
    double L = kPad, T = kPad;
    double by = T + h_ - 16 - kBtnH;
    if (sizeFields_ && In(x, y, L + w_ - 20 - applyW_, by, applyW_, kBtnH)) return Part::Apply;
    if (In(x, y, L + CancelX(), by, kBtnW, kBtnH)) return Part::Cancel;
    for (size_t i = 0; i < presetX_.size() && kind_ == Popup::Size; i++)
        if (In(x, y, L + presetX_[i].first, T + 46, presetX_[i].second, 30)) { *aspect = int(i); return Part::Preset; }
    if (kind_ == Popup::Quality && In(x, y, L + 12, T + 60, w_ - 24, 30)) return Part::Slider;
    if (kind_ == Popup::Pages && pages_ > 0) {
        double sx = L + 20, sw = w_ - 40, sy = T + kStripTop;
        if (MaxScroll() > 0 && In(x, y, sx, sy + kThumbH + kNumH, sw, kBarH)) return Part::ScrollBar;
        if (In(x, y, sx, sy, sw, kThumbH + kNumH)) {
            double cx = x - sx + scroll_, slot = PageW() + kGap;
            int i = int(cx / slot);
            if (i < pages_ && cx - i * slot < PageW()) { *aspect = i; return Part::Page; }
            return Part::Strip;   // between pages: still drags the strip
        }
    }
    if (kind_ == Popup::Order) {
        if (In(x, y, L + 20, by, kAddW, kBtnH)) return Part::AddFiles;
        if (In(x, y, L + 20, T + kListTop, w_ - 40, items_.size() * RowH())) {
            *aspect = int((y - T - kListTop) / RowH());
            // The cross left of the grip takes the file out; the rest of the row drags it.
            return In(x, y, L + w_ - 20 - 62, T + kListTop, 26, items_.size() * RowH()) ? Part::RowRemove : Part::Row;
        }
    }
    if ((kind_ == Popup::Size && sizeFields_) || kind_ == Popup::Trim) {
        double fy = kind_ == Popup::Size ? FieldsTop() : 0;
        if (In(x, y, L + 20, T + 64 + fy, 124, 34)) return Part::Width;
        if (In(x, y, L + w_ - 20 - 124, T + 64 + fy, 124, 34)) return Part::Height;
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
    if (drag_ == Part::Row) {   // the row follows the pointer; the others make way
        MoveRow(int(std::floor((y - kPad - kListTop) / RowH())));
        return;
    }
    if (drag_ == Part::Strip) {   // the pages follow the pointer, once it has really moved (else it's a click)
        if (std::fabs(x - dragX_) > 4) moved_ = true;
        if (moved_) {
            double now = Now(), dt = now - lastT_;
            if (dt > 0.004) { flingV_ = flingV_ * 0.4 + (x - lastX_) / dt * 0.6; lastX_ = x; lastT_ = now; }   // smoothed speed
            scroll_ = std::clamp(startX_ - (x - dragX_), 0.0, MaxScroll());
            glide_.Set(scroll_);
            AskVisible();
        }
        return;
    }
    if (drag_ == Part::ScrollBar) {   // the bar's thumb centres on the pointer
        double view = w_ - 40, content = MaxScroll() + view;
        scroll_ = std::clamp((x - kPad - 20) / view * content - view / 2, 0.0, MaxScroll());
        glide_.Set(scroll_);
        AskVisible();
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

/// A size from the row: done in one go, by that size's own tool.
void OptionsPopup::ApplyPreset(int i)
{
    Json opts = Json::Object().Set(L"op", presets_[size_t(i)].op);
    auto fn = onApply;
    Hide();
    if (fn) fn(opts);
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
    } else if (kind_ == Popup::Pages) {
        // The pages clicked away, as page numbers ("2, 5-7"); the worker checks them against the PDF again.
        if (removed_.empty() || int(removed_.size()) >= pages_) { MessageBeep(MB_OK); return; }
        wstring spec;
        for (auto it = removed_.begin(); it != removed_.end();) {
            int a = *it, b = a;
            while (++it != removed_.end() && *it == b + 1) b = *it;
            spec += (spec.empty() ? L"" : L", ") + std::to_wstring(a + 1) + (b > a ? L"-" + std::to_wstring(b + 1) : L"");
        }
        opts.Set(L"pages", spec);
    } else if (kind_ == Popup::Crop) {
        int x = int(std::lround(cx_ * origW_)), y = int(std::lround(cy_ * origH_));
        int w = std::max(1, int(std::lround(cw_ * origW_))), h = std::max(1, int(std::lround(ch_ * origH_)));
        opts.Set(L"crop", std::to_wstring(x) + L"," + std::to_wstring(y) + L"," + std::to_wstring(w) + L"," + std::to_wstring(h));
    } else if (kind_ == Popup::Order) {
        if (items_.size() < 2) { MessageBeep(MB_OK); return; }   // one file is no merge
        Json files = Json::Array();
        for (auto& p : items_) files.Push(Json(p));
        opts.Set(L"files", files);
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
            if (p == Part::Width || p == Part::Height) { field_ = p == Part::Width ? 0 : 1; presetSel_ = -1; dirty_ = true; RequestFrame(); return 0; }
            if (p == Part::Aspect) { SetAspect(a); dirty_ = true; RequestFrame(); return 0; }
            if (p == Part::Preset) { ApplyPreset(a); return 0; }
            if (p == Part::Page || p == Part::Strip) {
                // A click on a page removes it (or brings it back); a drag scrolls the pages.
                drag_ = Part::Strip;
                glide_.Set(scroll_);   // a press stops a glide where it is, as on a phone
                dragX_ = lastX_ = x;
                lastT_ = Now();
                flingV_ = 0;
                startX_ = scroll_;
                pressPage_ = p == Part::Page ? a : -1;
                moved_ = false;
                SetCapture(hwnd_);
                return 0;
            }
            if (p == Part::RowRemove) { RemoveRow(a); return 0; }
            if (p == Part::AddFiles) return 0;   // on the button's release, like the others
            if (p == Part::Row) selected_ = a;
            if (p == Part::ScrollBar) { drag_ = p; SetCapture(hwnd_); Drag(x, y); return 0; }
            if (p == Part::Slider || p == Part::Row || (p >= Part::CropMove && p <= Part::CropSW)) {
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
            int a = -1;
            Part p = HitTest(x, y, &a);
            int preset = p == Part::Preset ? a : -1, page = p == Part::Page ? a : -1, row = p == Part::Row || p == Part::RowRemove ? a : -1;
            if (p != hover_ || preset != hoverPreset_ || page != hoverPage_ || row != hoverRow_) {
                hover_ = p;
                hoverPreset_ = preset;
                hoverPage_ = page;
                hoverRow_ = row;
                if (page >= 0) pageKeys_ = false;   // the mouse is back: its page is the one marked
                dirty_ = true;
                RequestFrame();
            }
            return 0;
        }
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            if (kind_ == Popup::Pages) {
                // Either wheel scrolls the pages sideways, a page at a step.
                double steps = GET_WHEEL_DELTA_WPARAM(w) / double(WHEEL_DELTA);
                ScrollBy((m == WM_MOUSEWHEEL ? -steps : steps) * (PageW() + kGap));
                return 0;
            }
            break;
        case WM_LBUTTONUP: {
            auto [x, y] = local(l);
            if (drag_ == Part::Strip && !moved_ && pressPage_ >= 0) { TogglePage(pressPage_); focusPage_ = pressPage_; }
            if (drag_ == Part::Strip && moved_ && Now() - lastT_ < 0.08 && std::fabs(flingV_) > 150 && !ReducedMotion()) {
                // Let go while moving: the strip slides on and slows to a stop.
                glide_.Set(scroll_);
                glide_.Ease(std::clamp(scroll_ - flingV_ * 0.32, 0.0, MaxScroll()), 0.55, Curve::EaseOut);
                AskVisible();
            }
            if (drag_ != Part::None) { drag_ = Part::None; ReleaseCapture(); return 0; }
            int a;
            Part p = HitTest(x, y, &a);
            if (p == Part::Apply) Apply();
            else if (p == Part::Cancel) { Log(L"popup: cancelled"); Hide(); }
            else if (p == Part::AddFiles && onAddFiles) onAddFiles();
            return 0;
        }        case WM_SETCURSOR: {
            LPCWSTR c = IDC_ARROW;
            switch (hover_) {
                case Part::CropMove: c = IDC_SIZEALL; break;
                case Part::CropN: case Part::CropS: c = IDC_SIZENS; break;
                case Part::CropE: case Part::CropW: c = IDC_SIZEWE; break;
                case Part::CropNW: case Part::CropSE: c = IDC_SIZENWSE; break;
                case Part::CropNE: case Part::CropSW: c = IDC_SIZENESW; break;
                case Part::Width: case Part::Height: c = IDC_IBEAM; break;
                case Part::Apply: case Part::Cancel: case Part::Aspect: case Part::Slider: case Part::Preset: case Part::Page: c = IDC_HAND; break;
                case Part::ScrollBar: case Part::Strip: c = IDC_SIZEWE; break;
                case Part::Row: c = IDC_HAND; break;   // a hand: take hold of the file and move it
                case Part::RowRemove: case Part::AddFiles: c = IDC_HAND; break;
                default: break;
            }
            SetCursor(LoadCursorW(nullptr, c));
            return TRUE;
        }
        case WM_SYSKEYDOWN:   // Alt+arrows come this way
            if (kind_ != Popup::Order || (w != VK_UP && w != VK_DOWN)) break;
            [[fallthrough]];
        case WM_KEYDOWN: {
            if (w == VK_ESCAPE) { Log(L"popup: cancelled"); Hide(); return 0; }
            if (kind_ == Popup::Order && (w == VK_UP || w == VK_DOWN)) {
                // Arrows pick a row; with Alt or Ctrl they move it.
                int to = selected_ + (w == VK_UP ? -1 : 1);
                if (m == WM_SYSKEYDOWN || (GetKeyState(VK_CONTROL) & 0x8000)) MoveRow(to);
                else selected_ = std::clamp(to, 0, std::max(0, int(items_.size()) - 1));
            }
            if (kind_ == Popup::Order && w == VK_DELETE) RemoveRow(selected_);
            if (kind_ == Popup::Pages && pages_ > 0) {
                // Arrows walk the pages; Delete or Space removes the one they're on (or brings it back).
                int to = w == VK_LEFT ? focusPage_ - 1 : w == VK_RIGHT ? focusPage_ + 1 : w == VK_HOME ? 0 : w == VK_END ? pages_ - 1 : -1;
                if (to >= 0 || w == VK_LEFT) { focusPage_ = std::clamp(to, 0, pages_ - 1); pageKeys_ = true; ShowPage(focusPage_); }
                if (w == VK_DELETE || w == VK_SPACE) { pageKeys_ = true; TogglePage(focusPage_); }
            }
            if (w == VK_RETURN) {
                if (presetSel_ >= 0 && presetSel_ < int(presets_.size())) ApplyPreset(presetSel_);
                else if (sizeFields_) Apply();
                return 0;
            }
            if (kind_ == Popup::Size && !presets_.empty() && w == VK_TAB) {
                // Tab walks the sizes, then the width and height fields (when there are any), then round again.
                int n = int(presets_.size());
                if (presetSel_ >= 0) { presetSel_++; if (presetSel_ >= n) presetSel_ = sizeFields_ ? -1 : 0; if (presetSel_ < 0) field_ = 0; }
                else if (field_ == 0) field_ = 1;
                else presetSel_ = 0;
                dirty_ = true;
                RequestFrame();
                return 0;
            }
            if (kind_ == Popup::Size && presetSel_ >= 0 && (w == VK_LEFT || w == VK_RIGHT)) {
                int n = int(presets_.size());
                presetSel_ = (presetSel_ + (w == VK_RIGHT ? 1 : n - 1)) % n;
            }
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
            if (kind_ == Popup::Size && sizeFields_ && w >= L'0' && w <= L'9') {
                if (presetSel_ >= 0) { presetSel_ = -1; field_ = 0; }   // typing a number means your own size
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
            // Clicking elsewhere cancels, like a menu; but Merge's basket stays while files are picked in Explorer.
            if (LOWORD(w) == WA_INACTIVE && shown_ && drag_ == Part::None && kind_ != Popup::Order) { Log(L"popup: closed (clicked elsewhere)"); Hide(); }
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
    if (kind_ == Popup::Order) {
        // The title counts the files; the line above the buttons says where the PDF goes (the first file
        // decides), or why some dropped files were left out.
        title_ = items_.size() < 2 ? L"Merge into one PDF" : L"Merge " + std::to_wstring(items_.size()) + L" files into one PDF";
        where_ = !orderNote_.empty() ? orderNote_ : items_.empty() || !whereFor ? L"Add at least two files" : whereFor(items_[0]);
    }
    surf_.DrawShadow(RectF(L, T, W, H), 16, 22, 0, 8, th.shadow);
    rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(L, T, W, H), 16, 16), surf_.Solid(th.glass | 0xFF000000));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(RectF(L + 0.5, T + 0.5, W - 1, H - 1), 15.5f, 15.5f), surf_.Solid(th.edge), 1);
    surf_.DrawText(title_.c_str(), 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(L + 20, T + 16, W - 40, 22), th.text);

    auto button = [&](float x, float y, float bw, const wchar_t* label, bool primary, bool hot) {
        uint32_t bg = primary ? th.accent : th.button;
        if (hot) bg = primary ? WithAlpha(th.accent, 0.85) : WithAlpha(th.button, 0.7);
        rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(x, y, bw, kBtnH), 8, 8), surf_.Solid(bg));
        float tw = surf_.MeasureText(label, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        surf_.DrawText(label, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(x + (bw - tw) / 2, y + 6.5, tw + 4, 18), primary ? th.accentText : th.text);
    };
    float by = T + H - 16 - float(kBtnH);
    const wchar_t* verb = kind_ == Popup::Quality ? L"Compress" : kind_ == Popup::Size ? L"Resize" : kind_ == Popup::Trim ? L"Trim" : kind_ == Popup::Pages ? L"Remove" :
                          kind_ == Popup::Order ? L"Merge" : L"Crop";
    wstring applyText = apply_.empty() ? verb : apply_;
    if (kind_ == Popup::Pages)   // the button counts what it will do
        applyText = removed_.empty() ? L"Remove pages" : L"Remove " + std::to_wstring(removed_.size()) + (removed_.size() == 1 ? L" page" : L" pages");
    bool can = kind_ != Popup::Order || items_.size() >= 2;   // Merge needs two files: till then the button rests
    if (sizeFields_) button(L + W - 20 - float(applyW_), by, float(applyW_), applyText.c_str(), can, can && hover_ == Part::Apply);
    button(L + float(CancelX()), by, float(kBtnW), L"Cancel", false, hover_ == Part::Cancel);
    if (!where_.empty()) {
        // Where the result goes, so nobody has to hunt for it afterwards.
        wstring w = Ellipsize(surf_, where_, 11, DWRITE_FONT_WEIGHT_NORMAL, W - 40);
        surf_.DrawText(w.c_str(), 11, DWRITE_FONT_WEIGHT_NORMAL, RectF(L + 20, by - 22, W - 40, 16), kind_ == Popup::Order && !orderNote_.empty() ? th.error : th.dim);
    }
    if (kind_ == Popup::Size && !sizeFields_) {
        surf_.DrawText(L"Tab or arrows to pick, Enter to resize", 11, DWRITE_FONT_WEIGHT_NORMAL, RectF(L + 20, T + 84, W - 40, 16), th.dim);
    }
    if (kind_ == Popup::Size && !presets_.empty()) {
        presetX_.clear();
        float x = 20;
        for (size_t i = 0; i < presets_.size(); i++) {
            float pw = surf_.MeasureText(presets_[i].label.c_str(), 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 28;
            presetX_.push_back({ x, pw });
            bool hot = hoverPreset_ == int(i);
            rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(L + x, T + 46, pw, 30), 8, 8), surf_.Solid(hot ? th.accent : WithAlpha(th.accent, 0.16)));
            if (presetSel_ == int(i))   // the keys' choice: Enter applies it
                rt->DrawRoundedRectangle(D2D1::RoundedRect(RectF(L + x + 0.75f, T + 46.75f, pw - 1.5f, 28.5f), 7.5f, 7.5f), surf_.Solid(th.accent), 2);
            surf_.DrawText(presets_[i].label.c_str(), 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(L + x + 14, T + 52.5f, pw, 18), hot ? th.accentText : th.accent);
            x += pw + 8;
        }
    }

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
    } else if ((kind_ == Popup::Size && sizeFields_) || kind_ == Popup::Trim) {
        bool trim = kind_ == Popup::Trim;
        float Tf = T + (trim ? 0 : float(FieldsTop()));   // below the one-click sizes
        auto field = [&](float x, const wchar_t* caption, const wstring& value, bool active) {
            surf_.DrawText(caption, 11, DWRITE_FONT_WEIGHT_NORMAL, RectF(x, Tf + 46, 124, 16), th.dim);
            rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(x, Tf + 64, 124, 34), 8, 8), surf_.Solid(th.button));
            if (active) rt->DrawRoundedRectangle(D2D1::RoundedRect(RectF(x + 0.75, Tf + 64.75, 122.5, 32.5), 7.5f, 7.5f), surf_.Solid(th.accent), 1.5f);
            wstring shown = value.empty() ? (trim ? (caption[0] == L'S' ? L"0:00" : L"the end") : L"auto") : trim ? value : value + L" px";
            surf_.DrawText(shown.c_str(), 13, DWRITE_FONT_WEIGHT_NORMAL, RectF(x + 12, Tf + 72, 100, 20), value.empty() ? th.dim : th.text);
            if (active) {
                float cw = value.empty() ? 0 : surf_.MeasureText(value.c_str(), 13, DWRITE_FONT_WEIGHT_NORMAL);
                rt->DrawLine(D2D1::Point2F(x + 12 + cw + 1, Tf + 73), D2D1::Point2F(x + 12 + cw + 1, Tf + 89), surf_.Solid(th.text), 1);
            }
        };
        bool typing = presetSel_ < 0;
        field(L + 20, trim ? L"Start" : presets_.empty() ? L"Width" : L"Or your own width", width_, typing && field_ == 0);
        field(L + W - 20 - 124, trim ? L"End" : L"Height", height_, typing && field_ == 1);
        surf_.DrawText(trim ? L"–" : L"×", 14, DWRITE_FONT_WEIGHT_NORMAL, RectF(L + W / 2 - 5, Tf + 71, 12, 20), th.dim);
        surf_.DrawText(trim ? L"Minutes:seconds, like 1:30" : L"Leave one on auto to keep the proportions", 11, DWRITE_FONT_WEIGHT_NORMAL,
                       RectF(L + 20, Tf + 104, W - 40, 16), th.dim);
    } else if (kind_ == Popup::Pages) {
        // The pages side by side: a click removes one (a red cross over it), another click brings it back.
        if (glide_.Active()) {   // on its way: follow it, and keep the frames coming
            scroll_ = std::clamp(glide_.value, 0.0, MaxScroll());
            AskVisible();
            dirty_ = true;
        }
        float sx = L + 20, sw = W - 40, sy = T + float(kStripTop), th_ = float(kThumbH);
        if (pages_ > 0) {
            wstring count = std::to_wstring(pages_) + (pages_ == 1 ? L" page" : L" pages");
            float cw = surf_.MeasureText(count.c_str(), 12, DWRITE_FONT_WEIGHT_NORMAL);
            surf_.DrawText(count.c_str(), 12, DWRITE_FONT_WEIGHT_NORMAL, RectF(L + W - 20 - cw, T + 18, cw + 4, 18), th.dim);
        }
        rt->PushAxisAlignedClip(D2D1::RectF(sx, sy - 3, sx + sw, sy + th_ + float(kNumH)), D2D1_ANTIALIAS_MODE_ALIASED);
        float pw = float(PageW()), slot = pw + float(kGap);
        for (int i = 0; i < std::max(1, pages_); i++) {
            float x = sx + i * slot - float(scroll_);
            if (x + pw < sx || x > sx + sw) continue;
            D2D1_RECT_F box = RectF(x, sy, pw, th_);
            bool removed = removed_.count(i) > 0, hot = hoverPage_ == i, keyed = pageKeys_ && focusPage_ == i;
            auto it = thumbs_.find(i);
            if (it != thumbs_.end()) {
                // The page on white paper, its own shape fitted in the slot.
                const Mips& m = it->second;
                double k = std::min(pw / std::max(1, m.width()), th_ / std::max(1, m.height()));
                float mw = float(m.width() * k), mh = float(m.height() * k);
                D2D1_RECT_F fit = RectF(x + (pw - mw) / 2, sy + (th_ - mh) / 2, mw, mh);
                rt->FillRectangle(fit, surf_.Solid(0xFFFFFFFF));
                surf_.DrawMips(m, fit, removed ? 0.3f : 1.0f);
                rt->DrawRectangle(fit, surf_.Solid(th.edge), 1);
            } else {
                rt->FillRoundedRectangle(D2D1::RoundedRect(box, 4, 4), surf_.Solid(th.button));   // still on its way
            }
            if (removed) {
                float m = std::min(pw, th_) * 0.22f;
                auto red = surf_.Solid(th.error);
                rt->DrawLine(D2D1::Point2F(x + m, sy + th_ / 2 - (pw / 2 - m)), D2D1::Point2F(x + pw - m, sy + th_ / 2 + (pw / 2 - m)), red, 3);
                rt->DrawLine(D2D1::Point2F(x + pw - m, sy + th_ / 2 - (pw / 2 - m)), D2D1::Point2F(x + m, sy + th_ / 2 + (pw / 2 - m)), red, 3);
            }
            if (hot || keyed) {
                // The page fades back and a big button sits in its middle: the trash can, or on a removed page
                // the way back.
                rt->FillRectangle(box, surf_.Solid(WithAlpha(th.glass | 0xFF000000, 0.55)));
                rt->DrawRoundedRectangle(D2D1::RoundedRect(RectF(x - 2, sy - 2, pw + 4, th_ + 4), 5, 5), surf_.Solid(removed ? th.accent : th.error), 2);
                D2D1_POINT_2F c = D2D1::Point2F(x + pw / 2, sy + th_ / 2);
                rt->FillEllipse(D2D1::Ellipse(c, 25, 25), surf_.Solid(removed ? th.accent : th.error));
                auto ink = surf_.Solid(0xFFFFFFFF);
                if (removed) {   // a curved arrow: bring it back
                    surf_.DrawText(L"↺", 26, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(c.x - 11.5f, c.y - 19, 28, 34), 0xFFFFFFFF);
                } else {
                    float s = 1.9f;   // the trash can, drawn on a 12-unit grid
                    rt->DrawLine(D2D1::Point2F(c.x - 6 * s, c.y - 4 * s), D2D1::Point2F(c.x + 6 * s, c.y - 4 * s), ink, 2);   // lid
                    rt->DrawLine(D2D1::Point2F(c.x - 2 * s, c.y - 6 * s), D2D1::Point2F(c.x + 2 * s, c.y - 6 * s), ink, 2);   // handle
                    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.x - 4.5f * s, c.y - 2 * s, c.x + 4.5f * s, c.y + 6.5f * s), 2, 2), ink, 2);   // can
                    rt->DrawLine(D2D1::Point2F(c.x - 1.6f * s, c.y), D2D1::Point2F(c.x - 1.6f * s, c.y + 4.5f * s), ink, 1.5f);
                    rt->DrawLine(D2D1::Point2F(c.x + 1.6f * s, c.y), D2D1::Point2F(c.x + 1.6f * s, c.y + 4.5f * s), ink, 1.5f);
                }
            }
            wstring label = pages_ == 0 ? L"Loading…" : removed ? L"Removed" : std::to_wstring(i + 1);
            float lw = surf_.MeasureText(label.c_str(), 11.5f, removed ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
            surf_.DrawText(label.c_str(), 11.5f, removed ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                           RectF(x + (pw - lw) / 2, sy + th_ + 5, lw + 4, 16), removed ? th.error : th.dim);
        }
        rt->PopAxisAlignedClip();
        if (MaxScroll() > 0) {   // where in the PDF the strip is
            float by_ = sy + th_ + float(kNumH) + 4, content = float(MaxScroll()) + sw;
            rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(sx, by_, sw, 5), 2.5f, 2.5f), surf_.Solid(th.sep));
            float tw = std::max(30.0f, sw * sw / content), tx = sx + float(scroll_) / float(MaxScroll()) * (sw - tw);
            rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(tx, by_, tw, 5), 2.5f, 2.5f), surf_.Solid(hover_ == Part::ScrollBar ? th.accent : th.dim));
        }
        const wchar_t* tip = removed_.empty() ? L"Click a page to remove it" : int(removed_.size()) >= pages_ ? L"Keep at least one page" : L"Click again to keep it";
        wstring hint = Ellipsize(surf_, tip, 11, DWRITE_FONT_WEIGHT_NORMAL, float(CancelX()) - 28);   // never under the buttons
        surf_.DrawText(hint.c_str(), 11, DWRITE_FONT_WEIGHT_NORMAL, RectF(L + 20, by + 8, float(CancelX()) - 28, 16),
                       int(removed_.size()) >= pages_ && pages_ > 0 ? th.error : th.dim);
    } else if (kind_ == Popup::Order) {
        // The files top to bottom, in the order their pages will come: number, name, a cross to take it out
        // (on the row under the pointer), and a grip to drag by. Below them, the place to drop more.
        float rh = float(RowH()), y0 = T + float(kListTop);
        for (size_t i = 0; i < items_.size(); i++) {
            float y = y0 + float(i) * rh;
            bool picked = int(i) == selected_, hot = int(i) == hoverRow_;
            rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(L + 20, y + 1, W - 40, rh - 3), 7, 7),
                                     surf_.Solid(picked ? WithAlpha(th.accent, 0.18) : th.button));
            if (picked) rt->DrawRoundedRectangle(D2D1::RoundedRect(RectF(L + 20.75, y + 1.75, W - 41.5f, rh - 4.5f), 6.5f, 6.5f), surf_.Solid(th.accent), 1.5f);
            float ty = y + (rh - 18) / 2;
            wstring n = std::to_wstring(i + 1);
            surf_.DrawText(n.c_str(), 12, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(L + 32, ty, 22, 18), picked ? th.accent : th.dim);
            wstring name = Ellipsize(surf_, FileName(items_[i]), 12.5f, DWRITE_FONT_WEIGHT_NORMAL, W - 40 - 34 - 64);
            surf_.DrawText(name.c_str(), 12.5f, DWRITE_FONT_WEIGHT_NORMAL, RectF(L + 56, ty, W - 40 - 34 - 64, 18), th.text);
            if (hot) {   // the cross
                bool onX = hover_ == Part::RowRemove;
                D2D1_POINT_2F c = D2D1::Point2F(L + W - 20 - 49, y + rh / 2);
                if (onX) rt->FillEllipse(D2D1::Ellipse(c, 10, 10), surf_.Solid(WithAlpha(th.error, 0.18)));
                auto ink = surf_.Solid(onX ? th.error : th.dim);
                rt->DrawLine(D2D1::Point2F(c.x - 4, c.y - 4), D2D1::Point2F(c.x + 4, c.y + 4), ink, 1.5f);
                rt->DrawLine(D2D1::Point2F(c.x + 4, c.y - 4), D2D1::Point2F(c.x - 4, c.y + 4), ink, 1.5f);
            }
            for (int k = 0; k < 3; k++)   // the grip: three short lines
                rt->DrawLine(D2D1::Point2F(L + W - 42, y + rh / 2 - 4 + k * 4), D2D1::Point2F(L + W - 30, y + rh / 2 - 4 + k * 4), surf_.Solid(th.dim), 1.2f);
        }
        // Where to drop: the whole list while it's empty, a strip below the rows after. It lights up under a drag.
        float dy = y0 + float(items_.size()) * rh + (items_.empty() ? 0 : 4), dh = float(items_.empty() ? 3 * 34 - 4 : kDropH);
        D2D1_RECT_F zone = RectF(L + 20, dy, W - 40, dh);
        rt->FillRoundedRectangle(D2D1::RoundedRect(zone, 8, 8), surf_.Solid(dropHover_ ? WithAlpha(th.accent, 0.16) : WithAlpha(th.button, 0.5)));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(RectF(L + 20.75f, dy + 0.75f, W - 41.5f, dh - 1.5f), 7.5f, 7.5f),
                                 surf_.Solid(dropHover_ ? th.accent : th.sep), dropHover_ ? 1.5f : 1);
        const wchar_t* say = items_.empty() ? L"Drop PDFs or pictures here" : L"Drop more here";
        float sw2 = surf_.MeasureText(say, 12, DWRITE_FONT_WEIGHT_NORMAL);
        surf_.DrawText(say, 12, DWRITE_FONT_WEIGHT_NORMAL, RectF(L + (W - sw2) / 2, dy + (items_.empty() ? dh / 2 - 18 : dh / 2 - 9), sw2 + 4, 18),
                       dropHover_ ? th.accent : th.dim);
        if (items_.empty()) {
            const wchar_t* sub = L"from Explorer, as many times as you like";
            float bw = surf_.MeasureText(sub, 11, DWRITE_FONT_WEIGHT_NORMAL);
            surf_.DrawText(sub, 11, DWRITE_FONT_WEIGHT_NORMAL, RectF(L + (W - bw) / 2, dy + dh / 2 + 2, bw + 4, 16), th.dim);
        }
        button(L + 20, by, float(kAddW), L"Add files…", false, hover_ == Part::AddFiles);
        if (items_.size() > 1) {   // the hint beside the title, clear of the buttons whatever their words make them
            const wchar_t* hint = L"Drag to reorder, or Alt+↑↓";
            float hw = surf_.MeasureText(hint, 11, DWRITE_FONT_WEIGHT_NORMAL);
            surf_.DrawText(hint, 11, DWRITE_FONT_WEIGHT_NORMAL, RectF(L + W - 20 - hw, T + 19, hw + 4, 16), th.dim);
        }
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
        for (int i = 0; i < 4 && 20 + i * 52 + 48 < CancelX(); i++) {
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
