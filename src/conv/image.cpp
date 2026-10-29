// Images: decode (WIC for the formats Windows always has, libwebp for WebP, lunasvg for SVG),
// change (resize, crop, rotate, flip), encode (WIC, libwebp, our own ICO and PDF writers).
#include "conv.h"
#include "pdf.h"

#include <lunasvg.h>
#include <webp/decode.h>
#include <webp/encode.h>

namespace rd {

namespace {

constexpr int64_t kMaxPixels = 250'000'000;     // ~1 GB of BGRA: beyond that, refuse rather than thrash
constexpr int kWebpMax = 16383;                 // the format's limit

IWICImagingFactory* Factory()
{
    // The worker's own factory: no Direct2D here. Never released: the process ends after one job, and
    // a static smart pointer would release it after COM has already shut down.
    static IWICImagingFactory* f = nullptr;
    if (!f) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
}

bool HasAlpha(const Image& img)
{
    for (size_t i = 3; i < img.px.size(); i += 4)
        if (img.px[i] != 255) return true;
    return false;
}

// ---------------------------------------------------------------- orientation and simple changes

void Rotate90(Image& img)   // clockwise
{
    std::vector<uint8_t> out(img.px.size());
    int w = img.w, h = img.h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            memcpy(&out[(size_t(x) * h + (h - 1 - y)) * 4], &img.px[(size_t(y) * w + x) * 4], 4);
    img.px.swap(out);
    std::swap(img.w, img.h);
    std::swap(img.dpiX, img.dpiY);
}

void FlipH(Image& img)
{
    for (int y = 0; y < img.h; y++) {
        uint32_t* row = reinterpret_cast<uint32_t*>(&img.px[size_t(y) * img.w * 4]);
        std::reverse(row, row + img.w);
    }
}

void FlipV(Image& img)
{
    size_t stride = size_t(img.w) * 4;
    std::vector<uint8_t> tmp(stride);
    for (int y = 0; y < img.h / 2; y++) {
        uint8_t* a = &img.px[size_t(y) * stride];
        uint8_t* b = &img.px[size_t(img.h - 1 - y) * stride];
        memcpy(tmp.data(), a, stride); memcpy(a, b, stride); memcpy(b, tmp.data(), stride);
    }
}

/// EXIF orientation 1..8 to upright pixels.
void ApplyOrientation(Image& img, int o)
{
    switch (o) {
        case 2: FlipH(img); break;
        case 3: FlipH(img); FlipV(img); break;
        case 4: FlipV(img); break;
        case 5: Rotate90(img); FlipH(img); break;
        case 6: Rotate90(img); break;
        case 7: Rotate90(img); FlipV(img); break;
        case 8: Rotate90(img); FlipH(img); FlipV(img); break;
        default: break;
    }
}

bool Crop(Image& img, int x, int y, int w, int h)
{
    x = std::clamp(x, 0, img.w - 1);
    y = std::clamp(y, 0, img.h - 1);
    w = std::clamp(w, 1, img.w - x);
    h = std::clamp(h, 1, img.h - y);
    std::vector<uint8_t> out(size_t(w) * h * 4);
    for (int r = 0; r < h; r++) memcpy(&out[size_t(r) * w * 4], &img.px[(size_t(y + r) * img.w + x) * 4], size_t(w) * 4);
    img.px.swap(out);
    img.w = w;
    img.h = h;
    return true;
}

/// High-quality resize. Scaled premultiplied, so transparent edges don't grow dark fringes.
bool Resize(Image& img, int w, int h, wstring& err)
{
    if (w == img.w && h == img.h) return true;
    if (w < 1 || h < 1 || int64_t(w) * h > kMaxPixels) { err = L"That size is too big."; return false; }
    std::vector<uint8_t> pm(img.px);
    for (size_t i = 0; i < pm.size(); i += 4) {
        unsigned a = pm[i + 3];
        if (a != 255) { pm[i] = uint8_t(pm[i] * a / 255); pm[i + 1] = uint8_t(pm[i + 1] * a / 255); pm[i + 2] = uint8_t(pm[i + 2] * a / 255); }
    }
    Com<IWICBitmap> src;
    Com<IWICBitmapScaler> sc;
    if (FAILED(Factory()->CreateBitmapFromMemory(UINT(img.w), UINT(img.h), GUID_WICPixelFormat32bppPBGRA, UINT(img.w * 4), UINT(pm.size()), pm.data(), src.Put())) ||
        FAILED(Factory()->CreateBitmapScaler(sc.Put()))) { err = L"Could not resize."; return false; }
    // High-quality cubic is Windows 10's best; Fant (box-like) is better for big reductions.
    bool shrinkLots = w * 2 < img.w || h * 2 < img.h;
    if (FAILED(sc->Initialize(src.Get(), UINT(w), UINT(h), shrinkLots ? WICBitmapInterpolationModeFant : WICBitmapInterpolationMode(4) /* high-quality cubic: Windows 10, missing from MinGW headers */))) {
        err = L"Could not resize.";
        return false;
    }
    std::vector<uint8_t> out(size_t(w) * h * 4);
    if (FAILED(sc->CopyPixels(nullptr, UINT(w * 4), UINT(out.size()), out.data()))) { err = L"Could not resize."; return false; }
    for (size_t i = 0; i < out.size(); i += 4) {
        unsigned a = out[i + 3];
        if (a != 0 && a != 255) {
            out[i] = uint8_t(std::min(255u, out[i] * 255u / a));
            out[i + 1] = uint8_t(std::min(255u, out[i + 1] * 255u / a));
            out[i + 2] = uint8_t(std::min(255u, out[i + 2] * 255u / a));
        }
    }
    img.px.swap(out);
    img.w = w;
    img.h = h;
    return true;
}

/// Over white, for formats without transparency.
Image Flattened(const Image& img)
{
    Image f = img;
    if (!img.alpha) return f;
    for (size_t i = 0; i < f.px.size(); i += 4) {
        unsigned a = f.px[i + 3];
        for (int c = 0; c < 3; c++) f.px[i + c] = uint8_t((f.px[i + c] * a + 255 * (255 - a) + 127) / 255);
        f.px[i + 3] = 255;
    }
    f.alpha = false;
    return f;
}

// ---------------------------------------------------------------- decoders

bool DecodeWebp(const std::vector<uint8_t>& b, Image& img, wstring& err)
{
    WebPBitstreamFeatures f{};
    if (WebPGetFeatures(b.data(), b.size(), &f) != VP8_STATUS_OK) { err = L"This WebP file is damaged."; return false; }
    int w = 0, h = 0;
    uint8_t* p = WebPDecodeBGRA(b.data(), b.size(), &w, &h);
    if (!p) { err = f.has_animation ? L"Animated WebP isn't supported yet." : L"This WebP file is damaged."; return false; }
    img.w = w; img.h = h;
    img.px.assign(p, p + size_t(w) * h * 4);
    WebPFree(p);
    return true;
}

bool DecodeSvg(const std::vector<uint8_t>& b, Image& img, wstring& err)
{
    auto doc = lunasvg::Document::loadFromData(reinterpret_cast<const char*>(b.data()), b.size());
    if (!doc) { err = L"This SVG file can't be read."; return false; }
    double w = doc->width(), h = doc->height();
    if (w <= 0 || h <= 0) { w = 1024; h = 1024; }
    // Drawings without a real size, or tiny icons, come out at a useful size; huge ones are capped.
    double longest = std::max(w, h);
    double k = longest < 512 ? 1024 / longest : longest > 8192 ? 8192 / longest : 1.0;
    int pw = std::max(1, int(std::lround(w * k))), ph = std::max(1, int(std::lround(h * k)));
    lunasvg::Bitmap bmp = doc->renderToBitmap(pw, ph, 0);
    if (bmp.isNull()) { err = L"This SVG file can't be drawn."; return false; }
    img.w = bmp.width(); img.h = bmp.height();
    img.px.resize(size_t(img.w) * img.h * 4);
    for (int y = 0; y < img.h; y++) {
        const uint8_t* s = bmp.data() + size_t(y) * bmp.stride();
        uint8_t* d = &img.px[size_t(y) * img.w * 4];
        for (int x = 0; x < img.w; x++, s += 4, d += 4) {   // premultiplied BGRA in, straight out
            unsigned a = s[3];
            d[3] = uint8_t(a);
            for (int c = 0; c < 3; c++) d[c] = a ? uint8_t(std::min(255u, s[c] * 255u / a)) : 0;
        }
    }
    return true;
}

int WicOrientation(IWICBitmapFrameDecode* frame)
{
    Com<IWICMetadataQueryReader> q;
    if (FAILED(frame->GetMetadataQueryReader(q.Put()))) return 1;
    PROPVARIANT v;
    PropVariantInit(&v);
    int o = 1;
    if (SUCCEEDED(q->GetMetadataByName(L"System.Photo.Orientation", &v)) && v.vt == VT_UI2) o = v.uiVal;
    PropVariantClear(&v);
    return o >= 1 && o <= 8 ? o : 1;
}

/// WIC fills the missing rows of a cut-off PNG or JPEG instead of failing; Roda refuses those files, so a
/// half-grey picture is never saved as if it were fine.
bool Truncated(const std::vector<uint8_t>& b, Kind kind)
{
    if (kind == Kind::Png) {
        size_t from = b.size() > 64 ? b.size() - 64 : 0;
        for (size_t i = from; i + 4 <= b.size(); i++) if (memcmp(&b[i], "IEND", 4) == 0) return false;
        return true;
    }
    if (kind == Kind::Jpeg) {
        // Entropy-coded data can't contain FF DA or FF D9 (FF is followed by 00 there), so the end
        // marker must come after the last start of scan.
        size_t lastSos = 0, lastEoi = 0;
        for (size_t i = 0; i + 1 < b.size(); i++) {
            if (b[i] != 0xFF) continue;
            if (b[i + 1] == 0xDA) lastSos = i;
            else if (b[i + 1] == 0xD9) lastEoi = i;
        }
        return lastEoi < lastSos;
    }
    return false;
}

bool DecodeWic(const std::vector<uint8_t>& b, Kind kind, Image& img, wstring& err)
{
    if (Truncated(b, kind)) { err = L"This image is damaged (the file ends too early)."; return false; }
    auto* f = Factory();
    Com<IWICStream> st;
    Com<IWICBitmapDecoder> dec;
    Com<IWICBitmapFrameDecode> frame;
    if (!f || FAILED(f->CreateStream(st.Put())) || FAILED(st->InitializeFromMemory(const_cast<BYTE*>(b.data()), DWORD(b.size())))) {
        err = L"Could not start the image decoder.";
        return false;
    }
    if (FAILED(f->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.Put()))) {
        if (kind == Kind::Heic) err = L"HEIC photos need Microsoft's free \"HEIF Image Extensions\" on this PC for now.";
        else if (kind == Kind::Avif) err = L"AVIF images need Microsoft's free \"AV1 Video Extension\" on this PC for now.";
        else err = L"This image is damaged or in a variant Windows can't read.";
        return false;
    }
    // The first frame, except in icons: there, the biggest picture.
    UINT pick = 0, frames = 0;
    dec->GetFrameCount(&frames);
    if (kind == Kind::Ico) {
        UINT best = 0;
        for (UINT i = 0; i < frames; i++) {
            Com<IWICBitmapFrameDecode> f2;
            UINT fw = 0, fh = 0;
            if (SUCCEEDED(dec->GetFrame(i, f2.Put())) && SUCCEEDED(f2->GetSize(&fw, &fh)) && fw * fh > best) { best = fw * fh; pick = i; }
        }
    }
    if (FAILED(dec->GetFrame(pick, frame.Put()))) { err = L"This image has no picture in it."; return false; }
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0 || int64_t(w) * h > kMaxPixels) { err = L"This image is too big to convert."; return false; }
    frame->GetResolution(&img.dpiX, &img.dpiY);
    Com<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(conv.Put())) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        err = L"This image's colour format can't be converted.";
        return false;
    }
    img.w = int(w); img.h = int(h);
    img.px.resize(size_t(w) * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, UINT(img.px.size()), img.px.data()))) {
        err = kind == Kind::Heic || kind == Kind::Avif ? L"Windows couldn't decode this image (its codec may be missing)." : L"This image is damaged.";
        return false;
    }
    int o = WicOrientation(frame.Get());
    ApplyOrientation(img, o);
    img.turned = o != 1;
    return true;
}

