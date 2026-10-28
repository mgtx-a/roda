#include "registry.h"

#include "conv/conv.h"

namespace rd {

const wchar_t* WheelName(WheelKind k)
{
    switch (k) {
        case WheelKind::Convert: return L"Convert";
        case WheelKind::Tools: return L"Tools";
        default: return L"Actions";
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
    if (at(0, "GIF87a") || at(0, "GIF89a")) return Kind::Gif;
    if (at(0, "BM") && n >= 14) return Kind::Bmp;
    if (at(0, "II*\0") || at(0, "MM\0*")) return Kind::Tiff;
    if (n >= 6 && p[0] == 0 && p[1] == 0 && p[2] == 1 && p[3] == 0 && (p[4] | p[5]) != 0) return Kind::Ico;
    if (at(4, "ftyp")) {
        if (at(8, "avif") || at(8, "avis")) return Kind::Avif;
        if (at(8, "heic") || at(8, "heix") || at(8, "mif1") || at(8, "msf1") || at(8, "heim") || at(8, "heis") || at(8, "hevc")) return Kind::Heic;
    }
    if (at(0, "PK\x03\x04") || at(0, "PK\x05\x06")) {
        Kind e = KindFromExtension(path);
        return e == Kind::Unknown ? Kind::Zip : (e == Kind::Zip ? Kind::Zip : Kind::Unknown);   // .docx, .xlsx and friends are zips too
    }
    if (at(0, "\x1f\x8b")) return Kind::Gzip;
    if (at(0, "Rar!\x1a\x07")) return Kind::Rar;
    if (at(257, "ustar")) return Kind::Tar;
    if (at(0, "%PDF-")) return Kind::Pdf;
    Kind e = KindFromExtension(path);
    if (e == Kind::Svg) return Kind::Svg;   // text: trust the name
    if (e == Kind::Tar) return Kind::Tar;   // old tars have no magic
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
                                      L"HEIC", L"AVIF", L"ZIP", L"TAR", L"GZIP", L"RAR", L"PDF" };
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
        // Convert: one output per input. AVIF and HEIC output need encoders Roda doesn't ship yet.
        { L"to.jpg",  WheelKind::Convert, L"JPG",  kImages, Icon::None, C::PerFile, 1, 0, Popup::None, RunToImage },
        { L"to.png",  WheelKind::Convert, L"PNG",  kImages, Icon::None, C::PerFile, 1, 0, Popup::None, RunToImage },
        { L"to.webp", WheelKind::Convert, L"WEBP", kImages, Icon::None, C::PerFile, 1, 0, Popup::None, RunToImage },
        { L"to.pdf",  WheelKind::Convert, L"PDF",  kImages, Icon::None, C::PerFile, 1, 0, Popup::None, RunToImage },
        { L"to.tiff", WheelKind::Convert, L"TIFF", kImages, Icon::None, C::PerFile, 1, 0, Popup::None, RunToImage },
        { L"to.bmp",  WheelKind::Convert, L"BMP",  kImages, Icon::None, C::PerFile, 1, 0, Popup::None, RunToImage },
        { L"to.ico",  WheelKind::Convert, L"ICO",  kImages, Icon::None, C::PerFile, 1, 0, Popup::None, RunToImage },

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
        { L"tool.mergepdf",   WheelKind::Tools, L"Merge PDF",   kImages, Icon::Merge, C::AllInOne, 2, 0, Popup::None, RunMergePdf },
        { L"tool.extract",    WheelKind::Tools, L"Extract",     kArchives, Icon::Extract, C::PerFile, 1, 0, Popup::None, RunExtract },

        // Actions: anything, folders and mixed selections included. Run by the UI, except packing.
        { L"act.copyto",   WheelKind::Actions, L"Copy to…",  kAnything, Icon::Copy, C::Action, 1, 0, Popup::None, nullptr },
        { L"act.moveto",   WheelKind::Actions, L"Move to…",  kAnything, Icon::Move, C::Action, 1, 0, Popup::None, nullptr },
        { L"act.copypath", WheelKind::Actions, L"Copy path", kAnything, Icon::Path, C::Action, 1, 0, Popup::None, nullptr },
        { L"act.terminal", WheelKind::Actions, L"Terminal",  kAnything, Icon::Terminal, C::Action, 1, 0, Popup::None, nullptr },
        { L"act.float",    WheelKind::Actions, L"Float",     kImages, Icon::Float, C::Action, 1, 1, Popup::None, nullptr },
        { L"act.share",    WheelKind::Actions, L"Share",     kAnything & ~Bit(Kind::Folder), Icon::Share, C::Action, 1, 0, Popup::None, nullptr },
        { L"pack.zip",     WheelKind::Actions, L"ZIP",       kAnything, Icon::Archive, C::AllInOne, 1, 0, Popup::None, RunPack },
        { L"pack.targz",   WheelKind::Actions, L"TAR.GZ",    kAnything, Icon::Archive, C::AllInOne, 1, 0, Popup::None, RunPack },
        { L"pack.tar",     WheelKind::Actions, L"TAR",       kAnything, Icon::Archive, C::AllInOne, 1, 0, Popup::None, RunPack },
        // A recent "Copy to" folder: one segment per folder, added below.
        { L"act.copyrecent", WheelKind::Actions, L"",        kAnything, Icon::Folder, C::Action, 1, 0, Popup::None, nullptr },
    };
    return table;
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
    for (auto& c : Converters()) {
        if (c.wheel != wheel || (all & ~c.inputs) != 0 || n < c.minItems || (c.maxItems && n > c.maxItems)) continue;
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
