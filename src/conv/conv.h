// The run functions behind the converter table, and the helpers they share. They run in a worker
// process (or the CLI), never on the UI thread: file I/O and decoders live here.
#pragma once
#include "../registry.h"

namespace rd {

// image.cpp
bool RunToImage(const Job& job, const Progress& p, Result& r);     // "to.jpg", "to.png", ...
bool RunImageTool(const Job& job, const Progress& p, Result& r);   // "tool.*"
bool RunMergePdf(const Job& job, const Progress& p, Result& r);    // "tool.mergepdf"
bool RunPreview(const Job& job, const Progress& p, Result& r);     // "prep.preview": PNG at outPath, info = original size

// convert.cpp, pdfconv.cpp
bool RunConvert(const Job& job, const Progress& p, Result& r);     // every "to.*": picks the reader by the input's kind
bool RunPdfTool(const Job& job, const Progress& p, Result& r);     // "pdf.split", "pdf.images", "pdf.rotate"
bool MergeWithPdfium(const Job& job, const Progress& p, Result& r); // Merge PDF when a PDF is among the inputs
bool RunMedia(const Job& job, const Progress& p, Result& r);       // audio and video, "to.mp4" ... and "media.*"
bool MediaFrame(const wstring& path, std::vector<uint8_t>& png, wstring& err);   // a still from a video, for previews
bool FfmpegImage(const wstring& path, std::vector<uint8_t>& png, wstring& err);  // HEIC/AVIF pictures Windows can't read, as PNG
struct Image;
bool PdfPreview(const wstring& path, int maxSide, Image& img, wstring& err);

// archive.cpp
bool RunPack(const Job& job, const Progress& p, Result& r);        // "pack.zip", "pack.tar", "pack.targz"
bool RunExtract(const Job& job, const Progress& p, Result& r);     // "tool.extract"

/// Decoded picture: straight-alpha BGRA, top row first.
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    bool alpha = false;              // some pixel is not fully opaque
    Kind kind = Kind::Unknown;       // what it was decoded from
    double dpiX = 96, dpiY = 96;
    bool turned = false;             // EXIF orientation was applied: the file's own bytes are not upright
};
/// Decodes the first frame, with EXIF orientation applied. `bytes` keeps the file's contents for
/// lossless paths (JPEG into PDF, metadata copies). Errors are plain words.
bool DecodeImage(const wstring& path, Image& img, std::vector<uint8_t>& bytes, wstring& err);
/// Reads a whole file, sharing it with whoever else has it open.
bool ReadShared(const wstring& path, std::vector<uint8_t>& out, wstring& err);
/// Encodes to `kind` (Jpeg, Png, Webp, Tiff, Bmp, Ico, Pdf). `quality` 1..100 for lossy formats.
bool EncodeImage(const Image& img, Kind kind, int quality, bool lossless, std::vector<uint8_t>& out, wstring& err);

// output.cpp: unique names, written as <name>.part and renamed only on success.
struct OutFile {
    wstring final, part;
    bool folder = false, exact = false;
};
/// `base` without extension; `ext` with the dot ("" for a folder).
bool ReserveOutput(const Job& job, const wstring& nextTo, const wstring& base, const wstring& ext, bool folder, OutFile& o, wstring& err);
bool CommitOutput(OutFile& o, wstring& err);    // o.final is the name actually used
void AbandonOutput(OutFile& o);                 // deletes the partial output
bool WriteOutput(const Job& job, const wstring& nextTo, const wstring& base, const wstring& ext, const std::vector<uint8_t>& data,
                 const Progress& p, Result& r);
wstring LongPath(const wstring& path);          // \\?\ form for paths near or over MAX_PATH
wstring BaseName(const wstring& path);          // file name without its extension
uint64_t FileSize(const wstring& path);

}  // namespace rd
