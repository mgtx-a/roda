// PDFium, loaded by full path from engines\pdfium.dll (never by bare name, so no DLL from another
// folder can stand in), only inside a worker and only when a PDF is opened. Functions are looked up
// by name; the types come from PDFium's own headers.
#pragma once
#include "conv.h"

#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdf_ppo.h>
#include <fpdf_save.h>
#include <fpdf_text.h>
#include <fpdfview.h>

namespace rd {

#define RD_PDFIUM_FUNCTIONS(X) \
    X(FPDF_InitLibraryWithConfig) X(FPDF_LoadMemDocument64) X(FPDF_CloseDocument) X(FPDF_GetLastError) \
    X(FPDF_GetPageCount) X(FPDF_LoadPage) X(FPDF_ClosePage) X(FPDF_GetPageWidthF) X(FPDF_GetPageHeightF) \
    X(FPDFBitmap_CreateEx) X(FPDFBitmap_FillRect) X(FPDFBitmap_GetBuffer) X(FPDFBitmap_GetStride) \
    X(FPDFBitmap_GetWidth) X(FPDFBitmap_GetHeight) X(FPDFBitmap_GetFormat) X(FPDFBitmap_Destroy) X(FPDF_RenderPageBitmap) \
    X(FPDFText_LoadPage) X(FPDFText_ClosePage) X(FPDFText_CountChars) X(FPDFText_GetUnicode) X(FPDFText_GetFontSize) \
    X(FPDFText_GetCharBox) X(FPDFText_GetFontWeight) X(FPDFText_GetFontInfo) X(FPDFImageObj_GetImageDataDecoded) \
    X(FPDF_CreateNewDocument) X(FPDF_ImportPages) X(FPDF_SaveAsCopy) X(FPDFPage_GetRotation) X(FPDFPage_SetRotation) \
    X(FPDFPage_CountObjects) X(FPDFPage_GetObject) X(FPDFPageObj_GetType) X(FPDFImageObj_GetBitmap) \
    X(FPDFImageObj_GetImageDataRaw) X(FPDFImageObj_GetImageFilterCount) X(FPDFImageObj_GetImageFilter) \
    X(FPDFFormObj_CountObjects) X(FPDFFormObj_GetObject)

struct Pdfium {
#define RD_PDFIUM_MEMBER(name) decltype(&::name) name = nullptr;
    RD_PDFIUM_FUNCTIONS(RD_PDFIUM_MEMBER)
#undef RD_PDFIUM_MEMBER
};

/// The loaded library, or null (err says why: missing DLL, or a version without these functions).
const Pdfium* LoadPdfium(wstring& err);

/// An open PDF, closed with the object. The file's bytes stay alive as long as the document.
class PdfDoc {
public:
    ~PdfDoc();
    bool Open(const wstring& path, wstring& err);
    bool OpenMemory(std::vector<uint8_t> bytes, wstring& err);
    FPDF_DOCUMENT get() const { return doc_; }
    int pages() const;
    const Pdfium* api() const { return p_; }
private:
    const Pdfium* p_ = nullptr;
    FPDF_DOCUMENT doc_ = nullptr;
    std::vector<uint8_t> bytes_;
};

/// Renders one page to straight-alpha BGRA (white paper, as a viewer shows it).
bool RenderPage(const PdfDoc& doc, int index, double dpi, Image& out, wstring& err, int maxSide = 0);
/// Saves a document into memory.
bool SavePdf(const Pdfium* p, FPDF_DOCUMENT doc, std::vector<uint8_t>& out);

}  // namespace rd