// ---------------------------------------------------------------- encoders

Com<IWICBitmap> AsBitmap(const Image& img)
{
    Com<IWICBitmap> b;
    Factory()->CreateBitmapFromMemory(UINT(img.w), UINT(img.h), GUID_WICPixelFormat32bppBGRA, UINT(img.w * 4), UINT(img.px.size()),
                                      const_cast<BYTE*>(img.px.data()), b.Put());
    return b;
}

bool StreamBytes(IStream* s, std::vector<uint8_t>& out)
{
    HGLOBAL g = nullptr;
    if (FAILED(GetHGlobalFromStream(s, &g))) return false;
    STATSTG stat{};
    s->Stat(&stat, STATFLAG_NONAME);
    void* p = GlobalLock(g);
    if (!p) return false;
    out.assign(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + size_t(stat.cbSize.QuadPart));
    GlobalUnlock(g);
    return true;
}

struct WicProp { const wchar_t* name; VARIANT v; };

/// One-frame WIC encode of `src` into `fmt`, converting to `pixel` on the way.
bool EncodeWic(const GUID& container, IWICBitmapSource* src, WICPixelFormatGUID pixel, IWICPalette* palette,
               const std::vector<WicProp>& props, std::vector<uint8_t>& out)
{
    auto* f = Factory();
    Com<IStream> stream;
    Com<IWICBitmapEncoder> enc;
    Com<IWICBitmapFrameEncode> frame;
    Com<IPropertyBag2> bag;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.Put())) || FAILED(f->CreateEncoder(container, nullptr, enc.Put())) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(frame.Put(), bag.Put()))) return false;
    for (auto& p : props) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(p.name);
        VARIANT v = p.v;
        bag->Write(1, &opt, &v);
    }
    UINT w = 0, h = 0;
    src->GetSize(&w, &h);
    if (FAILED(frame->Initialize(bag.Get())) || FAILED(frame->SetSize(w, h))) return false;
    WICPixelFormatGUID want = pixel;
    frame->SetPixelFormat(&want);
    if (palette) frame->SetPalette(palette);
    frame->SetResolution(96, 96);
    if (FAILED(frame->WriteSource(src, nullptr)) || FAILED(frame->Commit()) || FAILED(enc->Commit())) return false;
    return StreamBytes(stream.Get(), out);
}

