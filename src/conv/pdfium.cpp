#include "pdfium.h"

namespace rd {

const Pdfium* LoadPdfium(wstring& err)
{
    static Pdfium api;
    static int state = 0;   // 0 not tried, 1 loaded, -1 failed
    static wstring why;
    if (state == 0) {
        state = -1;
        wstring path = EnginePath(Engine::Pdfium);
        HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) why = L"PDF support isn't installed (engines\\pdfium.dll is missing).";
        else {
            bool all = true;
#define RD_PDFIUM_LOOKUP(name) api.name = reinterpret_cast<decltype(api.name)>(reinterpret_cast<void*>(GetProcAddress(m, #name))); all = all && api.name;
            RD_PDFIUM_FUNCTIONS(RD_PDFIUM_LOOKUP)
#undef RD_PDFIUM_LOOKUP
            if (!all) why = L"engines\\pdfium.dll is a version Roda can't use.";
            else {
                FPDF_LIBRARY_CONFIG cfg{};
                cfg.version = 2;
                api.FPDF_InitLibraryWithConfig(&cfg);
                state = 1;
            }
        }
    }
    if (state < 0) { err = why; return nullptr; }
    return &api;
}

PdfDoc::~PdfDoc()
{
    if (doc_ && p_) p_->FPDF_CloseDocument(doc_);
}

int PdfDoc::pages() const { return doc_ ? p_->FPDF_GetPageCount(doc_) : 0; }

bool PdfDoc::Open(const wstring& path, wstring& err)
{
    std::vector<uint8_t> b;
    if (!ReadShared(path, b, err)) return false;
    if (b.empty()) { err = L"The file is empty."; return false; }
    return OpenMemory(std::move(b), err);
}

bool PdfDoc::OpenMemory(std::vector<uint8_t> bytes, wstring& err)
{
    p_ = LoadPdfium(err);
    if (!p_) return false;
    bytes_ = std::move(bytes);
    doc_ = p_->FPDF_LoadMemDocument64(bytes_.data(), bytes_.size(), nullptr);
    if (doc_) return true;
    switch (p_->FPDF_GetLastError()) {
        case FPDF_ERR_PASSWORD: err = L"This PDF is password protected."; break;
        case FPDF_ERR_SECURITY: err = L"This PDF uses a kind of protection Roda can't open."; break;
        case FPDF_ERR_FORMAT: err = L"This PDF is damaged."; break;
        default: err = L"This PDF can't be opened (it may be damaged)."; break;
    }
    return false;
}

bool RenderPage(const PdfDoc& doc, int index, double dpi, Image& out, wstring& err, int maxSide)
{
    const Pdfium* p = doc.api();
    FPDF_PAGE page = p->FPDF_LoadPage(doc.get(), index);
    if (!page) { err = L"Page " + std::to_wstring(index + 1) + L" can't be read."; return false; }
    double wPt = p->FPDF_GetPageWidthF(page), hPt = p->FPDF_GetPageHeightF(page);
    double k = dpi / 72.0;
    if (maxSide > 0) k = std::min(k, maxSide / std::max(1.0, std::max(wPt, hPt)));
    // A page drawn bigger than ~100 megapixels is refused rather than run out of memory.
    k = std::min(k, std::sqrt(100e6 / std::max(1.0, wPt * hPt)));
    int w = std::max(1, int(std::lround(wPt * k))), h = std::max(1, int(std::lround(hPt * k)));
    FPDF_BITMAP bmp = p->FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRx, nullptr, 0);
    if (!bmp) { p->FPDF_ClosePage(page); err = L"Not enough memory to draw the page."; return false; }
    p->FPDFBitmap_FillRect(bmp, 0, 0, w, h, 0xFFFFFFFF);
    p->FPDF_RenderPageBitmap(bmp, page, 0, 0, w, h, 0, FPDF_ANNOT);   // form fields and comments as a viewer shows them
    out = {};
    out.w = w;
    out.h = h;
    out.kind = Kind::Pdf;
    out.dpiX = out.dpiY = 72.0 * k;
    out.px.resize(size_t(w) * h * 4);
    const uint8_t* src = static_cast<const uint8_t*>(p->FPDFBitmap_GetBuffer(bmp));
    int stride = p->FPDFBitmap_GetStride(bmp);
    for (int y = 0; y < h; y++) {
        uint8_t* d = &out.px[size_t(y) * w * 4];
        memcpy(d, src + size_t(y) * stride, size_t(w) * 4);
        for (int x = 0; x < w; x++) d[x * 4 + 3] = 255;   // BGRx: the fourth byte means nothing
    }
    p->FPDFBitmap_Destroy(bmp);
    p->FPDF_ClosePage(page);
    return true;
}

namespace {

struct Writer : FPDF_FILEWRITE {
    std::vector<uint8_t>* out;
};

int PutBlock(FPDF_FILEWRITE* self, const void* data, unsigned long size)
{
    auto* w = static_cast<Writer*>(self);
    const uint8_t* b = static_cast<const uint8_t*>(data);
    w->out->insert(w->out->end(), b, b + size);
    return 1;
}

}  // namespace

bool SavePdf(const Pdfium* p, FPDF_DOCUMENT doc, std::vector<uint8_t>& out)
{
    out.clear();
    Writer w{};
    w.version = 1;
    w.WriteBlock = PutBlock;
    w.out = &out;
    return p->FPDF_SaveAsCopy(doc, &w, FPDF_NO_INCREMENTAL) && !out.empty();
}

}  // namespace rd
