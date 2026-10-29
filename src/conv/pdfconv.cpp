// PDFs through PDFium: pages to pictures, text (with headings and lists recovered from font sizes),
// DOCX, EPUB and PPTX, and the PDF tools (merge, split, extract images, rotate).
#include "doc.h"
#include "pdf.h"
#include "pdfium.h"

#include <map>

namespace rd {

namespace {

/// Writes several files into a new folder (as "<name>.part" until all are there).
struct FolderOut {
    OutFile o;
    bool Begin(const Job& job, const wstring& nextTo, const wstring& base, const Progress& p, wstring& err)
    {
        if (!ReserveOutput(job, nextTo, base, L"", true, o, err)) return false;
        if (p.part) p.part(o.part);
        return true;
    }
    bool Put(const wstring& name, const std::vector<uint8_t>& data, wstring& err)
    {
        if (WriteAllBytes(LongPath(JoinPath(o.part, name)), data.data(), data.size())) return true;
        err = L"Could not write " + name + L" (is the disk full?).";
        return false;
    }
    bool End(Result& r)
    {
        if (!CommitOutput(o, r.error)) { AbandonOutput(o); return false; }
        r.out = o.final;
        return true;
    }
};

wstring PageName(const wstring& base, int page, int pages, const wchar_t* ext)
{
    // Zero-padded, so Explorer sorts "page 02" before "page 10".
    int digits = pages >= 100 ? 3 : pages >= 10 ? 2 : 1;
    wchar_t num[16];
    swprintf(num, 16, L"%0*d", digits, page);
    return base + L" page " + num + ext;
}

// ---------------------------------------------------------------- text with structure

struct Line {
    std::vector<Span> spans;      // text with bold and italic, from the fonts' names
    double size = 0, top = 0, bottom = 0;
    int boldChars = 0, chars = 0;
    wstring Text() const { wstring t; for (auto& s : spans) t += s.text; return t; }
};

std::vector<Line> PageLines(const Pdfium* p, FPDF_PAGE page)
{
    std::vector<Line> lines;
    FPDF_TEXTPAGE tp = p->FPDFText_LoadPage(page);
    if (!tp) return lines;
    int n = p->FPDFText_CountChars(tp);
    Line cur;
    std::map<int, int> sizes;   // half points -> characters, for this line
    auto add = [&](const wstring& t, bool bold, bool italic) {
        if (!cur.spans.empty() && cur.spans.back().bold == bold && cur.spans.back().italic == italic) cur.spans.back().text += t;
        else cur.spans.push_back({ t, bold, italic });
    };
    auto end = [&] {
        // Trim, and size the line by its most common font size: a big bullet or drop cap doesn't make a heading.
        while (!cur.spans.empty()) {
            wstring& t = cur.spans.front().text;
            size_t a = t.find_first_not_of(L" \t");
            if (a == wstring::npos) { cur.spans.erase(cur.spans.begin()); continue; }
            t.erase(0, a);
            break;
        }
        while (!cur.spans.empty()) {
            wstring& t = cur.spans.back().text;
            size_t z = t.find_last_not_of(L" \t");
            if (z == wstring::npos) { cur.spans.pop_back(); continue; }
            t.resize(z + 1);
            break;
        }
        int best = -1;
        for (auto& [sz, cnt] : sizes) if (cnt > best) { best = cnt; cur.size = sz / 2.0; }
        if (!cur.spans.empty()) lines.push_back(cur);
        cur = Line();
        sizes.clear();
    };
    char font[128];
    for (int i = 0; i < n; i++) {
        unsigned u = p->FPDFText_GetUnicode(tp, i);
        if (u == L'\r' || u == L'\n') { if (u == L'\n' || !cur.spans.empty()) end(); continue; }
        if (u == 0xFFFE || u == 2) continue;   // soft hyphen markers PDFium adds
        bool bold = false, italic = false;
        double l, r, b, t;
        if (p->FPDFText_GetCharBox(tp, i, &l, &r, &b, &t) && u != L' ') {
            double sz = p->FPDFText_GetFontSize(tp, i);
            if (cur.chars == 0) { cur.top = t; cur.bottom = b; }
            cur.top = std::max(cur.top, t);
            cur.bottom = std::min(cur.bottom, b);
            cur.chars++;
            bool marker = u == 0x2022 || u == 0x25CF || u == 0x25E6 || u == 0x25AA;
            if (!marker) sizes[int(std::lround(sz * 2))]++;
            int flags = 0;
            font[0] = 0;
            p->FPDFText_GetFontInfo(tp, i, font, sizeof font, &flags);
            std::string f = font;
            for (auto& c : f) c = char(tolower(uint8_t(c)));
            bold = p->FPDFText_GetFontWeight(tp, i) >= 600 || f.find("bold") != std::string::npos || f.find("black") != std::string::npos ||
                   f.find("heavy") != std::string::npos || (flags & (1 << 18)) != 0;
            italic = f.find("italic") != std::string::npos || f.find("oblique") != std::string::npos || (flags & (1 << 6)) != 0;
            if (bold) cur.boldChars++;
        } else if (!cur.spans.empty()) {
            bold = cur.spans.back().bold;   // a space keeps the style around it
            italic = cur.spans.back().italic;
        }
        wstring ch;
        if (u >= 0x10000) { u -= 0x10000; ch += wchar_t(0xD800 + (u >> 10)); ch += wchar_t(0xDC00 + (u & 0x3FF)); }
        else ch = wchar_t(u);
        add(ch, bold, italic);
    }
    end();
    p->FPDFText_ClosePage(tp);
    return lines;
}

bool ListMarker(wstring& t, bool& ordered)
{
    static const wchar_t* bullets = L"•◦▪●‣⁃·-–*";
    if (t.size() > 2 && wcschr(bullets, t[0]) && (t[1] == L' ' || t[1] == L'\t')) {
        ordered = false;
        t = t.substr(t.find_first_not_of(L" \t", 1));
        return true;
    }
    size_t d = 0;
    while (d < t.size() && d < 3 && iswdigit(t[d])) d++;
    if (d > 0 && d + 1 < t.size() && (t[d] == L'.' || t[d] == L')') && (t[d + 1] == L' ' || t[d + 1] == L'\t')) {
        ordered = true;
        t = t.substr(t.find_first_not_of(L" \t", d + 1));
        return true;
    }
    return false;
}

/// Rendered pictures of a page's image objects (forms included), as PNG, or the original JPEG bytes.
void PageImages(const Pdfium* p, FPDF_DOCUMENT doc, FPDF_PAGE page, std::vector<Block>& out)
{
    std::function<void(FPDF_PAGEOBJECT)> visit = [&](FPDF_PAGEOBJECT obj) {
        int type = p->FPDFPageObj_GetType(obj);
        if (type == FPDF_PAGEOBJ_FORM) {
            for (int i = 0, n = p->FPDFFormObj_CountObjects(obj); i < n; i++) visit(p->FPDFFormObj_GetObject(obj, unsigned(i)));
            return;
        }
        if (type != FPDF_PAGEOBJ_IMAGE || out.size() >= 500) return;
        Block b;
        b.type = Block::Picture;
        // A JPEG inside the PDF is taken as it is: no quality lost.
        // (Behind other filters such as ASCII85 too: "decoded" data stops short of the image codec.)
        char filter[32] = "";
        int filters = p->FPDFImageObj_GetImageFilterCount(obj);
        if (filters >= 1 && p->FPDFImageObj_GetImageFilter(obj, unsigned(filters - 1), filter, sizeof filter) && !strcmp(filter, "DCTDecode")) {
            auto get = filters == 1 ? p->FPDFImageObj_GetImageDataRaw : p->FPDFImageObj_GetImageDataDecoded;
            unsigned long n = get(obj, nullptr, 0);
            if (n > 0) {
                b.image.resize(n);
                get(obj, b.image.data(), n);
                if (b.image.size() > 3 && b.image[0] == 0xFF && b.image[1] == 0xD8) b.imageExt = L".jpg";
                else b.image.clear();
            }
        }
        FPDF_BITMAP bmp = p->FPDFImageObj_GetBitmap(obj);
        if (!bmp) return;
        int w = p->FPDFBitmap_GetWidth(bmp), h = p->FPDFBitmap_GetHeight(bmp), stride = p->FPDFBitmap_GetStride(bmp), fmt = p->FPDFBitmap_GetFormat(bmp);
        b.imgW = w;
        b.imgH = h;
        if (b.image.empty() && w > 1 && h > 1) {
            Image img;
            img.w = w; img.h = h;
            img.px.resize(size_t(w) * h * 4);
            const uint8_t* s = static_cast<const uint8_t*>(p->FPDFBitmap_GetBuffer(bmp));
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    uint8_t* d = &img.px[(size_t(y) * w + x) * 4];
                    const uint8_t* q = s + size_t(y) * stride;
                    if (fmt == FPDFBitmap_Gray) { d[0] = d[1] = d[2] = q[x]; d[3] = 255; }
                    else if (fmt == FPDFBitmap_BGR) { memcpy(d, q + x * 3, 3); d[3] = 255; }
                    else { memcpy(d, q + x * 4, 4); if (fmt == FPDFBitmap_BGRx) d[3] = 255; }
                }
            for (size_t i = 3; i < img.px.size(); i += 4) if (img.px[i] != 255) { img.alpha = true; break; }
            wstring err;
            if (EncodeImage(img, Kind::Png, 0, true, b.image, err)) b.imageExt = L".png";
        }
        p->FPDFBitmap_Destroy(bmp);
        if (!b.image.empty()) out.push_back(std::move(b));
    };
    (void)doc;
    for (int i = 0, n = p->FPDFPage_CountObjects(page); i < n; i++) visit(p->FPDFPage_GetObject(page, i));
}

bool ToDoc(const PdfDoc& pdf, const wstring& path, bool withImages, Doc& doc, const Progress& prog, wstring& err)
{
    const Pdfium* p = pdf.api();
    int pages = pdf.pages();
    std::vector<std::vector<Line>> all(static_cast<size_t>(pages));
    std::vector<std::vector<Block>> pictures(static_cast<size_t>(pages));
    std::map<int, int> sizes;   // font size (half points) -> characters
    for (int i = 0; i < pages; i++) {
        FPDF_PAGE page = p->FPDF_LoadPage(pdf.get(), i);
        if (!page) continue;
        all[size_t(i)] = PageLines(p, page);
        if (withImages) PageImages(p, pdf.get(), page, pictures[size_t(i)]);
        p->FPDF_ClosePage(page);
        for (auto& l : all[size_t(i)]) sizes[int(std::lround(l.size * 2))] += l.chars;
        prog.Set(0.6 * (i + 1) / std::max(1, pages));
    }
    int bodyHalf = 0, most = -1;
    for (auto& [s, n] : sizes) if (n > most) { most = n; bodyHalf = s; }
    double body = std::max(1.0, bodyHalf / 2.0);
    bool anyText = most > 0;
    if (!anyText && !withImages) { err = L"This PDF has no text in it (it may be scanned pages)."; return false; }

    for (int i = 0; i < pages; i++) {
        if (i > 0) doc.Add(Block::PageBreak);
        const auto& lines = all[size_t(i)];
        const Line* prev = nullptr;
        for (auto& l : lines) {
            double ratio = l.size / body;
            wstring text = l.Text();
            std::vector<Span> spans = l.spans;
            bool ordered = false;
            wstring stripped = text;
            bool item = ListMarker(stripped, ordered);
            if (item) {
                // Drop the marker from the spans too.
                size_t cut = text.size() - stripped.size();
                while (cut && !spans.empty()) {
                    size_t k = std::min(cut, spans.front().text.size());
                    spans.front().text.erase(0, k);
                    cut -= k;
                    if (spans.front().text.empty()) spans.erase(spans.begin());
                }
            }
            double lineH = std::max(1.0, l.top - l.bottom);
            double gap = prev ? prev->bottom - l.top : 1e9;
            int heading = 0;
            if (!item && text.size() < 140 && ratio >= 1.18) heading = ratio >= 1.75 ? 1 : ratio >= 1.4 ? 2 : 3;
            if (!heading && !item && l.chars > 0 && l.boldChars * 10 >= l.chars * 9 && text.size() < 80 && ratio >= 0.99 && gap > lineH * 0.9)
                heading = 4;   // a whole bold short line, set apart
            Block* last = doc.blocks.empty() ? nullptr : &doc.blocks.back();
            bool close = prev && gap < lineH * 0.9 && std::fabs(prev->size - l.size) < 0.6;
            auto addSpans = [&](const std::vector<Span>& sp) { for (auto& x : sp) doc.Text(x.text, x.bold && !heading, x.italic, false); };
            if (heading) {
                // A heading that wraps continues on the next line, close below and the same size.
                if (last && last->type == Block::Heading && last->level == heading && prev && gap < lineH * 0.5 && std::fabs(prev->size - l.size) < 0.6) doc.Text(L" " + text);
                else { doc.Add(Block::Heading, heading); doc.Text(text); }
            } else if (item) {
                doc.Add(Block::ListItem).ordered = ordered;
                addSpans(spans);
            } else if (last && (last->type == Block::Para || last->type == Block::ListItem) && close && !last->spans.empty()) {
                // The same paragraph: join the lines (and words hyphenated across them).
                wstring& t = last->spans.back().text;
                if (!t.empty() && t.back() == L'-' && !text.empty() && iswlower(text[0])) t.pop_back();
                else doc.Text(L" ", last->spans.back().bold, last->spans.back().italic);
                addSpans(spans);
            } else {
                doc.Add(Block::Para);
                addSpans(spans);
            }
            prev = &l;
        }
        for (auto& pic : pictures[size_t(i)]) doc.blocks.push_back(std::move(pic));
    }
    for (auto& b : doc.blocks) if (b.type == Block::Heading) { doc.title = PlainText(b); break; }
    if (doc.title.empty()) doc.title = BaseName(path);
    return true;
}

bool RunRotate(const Job& job, const Progress& p, Result& r)
{
    PdfDoc pdf;
    if (!pdf.Open(job.inputs[0], r.error)) return false;
    const Pdfium* api = pdf.api();
    for (int i = 0, n = pdf.pages(); i < n; i++) {
        FPDF_PAGE page = api->FPDF_LoadPage(pdf.get(), i);
        if (!page) continue;
        api->FPDFPage_SetRotation(page, (api->FPDFPage_GetRotation(page) + 1) % 4);
        api->FPDF_ClosePage(page);
    }
    std::vector<uint8_t> out;
    if (!SavePdf(api, pdf.get(), out)) { r.error = L"Could not save the rotated PDF."; return false; }
    r.before = FileSize(job.inputs[0]);
    return WriteOutput(job, job.inputs[0], BaseName(job.inputs[0]) + L" rotated", L".pdf", out, p, r);
}

/// "2, 5-7" -> the page numbers (1-based) in it. False, with a message, when it doesn't make sense.
bool ParsePages(const wstring& spec, int pages, std::vector<bool>& marked, wstring& err)
{
    marked.assign(size_t(pages) + 1, false);
    size_t i = 0;
    auto number = [&](int& v) {
        while (i < spec.size() && spec[i] == L' ') i++;
        if (i >= spec.size() || !iswdigit(spec[i])) return false;
        v = 0;
        while (i < spec.size() && iswdigit(spec[i])) v = std::min(v * 10 + (spec[i++] - L'0'), 1000000);
        while (i < spec.size() && spec[i] == L' ') i++;
        return true;
    };
    while (i < spec.size()) {
        int a = 0, b = 0;
        if (!number(a)) { err = L"Write the pages like 2 or 5-7, with commas between."; return false; }
        b = a;
        if (i < spec.size() && spec[i] == L'-') { i++; if (!number(b)) { err = L"A range needs two page numbers, like 5-7."; return false; } }
        if (a > b) std::swap(a, b);
        if (a < 1 || b > pages) { err = L"This PDF has " + std::to_wstring(pages) + (pages == 1 ? L" page." : L" pages."); return false; }
        for (int k = a; k <= b; k++) marked[size_t(k)] = true;
        if (i < spec.size() && spec[i] == L',') i++;
        else if (i < spec.size()) { err = L"Write the pages like 2 or 5-7, with commas between."; return false; }
    }
    return true;
}

bool RunRemovePages(const Job& job, const Progress& p, Result& r)
{
    const wstring& in = job.inputs[0];
    PdfDoc pdf;
    if (!pdf.Open(in, r.error)) return false;
    const Pdfium* api = pdf.api();
    std::vector<bool> drop;
    if (!ParsePages(job.opts[L"pages"].Str(), pdf.pages(), drop, r.error)) return false;
    // The pages that stay, as a range list ("1,3-4,8"), copied into a new PDF.
    std::string keep;
    for (int k = 1; k <= pdf.pages(); k++) {
        if (drop[size_t(k)]) continue;
        int end = k;
        while (end < pdf.pages() && !drop[size_t(end) + 1]) end++;
        keep += (keep.empty() ? "" : ",") + std::to_string(k) + (end > k ? "-" + std::to_string(end) : "");
        k = end;
    }
    if (keep.empty()) { r.error = L"That would remove every page."; return false; }
    FPDF_DOCUMENT out = api->FPDF_CreateNewDocument();
    std::vector<uint8_t> bytes;
    bool ok = out && api->FPDF_ImportPages(out, pdf.get(), keep.c_str(), 0) && SavePdf(api, out, bytes);
    if (out) api->FPDF_CloseDocument(out);
    if (!ok) { r.error = L"Could not save the PDF without those pages."; return false; }
    r.before = FileSize(in);
    return WriteOutput(job, in, BaseName(in) + L" pages removed", L".pdf", bytes, p, r);
}

/// One picture to put on a page: where, as fractions of the page as it is shown (top left 0,0).
struct Stamp {
    int page = 0;                    // 0-based
    double x = 0, y = 0, w = 0, h = 0;
    wstring image;
};

/// Puts `img` on `page` at `s` (fractions of the page as shown, whatever its /Rotate).
bool PlaceImage(const Pdfium* api, FPDF_DOCUMENT doc, FPDF_PAGE page, const Stamp& s, const Image& img)
{
    // The visible box, in the page's own (unrotated) units.
    float l = 0, b = 0, rt = 0, t = 0;
    if (!api->FPDFPage_GetCropBox(page, &l, &b, &rt, &t) && !api->FPDFPage_GetMediaBox(page, &l, &b, &rt, &t)) return false;
    double uw = rt - l, uh = t - b;
    int rot = api->FPDFPage_GetRotation(page);
    // A point on the page as shown (u right, v down, 0..1) -> the page's own coordinates.
    auto user = [&](double u, double v) -> std::pair<double, double> {
        switch (rot) {
            case 1:  return { l + v * uw, b + u * uh };     // turned a quarter clockwise
            case 2:  return { rt - u * uw, b + v * uh };
            case 3:  return { rt - v * uw, t - u * uh };
            default: return { l + u * uw, t - v * uh };
        }
    };
    // A PDF image fills the unit square, its bottom left at (0,0): map that onto the stamp's rectangle.
    auto p00 = user(s.x, s.y + s.h), p10 = user(s.x + s.w, s.y + s.h), p01 = user(s.x, s.y);
    FS_MATRIX m{ float(p10.first - p00.first), float(p10.second - p00.second), float(p01.first - p00.first), float(p01.second - p00.second),
                 float(p00.first), float(p00.second) };
    FPDF_BITMAP bmp = api->FPDFBitmap_CreateEx(img.w, img.h, FPDFBitmap_BGRA, const_cast<uint8_t*>(img.px.data()), img.w * 4);
    FPDF_PAGEOBJECT obj = api->FPDFPageObj_NewImageObj(doc);
    bool ok = bmp && obj && api->FPDFImageObj_SetBitmap(&page, 1, obj, bmp) && api->FPDFPageObj_SetMatrix(obj, &m);
    if (bmp) api->FPDFBitmap_Destroy(bmp);
    if (!ok) return false;   // (an object never inserted is freed with the document)
    api->FPDFPage_InsertObject(page, obj);
    return true;
}

/// The stamps of a job: "stamps" (from the Sign window), or one from image/page/x/y/w (the command line).
std::vector<Stamp> StampsOf(const Job& job)
{
    std::vector<Stamp> out;
    for (auto& j : job.opts[L"stamps"].arr)
        out.push_back({ int(j[L"page"].Num()), j[L"x"].Num(), j[L"y"].Num(), j[L"w"].Num(), j[L"h"].Num(), j[L"image"].Str() });
    if (out.empty() && job.opts.has(L"image"))
        out.push_back({ int(_wtof(job.opts[L"page"].Str(L"1").c_str())) - 1, _wtof(job.opts[L"x"].Str(L"0.1").c_str()), _wtof(job.opts[L"y"].Str(L"0.1").c_str()),
                        _wtof(job.opts[L"w"].Str(L"0.3").c_str()), 0, job.opts[L"image"].Str() });
    return out;
}

bool RunStamp(const Job& job, const Progress& p, Result& r)
{
    const wstring& in = job.inputs[0];
    auto stamps = StampsOf(job);
    if (stamps.empty()) { r.error = L"Nothing to put on the PDF."; return false; }
    PdfDoc pdf;
    if (!pdf.Open(in, r.error)) return false;
    const Pdfium* api = pdf.api();
    for (size_t i = 0; i < stamps.size(); i++) {
        Stamp& s = stamps[i];
        if (s.page < 0 || s.page >= pdf.pages()) { r.error = L"This PDF has " + std::to_wstring(pdf.pages()) + L" pages."; return false; }
        Image img;
        std::vector<uint8_t> bytes;
        if (!DecodeImage(s.image, img, bytes, r.error)) return false;
        FPDF_PAGE page = api->FPDF_LoadPage(pdf.get(), s.page);
        if (!page) { r.error = L"Could not open page " + std::to_wstring(s.page + 1) + L"."; return false; }
        if (s.h <= 0) {
            // Keep the picture's shape on the page as shown (a turned page swaps width and height).
            double pw = api->FPDF_GetPageWidthF(page), ph = api->FPDF_GetPageHeightF(page);
            s.h = s.w * img.h / img.w * pw / ph;
        }
        bool ok = PlaceImage(api, pdf.get(), page, s, img) && api->FPDFPage_GenerateContent(page);
        api->FPDF_ClosePage(page);
        if (!ok) { r.error = L"Could not put the picture on page " + std::to_wstring(s.page + 1) + L"."; return false; }
        p.Set(0.8 * double(i + 1) / double(stamps.size()));
    }
    std::vector<uint8_t> out;
    if (!SavePdf(api, pdf.get(), out)) { r.error = L"Could not save the PDF."; return false; }
    r.before = FileSize(in);
    return WriteOutput(job, in, BaseName(in) + (job.op == L"pdf.sign" ? L" signed" : L" edited"), L".pdf", out, p, r);
}

bool RunSplit(const Job& job, const Progress& p, Result& r)
{
    const wstring& in = job.inputs[0];
    PdfDoc pdf;
    if (!pdf.Open(in, r.error)) return false;
    const Pdfium* api = pdf.api();
    int pages = pdf.pages();
    if (pages < 2) { r.error = L"This PDF has only one page."; return false; }
    FolderOut f;
    if (!f.Begin(job, in, BaseName(in) + L" pages", p, r.error)) return false;
    for (int i = 0; i < pages; i++) {
        FPDF_DOCUMENT one = api->FPDF_CreateNewDocument();
        std::string range = std::to_string(i + 1);
        std::vector<uint8_t> out;
        bool ok = one && api->FPDF_ImportPages(one, pdf.get(), range.c_str(), 0) && SavePdf(api, one, out);
        if (one) api->FPDF_CloseDocument(one);
        if (!ok || !f.Put(PageName(BaseName(in), i + 1, pages, L".pdf"), out, r.error)) {
            if (r.error.empty()) r.error = L"Could not split page " + std::to_wstring(i + 1) + L".";
            AbandonOutput(f.o);
            return false;
        }
        p.Set(0.98 * (i + 1) / pages);
    }
    return f.End(r);
}

bool RunImages(const Job& job, const Progress& p, Result& r)
{
    const wstring& in = job.inputs[0];
    PdfDoc pdf;
    if (!pdf.Open(in, r.error)) return false;
    const Pdfium* api = pdf.api();
    std::vector<Block> pics;
    for (int i = 0, n = pdf.pages(); i < n; i++) {
        FPDF_PAGE page = api->FPDF_LoadPage(pdf.get(), i);
        if (!page) continue;
        PageImages(api, pdf.get(), page, pics);
        api->FPDF_ClosePage(page);
        p.Set(0.6 * (i + 1) / n);
    }
    if (pics.empty()) { r.error = L"This PDF has no pictures in it."; return false; }
    FolderOut f;
    if (!f.Begin(job, in, BaseName(in) + L" images", p, r.error)) return false;
    for (size_t i = 0; i < pics.size(); i++) {
        int digits = pics.size() >= 100 ? 3 : pics.size() >= 10 ? 2 : 1;
        wchar_t num[16];
        swprintf(num, 16, L"%0*d", digits, int(i + 1));
        if (!f.Put(BaseName(in) + L" image " + num + pics[i].imageExt, pics[i].image, r.error)) {
            AbandonOutput(f.o);
            return false;
        }
    }
    return f.End(r);
}

}  // namespace