VARIANT FloatVar(float x) { VARIANT v; VariantInit(&v); v.vt = VT_R4; v.fltVal = x; return v; }
VARIANT ByteVar(uint8_t x) { VARIANT v; VariantInit(&v); v.vt = VT_UI1; v.bVal = x; return v; }

bool EncodePngPalette(const Image& img, std::vector<uint8_t>& out)
{
    // Up to 256 colours chosen for this picture, with error diffusion: the lossy way to shrink a PNG.
    auto* f = Factory();
    Com<IWICBitmap> src = AsBitmap(img);
    Com<IWICPalette> pal;
    Com<IWICFormatConverter> conv;
    if (!src || FAILED(f->CreatePalette(pal.Put())) || FAILED(pal->InitializeFromBitmap(src.Get(), 256, img.alpha))) return false;
    if (FAILED(f->CreateFormatConverter(conv.Put())) ||
        FAILED(conv->Initialize(src.Get(), GUID_WICPixelFormat8bppIndexed, WICBitmapDitherTypeErrorDiffusion, pal.Get(), 0, WICBitmapPaletteTypeCustom))) return false;
    return EncodeWic(GUID_ContainerFormatPng, conv.Get(), GUID_WICPixelFormat8bppIndexed, pal.Get(), { { L"FilterOption", ByteVar(WICPngFilterAdaptive) } }, out);
}

