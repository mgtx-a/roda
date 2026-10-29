#include "registry.h"

#include "conv/conv.h"

namespace rd {

const wchar_t* WheelName(WheelKind k)
{
    switch (k) {
        case WheelKind::Convert: return L"Convert";
        case WheelKind::Tools: return L"Tools";
        default: return L"Files";
    }
}

// ---------------------------------------------------------------- kinds

Kind KindFromExtension(const wstring& path)
{
    wstring e = Extension(path);
    wstring name = FileName(path);
    for (auto& c : name) c = wchar_t(towlower(c));
    if (e == L".jpg" || e == L".jpeg" || e == L".jpe" || e == L".jfif") return Kind::Jpeg;
    if (e == L".png") return Kind::Png;
    if (e == L".webp") return Kind::Webp;
    if (e == L".gif") return Kind::Gif;
    if (e == L".bmp" || e == L".dib") return Kind::Bmp;
    if (e == L".tif" || e == L".tiff") return Kind::Tiff;
    if (e == L".ico") return Kind::Ico;
    if (e == L".svg") return Kind::Svg;
    if (e == L".heic" || e == L".heif") return Kind::Heic;
    if (e == L".avif") return Kind::Avif;
    if (e == L".zip") return Kind::Zip;
    if (e == L".tar") return Kind::Tar;
    if (e == L".gz" || e == L".tgz") return Kind::Gzip;
    if (e == L".rar") return Kind::Rar;
    if (e == L".pdf") return Kind::Pdf;
    if (e == L".docx") return Kind::Docx;
    if (e == L".md" || e == L".markdown") return Kind::Markdown;
    if (e == L".html" || e == L".htm" || e == L".xhtml") return Kind::Html;
    if (e == L".rtf") return Kind::Rtf;
    if (e == L".txt" || e == L".text") return Kind::Txt;
    if (e == L".xlsx") return Kind::Xlsx;
    if (e == L".ods") return Kind::Ods;
    if (e == L".csv") return Kind::Csv;
    if (e == L".tsv" || e == L".tab") return Kind::Tsv;
    if (e == L".xls") return Kind::Xls;
    if (e == L".doc") return Kind::Doc;
    if (e == L".pptx") return Kind::Pptx;
    if (e == L".ppt") return Kind::Ppt;
    if (e == L".mp3") return Kind::Mp3;
    if (e == L".m4a" || e == L".aac" || e == L".m4b") return Kind::M4a;
    if (e == L".wav") return Kind::Wav;
    if (e == L".flac") return Kind::Flac;
    if (e == L".ogg" || e == L".oga") return Kind::Ogg;
    if (e == L".opus") return Kind::Opus;
    if (e == L".aif" || e == L".aiff" || e == L".aifc") return Kind::Aiff;
    if (e == L".wma") return Kind::Wma;
    if (e == L".mp4" || e == L".m4v") return Kind::Mp4;
    if (e == L".mov" || e == L".qt") return Kind::Mov;
    if (e == L".mkv") return Kind::Mkv;
    if (e == L".webm") return Kind::Webm;
    if (e == L".avi") return Kind::Avi;
    if (e == L".wmv" || e == L".asf") return Kind::Wmv;
    if (e == L".srt") return Kind::Srt;
    if (e == L".vtt") return Kind::Vtt;
    return Kind::Unknown;
}

Kind KindFromBytes(const uint8_t* p, size_t n, const wstring& path)
{
    auto at = [&](size_t off, const char* sig) {
        size_t len = strlen(sig);
        return n >= off + len && memcmp(p + off, sig, len) == 0;
    };
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return Kind::Jpeg;
    if (at(0, "\x89PNG\r\n\x1a\n")) return Kind::Png;
    if (at(0, "RIFF") && at(8, "WEBP")) return Kind::Webp;
    if (at(0, "RIFF") && at(8, "WAVE")) return Kind::Wav;
    if (at(0, "RIFF") && at(8, "AVI ")) return Kind::Avi;
    if (at(0, "FORM") && (at(8, "AIFF") || at(8, "AIFC"))) return Kind::Aiff;
    if (at(0, "fLaC")) return Kind::Flac;
    if (at(0, "OggS")) return n > 36 && memcmp(p + 28, "OpusHead", 8) == 0 ? Kind::Opus : Kind::Ogg;
    if (at(0, "\x1A\x45\xDF\xA3")) {   // Matroska; WebM says so in its header
        for (size_t i = 0; i + 4 < std::min<size_t>(n, 64); i++) if (memcmp(p + i, "webm", 4) == 0) return Kind::Webm;
        return Kind::Mkv;
    }
    if (at(0, "\x30\x26\xB2\x75\x8E\x66\xCF\x11")) return KindFromExtension(path) == Kind::Wma ? Kind::Wma : Kind::Wmv;   // ASF
    if (at(0, "ID3") || (n > 2 && p[0] == 0xFF && (p[1] & 0xE6) == 0xE2 && KindFromExtension(path) == Kind::Mp3)) return Kind::Mp3;
    if (at(0, "GIF87a") || at(0, "GIF89a")) return Kind::Gif;
    if (at(0, "BM") && n >= 14) return Kind::Bmp;
    if (at(0, "II*\0") || at(0, "MM\0*")) return Kind::Tiff;
    if (n >= 6 && p[0] == 0 && p[1] == 0 && p[2] == 1 && p[3] == 0 && (p[4] | p[5]) != 0) return Kind::Ico;
    if (at(4, "ftyp")) {
        if (at(8, "avif") || at(8, "avis")) return Kind::Avif;
        if (at(8, "heic") || at(8, "heix") || at(8, "mif1") || at(8, "msf1") || at(8, "heim") || at(8, "heis") || at(8, "hevc")) return Kind::Heic;
        if (at(8, "qt  ")) return Kind::Mov;
        if (at(8, "M4A ") || at(8, "M4B ")) return Kind::M4a;
        Kind e = KindFromExtension(path);
        return e == Kind::M4a || e == Kind::Mov ? e : Kind::Mp4;
    }
    if (at(0, "PK\x03\x04") || at(0, "PK\x05\x06")) {
        // .docx, .xlsx and .ods are zips too: the name says which.
        Kind e = KindFromExtension(path);
        return e == Kind::Docx || e == Kind::Xlsx || e == Kind::Ods || e == Kind::Pptx ? e : Kind::Zip;
    }
    if (at(0, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1")) {   // old Office files: the name says which
        Kind e = KindFromExtension(path);
        return e == Kind::Xls || e == Kind::Doc || e == Kind::Ppt ? e : Kind::Unknown;
    }
    if (at(0, "{\\rtf")) return Kind::Rtf;
    if (at(0, "WEBVTT") || at(3, "WEBVTT")) return Kind::Vtt;
    if (at(0, "\x1f\x8b")) return Kind::Gzip;
    if (at(0, "Rar!\x1a\x07")) return Kind::Rar;
    if (at(257, "ustar")) return Kind::Tar;
    if (at(0, "%PDF-")) return Kind::Pdf;
    Kind e = KindFromExtension(path);
    // Text formats have no magic: trust the name. Old tars have none either.
    if (e == Kind::Svg || e == Kind::Tar || e == Kind::Markdown || e == Kind::Html || e == Kind::Txt || e == Kind::Csv ||
        e == Kind::Tsv || e == Kind::Srt) return e;
    return Kind::Unknown;   // the name says one thing, the bytes another (or nothing): don't guess
}

Kind KindOfPath(const wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return KindFromExtension(path);
    if (a & FILE_ATTRIBUTE_DIRECTORY) return Kind::Folder;
    // Reading a cloud-only file would download it: go by the name.
    if (a & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN)) return KindFromExtension(path);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return KindFromExtension(path);
    uint8_t buf[512];
    DWORD got = 0;
    ReadFile(h, buf, sizeof buf, &got, nullptr);
    CloseHandle(h);
    return KindFromBytes(buf, got, path);
}

const wchar_t* KindLabel(Kind k)
{
    static const wchar_t* names[] = { L"", L"folder", L"JPG", L"PNG", L"WEBP", L"GIF", L"BMP", L"TIFF", L"ICO", L"SVG",
                                      L"HEIC", L"AVIF", L"ZIP", L"TAR", L"GZIP", L"RAR", L"PDF", L"DOCX", L"Markdown", L"HTML",
                                      L"RTF", L"TXT", L"XLSX", L"ODS", L"CSV", L"TSV", L"XLS", L"SRT", L"VTT", L"DOC", L"PPTX", L"PPT", L"MP3", L"M4A", L"WAV", L"FLAC", L"OGG", L"OPUS",
                                      L"AIFF", L"WMA", L"MP4", L"MOV", L"MKV", L"WEBM", L"AVI", L"WMV" };
    return unsigned(k) < sizeof names / sizeof names[0] ? names[unsigned(k)] : L"";
}

// ---------------------------------------------------------------- jobs

Json Job::ToJson() const
{
    Json j = Json::Object();
    j.Set(L"op", op);
    Json in = Json::Array();
    for (auto& p : inputs) in.Push(p);
    j.Set(L"inputs", in);
    if (!outDir.empty()) j.Set(L"outDir", outDir);
    if (!outPath.empty()) j.Set(L"outPath", outPath);
    j.Set(L"opts", opts);
    return j;
}

bool Job::FromJson(const Json& j, Job& out)
{
    out.op = j[L"op"].Str();
    out.inputs.clear();
    for (auto& p : j[L"inputs"].arr) out.inputs.push_back(p.Str());
    out.outDir = j[L"outDir"].Str();
    out.outPath = j[L"outPath"].Str();
    out.opts = j[L"opts"].type == Json::Type::Object ? j[L"opts"] : Json::Object();
    return !out.op.empty() && !out.inputs.empty();
}

// ---------------------------------------------------------------- the table

namespace {

constexpr KindSet kCompressible = Bit(Kind::Jpeg) | Bit(Kind::Png) | Bit(Kind::Webp);
constexpr KindSet kStrippable = Bit(Kind::Jpeg) | Bit(Kind::Png) | Bit(Kind::Webp) | Bit(Kind::Tiff);
constexpr KindSet kArchives = Bit(Kind::Zip) | Bit(Kind::Tar) | Bit(Kind::Gzip);
using C = Converter;

}  // namespace

const std::vector<Converter>& Converters()
{
    static const std::vector<Converter> table = {
        // Convert: one output per input, named after the target. A PDF only shows up on these wheels
        // when PDFium is there. No AVIF or HEIC output: their encoders would double the size of the Full build.
        { L"to.jpg",  WheelKind::Convert, L"JPG",  kImages | Bit(Kind::Pdf), Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert },
        { L"to.png",  WheelKind::Convert, L"PNG",  kImages | Bit(Kind::Pdf), Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert },
        { L"to.webp", WheelKind::Convert, L"WEBP", kImages, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert },
        { L"to.pdf",  WheelKind::Convert, L"PDF",  kImages, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert },
        { L"to.tiff", WheelKind::Convert, L"TIFF", kImages, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert },
        { L"to.bmp",  WheelKind::Convert, L"BMP",  kImages, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert },
        { L"to.ico",  WheelKind::Convert, L"ICO",  kImages, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert },
        { L"to.docx", WheelKind::Convert, L"DOCX", Bit(Kind::Pdf) | Bit(Kind::Markdown) | Bit(Kind::Txt) | Bit(Kind::Html) | Bit(Kind::Rtf), Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert },
        { L"to.html", WheelKind::Convert, L"HTML", Bit(Kind::Docx) | Bit(Kind::Markdown) | Bit(Kind::Rtf) | Bit(Kind::Txt), Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert },
        { L"to.md",   WheelKind::Convert, L"MD",   Bit(Kind::Pdf) | Bit(Kind::Docx) | Bit(Kind::Html) | Bit(Kind::Rtf), Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert },
        { L"to.txt",  WheelKind::Convert, L"TXT",  Bit(Kind::Pdf) | kDocs | kSubtitles, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert },
        { L"to.epub", WheelKind::Convert, L"EPUB", Bit(Kind::Pdf) | Bit(Kind::Docx) | Bit(Kind::Txt) | Bit(Kind::Markdown), Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert },
        { L"to.pptx", WheelKind::Convert, L"PPTX", Bit(Kind::Pdf), Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert },
        { L"to.xlsx", WheelKind::Convert, L"XLSX", kSheets | Bit(Kind::Pdf), Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert },
        { L"to.ods",  WheelKind::Convert, L"ODS",  kSheets, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert },
        { L"to.csv",  WheelKind::Convert, L"CSV",  kSheets, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert },
        { L"to.tsv",  WheelKind::Convert, L"TSV",  kSheets, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert },
        { L"to.srt",  WheelKind::Convert, L"SRT",  kSubtitles, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert },
        { L"to.vtt",  WheelKind::Convert, L"VTT",  kSubtitles, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert },
        // Audio and video, through FFmpeg (the Full build). A video's sound track can become any audio format.
        { L"to.mp4", WheelKind::Convert, L"MP4", kVideo | Bit(Kind::Gif), Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert, Engine::Ffmpeg },
        { L"to.mov", WheelKind::Convert, L"MOV", kVideo, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert, Engine::Ffmpeg },
        { L"to.mkv", WheelKind::Convert, L"MKV", kVideo, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert, Engine::Ffmpeg },
        { L"to.webm", WheelKind::Convert, L"WEBM", kVideo | Bit(Kind::Gif), Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert, Engine::Ffmpeg },
        { L"to.avi", WheelKind::Convert, L"AVI", kVideo, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert, Engine::Ffmpeg },
        { L"to.wmv", WheelKind::Convert, L"WMV", kVideo, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert, Engine::Ffmpeg },
        { L"to.gif", WheelKind::Convert, L"GIF", kVideo, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert, Engine::Ffmpeg },
        { L"to.mp3", WheelKind::Convert, L"MP3", kAudio | kVideo | Bit(Kind::Pdf), Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert, Engine::Ffmpeg },
        { L"to.m4a", WheelKind::Convert, L"M4A", kAudio | kVideo, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert, Engine::Ffmpeg },
        { L"to.wav", WheelKind::Convert, L"WAV", kAudio | kVideo, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert, Engine::Ffmpeg },
        { L"to.flac", WheelKind::Convert, L"FLAC", kAudio | kVideo, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert, Engine::Ffmpeg },
        { L"to.ogg", WheelKind::Convert, L"OGG", kAudio | kVideo, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert, Engine::Ffmpeg },
        { L"to.opus", WheelKind::Convert, L"OPUS", kAudio | kVideo, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert, Engine::Ffmpeg },
        { L"to.aiff", WheelKind::Convert, L"AIFF", kAudio | kVideo, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert, Engine::Ffmpeg },
        { L"to.wma", WheelKind::Convert, L"WMA", kAudio | kVideo, Icon::None, C::PerFile, 1, 0, Popup::None, RunConvert, Engine::Ffmpeg },

        // Tools.
        { L"tool.compress",   WheelKind::Tools, L"Compress",    kCompressible, Icon::Compress, C::PerFile, 1, 0, Popup::Quality, RunImageTool },
        { L"tool.resize50",   WheelKind::Tools, L"50%",         kImages, Icon::Resize, C::PerFile, 1, 0, Popup::None, RunImageTool },
        { L"tool.resize1920", WheelKind::Tools, L"1920 px",     kImages, Icon::Resize, C::PerFile, 1, 0, Popup::None, RunImageTool },
        { L"tool.resize1080", WheelKind::Tools, L"1080 px",     kImages, Icon::Resize, C::PerFile, 1, 0, Popup::None, RunImageTool },
        { L"tool.resize",     WheelKind::Tools, L"Size…",       kImages, Icon::Resize, C::PerFile, 1, 0, Popup::Size, RunImageTool },
        { L"tool.crop",       WheelKind::Tools, L"Crop",        kImages, Icon::Crop, C::PerFile, 1, 1, Popup::Crop, RunImageTool },
        { L"tool.rotate",     WheelKind::Tools, L"Rotate",      kImages, Icon::Rotate, C::PerFile, 1, 0, Popup::None, RunImageTool },
        { L"tool.flip",       WheelKind::Tools, L"Flip",        kImages, Icon::Flip, C::PerFile, 1, 0, Popup::None, RunImageTool },
        { L"tool.strip",      WheelKind::Tools, L"Strip info",  kStrippable, Icon::Strip, C::PerFile, 1, 0, Popup::None, RunImageTool },
        { L"tool.mergepdf",   WheelKind::Tools, L"Merge PDF",   kImages | Bit(Kind::Pdf), Icon::Merge, C::AllInOne, 2, 0, Popup::Order, RunMergePdf },
        { L"pdf.split",       WheelKind::Tools, L"Split",       Bit(Kind::Pdf), Icon::Split, C::PerFile, 1, 0, Popup::None, RunPdfTool, Engine::Pdfium },
        { L"pdf.images",      WheelKind::Tools, L"Images",      Bit(Kind::Pdf), Icon::Images, C::PerFile, 1, 0, Popup::None, RunPdfTool, Engine::Pdfium },
        { L"pdf.rotate",      WheelKind::Tools, L"Rotate",      Bit(Kind::Pdf), Icon::Rotate, C::PerFile, 1, 0, Popup::None, RunPdfTool, Engine::Pdfium },
        { L"pdf.remove",      WheelKind::Tools, L"Remove pages", Bit(Kind::Pdf), Icon::RemovePage, C::PerFile, 1, 0, Popup::Pages, RunPdfTool, Engine::Pdfium },
        { L"pdf.sign",        WheelKind::Tools, L"Sign",        Bit(Kind::Pdf), Icon::Sign, C::PerFile, 1, 1, Popup::Place, RunPdfTool, Engine::Pdfium },
        { L"pdf.fill",        WheelKind::Tools, L"Fill form",   Bit(Kind::Pdf), Icon::Form, C::PerFile, 1, 1, Popup::Place, RunPdfTool, Engine::Pdfium },
        { L"pdf.picture",     WheelKind::Tools, L"Add picture", Bit(Kind::Pdf), Icon::Picture, C::PerFile, 1, 1, Popup::Place, RunPdfTool, Engine::Pdfium },
        { L"tool.extract",    WheelKind::Tools, L"Extract",     kArchives, Icon::Extract, C::PerFile, 1, 0, Popup::None, RunExtract },
        { L"media.trim",      WheelKind::Tools, L"Trim",        kAudio | kVideo, Icon::Trim, C::PerFile, 1, 1, Popup::Trim, RunMedia, Engine::Ffmpeg },
        { L"media.compress",  WheelKind::Tools, L"Compress",    kAudio | kVideo, Icon::Compress, C::PerFile, 1, 0, Popup::None, RunMedia, Engine::Ffmpeg },
        { L"media.audio",     WheelKind::Tools, L"Audio only",  kVideo, Icon::Audio, C::PerFile, 1, 0, Popup::None, RunMedia, Engine::Ffmpeg },
        { L"media.mute",      WheelKind::Tools, L"Mute",        kVideo, Icon::Mute, C::PerFile, 1, 0, Popup::None, RunMedia, Engine::Ffmpeg },
        { L"media.1080",      WheelKind::Tools, L"1080p",       kVideo, Icon::Resize, C::PerFile, 1, 0, Popup::None, RunMedia, Engine::Ffmpeg },
        { L"media.720",       WheelKind::Tools, L"720p",        kVideo, Icon::Resize, C::PerFile, 1, 0, Popup::None, RunMedia, Engine::Ffmpeg },
        { L"media.480",       WheelKind::Tools, L"480p",        kVideo, Icon::Resize, C::PerFile, 1, 0, Popup::None, RunMedia, Engine::Ffmpeg },
        { L"media.crop",      WheelKind::Tools, L"Crop",        kVideo, Icon::Crop, C::PerFile, 1, 1, Popup::Crop, RunMedia, Engine::Ffmpeg },
        { L"media.merge",     WheelKind::Tools, L"Join",        kAudio, Icon::Merge, C::AllInOne, 2, 0, Popup::None, RunMedia, Engine::Ffmpeg },

        // Actions: anything, folders and mixed selections included. Run by the UI, except packing.
        { L"act.copyto",   WheelKind::Actions, L"Copy to…",  kAnything, Icon::Copy, C::Action, 1, 0, Popup::None, nullptr },
        { L"act.moveto",   WheelKind::Actions, L"Move to…",  kAnything, Icon::Move, C::Action, 1, 0, Popup::None, nullptr },
        // A recent "Copy to" folder: one segment per folder, added below.
        { L"act.copyrecent", WheelKind::Actions, L"",        kAnything, Icon::Folder, C::Action, 1, 0, Popup::None, nullptr },
        { L"act.copypath", WheelKind::Actions, L"Copy path", kAnything, Icon::Path, C::Action, 1, 0, Popup::None, nullptr },
        { L"act.terminal", WheelKind::Actions, L"Terminal",  kAnything, Icon::Terminal, C::Action, 1, 0, Popup::None, nullptr },
        { L"act.float",    WheelKind::Actions, L"Float",     kImages | Bit(Kind::Pdf) | kVideo, Icon::Float, C::Action, 1, 1, Popup::None, nullptr },
        { L"pack.zip",     WheelKind::Actions, L"ZIP",       kAnything, Icon::Archive, C::AllInOne, 1, 0, Popup::None, RunPack },
        { L"pack.targz",   WheelKind::Actions, L"TAR.GZ",    kAnything, Icon::Archive, C::AllInOne, 1, 0, Popup::None, RunPack },
        { L"pack.tar",     WheelKind::Actions, L"TAR",       kAnything, Icon::Archive, C::AllInOne, 1, 0, Popup::None, RunPack },
        // Not on any wheel (accepts nothing): a downscaled PNG for Float and the crop popup.
        { L"prep.fields",  WheelKind::Actions, L"", 0, Icon::None, C::PerFile, 1, 1, Popup::None, RunPdfTool },
        { L"prep.preview", WheelKind::Actions, L"", 0, Icon::None, C::PerFile, 1, 1, Popup::None, RunPreview },
    };
    return table;
}

wstring EnginePath(Engine e)
{
    if (e == Engine::Pdfium) return JoinPath(ExeDir(), L"engines\\pdfium.dll");
    if (e == Engine::Ffmpeg) return JoinPath(ExeDir(), L"engines\\ffmpeg\\ffmpeg.exe");
    return L"";
}

bool EngineAvailable(Engine e)
{
    if (e == Engine::None) return true;
    // Looked for once: installing Office or adding the DLL while Roda runs is rare (a restart picks it up).
    static int found[6] = { 1, -1, -1, -1, -1, -1 };
    int& f = found[int(e)];
    if (f < 0) {
        if (e == Engine::Pdfium || e == Engine::Ffmpeg) f = GetFileAttributesW(EnginePath(e).c_str()) != INVALID_FILE_ATTRIBUTES;
        else {
            // An Office app is installed when its automation name is registered (only read, never written).
            const wchar_t* prog = e == Engine::Word ? L"Word.Application" : e == Engine::Excel ? L"Excel.Application" : L"PowerPoint.Application";
            CLSID clsid;
            f = SUCCEEDED(CLSIDFromProgID(prog, &clsid));
        }
    }
    return f == 1;
}

const Converter* FindConverter(const wstring& id)
{
    for (auto& c : Converters())
        if (id == c.id) return &c;
    return nullptr;
}

std::vector<WheelOption> OptionsFor(WheelKind wheel, const std::vector<Kind>& kinds, const Settings& settings)
{
    std::vector<WheelOption> out;
    if (kinds.empty()) return out;
    KindSet all = 0;
    bool allSame = true;
    for (Kind k : kinds) { all |= Bit(k); allSame = allSame && k == kinds[0]; }
    int n = int(kinds.size());
    // Without PDFium, PDFs are read by nothing: no row accepts them.
    KindSet readable = EngineAvailable(Engine::Pdfium) ? kAnything : ~Bit(Kind::Pdf);
    for (auto& c : Converters()) {
        KindSet inputs = c.inputs == kAnything ? kAnything : (c.inputs & readable);
        if (wcscmp(c.id, L"to.pdf") == 0) {
            // Office documents become PDFs through Office itself, when it is there.
            if (EngineAvailable(Engine::Word)) inputs |= kWordFiles;
            if (EngineAvailable(Engine::Excel)) inputs |= kExcelFiles;
            if (EngineAvailable(Engine::PowerPoint)) inputs |= kPowerPointFiles;
        }
        if (c.wheel != wheel || (all & ~inputs) != 0 || n < c.minItems || (c.maxItems && n > c.maxItems)) continue;
        if (c.needs != Engine::None && !EngineAvailable(c.needs)) continue;
        // No "PNG" on a wheel of PNGs: converting a file to its own format does nothing useful.
        if (wheel == WheelKind::Convert && allSame) {
            Kind target = KindFromExtension(wstring(L"x.") + (c.id + 3));
            if (target == kinds[0]) continue;
        }
        if (wcscmp(c.id, L"act.copyrecent") == 0) {
            for (auto& folder : settings.recentFolders) {
                if (folder.empty()) continue;
                out.push_back({ &c, L"→ " + FileName(folder.back() == L'\\' ? folder.substr(0, folder.size() - 1) : folder), folder, c.icon });
            }
            continue;
        }
        out.push_back({ &c, c.label, L"", c.icon });
    }
    return out;
}

}  // namespace rd