// ---------------------------------------------------------------- public

bool PdfPreview(const wstring& path, int maxSide, Image& img, wstring& err, int page, int* pages)
{
    PdfDoc pdf;
    if (!pdf.Open(path, err)) return false;
    if (pdf.pages() < 1) { err = L"This PDF has no pages."; return false; }
    if (pages) *pages = pdf.pages();
    return RenderPage(pdf, std::clamp(page, 0, pdf.pages() - 1), 300, img, err, maxSide);
}

bool RunPdfTo(const Job& job, const wstring& target, const Progress& p, Result& r)
{
    const wstring& in = job.inputs[0];
    PdfDoc pdf;
    if (!pdf.Open(in, r.error)) return false;
    r.before = FileSize(in);
    int pages = pdf.pages();
    if (pages < 1) { r.error = L"This PDF has no pages."; return false; }
    wstring base = BaseName(in);

    if (target == L"png" || target == L"jpg") {
        // Every page at the chosen DPI: one file for a one-page PDF, else a folder named after it.
        double dpi = std::clamp(job.opts[L"dpi"].Num(300), 36.0, 1200.0);
        Kind k = target == L"png" ? Kind::Png : Kind::Jpeg;
        int q = int(job.opts[L"jpegQ"].Num(88));
        const wchar_t* ext = target == L"png" ? L".png" : L".jpg";
        if (pages == 1) {
            Image img;
            std::vector<uint8_t> out;
            if (!RenderPage(pdf, 0, dpi, img, r.error) || !EncodeImage(img, k, q, true, out, r.error)) return false;
            return WriteOutput(job, in, base, ext, out, p, r);
        }
        FolderOut f;
        if (!f.Begin(job, in, base, p, r.error)) return false;
        for (int i = 0; i < pages; i++) {
            Image img;
            std::vector<uint8_t> out;
            if (!RenderPage(pdf, i, dpi, img, r.error) || !EncodeImage(img, k, q, true, out, r.error) || !f.Put(PageName(base, i + 1, pages, ext), out, r.error)) {
                AbandonOutput(f.o);
                return false;
            }
            p.Set(0.98 * (i + 1) / pages);
        }
        return f.End(r);
    }
    if (target == L"pptx") {
        std::vector<Image> slides;
        for (int i = 0; i < pages; i++) {
            Image img;
            if (!RenderPage(pdf, i, 150, img, r.error, 2400)) return false;
            slides.push_back(std::move(img));
            p.Set(0.8 * (i + 1) / pages);
        }
        std::vector<uint8_t> out;
        if (!WritePptx(slides, out, r.error)) return false;
        return WriteOutput(job, in, base, L".pptx", out, p, r);
    }
    Doc doc;
    bool images = target == L"docx" || target == L"epub";
    if (target == L"mp3") {
        // Read aloud: the text as for TXT, spoken by Windows' voice, then made an MP3.
        Progress quiet;   // reading the text is quick next to speaking it
        if (!ToDoc(pdf, in, false, doc, quiet, r.error)) return false;
        wstring text = Widen(WriteText(doc));
        if (text.find_first_not_of(L" \t\r\n") == wstring::npos) { r.error = L"This PDF has no text to read out (it may be scanned pages)."; return false; }
        wstring wav = JoinPath(TempDir(), L"speech-" + std::to_wstring(GetCurrentProcessId()) + L".wav");
        std::vector<uint8_t> mp3;
        bool ok = SpeakToWav(text, wav, p, r.error) && WavToMp3(wav, mp3, p, r.error);
        DeleteFileW(wav.c_str());
        return ok && WriteOutput(job, in, base, L".mp3", mp3, p, r);
    }
    if (!ToDoc(pdf, in, images, doc, p, r.error)) return false;
    std::vector<uint8_t> out;
    std::string text;
    if (target == L"txt") text = WriteText(doc);
    else if (target == L"md") text = WriteMarkdown(doc);
    else if (target == L"docx") { if (!WriteDocx(doc, out)) { r.error = L"Could not write the Word document."; return false; } }
    else if (target == L"epub") { if (!WriteEpub(doc, out)) { r.error = L"Could not write the e-book."; return false; } }
    else { r.error = L"Roda can't turn a PDF into that."; return false; }
    if (out.empty()) {
        if (target == L"txt") out = { 0xEF, 0xBB, 0xBF };   // a BOM: Notepad and Excel then read it as UTF-8
        out.insert(out.end(), text.begin(), text.end());
    }
    return WriteOutput(job, in, base, L"." + target, out, p, r);
}