bool EncodeIco(const Image& img, std::vector<uint8_t>& out, wstring& err)
{
    // Square, transparent padding, PNG inside each entry (fine since Windows Vista).
    int side = std::max(img.w, img.h);
    Image sq;
    sq.w = sq.h = side;
    sq.px.assign(size_t(side) * side * 4, 0);
    for (int y = 0; y < img.h; y++)
        memcpy(&sq.px[(size_t(y + (side - img.h) / 2) * side + (side - img.w) / 2) * 4], &img.px[size_t(y) * img.w * 4], size_t(img.w) * 4);
    sq.alpha = true;
    std::vector<std::vector<uint8_t>> pngs;
    std::vector<int> sizes;
    for (int s : { 16, 24, 32, 48, 64, 128, 256 }) {
        if (s > side && s > 48) continue;     // small pictures still get the sizes Windows asks for most
        Image e = sq;
        if (!Resize(e, s, s, err)) return false;
        std::vector<uint8_t> png;
        Com<IWICBitmap> b = AsBitmap(e);
        if (!b || !EncodeWic(GUID_ContainerFormatPng, b.Get(), GUID_WICPixelFormat32bppBGRA, nullptr, {}, png)) { err = L"Could not write the icon."; return false; }
        pngs.push_back(std::move(png));
        sizes.push_back(s);
    }
    auto u16 = [&](unsigned v) { out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8)); };
    auto u32 = [&](unsigned v) { u16(v & 0xFFFF); u16(v >> 16); };
    out.clear();
    u16(0); u16(1); u16(unsigned(pngs.size()));
    unsigned offset = 6 + 16 * unsigned(pngs.size());
    for (size_t i = 0; i < pngs.size(); i++) {
        out.push_back(uint8_t(sizes[i] >= 256 ? 0 : sizes[i]));
        out.push_back(uint8_t(sizes[i] >= 256 ? 0 : sizes[i]));
        out.push_back(0); out.push_back(0);
        u16(1); u16(32);
        u32(unsigned(pngs[i].size()));
        u32(offset);
        offset += unsigned(pngs[i].size());
    }
    for (auto& p : pngs) out.insert(out.end(), p.begin(), p.end());
    return true;
}

// ---------------------------------------------------------------- JPEG, PNG and WebP metadata, losslessly

struct Segment { uint8_t marker; size_t at, len; };   // len includes the marker and length bytes

/// The segments before the image data (SOS). Empty if this isn't a JPEG.
std::vector<Segment> JpegHeaders(const std::vector<uint8_t>& b)
{
    std::vector<Segment> out;
    if (b.size() < 4 || b[0] != 0xFF || b[1] != 0xD8) return out;
    size_t i = 2;
    while (i + 4 <= b.size() && b[i] == 0xFF) {
        uint8_t m = b[i + 1];
        if (m == 0xFF) { i++; continue; }
        size_t len = (size_t(b[i + 2]) << 8 | b[i + 3]) + 2;
        if (i + len > b.size()) break;
        out.push_back({ m, i, len });
        if (m == 0xDA) break;   // start of scan
        i += len;
    }
    return out;
}

bool IsExif(const std::vector<uint8_t>& b, const Segment& s) { return s.marker == 0xE1 && s.len > 10 && memcmp(&b[s.at + 4], "Exif\0\0", 6) == 0; }

/// Sets the EXIF orientation to 1 (upright) inside an APP1 segment's bytes: Roda has already turned the pixels.
void ResetOrientation(std::vector<uint8_t>& seg)
{
    if (seg.size() < 18) return;
    size_t t = 10;   // TIFF header after FF E1 len "Exif\0\0"
    bool le = seg[t] == 'I';
    auto rd16 = [&](size_t o) -> unsigned { return o + 2 > seg.size() ? 0 : le ? seg[o] | seg[o + 1] << 8 : seg[o] << 8 | seg[o + 1]; };
    auto rd32 = [&](size_t o) -> size_t { return o + 4 > seg.size() ? 0 : le ? size_t(seg[o] | seg[o + 1] << 8 | seg[o + 2] << 16 | uint32_t(seg[o + 3]) << 24)
                                                                              : size_t(uint32_t(seg[o]) << 24 | seg[o + 1] << 16 | seg[o + 2] << 8 | seg[o + 3]); };
    size_t ifd = t + rd32(t + 4);
    unsigned n = rd16(ifd);
    for (unsigned k = 0; k < n; k++) {
        size_t e = ifd + 2 + k * 12;
        if (e + 12 > seg.size()) return;
        if (rd16(e) == 0x0112) {
            size_t v = e + 8;
            if (le) { seg[v] = 1; seg[v + 1] = 0; } else { seg[v] = 0; seg[v + 1] = 1; }
            return;
        }
    }
}

