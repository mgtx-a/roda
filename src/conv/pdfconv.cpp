// PDFs through PDFium: pages to pictures, text (with headings and lists recovered from font sizes),
// DOCX, EPUB and PPTX, and the PDF tools (merge, split, extract images, rotate).
#include "doc.h"
#include "pdf.h"
#include "pdfium.h"

#include <map>
#include <set>
#include <tuple>

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

/// Bold and italic from a character's font (its weight, name and flags), and the font's family ("Arial").
void CharStyle(const Pdfium* p, FPDF_TEXTPAGE tp, int i, bool& bold, bool& italic, wstring* family = nullptr)
{
    char font[128] = "";
    int flags = 0;
    p->FPDFText_GetFontInfo(tp, i, font, sizeof font, &flags);
    std::string f = font;
    for (auto& c : f) c = char(tolower(uint8_t(c)));
    bold = p->FPDFText_GetFontWeight(tp, i) >= 600 || f.find("bold") != std::string::npos || f.find("black") != std::string::npos ||
           f.find("heavy") != std::string::npos || (flags & (1 << 18)) != 0;
    italic = f.find("italic") != std::string::npos || f.find("oblique") != std::string::npos || (flags & (1 << 6)) != 0;
    if (!family) return;
    // "ABCDEF+Arial-BoldMT" -> "Arial": no subset tag, no style, no foundry suffix.
    std::string name = font;
    if (name.size() > 7 && name[6] == '+') name.erase(0, 7);
    name = name.substr(0, name.find_first_of("-,"));
    for (const char* tail : { "MT", "PS", "PSMT" })
        if (name.size() > strlen(tail) + 2 && name.compare(name.size() - strlen(tail), std::string::npos, tail) == 0) name.resize(name.size() - strlen(tail));
    if (name.empty() || name == "Helvetica") name = "Arial";   // what Windows has for it
    *family = Widen(name);
}

/// A character's font size as the page shows it. PDFium gives the font's own size, before the text is
/// stretched (some PDFs draw all text at size 1 and stretch it to 7): then the letter's height says it.
double ShownSize(const Pdfium* p, FPDF_TEXTPAGE tp, int i)
{
    double size = p->FPDFText_GetFontSize(tp, i);
    FS_RECTF box{};
    if (!p->FPDFText_GetLooseCharBox(tp, i, &box)) return std::max(1.0, size);
    double fromBox = (box.top - box.bottom) / 1.12;   // ascent and descent: about 1.12 sizes in common fonts
    return fromBox > 0 && (size < fromBox * 0.6 || size > fromBox * 1.6) ? fromBox : std::max(1.0, size);
}

/// True for the second copy of a character drawn twice in the same place (some web pages print every
/// letter twice, or fake bold that way): it must not come out twice.
bool Repeated(std::set<std::tuple<long, long, unsigned>>& seen, unsigned u, double l, double b)
{
    return !seen.insert({ std::lround(l * 4), std::lround(b * 4), u }).second;
}

