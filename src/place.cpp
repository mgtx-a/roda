#include "place.h"

#include "ui.h"

namespace rd {

namespace {

constexpr double kW = 780, kH = 760, kShadow = 26;
constexpr double kTop = 60, kBar = 56;            // title area, bottom button bar
constexpr double kBtnH = 34;
constexpr double kPadW = 560, kPadH = 220;
constexpr uint32_t kInk = 0xFF14213D;             // dark blue, like a pen

bool In(double x, double y, double l, double t, double w, double h) { return x >= l && x < l + w && y >= t && y < t + h; }
bool In(double x, double y, const D2D1_RECT_F& r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

// The bottom bar's buttons: x and width (DIPs from the card's left).
struct Btn { double x, w; };
constexpr Btn kPrev{ 24, 36 }, kNext{ 178, 36 }, kDrawB{ 232, 140 }, kPicB{ 380, 124 };
constexpr Btn kCancel{ kW - 24 - 92 - 8 - 92, 92 }, kSaveB{ kW - 24 - 92, 92 };

/// A stroke as a smooth path: quadratic curves through the midpoints of its points.
Com<ID2D1PathGeometry> StrokePath(const std::vector<D2D1_POINT_2F>& pts, double k, double dx, double dy)
{
    Com<ID2D1PathGeometry> geo;
    Com<ID2D1GeometrySink> sink;
    if (pts.empty() || FAILED(D2D()->CreatePathGeometry(geo.Put())) || FAILED(geo->Open(sink.Put()))) return {};
    auto P = [&](const D2D1_POINT_2F& p) { return D2D1::Point2F(float((p.x + dx) * k), float((p.y + dy) * k)); };
    sink->BeginFigure(P(pts[0]), D2D1_FIGURE_BEGIN_HOLLOW);
    if (pts.size() == 1) sink->AddLine(D2D1::Point2F(P(pts[0]).x + 0.01f, P(pts[0]).y));   // a dot
    for (size_t i = 1; i + 1 < pts.size(); i++) {
        D2D1_POINT_2F mid{ (pts[i].x + pts[i + 1].x) / 2, (pts[i].y + pts[i + 1].y) / 2 };
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(P(pts[i]), P(mid)));
    }
    if (pts.size() > 1) sink->AddLine(P(pts.back()));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    return geo;
}

ID2D1StrokeStyle* RoundPen()
{
    static Com<ID2D1StrokeStyle> s;
    if (!s) D2D()->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, s.Put());
    return s.Get();
}

}  // namespace

PlaceWindow::PlaceWindow() {}
PlaceWindow::~PlaceWindow() { if (hwnd_) DestroyWindow(hwnd_); }

void PlaceWindow::Show(const wstring& pdf, bool draw)
{
    if (!hwnd_) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
        wc.lpszClassName = L"RodaPlace";
        RegisterClassW(&wc);
        // An ordinary window in the taskbar, drawn by Roda itself (like Settings).
        hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_APPWINDOW, wc.lpszClassName, L"Roda", WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX,
                                0, 0, 1, 1, nullptr, nullptr, wc.hInstance, this);
        if (!hwnd_) return;
    }
    pdf_ = pdf;
    signMode_ = draw;
    page_ = 0;
    pages_ = 0;
    pageMips_ = {};
    items_.clear();
    selected_ = -1;
    pendingPath_.clear();
    pageW_ = pageH_ = 0;
    strokes_.clear();
    drawing_ = draw;
    note_ = L"Loading…";
    SetWindowTextW(hwnd_, (wstring(draw ? L"Sign " : L"Add a picture to ") + FileName(pdf)).c_str());

    POINT at = CursorPos();
    scale_ = ScaleAt(at);
    surf_.SetScale(scale_);
    Mon mon = MonitorOf(at);
    double h = std::min(kH, (mon.work.bottom - mon.work.top) / scale_ - 2 * kShadow);   // small screens: shorter
    int pw = int(std::lround((kW + 2 * kShadow) * scale_)), ph = int(std::lround((h + 2 * kShadow) * scale_));
    int x = mon.work.left + (mon.work.right - mon.work.left - pw) / 2, y = std::max<int>(mon.work.top, mon.work.top + (mon.work.bottom - mon.work.top - ph) / 2);
    frame_ = { x, y, x + pw, y + ph };
    surf_.Resize(pw, ph);
    if (ReducedMotion()) fade_.Set(1); else { fade_.Set(0); fade_.Ease(1, 0.15, Curve::EaseOut); }
    SetWindowPos(hwnd_, HWND_TOP, x, y, pw, ph, SWP_SHOWWINDOW);
    shown_ = true;
    Changed();
    SetForegroundWindow(hwnd_);
    Log(L"sign: opened for %s", FileName(pdf).c_str());
    Go(0);
    if (!draw) PickPicture();
}