/// Copies the source's EXIF, XMP, ICC and IPTC segments into a freshly encoded JPEG.
void CopyJpegMetadata(const std::vector<uint8_t>& from, std::vector<uint8_t>& to)
{
    auto src = JpegHeaders(from);
    auto dst = JpegHeaders(to);
    if (src.empty() || dst.empty()) return;
    std::vector<uint8_t> meta;
    for (auto& s : src) {
        if (s.marker == 0xE1 || s.marker == 0xE2 || s.marker == 0xED) {
            std::vector<uint8_t> seg(from.begin() + long(s.at), from.begin() + long(s.at + s.len));
            if (IsExif(from, s)) ResetOrientation(seg);
            meta.insert(meta.end(), seg.begin(), seg.end());
        }
    }
    // After SOI and the new file's JFIF/APP0, where readers look for them.
    size_t at = 2;
    for (auto& d : dst) { if (d.marker == 0xE0) at = d.at + d.len; else break; }
    to.insert(to.begin() + long(at), meta.begin(), meta.end());
}

/// Drops EXIF, XMP, IPTC and comments; keeps the colour profile (APP2) and Adobe's marker (CMYK needs it).
bool StripJpeg(const std::vector<uint8_t>& b, std::vector<uint8_t>& out)
{
    auto segs = JpegHeaders(b);
    if (segs.empty() || segs.back().marker != 0xDA) return false;
    out.assign(b.begin(), b.begin() + 2);
    for (auto& s : segs) {
        bool drop = (s.marker >= 0xE1 && s.marker <= 0xEF && s.marker != 0xE2 && s.marker != 0xEE) || s.marker == 0xFE;
        if (s.marker == 0xDA) { out.insert(out.end(), b.begin() + long(s.at), b.end()); break; }
        if (!drop) out.insert(out.end(), b.begin() + long(s.at), b.begin() + long(s.at + s.len));
    }
    return true;
}

bool StripPng(const std::vector<uint8_t>& b, std::vector<uint8_t>& out)
{
    if (b.size() < 8 || memcmp(b.data(), "\x89PNG\r\n\x1a\n", 8) != 0) return false;
    out.assign(b.begin(), b.begin() + 8);
    size_t i = 8;
    while (i + 12 <= b.size()) {
        size_t len = size_t(b[i]) << 24 | size_t(b[i + 1]) << 16 | size_t(b[i + 2]) << 8 | b[i + 3];
        if (i + 12 + len > b.size()) return false;
        std::string type(reinterpret_cast<const char*>(&b[i + 4]), 4);
        bool keep = type == "IHDR" || type == "PLTE" || type == "IDAT" || type == "IEND" || type == "tRNS" || type == "gAMA" ||
                    type == "cHRM" || type == "sRGB" || type == "iCCP" || type == "sBIT" || type == "pHYs" ||
                    type == "acTL" || type == "fcTL" || type == "fdAT";   // animated PNG frames stay
        if (keep) out.insert(out.end(), b.begin() + long(i), b.begin() + long(i + 12 + len));
        i += 12 + len;
        if (type == "IEND") break;
    }
    return true;
}

bool StripWebp(const std::vector<uint8_t>& b, std::vector<uint8_t>& out)
{
    if (b.size() < 12 || memcmp(b.data(), "RIFF", 4) != 0 || memcmp(b.data() + 8, "WEBP", 4) != 0) return false;
    out.assign(b.begin(), b.begin() + 12);
    size_t i = 12;
    while (i + 8 <= b.size()) {
        size_t len = size_t(b[i + 4]) | size_t(b[i + 5]) << 8 | size_t(b[i + 6]) << 16 | size_t(b[i + 7]) << 24;
        size_t total = 8 + len + (len & 1);
        if (i + 8 + len > b.size()) return false;
        std::string type(reinterpret_cast<const char*>(&b[i]), 4);
        if (type != "EXIF" && type != "XMP ") {
            size_t at = out.size();
            out.insert(out.end(), b.begin() + long(i), b.begin() + long(std::min(b.size(), i + total)));
            if (type == "VP8X" && at + 8 < out.size()) out[at + 8] &= uint8_t(~(0x08 | 0x04));   // no EXIF, no XMP
        }
        i += total;
    }
    uint32_t riff = uint32_t(out.size() - 8);
    for (int k = 0; k < 4; k++) out[4 + k] = uint8_t(riff >> (8 * k));
    return true;
}