std::vector<Line> PageLines(const Pdfium* p, FPDF_PAGE page)
{
    std::vector<Line> lines;
    FPDF_TEXTPAGE tp = p->FPDFText_LoadPage(page);
    if (!tp) return lines;
    int n = p->FPDFText_CountChars(tp);
    std::set<std::tuple<long, long, unsigned>> seen;
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
    bool skipping = false;   // inside a repeated copy: its spaces go too
    for (int i = 0; i < n; i++) {
        unsigned u = p->FPDFText_GetUnicode(tp, i);
        if (u == L'\r' || u == L'\n') { if (u == L'\n' || !cur.spans.empty()) end(); continue; }
        if (u == 0xFFFE || u == 2) continue;   // soft hyphen markers PDFium adds
        bool bold = false, italic = false;
        double l, r, b, t;
        bool boxed = p->FPDFText_GetCharBox(tp, i, &l, &r, &b, &t);
        if (boxed && u != L' ') skipping = Repeated(seen, u, l, b);
        if (skipping) continue;
        if (boxed && u != L' ') {
            double sz = ShownSize(p, tp, i);
            if (cur.chars == 0) { cur.top = t; cur.bottom = b; }
            cur.top = std::max(cur.top, t);
            cur.bottom = std::min(cur.bottom, b);
            cur.chars++;
            bool marker = u == 0x2022 || u == 0x25CF || u == 0x25E6 || u == 0x25AA;
            if (!marker) sizes[int(std::lround(sz * 2))]++;
            CharStyle(p, tp, i, bold, italic);
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

/// An image object's picture as PNG, or its original JPEG bytes; false when it has none.
bool ImageBytes(const Pdfium* p, FPDF_PAGEOBJECT obj, Block& b)
{
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
    if (!bmp) return false;
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
    return !b.image.empty();
}

/// Rendered pictures of a page's image objects (forms included), each once: a page that draws a picture
/// twice in the same place still has one picture.
void PageImages(const Pdfium* p, FPDF_DOCUMENT doc, FPDF_PAGE page, std::vector<Block>& out)
{
    std::set<std::vector<uint8_t>> seen;
    std::function<void(FPDF_PAGEOBJECT)> visit = [&](FPDF_PAGEOBJECT obj) {
        int type = p->FPDFPageObj_GetType(obj);
        if (type == FPDF_PAGEOBJ_FORM) {
            for (int i = 0, n = p->FPDFFormObj_CountObjects(obj); i < n; i++) visit(p->FPDFFormObj_GetObject(obj, unsigned(i)));
            return;
        }
        if (type != FPDF_PAGEOBJ_IMAGE || out.size() >= 500) return;
        Block b;
        if (ImageBytes(p, obj, b) && seen.insert(b.image).second) out.push_back(std::move(b));
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

/// One character on the page: where (points from the page's top left, y down) and how it looks.
struct Glyph {
    double l = 0, r = 0, t = 0, b = 0, size = 1;
    wstring c, font;
    bool bold = false, italic = false;
    uint32_t color = 0;
};

/// A straight line drawn on the page (y down), or a shaded box.
struct Rule { double x0, y0, x1, y1; uint32_t color; };

/// Positions within `tol` of the one before become one: the edges of a sheet's grid.
std::vector<double> Edges(std::vector<double> v, double tol)
{
    std::sort(v.begin(), v.end());
    std::vector<double> out;
    for (double x : v) if (out.empty() || x - out.back() > tol) out.push_back(x);
    return out;
}

/// The grid slot x falls in: the last edge at or before x + tol.
size_t Slot(const std::vector<double>& edges, double x, double tol)
{
    size_t i = size_t(std::upper_bound(edges.begin(), edges.end(), x + tol) - edges.begin());
    return i ? i - 1 : 0;
}

/// The edge nearest to x.
size_t Nearest(const std::vector<double>& edges, double x)
{
    size_t k = Slot(edges, x, 0);
    return k + 1 < edges.size() && edges[k + 1] - x < x - edges[k] ? k + 1 : k;
}

/// A piece of text on the page: where (points, y down) and its parts, each in its own style.
struct Run { double l, r, t, b; wstring text; std::vector<PageSheet::Piece> pieces; };

/// What a page holds, read once: its size, its pieces of text, the lines and boxes drawn, its pictures.
struct PageContent {
    double w = 1, h = 1;
    std::vector<Run> runs;
    std::vector<Rule> hs, vs, boxes;
    struct Pic { double x0, y0, x1, y1; Block b; };
    std::vector<Pic> pics;
};

/// Reads a page's text (each character once, invisible text left out), lines, boxes and pictures.
PageContent ReadPage(const Pdfium* p, FPDF_PAGE page)
{
    PageContent pc;
    float L = 0, B = 0, R = 0, T = 0;
    if (!p->FPDFPage_GetCropBox(page, &L, &B, &R, &T) && !p->FPDFPage_GetMediaBox(page, &L, &B, &R, &T)) { R = p->FPDF_GetPageWidthF(page); T = p->FPDF_GetPageHeightF(page); }
    // shortcut: the page is read as stored, ignoring /Rotate; a turned page comes out on its side. Turn it when one turns up.
    auto across = [&](double x) { return x - L; };
    auto down = [&](double y) { return T - y; };

    // Characters, each once.
    std::vector<Glyph> glyphs;
    if (FPDF_TEXTPAGE tp = p->FPDFText_LoadPage(page)) {
        std::set<std::tuple<long, long, unsigned>> seen;
        for (int i = 0, n = p->FPDFText_CountChars(tp); i < n; i++) {
            unsigned u = p->FPDFText_GetUnicode(tp, i);
            // The loose box spans the letter's whole width (its advance), so the gaps between boxes are real spaces.
            FS_RECTF box{};
            if (u <= L' ' || u == 0xFFFE || u == 0xA0 || !p->FPDFText_GetLooseCharBox(tp, i, &box)) continue;
            if (Repeated(seen, u, box.left, box.bottom)) continue;
            // Invisible text (the words found in a scanned page) shows nothing: the scan shows them.
            FPDF_PAGEOBJECT obj = p->FPDFText_GetTextObject(tp, i);
            if (obj && p->FPDFTextObj_GetTextRenderMode(obj) == FPDF_TEXTRENDERMODE_INVISIBLE) continue;
            Glyph g;
            g.l = across(box.left); g.r = across(box.right); g.t = down(box.top); g.b = down(box.bottom);
            g.size = ShownSize(p, tp, i);
            if (u >= 0x10000) { u -= 0x10000; g.c += wchar_t(0xD800 + (u >> 10)); g.c += wchar_t(0xDC00 + (u & 0x3FF)); }
            else g.c = wchar_t(u);
            CharStyle(p, tp, i, g.bold, g.italic, &g.font);
            unsigned cr = 0, cg = 0, cb = 0, ca = 0;
            if (p->FPDFText_GetFillColor(tp, i, &cr, &cg, &cb, &ca)) g.color = cr << 16 | cg << 8 | cb;
            glyphs.push_back(std::move(g));
        }
        p->FPDFText_ClosePage(tp);
    }

    // Lines drawn (thin filled bars, or the sides of outlined boxes), shaded boxes and pictures, forms included.
    auto &hs = pc.hs, &vs = pc.vs, &boxes = pc.boxes;
    auto& pics = pc.pics;
    std::function<void(FPDF_PAGEOBJECT, const FS_MATRIX&)> visit = [&](FPDF_PAGEOBJECT obj, const FS_MATRIX& m) {
        int type = p->FPDFPageObj_GetType(obj);
        if (type == FPDF_PAGEOBJ_FORM) {
            // What a form holds is in its own space: its matrix, then the one around it.
            FS_MATRIX f{};
            if (!p->FPDFPageObj_GetMatrix(obj, &f)) return;
            FS_MATRIX c{ f.a * m.a + f.b * m.c, f.a * m.b + f.b * m.d, f.c * m.a + f.d * m.c, f.c * m.b + f.d * m.d,
                         f.e * m.a + f.f * m.c + m.e, f.e * m.b + f.f * m.d + m.f };
            for (int i = 0, n = p->FPDFFormObj_CountObjects(obj); i < n; i++) visit(p->FPDFFormObj_GetObject(obj, unsigned(i)), c);
            return;
        }
        if (type != FPDF_PAGEOBJ_IMAGE && type != FPDF_PAGEOBJ_PATH) return;
        float bl = 0, bb = 0, br = 0, bt = 0;
        if (!p->FPDFPageObj_GetBounds(obj, &bl, &bb, &br, &bt)) return;
        double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
        for (auto [x, y] : { std::pair<double, double>{ bl, bb }, { br, bb }, { bl, bt }, { br, bt } }) {
            double X = across(m.a * x + m.c * y + m.e), Y = down(m.b * x + m.d * y + m.f);
            x0 = std::min(x0, X); x1 = std::max(x1, X); y0 = std::min(y0, Y); y1 = std::max(y1, Y);
        }
        if (type == FPDF_PAGEOBJ_IMAGE) {
            PageContent::Pic pic{ x0, y0, x1, y1, {} };
            if (pics.size() < 500 && ImageBytes(p, obj, pic.b)) pics.push_back(std::move(pic));
            return;
        }
        int fillMode = 0;
        FPDF_BOOL stroke = 0;
        p->FPDFPath_GetDrawMode(obj, &fillMode, &stroke);
        unsigned r = 0, g = 0, b = 0, a = 0;
        bool painted = fillMode ? p->FPDFPageObj_GetFillColor(obj, &r, &g, &b, &a) : stroke && p->FPDFPageObj_GetStrokeColor(obj, &r, &g, &b, &a);
        if (!painted || a == 0 || (r > 245 && g > 245 && b > 245)) return;   // white on white paper shows nothing
        uint32_t color = r << 16 | g << 8 | b;
        double w = x1 - x0, h = y1 - y0;
        if (h <= 2 && w >= 3) hs.push_back({ x0, (y0 + y1) / 2, x1, (y0 + y1) / 2, color });
        else if (w <= 2 && h >= 3) vs.push_back({ (x0 + x1) / 2, y0, (x0 + x1) / 2, y1, color });
        else if (w > 2 && h > 2 && p->FPDFPath_CountSegments(obj) <= 5) {
            // A rectangle: shaded, it fills cells; outlined, its sides are lines.
            if (fillMode) boxes.push_back({ x0, y0, x1, y1, color });
            else {
                hs.push_back({ x0, y0, x1, y0, color }); hs.push_back({ x0, y1, x1, y1, color });
                vs.push_back({ x0, y0, x0, y1, color }); vs.push_back({ x1, y0, x1, y1, color });
            }
        }
    };
    const FS_MATRIX identity{ 1, 0, 0, 1, 0, 0 };
    for (int i = 0, n = p->FPDFPage_CountObjects(page); i < n; i++) visit(p->FPDFPage_GetObject(page, i), identity);

    // Lines of text: top to bottom, a character joins the line whose middle is within half its size.
    std::sort(glyphs.begin(), glyphs.end(), [](const Glyph& a, const Glyph& b) { return a.t + a.b < b.t + b.b; });
    std::vector<std::vector<Glyph>> lines;
    double mid = -1e9;
    for (auto& g : glyphs) {
        double m = (g.t + g.b) / 2;
        if (lines.empty() || std::fabs(m - mid) > g.size * 0.5) { lines.emplace_back(); mid = m; }
        lines.back().push_back(std::move(g));
    }

    // Pieces of text: a gap wider than about a letter, or a line drawn between, starts a new piece.
    auto& runs = pc.runs;
    auto ruleBetween = [&](double a, double c, double t, double b) {
        for (auto& v : vs) if (v.x0 > a && v.x0 < c && v.y0 < b && v.y1 > t) return true;
        return false;
    };
    for (auto& line : lines) {
        std::sort(line.begin(), line.end(), [](const Glyph& a, const Glyph& b) { return a.l < b.l; });
        for (size_t i = 0; i < line.size(); i++) {
            const Glyph& g = line[i];
            double gap = i ? g.l - line[i - 1].r : 1e9;
            if (gap > g.size * 0.9 || ruleBetween(line[i - 1].r - 0.5, g.l + 0.5, g.t, g.b)) runs.push_back({ g.l, g.r, g.t, g.b, L"", {} });
            Run& run = runs.back();
            wstring add = (!run.text.empty() && gap > g.size * 0.12 ? L" " : L"") + g.c;
            run.text += add;
            run.r = std::max(run.r, g.r); run.t = std::min(run.t, g.t); run.b = std::max(run.b, g.b);
            auto& ps = run.pieces;
            if (!ps.empty() && ps.back().font == g.font && ps.back().bold == g.bold && ps.back().italic == g.italic && ps.back().color == g.color &&
                std::fabs(ps.back().size - g.size) < 0.3) ps.back().text += add;
            else ps.push_back({ add, g.font, g.size, g.bold, g.italic, g.color });
        }
    }
    pc.w = std::max(1.0, double(R - L));
    pc.h = std::max(1.0, double(T - B));
    return pc;
}

/// A page as a sheet that looks like it (see PageSheet). The grid's edges are where the page's lines are
/// and where its pieces of text start, so every piece lands in a cell of its own and the lines become
/// the cells' borders. Numbers also end on an edge, so they can sit right-aligned where they were.
PageSheet PageLayout(PageContent pc)
{
    PageSheet sheet;
    auto &runs = pc.runs;
    auto &hs = pc.hs, &vs = pc.vs, &boxes = pc.boxes;
    auto& pics = pc.pics;
    auto numeric = [](const wstring& s) {
        return s.find_first_of(L"0123456789") != wstring::npos && s.find_first_not_of(L"0123456789.,()-%") == wstring::npos;
    };

    // The grid: every line drawn, every piece of text's top and bottom. A piece that crosses a row edge (a
    // ":" centred beside a label and its translation, a big word beside small ones) spans those rows.
    const double tolX = 2, tolY = 1.5;
    double pageW = pc.w, pageH = pc.h;
    std::vector<double> xs{ 0 }, ys{ 0 };
    for (auto& h : hs) { xs.push_back(h.x0); xs.push_back(h.x1); ys.push_back(h.y0); }
    for (auto& v : vs) { xs.push_back(v.x0); ys.push_back(v.y0); ys.push_back(v.y1); }
    for (auto& b : boxes) { xs.push_back(b.x0); xs.push_back(b.x1); ys.push_back(b.y0); ys.push_back(b.y1); }
    for (auto& pic : pics) { xs.push_back(pic.x0); ys.push_back(pic.y0); }
    for (auto& run : runs) {
        xs.push_back(run.l);
        if (numeric(run.text)) xs.push_back(run.r);
        ys.push_back(run.t); ys.push_back(run.b);
    }
    xs = Edges(xs, tolX);
    ys = Edges(ys, tolY);
    for (size_t i = 0; i < xs.size(); i++) sheet.colW.push_back(std::max(0.5, (i + 1 < xs.size() ? xs[i + 1] : std::max(pageW, xs[i] + 8)) - xs[i]));
    for (size_t i = 0; i < ys.size(); i++) sheet.rowH.push_back(std::max(0.75, (i + 1 < ys.size() ? ys[i + 1] : std::max(pageH, ys[i] + 8)) - ys[i]));

    // Text into cells; two pieces that fall in one cell share it.
    std::map<std::pair<size_t, size_t>, size_t> at;
    for (auto& run : runs) {
        size_t row = Slot(ys, run.t, tolY), col = Slot(xs, run.l, tolX);
        auto [it, fresh] = at.insert({ { row, col }, sheet.texts.size() });
        if (!fresh) {
            auto& pieces = sheet.texts[it->second].pieces;
            run.pieces.front().text.insert(0, L" ");
            pieces.insert(pieces.end(), run.pieces.begin(), run.pieces.end());
            continue;
        }
        PageSheet::Text t;
        t.row = row;
        t.col = col;
        // A number sits right-aligned when it ends near a line on its right, or isn't in a box at all;
        // one that starts against the line on its left (a year in its box) sits left.
        auto lineNear = [&](double x, double within) {
            for (auto& v : vs) if (std::fabs(v.x0 - x) < within && v.y0 < run.b && v.y1 > run.t) return true;
            return false;
        };
        t.right = numeric(run.text) && (lineNear(run.r, 8) || !lineNear(run.l, 4));
        t.rowEnd = std::max(row, Slot(ys, run.b - tolY - 0.01, 0));
        // Text that spans rows can't spill sideways in Excel: its cell reaches as far as the text does.
        t.colEnd = t.right ? std::max(col, Slot(xs, run.r - tolX - 0.01, 0)) : t.rowEnd > row ? std::max(col, Slot(xs, run.r, 0)) : col;
        t.pieces = std::move(run.pieces);
        sheet.texts.push_back(std::move(t));
    }

    // Lines as borders, on the cells beside them; of two lines drawn together (a bevel), the darker.
    auto mark = [&](size_t row, size_t col, uint8_t side, uint32_t color) {
        auto& e = sheet.borders[{ row, col }];
        auto dark = [](uint32_t c) { return (c >> 16 & 255) + (c >> 8 & 255) + (c & 255); };
        if (!(e.sides & side) || dark(color) < dark(e.color)) e.color = color;
        e.sides |= side;
    };
    for (auto& h : hs) {
        size_t k = Nearest(ys, h.y0);
        bool above = ys[k] < h.y0 - tolY;   // the nearest edge is above the line: it is the bottom of that row
        for (size_t c = Slot(xs, h.x0, tolX); c < xs.size() && xs[c] < h.x1 - tolX; c++) mark(k, c, above ? 8 : 4, h.color);
    }
    for (auto& v : vs) {
        size_t j = Nearest(xs, v.x0);
        bool left = xs[j] < v.x0 - tolX;
        for (size_t r = Slot(ys, v.y0, tolY); r < ys.size() && ys[r] < v.y1 - tolY; r++) mark(r, j, left ? 2 : 1, v.color);
    }
    for (auto& b : boxes)
        for (size_t r = Slot(ys, b.y0, tolY); r < ys.size() && ys[r] < b.y1 - tolY; r++)
            for (size_t c = Slot(xs, b.x0, tolX); c < xs.size() && xs[c] < b.x1 - tolX; c++) sheet.fills[{ r, c }] = b.color;

    // Pictures where they were, each once.
    std::set<std::tuple<long, long, std::vector<uint8_t>>> seenPics;
    for (auto& pic : pics) {
        if (!seenPics.insert({ std::lround(pic.x0), std::lround(pic.y0), pic.b.image }).second) continue;
        size_t c = Slot(xs, pic.x0, 0), r = Slot(ys, pic.y0, 0);
        sheet.pictures.push_back({ r, c, std::max(0.0, pic.x0 - xs[c]), std::max(0.0, pic.y0 - ys[r]), pic.x1 - pic.x0, pic.y1 - pic.y0,
                                   std::move(pic.b.image), pic.b.imageExt });
    }
    return sheet;
}

/// A page's visible box and turn: converts between the page as shown (u right, v down, 0..1, what
/// the Sign window works in) and the page's own coordinates (points, y up, before /Rotate).
struct PageBox {
    double l = 0, b = 0, r = 0, t = 0;
    int rot = 0;
    PageBox(const Pdfium* api, FPDF_PAGE page)
    {
        float L = 0, B = 0, R = 0, T = 0;
        if (api->FPDFPage_GetCropBox(page, &L, &B, &R, &T) || api->FPDFPage_GetMediaBox(page, &L, &B, &R, &T)) { l = L; b = B; r = R; t = T; }
        else { r = api->FPDF_GetPageWidthF(page); t = api->FPDF_GetPageHeightF(page); }
        rot = api->FPDFPage_GetRotation(page) & 3;
    }
    double uw() const { return r - l; }
    double uh() const { return t - b; }
    double shownW() const { return rot & 1 ? uh() : uw(); }   // points, as shown
    double shownH() const { return rot & 1 ? uw() : uh(); }
    std::pair<double, double> User(double u, double v) const
    {
        switch (rot) {
            case 1:  return { l + v * uw(), b + u * uh() };   // turned a quarter clockwise
            case 2:  return { r - u * uw(), b + v * uh() };
            case 3:  return { r - v * uw(), t - u * uh() };
            default: return { l + u * uw(), t - v * uh() };
        }
    }
    std::pair<double, double> Shown(double x, double y) const
    {
        switch (rot) {
            case 1:  return { (y - b) / uh(), (x - l) / uw() };
            case 2:  return { (r - x) / uw(), (y - b) / uh() };
            case 3:  return { (t - y) / uh(), (r - x) / uw() };
            default: return { (x - l) / uw(), (t - y) / uh() };
        }
    }
};

/// One picture to put on a page: where, as fractions of the page as it is shown (top left 0,0).
struct Stamp {
    int page = 0;                    // 0-based
    double x = 0, y = 0, w = 0, h = 0;
    wstring image;
};

/// Puts `img` on `page` at `s`, upright on the page as shown.
bool PlaceImage(const Pdfium* api, FPDF_DOCUMENT doc, FPDF_PAGE page, const Stamp& s, const Image& img)
{
    PageBox box(api, page);
    // A PDF image fills the unit square, its bottom left at (0,0): map that onto the stamp's rectangle.
    auto p00 = box.User(s.x, s.y + s.h), p10 = box.User(s.x + s.w, s.y + s.h), p01 = box.User(s.x, s.y);
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

/// Typed text, one object per line, in Helvetica (every PDF reader has it): `size` points, its
/// top left at (x, y) on the page as shown.
bool PlaceText(const Pdfium* api, FPDF_DOCUMENT doc, FPDF_PAGE page, double x, double y, double size, const wstring& text)
{
    PageBox box(api, page);
    auto o = box.User(0, 0), ru = box.User(1, 0), dv = box.User(0, 1);
    double rx = (ru.first - o.first) / box.shownW(), ry = (ru.second - o.second) / box.shownW();   // one point to the right
    double ux = (o.first - dv.first) / box.shownH(), uy = (o.second - dv.second) / box.shownH();   // one point up
    FPDF_FONT font = api->FPDFText_LoadStandardFont(doc, "Helvetica");
    if (!font) return false;
    bool ok = true;
    size_t start = 0;
    for (int line = 0; start <= text.size() && ok; line++) {
        size_t end = text.find(L'\n', start);
        if (end == wstring::npos) end = text.size();
        wstring one = text.substr(start, end - start);
        start = end + 1;
        if (one.empty()) continue;
        // The baseline sits about 0.8 of the size below the top; lines are 1.2 sizes apart.
        double base = y + (0.8 + 1.2 * line) * size / box.shownH();
        auto at = box.User(x, base);
        FPDF_PAGEOBJECT t = api->FPDFPageObj_CreateTextObj(doc, font, float(size));
        FS_MATRIX m{ float(rx), float(ry), float(ux), float(uy), float(at.first), float(at.second) };
        ok = t && api->FPDFText_SetText(t, reinterpret_cast<FPDF_WIDESTRING>(one.c_str())) && api->FPDFPageObj_SetMatrix(t, &m);
        if (ok) api->FPDFPage_InsertObject(page, t);
    }
    api->FPDFFont_Close(font);
    return ok;
}

/// PDFium's form engine for one document, set up and torn down with it. No callbacks are needed to
/// read and set field values.
struct FormEnv {
    const Pdfium* api;
    FPDF_FORMFILLINFO info{};
    FPDF_FORMHANDLE form = nullptr;
    FormEnv(const Pdfium* a, FPDF_DOCUMENT doc) : api(a) { info.version = 1; form = api->FPDFDOC_InitFormFillEnvironment(doc, &info); }
    ~FormEnv() { if (form) api->FPDFDOC_ExitFormFillEnvironment(form); }
};

/// A field string (UTF-16 from PDFium) as a wstring.
wstring FieldString(const std::function<unsigned long(FPDF_WCHAR*, unsigned long)>& get)
{
    unsigned long n = get(nullptr, 0);
    if (n <= 2) return L"";
    std::vector<FPDF_WCHAR> buf(n / 2 + 1, 0);
    get(buf.data(), n);
    return wstring(reinterpret_cast<const wchar_t*>(buf.data()));
}

/// "prep.fields": the fillable fields of every page, for the Sign window (no file is written).
bool RunFields(const Job& job, const Progress& p, Result& r)
{
    PdfDoc pdf;
    if (!pdf.Open(job.inputs[0], r.error)) return false;
    const Pdfium* api = pdf.api();
    FormEnv env(api, pdf.get());
    Json fields = Json::Array();
    for (int pg = 0, n = pdf.pages(); pg < n && env.form; pg++) {
        FPDF_PAGE page = api->FPDF_LoadPage(pdf.get(), pg);
        if (!page) continue;
        api->FORM_OnAfterLoadPage(page, env.form);
        PageBox box(api, page);
        for (int k = 0, count = api->FPDFPage_GetAnnotCount(page); k < count; k++) {
            FPDF_ANNOTATION a = api->FPDFPage_GetAnnot(page, k);
            if (!a) continue;
            int type = api->FPDFAnnot_GetSubtype(a) == FPDF_ANNOT_WIDGET ? api->FPDFAnnot_GetFormFieldType(env.form, a) : 0;
            int flags = type ? api->FPDFAnnot_GetFormFieldFlags(env.form, a) : 0;
            FS_RECTF rc{};
            // Text, check boxes, radio buttons and lists; not buttons, signature fields or read-only ones.
            if (type >= FPDF_FORMFIELD_CHECKBOX && type <= FPDF_FORMFIELD_TEXTFIELD && !(flags & FPDF_FORMFLAG_READONLY) && api->FPDFAnnot_GetRect(a, &rc)) {
                auto c1 = box.Shown(rc.left, rc.top), c2 = box.Shown(rc.right, rc.bottom);
                Json f = Json::Object();
                f.Set(L"page", pg).Set(L"i", k).Set(L"type", type)
                 .Set(L"x", std::min(c1.first, c2.first)).Set(L"y", std::min(c1.second, c2.second))
                 .Set(L"w", std::fabs(c2.first - c1.first)).Set(L"h", std::fabs(c2.second - c1.second))
                 .Set(L"name", FieldString([&](FPDF_WCHAR* b, unsigned long n) { return api->FPDFAnnot_GetFormFieldName(env.form, a, b, n); }))
                 .Set(L"label", FieldString([&](FPDF_WCHAR* b, unsigned long n) { return api->FPDFAnnot_GetFormFieldAlternateName(env.form, a, b, n); }))
                 .Set(L"value", FieldString([&](FPDF_WCHAR* b, unsigned long n) { return api->FPDFAnnot_GetFormFieldValue(env.form, a, b, n); }))
                 .Set(L"export", FieldString([&](FPDF_WCHAR* b, unsigned long n) { return api->FPDFAnnot_GetFormFieldExportValue(env.form, a, b, n); }))
                 .Set(L"checked", bool(api->FPDFAnnot_IsChecked(env.form, a)))
                 .Set(L"multiline", type == FPDF_FORMFIELD_TEXTFIELD && (flags & FPDF_FORMFLAG_TEXT_MULTILINE) != 0)
                 .Set(L"pth", box.shownH());
                Json options = Json::Array();
                for (int o = 0, no = api->FPDFAnnot_GetOptionCount(env.form, a); o < no; o++)
                    options.Push(FieldString([&](FPDF_WCHAR* b, unsigned long n) { return api->FPDFAnnot_GetOptionLabel(env.form, a, o, b, n); }));
                f.Set(L"options", options);
                fields.Push(f);
            }
            api->FPDFPage_CloseAnnot(a);
        }
        api->FORM_OnBeforeClosePage(page, env.form);
        api->FPDF_ClosePage(page);
        p.Set(0.9 * (pg + 1) / n);
    }
    r.info.Set(L"fields", fields).Set(L"pages", pdf.pages());
    return true;
}

/// Sets the changed fields of one page, through PDFium's form engine as if typed or clicked, so
/// their appearance is redrawn too.
bool FillPage(const Pdfium* api, const FormEnv& env, FPDF_PAGE page, const std::vector<const Json*>& changes)
{
    for (const Json* f : changes) {
        FPDF_ANNOTATION a = api->FPDFPage_GetAnnot(page, int((*f)[L"i"].Num()));
        if (!a) return false;
        int type = api->FPDFAnnot_GetFormFieldType(env.form, a);
        if (type == FPDF_FORMFIELD_TEXTFIELD) {
            wstring v = (*f)[L"value"].Str();
            api->FORM_SetFocusedAnnot(env.form, a);
            api->FORM_SelectAllText(env.form, page);
            api->FORM_ReplaceSelection(env.form, page, reinterpret_cast<FPDF_WIDESTRING>(v.c_str()));
        } else if (type == FPDF_FORMFIELD_CHECKBOX || type == FPDF_FORMFIELD_RADIOBUTTON) {
            // A click in its middle toggles it (a radio button only turns on).
            if (bool(api->FPDFAnnot_IsChecked(env.form, a)) != (*f)[L"checked"].Bool()) {
                FS_RECTF rc{};
                api->FPDFAnnot_GetRect(a, &rc);
                double cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2;
                api->FORM_OnLButtonDown(env.form, page, 0, cx, cy);
                api->FORM_OnLButtonUp(env.form, page, 0, cx, cy);
            }
        } else if (type == FPDF_FORMFIELD_COMBOBOX || type == FPDF_FORMFIELD_LISTBOX) {
            api->FORM_SetFocusedAnnot(env.form, a);
            api->FORM_SetIndexSelected(env.form, page, int((*f)[L"index"].Num()), true);
        }
        api->FORM_ForceToKillFocus(env.form);
        api->FPDFPage_CloseAnnot(a);
    }
    return true;
}

/// The pictures of a job: "stamps" (from the Sign window), or one from image/page/x/y/w (the command line).
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

/// The command line's --option "set=name=Ahmad;agree=on;size=M;colour=Blue" (a radio button by its export value).
void FieldsFromCommandLine(const Job& job, Json& fields, const Json& known)
{
    wstring spec = job.opts[L"set"].Str();
    for (size_t start = 0; start < spec.size();) {
        size_t end = spec.find(L';', start);
        if (end == wstring::npos) end = spec.size();
        wstring kv = spec.substr(start, end - start);
        start = end + 1;
        size_t eq = kv.find(L'=');
        if (eq == wstring::npos) continue;
        wstring name = kv.substr(0, eq), value = kv.substr(eq + 1);
        for (auto& f : known.arr) {
            if (f[L"name"].Str() != name) continue;
            Json c = f;
            int type = int(f[L"type"].Num());
            if (type == FPDF_FORMFIELD_TEXTFIELD) c.Set(L"value", value);
            else if (type == FPDF_FORMFIELD_CHECKBOX) c.Set(L"checked", value == L"on" || value == L"yes" || value == L"true");
            else if (type == FPDF_FORMFIELD_RADIOBUTTON) { if (f[L"export"].Str() != value) continue; c.Set(L"checked", true); }   // "size=M"
            else {
                int idx = -1;
                for (size_t o = 0; o < f[L"options"].arr.size(); o++) if (f[L"options"].arr[o].Str() == value) idx = int(o);
                if (idx < 0) continue;
                c.Set(L"index", idx);
            }
            fields.Push(c);
            if (type != FPDF_FORMFIELD_RADIOBUTTON) break;
        }
    }
}

/// "pdf.sign", "pdf.picture", "pdf.fill": fields filled, text typed and pictures placed, in one new PDF.
bool RunEdit(const Job& job, const Progress& p, Result& r)
{
    const wstring& in = job.inputs[0];
    auto stamps = StampsOf(job);
    Json fields = job.opts[L"fields"];
    Json texts = job.opts[L"texts"];
    if (job.opts.has(L"text"))   // the command line: --option text=... page=1 x=0.1 y=0.1 size=12
        texts.Push(Json::Object().Set(L"page", _wtoi(job.opts[L"page"].Str(L"1").c_str()) - 1).Set(L"x", _wtof(job.opts[L"x"].Str(L"0.1").c_str()))
                                 .Set(L"y", _wtof(job.opts[L"y"].Str(L"0.1").c_str())).Set(L"size", _wtof(job.opts[L"size"].Str(L"12").c_str())).Set(L"text", job.opts[L"text"].Str()));
    if (job.opts.has(L"set")) {
        Job probe = job;
        Result known;
        if (!RunFields(probe, Progress{}, known)) { r.error = known.error; return false; }
        fields = Json::Array();
        FieldsFromCommandLine(job, fields, known.info[L"fields"]);
        if (fields.arr.empty()) { r.error = L"No field with that name (or that choice) in this PDF."; return false; }
    }
    if (stamps.empty() && fields.arr.empty() && texts.arr.empty()) { r.error = L"Nothing to change in the PDF."; return false; }
    PdfDoc pdf;
    if (!pdf.Open(in, r.error)) return false;
    const Pdfium* api = pdf.api();
    auto badPage = [&](int pg) { r.error = L"This PDF has " + std::to_wstring(pdf.pages()) + L" pages."; return pg < 0 || pg >= pdf.pages(); };

    if (!fields.arr.empty()) {
        FormEnv env(api, pdf.get());
        if (!env.form) { r.error = L"The form in this PDF could not be read."; return false; }
        std::map<int, std::vector<const Json*>> byPage;
        for (auto& f : fields.arr) byPage[int(f[L"page"].Num())].push_back(&f);
        for (auto& [pg, changes] : byPage) {
            if (badPage(pg)) return false;
            FPDF_PAGE page = api->FPDF_LoadPage(pdf.get(), pg);
            if (!page) { r.error = L"Could not open page " + std::to_wstring(pg + 1) + L"."; return false; }
            api->FORM_OnAfterLoadPage(page, env.form);
            bool ok = FillPage(api, env, page, changes);
            api->FORM_OnBeforeClosePage(page, env.form);
            api->FPDF_ClosePage(page);
            if (!ok) { r.error = L"Could not fill the form on page " + std::to_wstring(pg + 1) + L"."; return false; }
        }
        r.error.clear();
    }
    p.Set(0.3);
    for (auto& t : texts.arr) {
        int pg = int(t[L"page"].Num());
        if (badPage(pg)) return false;
        FPDF_PAGE page = api->FPDF_LoadPage(pdf.get(), pg);
        bool ok = page && PlaceText(api, pdf.get(), page, t[L"x"].Num(), t[L"y"].Num(), std::clamp(t[L"size"].Num(12), 4.0, 200.0), t[L"text"].Str())
               && api->FPDFPage_GenerateContent(page);
        if (page) api->FPDF_ClosePage(page);
        if (!ok) { r.error = L"Could not add the text on page " + std::to_wstring(pg + 1) + L"."; return false; }
    }
    r.error.clear();
    for (size_t i = 0; i < stamps.size(); i++) {
        Stamp& s = stamps[i];
        if (badPage(s.page)) return false;
        r.error.clear();
        Image img;
        std::vector<uint8_t> bytes;
        if (!DecodeImage(s.image, img, bytes, r.error)) return false;
        FPDF_PAGE page = api->FPDF_LoadPage(pdf.get(), s.page);
        if (!page) { r.error = L"Could not open page " + std::to_wstring(s.page + 1) + L"."; return false; }
        if (s.h <= 0) {
            // Keep the picture's shape on the page as shown.
            PageBox box(api, page);
            s.h = s.w * img.h / img.w * box.shownW() / box.shownH();
        }
        bool ok = PlaceImage(api, pdf.get(), page, s, img) && api->FPDFPage_GenerateContent(page);
        api->FPDF_ClosePage(page);
        if (!ok) { r.error = L"Could not put the picture on page " + std::to_wstring(s.page + 1) + L"."; return false; }
        p.Set(0.3 + 0.5 * double(i + 1) / double(stamps.size()));
    }
    std::vector<uint8_t> out;
    if (!SavePdf(api, pdf.get(), out)) { r.error = L"Could not save the PDF."; return false; }
    r.before = FileSize(in);
    const wchar_t* suffix = job.op == L"pdf.sign" ? L" signed" : job.op == L"pdf.fill" ? L" filled" : L" edited";
    return WriteOutput(job, in, BaseName(in) + suffix, L".pdf", out, p, r);
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
    if (target == L"xlsx") {
        // Every page as a sheet laid out like it.
        std::vector<PageSheet> sheets;
        bool text = false;
        for (int i = 0; i < pages; i++) {
            FPDF_PAGE page = pdf.api()->FPDF_LoadPage(pdf.get(), i);
            if (!page) continue;
            sheets.push_back(PageLayout(ReadPage(pdf.api(), page)));
            pdf.api()->FPDF_ClosePage(page);
            text = text || !sheets.back().texts.empty();
            p.Set(0.9 * (i + 1) / pages);
        }
        if (!text) { r.error = L"This PDF has no text to put in a spreadsheet (it may be scanned pages)."; return false; }
        std::vector<uint8_t> out;
        if (!WritePageSheets(sheets, out)) { r.error = L"Could not write the workbook."; return false; }
        return WriteOutput(job, in, base, L".xlsx", out, p, r);
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
    if (target == L"docx") {
        // Every page laid out like it: its drawing behind, its text typed over it where it was.
        std::vector<LaidPage> laid;
        for (int i = 0; i < pages; i++) {
            FPDF_PAGE page = pdf.api()->FPDF_LoadPage(pdf.get(), i);
            if (!page) continue;
            PageContent pc = ReadPage(pdf.api(), page);
            pdf.api()->FPDF_ClosePage(page);
            LaidPage lp;
            lp.w = pc.w;
            lp.h = pc.h;
            for (auto& run : pc.runs) lp.texts.push_back({ run.l, run.t, run.b - run.t, std::move(run.pieces) });
            Image img;
            if (!RenderPage(pdf, i, 200, img, r.error, 0, false)) return false;
            // The paper is see-through, the drawing on it isn't: in Word's dark mode the page darkens and the
            // text turns light, but a picture keeps its colours, so white paper would hide the text.
            // White becomes clear and the edges of lines fade into it (white taken out, not cut out).
            bool blank = true;   // nothing but text on the page: no picture behind it
            for (size_t k = 0; k + 3 < img.px.size(); k += 4) {
                uint8_t* c = &img.px[k];
                int a = 255 - std::min({ c[0], c[1], c[2] });
                for (int j = 0; j < 3; j++) c[j] = uint8_t(a ? (c[j] - 255 + a) * 255 / a : 0);
                c[3] = uint8_t(a);
                blank = blank && a < 5;
            }
            img.alpha = true;
            if (!blank && !EncodeImage(img, Kind::Png, 0, true, lp.drawing, r.error)) return false;
            laid.push_back(std::move(lp));
            p.Set(0.9 * (i + 1) / pages);
        }
        std::vector<uint8_t> out;
        if (!WriteLaidDocx(laid, BaseName(in), out)) { r.error = L"Could not write the Word document."; return false; }
        return WriteOutput(job, in, base, L".docx", out, p, r);
    }
    Doc doc;
    bool images = target == L"epub";
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
    if (job.op == L"pdf.sign" || job.op == L"pdf.picture" || job.op == L"pdf.fill") return RunEdit(job, p, r);
    if (job.op == L"prep.fields") return RunFields(job, p, r);
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