void PlaceWindow::Hide()
{
    if (!shown_) return;
    shown_ = false;
    ShowWindow(hwnd_, SW_HIDE);
    surf_.Release();
    pageMips_ = {};
    items_.clear();
    strokes_.clear();
}

void PlaceWindow::Go(int page)
{
    if (pages_ > 0) page = std::clamp(page, 0, pages_ - 1);
    page_ = page;
    selected_ = -1;
    waitingFor_ = page;
    note_ = L"Loading…";
    pageMips_ = {};
    Changed();
    if (onNeedPage) onNeedPage(page);
}

void PlaceWindow::SetPage(int page, const wstring& png, int pages)
{
    if (!shown_ || page != waitingFor_) { DeleteFileW(png.c_str()); return; }   // an old request
    pages_ = std::max(1, pages);
    pageMips_ = LoadMips(png, 2400, &pageW_, &pageH_);
    DeleteFileW(png.c_str());
    note_ = pageMips_.empty() ? L"This page could not be shown." : L"";
    if (!pageMips_.empty() && !pendingPath_.empty()) { AddItem(pendingPath_, std::move(pendingPic_), false); pendingPath_.clear(); }
    Changed();
}

void PlaceWindow::PageFailed(const wstring& why)
{
    note_ = why.empty() ? L"This page could not be shown." : why;
    Changed();
}

// ---------------------------------------------------------------- geometry

D2D1_RECT_F PlaceWindow::PageRect() const
{
    double h = (frame_.bottom - frame_.top) / scale_ - 2 * kShadow;
    double l = kShadow + 24, t = kShadow + kTop, w = kW - 48, ah = h - kTop - kBar - 12;
    if (pageW_ <= 0 || pageH_ <= 0) return RectF(l, t, w, ah);
    double k = std::min(w / pageW_, ah / pageH_);
    double pw = pageW_ * k, ph = pageH_ * k;
    return RectF(l + (w - pw) / 2, t + (ah - ph) / 2, pw, ph);
}

D2D1_RECT_F PlaceWindow::PadRect() const
{
    double h = (frame_.bottom - frame_.top) / scale_ - 2 * kShadow;
    return RectF(kShadow + (kW - kPadW) / 2, kShadow + kTop + (h - kTop - kBar - kPadH) / 2 - 20, kPadW, kPadH);
}

D2D1_RECT_F PlaceWindow::ItemRect(const Item& it) const
{
    D2D1_RECT_F pr = PageRect();
    double pw = pr.right - pr.left, ph = pr.bottom - pr.top;
    return RectF(pr.left + it.x * pw, pr.top + it.y * ph, it.w * pw, it.h * ph);
}