// ---------------------------------------------------------------- picking outputs

Kind TargetOf(const wstring& op)
{
    if (op == L"to.jpg") return Kind::Jpeg;
    if (op == L"to.png") return Kind::Png;
    if (op == L"to.webp") return Kind::Webp;
    if (op == L"to.pdf") return Kind::Pdf;
    if (op == L"to.tiff") return Kind::Tiff;
    if (op == L"to.bmp") return Kind::Bmp;
    if (op == L"to.ico") return Kind::Ico;
    return Kind::Unknown;
}

const wchar_t* ExtOf(Kind k)
{
    switch (k) {
        case Kind::Jpeg: return L".jpg";
        case Kind::Png: return L".png";
        case Kind::Webp: return L".webp";
        case Kind::Pdf: return L".pdf";
        case Kind::Tiff: return L".tiff";
        case Kind::Bmp: return L".bmp";
        case Kind::Ico: return L".ico";
        case Kind::Avif: return L".avif";
        default: return L"";
    }
}

/// A tool keeps the file's format when Roda can write it; otherwise PNG (or JPG for photos).
Kind ToolOutput(Kind src)
{
    switch (src) {
        case Kind::Jpeg: case Kind::Png: case Kind::Webp: case Kind::Tiff: case Kind::Bmp: return src;
        case Kind::Heic: case Kind::Avif: return Kind::Jpeg;
        default: return Kind::Png;
    }
}

/// Lossless WebP for drawings and screenshots (their sources were lossless), lossy for photos.
bool LosslessSource(Kind k) { return k == Kind::Png || k == Kind::Gif || k == Kind::Bmp || k == Kind::Ico || k == Kind::Svg; }

int Opt(const Job& job, const wchar_t* key, int fallback) { return int(job.opts[key].Num(fallback)); }

}  // namespace

// ---------------------------------------------------------------- public

bool DecodeImage(const wstring& path, Image& img, std::vector<uint8_t>& bytes, wstring& err)
{
    if (!ReadShared(path, bytes, err)) return false;
    if (bytes.empty()) { err = L"The file is empty."; return false; }
    img = {};
    img.kind = KindFromBytes(bytes.data(), bytes.size(), path);
    bool ok;
    switch (img.kind) {
        case Kind::Webp: ok = DecodeWebp(bytes, img, err); break;
        case Kind::Svg: ok = DecodeSvg(bytes, img, err); break;
        case Kind::Jpeg: case Kind::Png: case Kind::Gif: case Kind::Bmp: case Kind::Tiff: case Kind::Ico:
            ok = DecodeWic(bytes, img.kind, img, err);
            break;
        case Kind::Heic: case Kind::Avif: {
            // Windows' own codecs when Microsoft's extensions are installed; else the bundled FFmpeg
            // turns the picture into a PNG first (the Full build).
            ok = DecodeWic(bytes, img.kind, img, err);
            std::vector<uint8_t> png;
            wstring ffErr;
            if (!ok && EngineAvailable(Engine::Ffmpeg) && FfmpegImage(path, png, ffErr)) {
                Kind k = img.kind;
                err.clear();
                ok = DecodeWic(png, Kind::Png, img, err);
                img.kind = k;
            }
            break;
        }
        default: err = L"This isn't an image Roda can read."; return false;
    }
    if (!ok) return false;
    if (img.dpiX < 30 || img.dpiX > 2400) img.dpiX = 96;
    if (img.dpiY < 30 || img.dpiY > 2400) img.dpiY = 96;
    img.alpha = HasAlpha(img);
    return true;
}

bool EncodeImage(const Image& img, Kind kind, int quality, bool lossless, std::vector<uint8_t>& out, wstring& err)
{
    out.clear();
    switch (kind) {
        case Kind::Jpeg: {
            Image f = Flattened(img);
            Com<IWICBitmap> b = AsBitmap(f);
            if (b && EncodeWic(GUID_ContainerFormatJpeg, b.Get(), GUID_WICPixelFormat24bppBGR, nullptr, { { L"ImageQuality", FloatVar(quality / 100.0f) } }, out)) return true;
            break;
        }
        case Kind::Png: {
            Com<IWICBitmap> b = AsBitmap(img);
            if (b && EncodeWic(GUID_ContainerFormatPng, b.Get(), img.alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR, nullptr,
                               { { L"FilterOption", ByteVar(WICPngFilterAdaptive) } }, out)) return true;
            break;
        }
        case Kind::Bmp: {
            Image f = Flattened(img);
            Com<IWICBitmap> b = AsBitmap(f);
            if (b && EncodeWic(GUID_ContainerFormatBmp, b.Get(), GUID_WICPixelFormat24bppBGR, nullptr, {}, out)) return true;
            break;
        }
        case Kind::Tiff: {
            Com<IWICBitmap> b = AsBitmap(img);
            if (b && EncodeWic(GUID_ContainerFormatTiff, b.Get(), img.alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR, nullptr,
                               { { L"TiffCompressionMethod", ByteVar(WICTiffCompressionLZW) } }, out)) return true;
            break;
        }
        case Kind::Webp: {
            if (img.w > kWebpMax || img.h > kWebpMax) { err = L"WebP can't be wider or taller than 16383 pixels."; return false; }
            uint8_t* p = nullptr;
            size_t n = lossless ? WebPEncodeLosslessBGRA(img.px.data(), img.w, img.h, img.w * 4, &p)
                                : WebPEncodeBGRA(img.px.data(), img.w, img.h, img.w * 4, float(quality), &p);
            if (n && p) { out.assign(p, p + n); WebPFree(p); return true; }
            if (p) WebPFree(p);
            break;
        }
        case Kind::Ico: return EncodeIco(img, out, err);
        case Kind::Pdf: {
            PdfWriter pdf;
            pdf.AddImage(img, nullptr, 85);
            return pdf.Finish(out, err);
        }
        default: err = L"Roda can't write that format."; return false;
    }
    if (err.empty()) err = L"Could not write the image.";
    return false;
}