bool RunPdfTool(const Job& job, const Progress& p, Result& r)
{
    if (job.op == L"pdf.split") return RunSplit(job, p, r);
    if (job.op == L"pdf.images") return RunImages(job, p, r);
    if (job.op == L"pdf.rotate") return RunRotate(job, p, r);
    if (job.op == L"pdf.remove") return RunRemovePages(job, p, r);
    if (job.op == L"pdf.sign" || job.op == L"pdf.picture") return RunStamp(job, p, r);
    r.error = L"Unknown PDF tool.";
    return false;
}

bool MergeWithPdfium(const Job& job, const Progress& p, Result& r)
{
    wstring err;
    const Pdfium* api = LoadPdfium(err);
    if (!api) { r.error = err; return false; }
    FPDF_DOCUMENT dest = api->FPDF_CreateNewDocument();
    if (!dest) { r.error = L"Could not start the PDF."; return false; }
    bool ok = true;
    int jq = int(job.opts[L"jpegQ"].Num(88));
    for (size_t i = 0; i < job.inputs.size() && ok; i++) {
        const wstring& in = job.inputs[i];
        PdfDoc src;
        if (KindOfPath(in) == Kind::Pdf) ok = src.Open(in, err);
        else {
            // A picture becomes a one-page PDF first, then joins like any other.
            Image img;
            std::vector<uint8_t> bytes, page;
            PdfWriter w;
            ok = DecodeImage(in, img, bytes, err);
            if (ok) { w.AddImage(img, img.kind == Kind::Jpeg ? &bytes : nullptr, jq); ok = w.Finish(page, err) && src.OpenMemory(std::move(page), err); }
        }
        if (ok) ok = api->FPDF_ImportPages(dest, src.get(), nullptr, api->FPDF_GetPageCount(dest)) != 0;
        if (!ok) r.error = FileName(in) + L": " + (err.empty() ? L"could not be added." : err);
        r.before += FileSize(in);
        p.Set(0.9 * (i + 1) / job.inputs.size());
    }
    std::vector<uint8_t> out;
    if (ok && !SavePdf(api, dest, out)) { ok = false; r.error = L"Could not save the merged PDF."; }
    api->FPDF_CloseDocument(dest);
    if (!ok) return false;
    return WriteOutput(job, job.inputs[0], BaseName(job.inputs[0]) + L" merged", L".pdf", out, p, r);
}

}  // namespace rd