PlaceWindow::Part PlaceWindow::HitTest(double x, double y, int* item) const
{
    double h = (frame_.bottom - frame_.top) / scale_ - 2 * kShadow;
    double L = kShadow, T = kShadow, by = T + h - kBar + (kBar - kBtnH) / 2 - 8;
    *item = -1;
    if (In(x, y, L + kW - 48, T + 12, 36, 36)) return Part::Close;
    if (drawing_) {
        D2D1_RECT_F pad = PadRect();
        double py = pad.bottom + 14;
        if (In(x, y, pad.right - 92, py, 92, kBtnH)) return Part::PadUse;
        if (In(x, y, pad.right - 192, py, 92, kBtnH)) return Part::PadCancel;
        if (In(x, y, pad.left, py, 92, kBtnH)) return Part::PadClear;
        if (In(x, y, pad)) return Part::Pad;
        return Part::None;
    }
    if (In(x, y, L + kPrev.x, by, kPrev.w, kBtnH)) return Part::Prev;
    if (In(x, y, L + kNext.x, by, kNext.w, kBtnH)) return Part::Next;
    if (In(x, y, L + kDrawB.x, by, kDrawB.w, kBtnH)) return Part::Draw;
    if (In(x, y, L + kPicB.x, by, kPicB.w, kBtnH)) return Part::Picture;
    if (In(x, y, L + kCancel.x, by, kCancel.w, kBtnH)) return Part::Cancel;
    if (In(x, y, L + kSaveB.x, by, kSaveB.w, kBtnH)) return Part::Save;
    // Items on this page, the top one first; the selected one's handles before anything.
    if (selected_ >= 0) {
        D2D1_RECT_F r = ItemRect(items_[size_t(selected_)]);
        if (std::hypot(x - r.right, y - r.top) < 12) { *item = selected_; return Part::Delete; }
        if (std::hypot(x - r.right, y - r.bottom) < 12) { *item = selected_; return Part::Resize; }
    }
    for (int i = int(items_.size()) - 1; i >= 0; i--)
        if (items_[size_t(i)].page == page_ && In(x, y, ItemRect(items_[size_t(i)]))) { *item = i; return Part::Item; }
    return Part::None;
}

// ---------------------------------------------------------------- actions

void PlaceWindow::AddItem(const wstring& image, Mips pic, bool signature)
{
    if (pic.empty() || pageW_ <= 0) return;
    D2D1_RECT_F pr = PageRect();
    double pw = pr.right - pr.left, ph = pr.bottom - pr.top;
    Item it;
    it.page = page_;
    it.w = signature ? 0.3 : 0.4;
    it.h = it.w * pw * pic.height() / pic.width() / ph;
    if (it.h > 0.8) { it.w *= 0.8 / it.h; it.h = 0.8; }   // a tall picture: fit the page
    it.x = (1 - it.w) / 2;
    it.y = signature ? std::min(0.75, 1 - it.h - 0.04) : (1 - it.h) / 2;
    it.image = image;
    it.pic = std::move(pic);
    items_.push_back(std::move(it));
    selected_ = int(items_.size()) - 1;
    Log(L"sign: %s placed on page %d", signature ? L"a signature" : L"a picture", page_ + 1);
    Changed();
}