bool RunToImage(const Job& job, const Progress& p, Result& r)
{
    const wstring& in = job.inputs[0];
    Kind target = TargetOf(job.op);
    Image img;
    std::vector<uint8_t> bytes, out;
    if (!DecodeImage(in, img, bytes, r.error)) return false;
    r.before = bytes.size();
    p.Set(0.4);
    int jq = Opt(job, L"jpegQ", 88), wq = Opt(job, L"webpQ", 82);
    if (target == Kind::Pdf) {
        PdfWriter pdf;
        pdf.AddImage(img, img.kind == Kind::Jpeg ? &bytes : nullptr, jq);
        if (!pdf.Finish(out, r.error)) return false;
    } else if (!EncodeImage(img, target, target == Kind::Webp ? wq : jq, target == Kind::Webp && LosslessSource(img.kind), out, r.error)) {
        return false;
    }
    if (target == Kind::Jpeg && img.kind == Kind::Jpeg && job.opts[L"keepMeta"].Bool(true)) CopyJpegMetadata(bytes, out);
    p.Set(0.9);
    return WriteOutput(job, in, BaseName(in), ExtOf(target), out, p, r);
}

bool RunImageTool(const Job& job, const Progress& p, Result& r)
{
    const wstring& in = job.inputs[0];
    const wstring& op = job.op;
    std::vector<uint8_t> bytes, out;
    bool keepMeta = job.opts[L"keepMeta"].Bool(true);

    // Strip info: lossless where the format allows it.
    if (op == L"tool.strip") {
        if (!ReadShared(in, bytes, r.error)) return false;
        r.before = bytes.size();
        Kind k = KindFromBytes(bytes.data(), bytes.size(), in);
        bool done = (k == Kind::Jpeg && StripJpeg(bytes, out)) || (k == Kind::Png && StripPng(bytes, out)) || (k == Kind::Webp && StripWebp(bytes, out));
        if (!done) {
            Image img;
            if (!DecodeImage(in, img, bytes, r.error)) return false;
            if (!EncodeImage(img, ToolOutput(img.kind), 95, true, out, r.error)) return false;
            k = ToolOutput(img.kind);
        }
        return WriteOutput(job, in, BaseName(in) + L" no info", ExtOf(k), out, p, r);
    }

    Image img;
    if (!DecodeImage(in, img, bytes, r.error)) return false;
    r.before = bytes.size();
    p.Set(0.3);
    Kind outKind = ToolOutput(img.kind);
    int quality = outKind == Kind::Webp ? Opt(job, L"webpQ", 82) : Opt(job, L"jpegQ", 88);
    bool lossless = LosslessSource(img.kind);
    wstring suffix;

    auto fit = [&](int longest) {
        int side = std::max(img.w, img.h);
        if (side <= longest) return true;   // never enlarge
        double k = double(longest) / side;
        return Resize(img, std::max(1, int(std::lround(img.w * k))), std::max(1, int(std::lround(img.h * k))), r.error);
    };
    if (op == L"tool.compress") {
        quality = std::clamp(Opt(job, L"q", 70), 1, 100);
        suffix = L"compressed";
        lossless = false;
    } else if (op == L"tool.resize50") {
        if (!Resize(img, std::max(1, img.w / 2), std::max(1, img.h / 2), r.error)) return false;
        suffix = L"50%";
    } else if (op == L"tool.resize1920") {
        if (!fit(1920)) return false;
        suffix = L"1920px";
    } else if (op == L"tool.resize1080") {
        if (!fit(1080)) return false;
        suffix = L"1080px";
    } else if (op == L"tool.resize") {
        int w = Opt(job, L"w", 0), h = Opt(job, L"h", 0);
        if (w <= 0 && h <= 0) { r.error = L"No size was given."; return false; }
        if (w <= 0) w = std::max(1, int(std::lround(double(img.w) * h / img.h)));
        if (h <= 0) h = std::max(1, int(std::lround(double(img.h) * w / img.w)));
        if (!Resize(img, w, h, r.error)) return false;
        suffix = std::to_wstring(w) + L"x" + std::to_wstring(h);
    } else if (op == L"tool.crop") {
        // "x,y,w,h" in pixels of the upright picture.
        int c[4] = { 0, 0, 0, 0 };
        if (swscanf(job.opts[L"crop"].Str().c_str(), L"%d,%d,%d,%d", &c[0], &c[1], &c[2], &c[3]) != 4 || c[2] < 1 || c[3] < 1) {
            r.error = L"No crop area was given.";
            return false;
        }
        Crop(img, c[0], c[1], c[2], c[3]);
        suffix = L"cropped";
    } else if (op == L"tool.rotate") {
        Rotate90(img);
        suffix = L"rotated";
    } else if (op == L"tool.flip") {
        FlipH(img);
        suffix = L"flipped";
    } else {
        r.error = L"Unknown tool.";
        return false;
    }
    img.alpha = HasAlpha(img);
    p.Set(0.6);

    if (op == L"tool.compress" && outKind == Kind::Png) {
        // Two tries: 256 colours (much smaller, slightly lossy) and lossless; the smaller one wins.
        std::vector<uint8_t> pal, plain;
        bool havePal = EncodePngPalette(img, pal);
        if (!EncodeImage(img, Kind::Png, 0, true, plain, r.error)) return false;
        out = havePal && pal.size() < plain.size() ? std::move(pal) : std::move(plain);
    } else {
        // Lossless re-encodes of JPEG would still lose a little: use a high quality for them.
        if (outKind == Kind::Jpeg && op != L"tool.compress") quality = std::max(quality, 92);
        if (!EncodeImage(img, outKind, quality, outKind == Kind::Webp && lossless, out, r.error)) return false;
    }
    if (outKind == Kind::Jpeg && img.kind == Kind::Jpeg && keepMeta) CopyJpegMetadata(bytes, out);
    p.Set(0.9);
    return WriteOutput(job, in, BaseName(in) + L" " + suffix, ExtOf(outKind), out, p, r);
}

