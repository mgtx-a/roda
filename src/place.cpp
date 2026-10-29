#include "place.h"

#include "ui.h"

namespace rd {

namespace {

constexpr double kW = 780, kH = 760, kShadow = 26;
constexpr double kTop = 60, kBar = 56;            // title area, bottom button bar
constexpr double kBtnH = 34;
constexpr double kPadW = 560, kPadH = 220;
constexpr double kRowH = 28;                      // a choice in an open list
constexpr uint32_t kInk = 0xFF14213D;             // dark blue, like a pen
constexpr uint32_t kTyped = 0xFF000000;           // typed text: black, as it will be printed

bool In(double x, double y, double l, double t, double w, double h) { return x >= l && x < l + w && y >= t && y < t + h; }
bool In(double x, double y, const D2D1_RECT_F& r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

// The bottom bar's buttons: x and width (DIPs from the card's left).
struct Btn { double x, w; };
constexpr Btn kPrev{ 24, 36 }, kNext{ 178, 36 }, kTextB{ 232, 92 }, kSignB{ 332, 80 }, kPicB{ 420, 96 };
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

/// Arial at `size` DIPs: on screen it matches the Helvetica the worker writes into the PDF.
IDWriteTextFormat* Arial(float size)
{
    static std::unordered_map<int, Com<IDWriteTextFormat>> cache;
    int key = int(std::lround(size * 8));
    auto& f = cache[key];
    if (!f) {
        DWrite()->CreateTextFormat(L"Arial", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", f.Put());
        if (f) f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return f.Get();
}

float TextWidth(const wstring& s, float size)
{
    Com<IDWriteTextLayout> layout;
    if (s.empty() || FAILED(DWrite()->CreateTextLayout(s.c_str(), UINT32(s.size()), Arial(size), 10000, 1000, layout.Put()))) return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

std::vector<wstring> Lines(const wstring& s)
{
    std::vector<wstring> out(1);
    for (wchar_t c : s) { if (c == L'\n') out.emplace_back(); else out.back() += c; }
    return out;
}

/// Draws typed lines as the worker will write them: baselines 0.8 of the size below the top, 1.2 apart.
void DrawLines(ID2D1RenderTarget* rt, ID2D1Brush* brush, const wstring& s, float size, float x, float y)
{
    auto lines = Lines(s);
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].empty()) continue;
        float top = y + (0.8f + 1.2f * float(i)) * size - 0.905f * size;   // Arial's ascent is 0.905 of its size
        rt->DrawText(lines[i].c_str(), UINT32(lines[i].size()), Arial(size), D2D1::RectF(x, top, x + 10000, top + size * 1.3f), brush);
    }
}

}  // namespace

PlaceWindow::PlaceWindow() {}
PlaceWindow::~PlaceWindow() { if (hwnd_) DestroyWindow(hwnd_); }

void PlaceWindow::Show(const wstring& pdf, Mode mode)
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
    mode_ = mode;
    page_ = pages_ = 0;
    pageMips_ = {};
    pageW_ = pageH_ = 0;
    pagePt_ = 0;
    items_.clear();
    fields_.clear();
    fieldsKnown_ = false;
    selected_ = editItem_ = editField_ = openList_ = -1;
    pendingPath_.clear();
    strokes_.clear();
    drawing_ = mode == Mode::Sign;
    note_ = L"Loading…";
    const wchar_t* verb = mode == Mode::Sign ? L"Sign " : mode == Mode::Fill ? L"Fill in " : L"Add a picture to ";
    SetWindowTextW(hwnd_, (verb + FileName(pdf)).c_str());

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
    Log(L"pdf editor: opened for %s", FileName(pdf).c_str());
    Go(0);
    if (mode == Mode::Fill && onNeedFields) onNeedFields();
    if (mode == Mode::Picture) PickPicture();
}

void PlaceWindow::Hide()
{
    if (!shown_) return;
    shown_ = false;
    ShowWindow(hwnd_, SW_HIDE);
    surf_.Release();
    pageMips_ = {};
    items_.clear();
    fields_.clear();
    strokes_.clear();
}

void PlaceWindow::Go(int page)
{
    StopEditing();
    openList_ = -1;
    if (pages_ > 0) page = std::clamp(page, 0, pages_ - 1);
    page_ = page;
    selected_ = -1;
    waitingFor_ = page;
    note_ = L"Loading…";
    pageMips_ = {};
    Changed();
    if (onNeedPage) onNeedPage(page);
}

void PlaceWindow::SetPage(int page, const wstring& png, int pages, double heightPt)
{
    if (!shown_ || page != waitingFor_) { DeleteFileW(png.c_str()); return; }   // an old request
    pages_ = std::max(1, pages);
    pageMips_ = LoadMips(png, 2400, &pageW_, &pageH_);
    pagePt_ = heightPt;
    DeleteFileW(png.c_str());
    note_ = pageMips_.empty() ? L"This page could not be shown." : L"";
    if (!pageMips_.empty() && !pendingPath_.empty()) { AddPicture(pendingPath_, std::move(pendingPic_), false); pendingPath_.clear(); }
    Changed();
}

void PlaceWindow::PageFailed(const wstring& why)
{
    note_ = why.empty() ? L"This page could not be shown." : why;
    Changed();
}

void PlaceWindow::SetFields(const Json& fields)
{
    if (!shown_) return;
    fields_.clear();
    for (auto& j : fields.arr) {
        Field f;
        f.page = int(j[L"page"].Num());
        f.index = int(j[L"i"].Num());
        f.type = int(j[L"type"].Num());
        f.x = j[L"x"].Num(); f.y = j[L"y"].Num(); f.w = j[L"w"].Num(); f.h = j[L"h"].Num();
        f.name = j[L"name"].Str();
        f.label = j[L"label"].Str();
        f.exportValue = j[L"export"].Str();
        f.value = j[L"value"].Str();
        f.checked = j[L"checked"].Bool();
        f.multiline = j[L"multiline"].Bool();
        for (auto& o : j[L"options"].arr) f.options.push_back(o.Str());
        for (size_t o = 0; o < f.options.size(); o++) if (f.options[o] == f.value) f.choice = int(o);
        fields_.push_back(std::move(f));
    }
    fieldsKnown_ = true;
    Log(L"pdf editor: %d form field(s)", int(fields_.size()));
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

double PlaceWindow::PtToDip() const
{
    D2D1_RECT_F pr = PageRect();
    return pagePt_ > 0 ? (pr.bottom - pr.top) / pagePt_ : 1.0;
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
    if (!it.text) return RectF(pr.left + it.x * pw, pr.top + it.y * ph, it.w * pw, it.h * ph);
    float size = float(it.size * PtToDip());
    float w = size * 0.6f;
    auto lines = Lines(it.words);
    for (auto& l : lines) w = std::max(w, TextWidth(l, size));
    return RectF(pr.left + it.x * pw, pr.top + it.y * ph, w + 2, size * (1.2 * lines.size() - 0.2) + 2);
}

D2D1_RECT_F PlaceWindow::FieldRect(const Field& f) const
{
    D2D1_RECT_F pr = PageRect();
    double pw = pr.right - pr.left, ph = pr.bottom - pr.top;
    return RectF(pr.left + f.x * pw, pr.top + f.y * ph, f.w * pw, f.h * ph);
}

D2D1_RECT_F PlaceWindow::OptionsRect() const
{
    if (openList_ < 0) return {};
    const Field& f = fields_[size_t(openList_)];
    D2D1_RECT_F r = FieldRect(f);
    double w = std::max<double>(r.right - r.left, 160), h = kRowH * f.options.size() + 8;
    double h2 = (frame_.bottom - frame_.top) / scale_ - kShadow;
    double top = r.bottom + 4 + h > h2 - kBar ? r.top - 4 - h : r.bottom + 4;   // above the field when there is no room below
    return RectF(r.left, top, w, h);
}

PlaceWindow::Part PlaceWindow::HitTest(double x, double y, int* which) const
{
    double h = (frame_.bottom - frame_.top) / scale_ - 2 * kShadow;
    double L = kShadow, T = kShadow, by = T + h - kBar + (kBar - kBtnH) / 2 - 8;
    *which = -1;
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
    if (openList_ >= 0) {
        D2D1_RECT_F r = OptionsRect();
        if (In(x, y, r)) { *which = std::clamp(int((y - r.top - 4) / kRowH), 0, int(fields_[size_t(openList_)].options.size()) - 1); return Part::Option; }
        return Part::None;   // a click elsewhere closes the list
    }
    if (In(x, y, L + kPrev.x, by, kPrev.w, kBtnH)) return Part::Prev;
    if (In(x, y, L + kNext.x, by, kNext.w, kBtnH)) return Part::Next;
    if (In(x, y, L + kTextB.x, by, kTextB.w, kBtnH)) return Part::Text;
    if (In(x, y, L + kSignB.x, by, kSignB.w, kBtnH)) return Part::Sign;
    if (In(x, y, L + kPicB.x, by, kPicB.w, kBtnH)) return Part::Picture;
    if (In(x, y, L + kCancel.x, by, kCancel.w, kBtnH)) return Part::Cancel;
    if (In(x, y, L + kSaveB.x, by, kSaveB.w, kBtnH)) return Part::Save;
    if (pageMips_.empty()) return Part::None;
    // The selected item's handles, then items (the top one first), then form fields.
    if (selected_ >= 0) {
        D2D1_RECT_F r = ItemRect(items_[size_t(selected_)]);
        if (std::hypot(x - r.right, y - r.top) < 12) { *which = selected_; return Part::Delete; }
        if (std::hypot(x - r.right, y - r.bottom) < 12) { *which = selected_; return Part::Resize; }
    }
    for (int i = int(items_.size()) - 1; i >= 0; i--)
        if (items_[size_t(i)].page == page_ && In(x, y, ItemRect(items_[size_t(i)]))) { *which = i; return Part::Item; }
    for (int i = 0; i < int(fields_.size()); i++)
        if (fields_[size_t(i)].page == page_ && In(x, y, FieldRect(fields_[size_t(i)]))) { *which = i; return Part::Field; }
    return Part::None;
}

// ---------------------------------------------------------------- editing

wstring& PlaceWindow::EditedText()
{
    return editField_ >= 0 ? fields_[size_t(editField_)].value : items_[size_t(editItem_)].words;
}

bool PlaceWindow::EditingMultiline() const
{
    return editItem_ >= 0 || (editField_ >= 0 && fields_[size_t(editField_)].multiline);
}

void PlaceWindow::StopEditing()
{
    if (editItem_ >= 0 && editItem_ < int(items_.size()) && items_[size_t(editItem_)].words.find_first_not_of(L" \n") == wstring::npos) {
        items_.erase(items_.begin() + editItem_);   // nothing typed: no empty text left behind
        selected_ = -1;
    }
    editItem_ = editField_ = -1;
    Changed();
}

void PlaceWindow::Type(const wstring& s)
{
    if (editItem_ < 0 && editField_ < 0) return;
    wstring add = s;
    if (!EditingMultiline()) for (auto& c : add) if (c == L'\n') c = L' ';
    wstring& t = EditedText();
    if (t.size() + add.size() > 2000) return;
    t += add;
    if (editField_ >= 0) fields_[size_t(editField_)].changed = true;
    Changed();
}

void PlaceWindow::Backspace()
{
    if (editItem_ < 0 && editField_ < 0) return;
    wstring& t = EditedText();
    if (t.empty()) return;
    t.pop_back();
    if (!t.empty() && IS_HIGH_SURROGATE(t.back())) t.pop_back();
    if (editField_ >= 0) fields_[size_t(editField_)].changed = true;
    Changed();
}

void PlaceWindow::ClickField(int i)
{
    Field& f = fields_[size_t(i)];
    StopEditing();
    selected_ = -1;
    switch (f.type) {
        case 6: editField_ = i; break;                                  // text: type into it
        case 2: f.checked = !f.checked; f.changed = true; break;        // check box
        case 3:                                                          // radio: this one of its group
            for (auto& g : fields_) if (g.name == f.name && g.checked) { g.checked = false; g.changed = true; }
            f.checked = true;
            f.changed = true;
            break;
        case 4: case 5: if (!f.options.empty()) openList_ = i; break;  // a list: show the choices
    }
    Changed();
}

void PlaceWindow::NextField(int from)
{
    for (size_t k = 1; k <= fields_.size(); k++) {
        int i = int((size_t(from) + k) % fields_.size());
        if (fields_[size_t(i)].page == page_ && fields_[size_t(i)].type == 6) { StopEditing(); editField_ = i; Changed(); return; }
    }
}

// ---------------------------------------------------------------- adding things

void PlaceWindow::AddItem(Item it)
{
    items_.push_back(std::move(it));
    selected_ = int(items_.size()) - 1;
    Changed();
}

void PlaceWindow::AddPicture(const wstring& image, Mips pic, bool signature)
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
    Log(L"pdf editor: %s placed on page %d", signature ? L"a signature" : L"a picture", page_ + 1);
    AddItem(std::move(it));
}

void PlaceWindow::AddText()
{
    if (pageMips_.empty()) return;
    StopEditing();
    Item it;
    it.text = true;
    it.page = page_;
    it.x = 0.1;
    it.y = 0.45;
    it.size = 12;
    AddItem(std::move(it));
    editItem_ = selected_;
    Log(L"pdf editor: text added on page %d", page_ + 1);
}

void PlaceWindow::PickPicture()
{
    StopEditing();
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
        else if (!pageMips_.empty()) AddPicture(path, std::move(pic), false);
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
    if (!ok) Log(L"pdf editor: could not save the signature picture");
    return ok;
}

void PlaceWindow::Save()
{
    StopEditing();
    Json stamps = Json::Array(), texts = Json::Array(), fields = Json::Array();
    for (auto& it : items_) {
        if (it.text) texts.Push(Json::Object().Set(L"page", it.page).Set(L"x", it.x).Set(L"y", it.y).Set(L"size", it.size).Set(L"text", it.words));
        else stamps.Push(Json::Object().Set(L"page", it.page).Set(L"x", it.x).Set(L"y", it.y).Set(L"w", it.w).Set(L"h", it.h).Set(L"image", it.image));
    }
    for (auto& f : fields_) {
        if (!f.changed || (f.type == 3 && !f.checked)) continue;   // a radio button is set by turning another on
        fields.Push(Json::Object().Set(L"page", f.page).Set(L"i", f.index).Set(L"type", f.type).Set(L"value", f.value)
                                  .Set(L"checked", f.checked).Set(L"index", f.choice));
    }
    if (stamps.arr.empty() && texts.arr.empty() && fields.arr.empty()) {
        MessageBeep(MB_OK);
        note_ = L"Nothing changed yet.";
        Changed();
        return;
    }
    Log(L"pdf editor: saving %d field(s), %d text(s), %d picture(s)", int(fields.arr.size()), int(texts.arr.size()), int(stamps.arr.size()));
    auto fn = onSave;
    Json opts = Json::Object().Set(L"stamps", stamps).Set(L"texts", texts).Set(L"fields", fields);
    Hide();
    if (fn) fn(opts);
}

void PlaceWindow::Click(Part p, int which, double x, double y)
{
    // A click away from what is being typed into finishes it; a click away from an open list closes it.
    if (openList_ >= 0 && p != Part::Option) { openList_ = -1; Changed(); if (p == Part::None) return; }
    if ((editItem_ >= 0 || editField_ >= 0) && !(p == Part::Item && which == editItem_) && !(p == Part::Field && which == editField_)) StopEditing();
    switch (p) {
        case Part::Close: case Part::Cancel: Log(L"pdf editor: closed without saving"); Hide(); return;
        case Part::Prev: if (page_ > 0) Go(page_ - 1); return;
        case Part::Next: if (page_ + 1 < pages_) Go(page_ + 1); return;
        case Part::Text: AddText(); return;
        case Part::Sign: drawing_ = true; strokes_.clear(); Changed(); return;
        case Part::Picture: PickPicture(); return;
        case Part::Save: Save(); return;
        case Part::Option: {
            Field& f = fields_[size_t(openList_)];
            f.choice = which;
            f.value = f.options[size_t(which)];
            f.changed = true;
            openList_ = -1;
            Changed();
            return;
        }
        case Part::Field: ClickField(which); return;
        case Part::PadClear: strokes_.clear(); Changed(); return;
        case Part::PadCancel: drawing_ = false; strokes_.clear(); Changed(); return;
        case Part::PadUse: {
            wstring png;
            if (strokes_.empty() || !SaveSignature(png)) { MessageBeep(MB_OK); return; }
            drawing_ = false;
            strokes_.clear();
            AddPicture(png, LoadMips(png, 2400), true);
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
        case Part::Delete: items_.erase(items_.begin() + which); selected_ = -1; Changed(); return;
        case Part::Item: case Part::Resize:
            selected_ = which;
            drag_ = p;
            dragItem_ = which;
            dragX_ = x;
            dragY_ = y;
            dragMoved_ = false;
            dragStart_ = items_[size_t(which)];
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
    bool editing = editItem_ >= 0 || editField_ >= 0;
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
                if (!dragMoved_ && std::hypot(x - dragX_, y - dragY_) < 3) return 0;   // a click, not yet a drag
                dragMoved_ = true;
                Item& it = items_[size_t(dragItem_)];
                D2D1_RECT_F pr = PageRect();
                double pw = pr.right - pr.left, ph = pr.bottom - pr.top;
                double dx = (x - dragX_) / pw, dy = (y - dragY_) / ph;
                D2D1_RECT_F r = ItemRect(it);
                double iw = (r.right - r.left) / pw, ih = (r.bottom - r.top) / ph;
                if (drag_ == Part::Item) {
                    it.x = std::clamp(dragStart_.x + dx, 0.0, std::max(0.0, 1 - iw));
                    it.y = std::clamp(dragStart_.y + dy, 0.0, std::max(0.0, 1 - ih));
                } else if (it.text) {
                    // Text grows with the corner: a bigger font.
                    D2D1_RECT_F r0 = ItemRect(dragStart_);
                    double w0 = (r0.right - r0.left) / pw;
                    it.size = std::clamp(dragStart_.size * (w0 + dx) / w0, 5.0, 96.0);
                } else {
                    // Resize from the bottom right corner, keeping the shape.
                    double k = std::max((dragStart_.w + dx) / dragStart_.w, 0.03 / dragStart_.w);
                    k = std::min({ k, (1 - it.x) / dragStart_.w, (1 - it.y) / dragStart_.h });
                    it.w = dragStart_.w * k;
                    it.h = dragStart_.h * k;
                }
                Changed();
                return 0;
            }
            int which;
            Part p = HitTest(x, y, &which);
            if (p != hover_ || which != hoverWhich_) { hover_ = p; hoverWhich_ = which; Changed(); }
            return 0;
        }
        case WM_LBUTTONUP:
            if (stroking_) { stroking_ = false; ReleaseCapture(); }
            if (drag_ != Part::None) {
                // A text item clicked without moving it: type into it.
                if (!dragMoved_ && drag_ == Part::Item && dragItem_ >= 0 && items_[size_t(dragItem_)].text) { editItem_ = dragItem_; Changed(); }
                drag_ = Part::None;
                dragItem_ = -1;
                ReleaseCapture();
            }
            return 0;
        case WM_SETCURSOR: {
            LPCWSTR c = IDC_ARROW;
            if (hover_ == Part::Pad) c = IDC_CROSS;
            else if (hover_ == Part::Item) c = IDC_SIZEALL;
            else if (hover_ == Part::Resize) c = IDC_SIZENWSE;
            else if (hover_ == Part::Field && hoverWhich_ >= 0 && fields_[size_t(hoverWhich_)].type == 6) c = IDC_IBEAM;
            else if (hover_ != Part::None) c = IDC_HAND;
            SetCursor(LoadCursorW(nullptr, c));
            return TRUE;
        }
        case WM_CHAR:
            if (!editing) return 0;
            if (w == L'\b') Backspace();
            else if (w == L'\r') { if (EditingMultiline()) Type(L"\n"); else StopEditing(); }
            else if (w == L'\t') { if (editField_ >= 0) NextField(editField_); }
            else if (w >= 32 && w != 127) Type(wstring(1, wchar_t(w)));
            return 0;
        case WM_KEYDOWN: {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (w == VK_ESCAPE) {
                if (openList_ >= 0) { openList_ = -1; Changed(); }
                else if (editing) StopEditing();
                else if (drawing_) { drawing_ = false; strokes_.clear(); Changed(); }
                else { Log(L"pdf editor: closed without saving"); Hide(); }
            } else if (editing && ctrl && w == 'V' && OpenClipboard(hwnd_)) {
                if (HANDLE hd = GetClipboardData(CF_UNICODETEXT)) {
                    if (auto* text = static_cast<const wchar_t*>(GlobalLock(hd))) {
                        wstring s;
                        for (const wchar_t* c = text; *c; c++) if (*c != L'\r') s += *c;
                        Type(s);
                        GlobalUnlock(hd);
                    }
                }
                CloseClipboard();
            } else if (drawing_ && w == 'Z' && ctrl && !strokes_.empty()) { strokes_.pop_back(); Changed(); }
            else if (!editing && !drawing_ && w == VK_DELETE && selected_ >= 0) { items_.erase(items_.begin() + selected_); selected_ = -1; Changed(); }
            else if (!editing && !drawing_ && (w == VK_LEFT || w == VK_PRIOR) && page_ > 0) Go(page_ - 1);
            else if (!editing && !drawing_ && (w == VK_RIGHT || w == VK_NEXT) && page_ + 1 < pages_) Go(page_ + 1);
            return 0;
        }
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
    const wchar_t* verb = mode_ == Mode::Sign ? L"Sign " : mode_ == Mode::Fill ? L"Fill in " : L"Add a picture to ";
    wstring title = verb + FileName(pdf_);
    surf_.DrawText(Ellipsize(surf_, title, 16, DWRITE_FONT_WEIGHT_SEMI_BOLD, W - 120).c_str(), 16, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(L + 24, T + 20, W - 100, 24), th.text);
    D2D1_POINT_2F cc = D2D1::Point2F(L + W - 30, T + 30);
    if (hover_ == Part::Close) rt->FillEllipse(D2D1::Ellipse(cc, 13, 13), surf_.Solid(th.button));
    rt->DrawLine(D2D1::Point2F(cc.x - 5, cc.y - 5), D2D1::Point2F(cc.x + 5, cc.y + 5), surf_.Solid(th.text), 1.4f);
    rt->DrawLine(D2D1::Point2F(cc.x - 5, cc.y + 5), D2D1::Point2F(cc.x + 5, cc.y - 5), surf_.Solid(th.text), 1.4f);

    // The page, on a soft shadow.
    D2D1_RECT_F pr = PageRect();
    double k = PtToDip();
    if (!pageMips_.empty()) {
        surf_.DrawShadow(pr, 2, 10, 0, 2, th.shadow * 0.6f);
        rt->FillRectangle(pr, surf_.Solid(0xFFFFFFFF));
        surf_.DrawMips(pageMips_, pr);

        // Form fields: a light tint shows where to click; changed ones are redrawn over the page.
        for (size_t i = 0; i < fields_.size(); i++) {
            const Field& f = fields_[i];
            if (f.page != page_) continue;
            D2D1_RECT_F r = FieldRect(f);
            bool hot = hover_ == Part::Field && hoverWhich_ == int(i), editingThis = editField_ == int(i);
            if (f.changed || editingThis) rt->FillRectangle(r, surf_.Solid(0xFFFFFFFF));
            rt->FillRectangle(r, surf_.Solid(WithAlpha(th.accent, hot || editingThis ? 0.22 : 0.10)));
            rt->DrawRectangle(r, surf_.Solid(WithAlpha(th.accent, editingThis ? 1.0 : 0.55)), editingThis ? 1.8f : 1);
            float fh = r.bottom - r.top, fw = r.right - r.left;
            if (f.changed && (f.type == 2 || f.type == 3) && f.checked) {
                float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, s = std::min(fw, fh) * 0.32f;
                if (f.type == 3) rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), s, s), surf_.Solid(kTyped));
                else {
                    rt->DrawLine(D2D1::Point2F(cx - s, cy), D2D1::Point2F(cx - s * 0.3f, cy + s * 0.8f), surf_.Solid(kTyped), std::max(1.5f, s * 0.35f));
                    rt->DrawLine(D2D1::Point2F(cx - s * 0.3f, cy + s * 0.8f), D2D1::Point2F(cx + s, cy - s * 0.9f), surf_.Solid(kTyped), std::max(1.5f, s * 0.35f));
                }
            }
            if ((f.changed || editingThis) && (f.type == 6 || f.type == 4 || f.type == 5)) {
                float size = f.multiline ? float(10 * k) : std::min(fh * 0.62f, float(14 * k));
                rt->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
                float tx = r.left + 2 + float(k), ty = f.multiline ? r.top + 2 : r.top + (fh - size * 1.0f) / 2;
                DrawLines(rt, surf_.Solid(kTyped), f.value, size, tx, ty);
                if (editingThis) {
                    auto lines = Lines(f.value);
                    float cx = tx + TextWidth(lines.back(), size), cy = ty + 1.2f * size * float(lines.size() - 1);
                    rt->DrawLine(D2D1::Point2F(cx + 1, cy), D2D1::Point2F(cx + 1, cy + size), surf_.Solid(th.accent), 1.5f);
                }
                rt->PopAxisAlignedClip();
            }
        }

        // What was added.
        for (size_t i = 0; i < items_.size(); i++) {
            const Item& it = items_[i];
            if (it.page != page_) continue;
            D2D1_RECT_F r = ItemRect(it);
            if (it.text) {
                float size = float(it.size * k);
                DrawLines(rt, surf_.Solid(kTyped), it.words, size, r.left + 1, r.top + 1);
                if (int(i) == editItem_) {
                    auto lines = Lines(it.words);
                    float cx = r.left + 1 + TextWidth(lines.back(), size), cy = r.top + 1 + 1.2f * size * float(lines.size() - 1);
                    rt->DrawLine(D2D1::Point2F(cx + 1, cy), D2D1::Point2F(cx + 1, cy + size), surf_.Solid(th.accent), 1.5f);
                    if (it.words.empty()) surf_.DrawText(L"Type here", 11, DWRITE_FONT_WEIGHT_NORMAL, RectF(cx + 6, cy + size / 2 - 8, 120, 16), th.dim);
                }
            } else {
                surf_.DrawMips(it.pic, r);
            }
            bool hot = hover_ == Part::Item && hoverWhich_ == int(i) && drag_ == Part::None;
            if (int(i) == selected_ || hot || int(i) == editItem_)
                rt->DrawRectangle(D2D1::RectF(r.left - 2, r.top - 2, r.right + 2, r.bottom + 2), surf_.Solid(int(i) == selected_ ? th.accent : WithAlpha(th.accent, 0.5)), 1.2f);
            if (int(i) == selected_) {
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.right, r.bottom), 6, 6), surf_.Solid(th.accent));
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.right, r.bottom), 2.5f, 2.5f), surf_.Solid(0xFFFFFFFF));
                D2D1_POINT_2F dc = D2D1::Point2F(r.right, r.top);
                rt->FillEllipse(D2D1::Ellipse(dc, 9, 9), surf_.Solid(hover_ == Part::Delete ? th.error : 0xFF5A5F66));
                rt->DrawLine(D2D1::Point2F(dc.x - 3.5f, dc.y - 3.5f), D2D1::Point2F(dc.x + 3.5f, dc.y + 3.5f), surf_.Solid(0xFFFFFFFF), 1.5f);
                rt->DrawLine(D2D1::Point2F(dc.x - 3.5f, dc.y + 3.5f), D2D1::Point2F(dc.x + 3.5f, dc.y - 3.5f), surf_.Solid(0xFFFFFFFF), 1.5f);
            }
        }

        // An open list's choices.
        if (openList_ >= 0) {
            const Field& f = fields_[size_t(openList_)];
            D2D1_RECT_F r = OptionsRect();
            surf_.DrawShadow(r, 8, 12, 0, 4, th.shadow);
            rt->FillRoundedRectangle(D2D1::RoundedRect(r, 8, 8), surf_.Solid(th.glass | 0xFF000000));
            rt->DrawRoundedRectangle(D2D1::RoundedRect(r, 8, 8), surf_.Solid(th.edge), 1);
            for (size_t o = 0; o < f.options.size(); o++) {
                D2D1_RECT_F row = RectF(r.left + 4, r.top + 4 + kRowH * o, r.right - r.left - 8, kRowH);
                bool hot = hover_ == Part::Option && hoverWhich_ == int(o);
                if (hot || int(o) == f.choice) rt->FillRoundedRectangle(D2D1::RoundedRect(row, 6, 6), surf_.Solid(hot ? WithAlpha(th.accent, 0.2) : th.button));
                surf_.DrawText(f.options[o].c_str(), 12.5f, DWRITE_FONT_WEIGHT_NORMAL, RectF(row.left + 10, row.top + 5, row.right - row.left - 16, 18), th.text);
            }
        }
    }
    wstring note = note_;
    if (note.empty() && mode_ == Mode::Fill && fieldsKnown_ && fields_.empty() && items_.empty() && !pageMips_.empty())
        note = L"This PDF has no form fields. Click Add text to type anywhere on it.";
    if (!note.empty()) {
        float tw = surf_.MeasureText(note.c_str(), 13, DWRITE_FONT_WEIGHT_NORMAL);
        float cy = pageMips_.empty() ? (pr.top + pr.bottom) / 2 - 10 : pr.bottom - 30;
        if (!pageMips_.empty()) rt->FillRoundedRectangle(D2D1::RoundedRect(RectF((pr.left + pr.right) / 2 - tw / 2 - 12, cy - 6, tw + 24, 30), 8, 8), surf_.Solid(0xE0202428));
        surf_.DrawText(note.c_str(), 13, DWRITE_FONT_WEIGHT_NORMAL, RectF((pr.left + pr.right) / 2 - tw / 2, cy, tw + 4, 20), pageMips_.empty() ? th.dim : 0xFFFFFFFF);
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
    button(L + kTextB.x, by, kTextB.w, L"Add text", false, hover_ == Part::Text, !pageMips_.empty());
    button(L + kSignB.x, by, kSignB.w, L"Sign", false, hover_ == Part::Sign);
    button(L + kPicB.x, by, kPicB.w, L"Picture…", false, hover_ == Part::Picture);
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