void PlaceWindow::PickPicture()
{
    Com<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.Put())))) return;
    COMDLG_FILTERSPEC types[] = { { L"Pictures", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff" } };
    dlg->SetFileTypes(1, types);
    dlg->SetTitle(L"Choose a picture (a signature, a stamp, a logo…)");
    DWORD o = 0;
    dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    BeginModal();
    HRESULT hr = dlg->Show(hwnd_);
    EndModal();
    Com<IShellItem> item;
    PWSTR p = nullptr;
    if (SUCCEEDED(hr) && SUCCEEDED(dlg->GetResult(item.Put())) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        wstring path = p;
        Mips pic = LoadMips(path, 1600);
        if (pic.empty()) { note_ = L"That picture could not be opened."; Changed(); }
        else if (!pageMips_.empty()) AddItem(path, std::move(pic), false);
        else { pendingPath_ = path; pendingPic_ = std::move(pic); }   // the page is still on its way
    }
    CoTaskMemFree(p);
}

bool PlaceWindow::SaveSignature(wstring& png)
{
    // The strokes' box, drawn 4x as big as on screen onto a transparent picture, saved as PNG.
    float l = 1e9f, t = 1e9f, r = -1e9f, b = -1e9f;
    for (auto& s : strokes_) for (auto& p : s) { l = std::min(l, p.x); t = std::min(t, p.y); r = std::max(r, p.x); b = std::max(b, p.y); }
    if (r < l) return false;
    const double k = 4, margin = 4, pen = 2.6;
    double dx = margin - l, dy = margin - t;
    UINT w = UINT(std::ceil((r - l + 2 * margin) * k)), h = UINT(std::ceil((b - t + 2 * margin) * k));
    Com<IWICBitmap> bmp;
    Com<ID2D1RenderTarget> rt;
    if (FAILED(Wic()->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bmp.Put()))) return false;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(D2D()->CreateWicBitmapRenderTarget(bmp.Get(), props, rt.Put()))) return false;
    Com<ID2D1SolidColorBrush> ink;
    rt->CreateSolidColorBrush(Argb(kInk), ink.Put());
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    for (auto& s : strokes_) if (auto g = StrokePath(s, k, dx, dy)) rt->DrawGeometry(g.Get(), ink.Get(), float(pen * k), RoundPen());
    if (FAILED(rt->EndDraw())) return false;

    static int n = 0;
    png = JoinPath(TempDir(), L"signature-" + std::to_wstring(++n) + L".png");
    Com<IWICStream> stream;
    Com<IWICBitmapEncoder> enc;
    Com<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;   // straight alpha in the file; WIC converts
    bool ok = SUCCEEDED(Wic()->CreateStream(stream.Put())) && SUCCEEDED(stream->InitializeFromFilename(png.c_str(), GENERIC_WRITE))
           && SUCCEEDED(Wic()->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.Put())) && SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))
           && SUCCEEDED(enc->CreateNewFrame(frame.Put(), nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h))
           && SUCCEEDED(frame->SetPixelFormat(&fmt)) && SUCCEEDED(frame->WriteSource(bmp.Get(), nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    if (!ok) Log(L"sign: could not save the signature picture");
    return ok;
}

void PlaceWindow::Save()
{
    if (items_.empty()) { MessageBeep(MB_OK); note_ = signMode_ ? L"Draw a signature first." : L"Add a picture first."; Changed(); return; }
    Json stamps = Json::Array();
    for (auto& it : items_)
        stamps.Push(Json::Object().Set(L"page", it.page).Set(L"x", it.x).Set(L"y", it.y).Set(L"w", it.w).Set(L"h", it.h).Set(L"image", it.image));
    Log(L"sign: saving %d placement(s)", int(items_.size()));
    auto fn = onSave;
    Json opts = Json::Object().Set(L"stamps", stamps);
    Hide();
    if (fn) fn(opts);
}

void PlaceWindow::Click(Part p, int item, double x, double y)
{
    switch (p) {
        case Part::Close: case Part::Cancel: Log(L"sign: closed without saving"); Hide(); return;
        case Part::Prev: if (page_ > 0) Go(page_ - 1); return;
        case Part::Next: if (page_ + 1 < pages_) Go(page_ + 1); return;
        case Part::Draw: drawing_ = true; strokes_.clear(); Changed(); return;
        case Part::Picture: PickPicture(); return;
        case Part::Save: Save(); return;
        case Part::PadClear: strokes_.clear(); Changed(); return;
        case Part::PadCancel: drawing_ = false; strokes_.clear(); Changed(); return;
        case Part::PadUse: {
            wstring png;
            if (strokes_.empty() || !SaveSignature(png)) { MessageBeep(MB_OK); return; }
            drawing_ = false;
            strokes_.clear();
            AddItem(png, LoadMips(png, 2400), true);
            return;
        }
        case Part::Pad: {
            D2D1_RECT_F pad = PadRect();
            strokes_.push_back({ D2D1::Point2F(float(x - pad.left), float(y - pad.top)) });
            stroking_ = true;
            SetCapture(hwnd_);
            Changed();
            return;
        }
        case Part::Delete: items_.erase(items_.begin() + item); selected_ = -1; Changed(); return;
        case Part::Item: case Part::Resize:
            selected_ = item;
            drag_ = p;
            dragItem_ = item;
            dragX_ = x;
            dragY_ = y;
            dragStart_ = items_[size_t(item)];
            SetCapture(hwnd_);
            Changed();
            return;
        default:
            if (selected_ >= 0) { selected_ = -1; Changed(); }
            return;
    }
}

LRESULT CALLBACK PlaceWindow::WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<PlaceWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self && self->hwnd_ == h) return self->Handle(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT PlaceWindow::Handle(UINT m, WPARAM w, LPARAM l)
{
    auto local = [&](LPARAM lp) { return std::pair<double, double>{ GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_ }; };
    switch (m) {
        case WM_NCHITTEST: {
            // The title area moves the window.
            POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            ScreenToClient(hwnd_, &p);
            int it;
            double x = p.x / scale_, y = p.y / scale_;
            return HitTest(x, y, &it) == Part::None && y - kShadow < kTop ? HTCAPTION : HTCLIENT;
        }
        case WM_LBUTTONDOWN: { auto [x, y] = local(l); int it; Part p = HitTest(x, y, &it); Click(p, it, x, y); return 0; }
        case WM_MOUSEMOVE: {
            auto [x, y] = local(l);
            if (stroking_) {
                D2D1_RECT_F pad = PadRect();
                D2D1_POINT_2F q = D2D1::Point2F(float(std::clamp<double>(x, pad.left, pad.right) - pad.left), float(std::clamp<double>(y, pad.top, pad.bottom) - pad.top));
                auto& s = strokes_.back();
                if (std::hypot(q.x - s.back().x, q.y - s.back().y) >= 0.8) { s.push_back(q); Changed(); }
                return 0;
            }
            if (drag_ != Part::None && dragItem_ >= 0) {
                Item& it = items_[size_t(dragItem_)];
                D2D1_RECT_F pr = PageRect();
                double pw = pr.right - pr.left, ph = pr.bottom - pr.top;
                double dx = (x - dragX_) / pw, dy = (y - dragY_) / ph;
                if (drag_ == Part::Item) {
                    it.x = std::clamp(dragStart_.x + dx, 0.0, 1 - it.w);
                    it.y = std::clamp(dragStart_.y + dy, 0.0, 1 - it.h);
                } else {
                    // Resize from the bottom right corner, keeping the shape.
                    double k = std::max((dragStart_.w + dx) / dragStart_.w, 0.05 / dragStart_.w);
                    k = std::min({ k, (1 - it.x) / dragStart_.w, (1 - it.y) / dragStart_.h });
                    it.w = dragStart_.w * k;
                    it.h = dragStart_.h * k;
                }
                Changed();
                return 0;
            }
            int it;
            Part p = HitTest(x, y, &it);
            if (p != hover_) { hover_ = p; Changed(); }
            return 0;
        }
        case WM_LBUTTONUP:
            if (stroking_) { stroking_ = false; ReleaseCapture(); }
            if (drag_ != Part::None) { drag_ = Part::None; dragItem_ = -1; ReleaseCapture(); }
            return 0;
        case WM_SETCURSOR: {
            LPCWSTR c = IDC_ARROW;
            if (hover_ == Part::Pad) c = IDC_CROSS;
            else if (hover_ == Part::Item) c = IDC_SIZEALL;
            else if (hover_ == Part::Resize) c = IDC_SIZENWSE;
            else if (hover_ != Part::None) c = IDC_HAND;
            SetCursor(LoadCursorW(nullptr, c));
            return TRUE;
        }
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) {
                if (drawing_) { drawing_ = false; strokes_.clear(); Changed(); }
                else { Log(L"sign: closed without saving"); Hide(); }
            } else if (drawing_ && w == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000) && !strokes_.empty()) { strokes_.pop_back(); Changed(); }
            else if (!drawing_ && w == VK_DELETE && selected_ >= 0) { items_.erase(items_.begin() + selected_); selected_ = -1; Changed(); }
            else if (!drawing_ && (w == VK_LEFT || w == VK_PRIOR) && page_ > 0) Go(page_ - 1);
            else if (!drawing_ && (w == VK_RIGHT || w == VK_NEXT) && page_ + 1 < pages_) Go(page_ + 1);
            return 0;
        case WM_CLOSE: Hide(); return 0;
        case WM_MOVE: GetWindowRect(hwnd_, &frame_); return 0;
        case WM_DPICHANGED: return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

// ---------------------------------------------------------------- drawing

void PlaceWindow::Draw()
{
    dirty_ = false;
    if (!shown_) return;
    if (!surf_.ready()) surf_.Resize(frame_.right - frame_.left, frame_.bottom - frame_.top);
    ID2D1DCRenderTarget* rt = surf_.Begin();
    if (!rt) return;
    const Theme& th = CurrentTheme();
    float L = float(kShadow), T = float(kShadow), W = float(kW), Ht = float((frame_.bottom - frame_.top) / scale_ - 2 * kShadow);
    surf_.DrawShadow(RectF(L, T, W, Ht), 16, 22, 0, 8, th.shadow);
    rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(L, T, W, Ht), 16, 16), surf_.Solid(th.glass | 0xFF000000));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(RectF(L + 0.5, T + 0.5, W - 1, Ht - 1), 15.5f, 15.5f), surf_.Solid(th.edge), 1);
    wstring title = (signMode_ ? L"Sign " : L"Add a picture to ") + FileName(pdf_);
    surf_.DrawText(Ellipsize(surf_, title, 16, DWRITE_FONT_WEIGHT_SEMI_BOLD, W - 120).c_str(), 16, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(L + 24, T + 20, W - 100, 24), th.text);
    D2D1_POINT_2F cc = D2D1::Point2F(L + W - 30, T + 30);
    if (hover_ == Part::Close) rt->FillEllipse(D2D1::Ellipse(cc, 13, 13), surf_.Solid(th.button));
    rt->DrawLine(D2D1::Point2F(cc.x - 5, cc.y - 5), D2D1::Point2F(cc.x + 5, cc.y + 5), surf_.Solid(th.text), 1.4f);
    rt->DrawLine(D2D1::Point2F(cc.x - 5, cc.y + 5), D2D1::Point2F(cc.x + 5, cc.y - 5), surf_.Solid(th.text), 1.4f);

    // The page, on a soft shadow, and what is placed on it.
    D2D1_RECT_F pr = PageRect();
    if (!pageMips_.empty()) {
        surf_.DrawShadow(pr, 2, 10, 0, 2, th.shadow * 0.6f);
        rt->FillRectangle(pr, surf_.Solid(0xFFFFFFFF));
        surf_.DrawMips(pageMips_, pr);
        for (size_t i = 0; i < items_.size(); i++) {
            const Item& it = items_[i];
            if (it.page != page_) continue;
            D2D1_RECT_F r = ItemRect(it);
            surf_.DrawMips(it.pic, r);
            if (int(i) == selected_ || (hover_ == Part::Item && drag_ == Part::None)) {
                rt->DrawRectangle(r, surf_.Solid(int(i) == selected_ ? th.accent : WithAlpha(th.accent, 0.5)), 1.5f);
            }
            if (int(i) == selected_) {
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.right, r.bottom), 6, 6), surf_.Solid(th.accent));
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.right, r.bottom), 2.5f, 2.5f), surf_.Solid(0xFFFFFFFF));
                D2D1_POINT_2F dc = D2D1::Point2F(r.right, r.top);
                rt->FillEllipse(D2D1::Ellipse(dc, 9, 9), surf_.Solid(hover_ == Part::Delete ? th.error : 0xFF5A5F66));
                rt->DrawLine(D2D1::Point2F(dc.x - 3.5f, dc.y - 3.5f), D2D1::Point2F(dc.x + 3.5f, dc.y + 3.5f), surf_.Solid(0xFFFFFFFF), 1.5f);
                rt->DrawLine(D2D1::Point2F(dc.x - 3.5f, dc.y + 3.5f), D2D1::Point2F(dc.x + 3.5f, dc.y - 3.5f), surf_.Solid(0xFFFFFFFF), 1.5f);
            }
        }
    }
    if (!note_.empty()) {
        float tw = surf_.MeasureText(note_.c_str(), 13, DWRITE_FONT_WEIGHT_NORMAL);
        float cy = pageMips_.empty() ? (pr.top + pr.bottom) / 2 - 10 : pr.bottom - 30;
        if (!pageMips_.empty()) rt->FillRoundedRectangle(D2D1::RoundedRect(RectF((pr.left + pr.right) / 2 - tw / 2 - 12, cy - 6, tw + 24, 30), 8, 8), surf_.Solid(0xE0202428));
        surf_.DrawText(note_.c_str(), 13, DWRITE_FONT_WEIGHT_NORMAL, RectF((pr.left + pr.right) / 2 - tw / 2, cy, tw + 4, 20), pageMips_.empty() ? th.dim : 0xFFFFFFFF);
    }

    auto button = [&](double x, double y, double w, const wchar_t* label, bool primary, bool hot, bool enabled = true) {
        uint32_t bg = primary ? th.accent : th.button;
        if (hot && enabled) bg = primary ? WithAlpha(th.accent, 0.85) : WithAlpha(th.accent, 0.18);
        rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(x, y, w, kBtnH), 8, 8), surf_.Solid(bg));
        float tw = surf_.MeasureText(label, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        uint32_t fg = primary ? th.accentText : enabled ? th.text : th.dim;
        surf_.DrawText(label, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(x + (w - tw) / 2, y + kBtnH / 2 - 9, tw + 4, 18), fg);
    };

    // The bottom bar.
    float by = T + Ht - float(kBar) + float(kBar - kBtnH) / 2 - 8;
    button(L + kPrev.x, by, kPrev.w, L"‹", false, hover_ == Part::Prev, page_ > 0);
    button(L + kNext.x, by, kNext.w, L"›", false, hover_ == Part::Next, page_ + 1 < pages_);
    wstring pg = pages_ > 0 ? L"Page " + std::to_wstring(page_ + 1) + L" of " + std::to_wstring(pages_) : L"";
    float pgw = surf_.MeasureText(pg.c_str(), 12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    surf_.DrawText(pg.c_str(), 12.5f, DWRITE_FONT_WEIGHT_NORMAL, RectF(L + 117 - pgw / 2, by + kBtnH / 2 - 9, pgw + 4, 18), th.text);
    button(L + kDrawB.x, by, kDrawB.w, L"Draw signature", false, hover_ == Part::Draw);
    button(L + kPicB.x, by, kPicB.w, L"Add picture…", false, hover_ == Part::Picture);
    button(L + kCancel.x, by, kCancel.w, L"Cancel", false, hover_ == Part::Cancel);
    button(L + kSaveB.x, by, kSaveB.w, L"Save", true, hover_ == Part::Save);

    // The signature pad, over everything else.
    if (drawing_) {
        rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(L, T + kTop - 8, W, Ht - kTop + 8), 16, 16), surf_.Solid(WithAlpha(th.glass | 0xFF000000, 0.88)));
        D2D1_RECT_F pad = PadRect();
        surf_.DrawShadow(pad, 12, 16, 0, 4, th.shadow);
        rt->FillRoundedRectangle(D2D1::RoundedRect(pad, 12, 12), surf_.Solid(0xFFFFFFFF));
        float base = pad.top + float(kPadH) * 0.72f;
        rt->DrawLine(D2D1::Point2F(pad.left + 40, base), D2D1::Point2F(pad.right - 40, base), surf_.Solid(0xFFC9CED6), 1);
        surf_.DrawText(L"×", 16, DWRITE_FONT_WEIGHT_NORMAL, RectF(pad.left + 22, base - 22, 20, 22), 0xFF9AA1AB);
        if (strokes_.empty())
            surf_.DrawText(L"Sign here with your mouse (or pen)", 13, DWRITE_FONT_WEIGHT_NORMAL, RectF(pad.left + 40, pad.top + 24, kPadW - 80, 20), 0xFF9AA1AB);
        for (auto& s : strokes_) if (auto g = StrokePath(s, 1, pad.left, pad.top)) rt->DrawGeometry(g.Get(), surf_.Solid(kInk), 2.6f, RoundPen());
        surf_.DrawText(L"Ctrl+Z undoes the last stroke", 11, DWRITE_FONT_WEIGHT_NORMAL, RectF(pad.left, pad.top - 22, kPadW, 16), th.dim);
        float py = pad.bottom + 14;
        button(pad.left, py, 92, L"Clear", false, hover_ == Part::PadClear, !strokes_.empty());
        button(pad.right - 192, py, 92, L"Cancel", false, hover_ == Part::PadCancel);
        button(pad.right - 92, py, 92, L"Use", true, hover_ == Part::PadUse);
    }
    surf_.End();
    surf_.Present(hwnd_, frame_.left, frame_.top, BYTE(std::clamp(fade_.value, 0.0, 1.0) * 255));
    if (fade_.Active()) dirty_ = true;
}

}  // namespace rd