bool RunPreview(const Job& job, const Progress& p, Result& r)
{
    // A picture the UI can show (Float, the crop popup) without decoding the file itself.
    Image img;
    std::vector<uint8_t> bytes, out;
    if (job.outPath.empty()) { r.error = L"No place for the preview."; return false; }
    if (IsMedia(KindOfPath(job.inputs[0]))) {
        // A video: a still from FFmpeg; its size is in the PNG's header.
        if (!MediaFrame(job.inputs[0], out, r.error)) return false;
        if (out.size() > 24) r.info.Set(L"w", int(out[16] << 24 | out[17] << 16 | out[18] << 8 | out[19])).Set(L"h", int(out[20] << 24 | out[21] << 16 | out[22] << 8 | out[23]));
        return WriteOutput(job, job.inputs[0], L"", L".png", out, p, r);
    }
    if (KindOfPath(job.inputs[0]) == Kind::Pdf) {
        // A PDF's page (the first unless asked), drawn at the size the preview needs.
        int pages = 0;
        if (!PdfPreview(job.inputs[0], Opt(job, L"max", 2048), img, r.error, Opt(job, L"page", 0), &pages)) return false;
        r.info.Set(L"w", img.w).Set(L"h", img.h).Set(L"pages", pages);
        if (!EncodeImage(img, Kind::Png, 0, true, out, r.error)) return false;
        return WriteOutput(job, job.inputs[0], L"", L".png", out, p, r);
    }
    if (!DecodeImage(job.inputs[0], img, bytes, r.error)) return false;
    r.info.Set(L"w", img.w).Set(L"h", img.h);
    int longest = std::clamp(Opt(job, L"max", 2048), 64, 8192);
    int side = std::max(img.w, img.h);
    if (side > longest) {
        double k = double(longest) / side;
        if (!Resize(img, std::max(1, int(std::lround(img.w * k))), std::max(1, int(std::lround(img.h * k))), r.error)) return false;
    }
    if (!EncodeImage(img, Kind::Png, 0, true, out, r.error)) return false;
    p.Set(0.9);
    return WriteOutput(job, job.inputs[0], L"", L".png", out, p, r);
}

bool RunMergePdf(const Job& job, const Progress& p, Result& r)
{
    // PDFs among the inputs: PDFium joins their pages (pictures become pages first).
    for (auto& in : job.inputs)
        if (KindOfPath(in) == Kind::Pdf) return MergeWithPdfium(job, p, r);
    PdfWriter pdf;
    int jq = Opt(job, L"jpegQ", 88);
    for (size_t i = 0; i < job.inputs.size(); i++) {
        Image img;
        std::vector<uint8_t> bytes;
        wstring err;
        if (!DecodeImage(job.inputs[i], img, bytes, err)) { r.error = FileName(job.inputs[i]) + L": " + err; return false; }
        r.before += bytes.size();
        pdf.AddImage(img, img.kind == Kind::Jpeg ? &bytes : nullptr, jq);
        p.Set(0.9 * (i + 1) / job.inputs.size());
    }
    std::vector<uint8_t> out;
    if (!pdf.Finish(out, r.error)) return false;
    return WriteOutput(job, job.inputs[0], BaseName(job.inputs[0]) + L" merged", L".pdf", out, p, r);
}

}  // namespace rd
