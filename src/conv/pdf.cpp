#include "pdf.h"

#include <miniz.h>

namespace rd {

namespace {

constexpr double kMaxPagePt = 14400;   // PDF's largest page side (200 inches)

bool Deflate(const uint8_t* p, size_t n, std::vector<uint8_t>& out)
{
    mz_ulong cap = mz_compressBound(mz_ulong(n));
    out.resize(cap);
    if (mz_compress2(out.data(), &cap, p, mz_ulong(n), 6) != MZ_OK) return false;   // zlib format, as FlateDecode wants
    out.resize(cap);
    return true;
}

/// Components and precision from a JPEG's frame header; 0 if not found. Adobe-marked files may be CMYK/YCCK.
int JpegComponents(const std::vector<uint8_t>& b, bool& adobe, int& precision)
{
    adobe = false;
    precision = 8;
    size_t i = 2;
    while (i + 9 < b.size() && b[i] == 0xFF) {
        uint8_t m = b[i + 1];
        if (m == 0xFF) { i++; continue; }
        size_t len = size_t(b[i + 2]) << 8 | b[i + 3];
        if (m == 0xEE) adobe = true;
        if ((m >= 0xC0 && m <= 0xC3) || (m >= 0xC5 && m <= 0xC7) || (m >= 0xC9 && m <= 0xCB) || (m >= 0xCD && m <= 0xCF)) {
            precision = b[i + 4];
            return b[i + 9];
        }
        if (m == 0xDA) break;
        i += 2 + len;
    }
    return 0;
}

}  // namespace

void PdfWriter::AddImage(const Image& img, const std::vector<uint8_t>* jpeg, int jpegQuality)
{
    Page pg;
    pg.pxW = img.w;
    pg.pxH = img.h;
    pg.wPt = img.w * 72.0 / img.dpiX;
    pg.hPt = img.h * 72.0 / img.dpiY;
    double k = std::min(1.0, kMaxPagePt / std::max(pg.wPt, pg.hPt));
    pg.wPt *= k;
    pg.hPt *= k;

    bool adobe = false;
    int precision = 8;
    int comps = jpeg ? JpegComponents(*jpeg, adobe, precision) : 0;
    if (jpeg && !img.turned && precision == 8 && (comps == 3 || (comps == 1 && !adobe))) {
        pg.filter = "DCTDecode";
        pg.colour = comps == 1 ? "DeviceGray" : "DeviceRGB";
        pg.data = *jpeg;
    } else if (img.alpha || img.kind == Kind::Png || img.kind == Kind::Gif || img.kind == Kind::Bmp || img.kind == Kind::Ico ||
               img.kind == Kind::Svg || img.kind == Kind::Tiff) {
        std::vector<uint8_t> rgb(size_t(img.w) * img.h * 3), alpha;
        if (img.alpha) alpha.resize(size_t(img.w) * img.h);
        for (size_t i = 0, n = size_t(img.w) * img.h; i < n; i++) {
            rgb[i * 3] = img.px[i * 4 + 2];
            rgb[i * 3 + 1] = img.px[i * 4 + 1];
            rgb[i * 3 + 2] = img.px[i * 4];
            if (img.alpha) alpha[i] = img.px[i * 4 + 3];
        }
        pg.filter = "FlateDecode";
        pg.colour = "DeviceRGB";
        if (!Deflate(rgb.data(), rgb.size(), pg.data) || (img.alpha && !Deflate(alpha.data(), alpha.size(), pg.mask))) {
            error_ = L"Could not compress a page.";
            return;
        }
    } else {
        // A lossy source (WebP, HEIC...): JPEG keeps the PDF small.
        wstring err;
        if (!EncodeImage(img, Kind::Jpeg, jpegQuality, false, pg.data, err)) { error_ = err; return; }
        pg.filter = "DCTDecode";
        pg.colour = "DeviceRGB";
    }
    pages_.push_back(std::move(pg));
}

bool PdfWriter::Finish(std::vector<uint8_t>& out, wstring& err)
{
    if (!error_.empty()) { err = error_; return false; }
    if (pages_.empty()) { err = L"Nothing to put in the PDF."; return false; }
    out.clear();
    std::vector<size_t> offsets;   // per object number, from 1
    auto put = [&](const std::string& s) { out.insert(out.end(), s.begin(), s.end()); };
    auto begin = [&](int num) {
        if (offsets.size() < size_t(num)) offsets.resize(size_t(num));
        offsets[size_t(num) - 1] = out.size();
        put(std::to_string(num) + " 0 obj\n");
    };
    char buf[512];
    put("%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");

    // 1 catalog, 2 page tree, then per page: page, contents, image, [mask].
    int n = 3;
    std::vector<int> pageNums;
    std::string kids;
    for (size_t i = 0; i < pages_.size(); i++) {
        pageNums.push_back(n);
        kids += std::to_string(n) + " 0 R ";
        n += pages_[i].mask.empty() ? 3 : 4;
    }
    begin(1);
    put("<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    begin(2);
    put("<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages_.size()) + " >>\nendobj\n");

    for (size_t i = 0; i < pages_.size(); i++) {
        const Page& pg = pages_[i];
        int page = pageNums[i], content = page + 1, image = page + 2, mask = page + 3;
        begin(page);
        snprintf(buf, sizeof buf, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] /Resources << /XObject << /Im0 %d 0 R >> >> /Contents %d 0 R >>\nendobj\n",
                 pg.wPt, pg.hPt, image, content);
        put(buf);
        snprintf(buf, sizeof buf, "q %.2f 0 0 %.2f 0 0 cm /Im0 Do Q", pg.wPt, pg.hPt);
        std::string draw = buf;
        begin(content);
        put("<< /Length " + std::to_string(draw.size()) + " >>\nstream\n" + draw + "\nendstream\nendobj\n");
        begin(image);
        snprintf(buf, sizeof buf, "<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /%s /BitsPerComponent 8 /Filter /%s /Length %zu%s >>\nstream\n",
                 pg.pxW, pg.pxH, pg.colour.c_str(), pg.filter.c_str(), pg.data.size(),
                 pg.mask.empty() ? "" : (" /SMask " + std::to_string(mask) + " 0 R").c_str());
        put(buf);
        out.insert(out.end(), pg.data.begin(), pg.data.end());
        put("\nendstream\nendobj\n");
        if (!pg.mask.empty()) {
            begin(mask);
            snprintf(buf, sizeof buf, "<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode /Length %zu >>\nstream\n",
                     pg.pxW, pg.pxH, pg.mask.size());
            put(buf);
            out.insert(out.end(), pg.mask.begin(), pg.mask.end());
            put("\nendstream\nendobj\n");
        }
    }
    size_t xref = out.size();
    put("xref\n0 " + std::to_string(offsets.size() + 1) + "\n0000000000 65535 f \n");
    for (size_t off : offsets) { snprintf(buf, sizeof buf, "%010zu 00000 n \n", off); put(buf); }
    put("trailer\n<< /Size " + std::to_string(offsets.size() + 1) + " /Root 1 0 R /Info << /Producer (Roda) >> >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n");
    return true;
}

}  // namespace rd
