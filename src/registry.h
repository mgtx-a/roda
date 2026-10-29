// What Roda can do: file kinds, and the table of converters, tools and actions the wheels are
// built from. Adding a format means adding a row here and a run function; the wheel code is untouched.
#pragma once
#include "common.h"
#include "json.h"

namespace rd {

enum class WheelKind { Convert, Tools, Actions };
const wchar_t* WheelName(WheelKind k);

enum class Kind : uint8_t {
    Unknown, Folder, Jpeg, Png, Webp, Gif, Bmp, Tiff, Ico, Svg, Heic, Avif, Zip, Tar, Gzip, Rar, Pdf,
    Docx, Markdown, Html, Rtf, Txt, Xlsx, Ods, Csv, Tsv, Xls, Srt, Vtt, Doc, Pptx, Ppt,
    Mp3, M4a, Wav, Flac, Ogg, Opus, Aiff, Wma, Mp4, Mov, Mkv, Webm, Avi, Wmv, Count
};
using KindSet = uint64_t;
constexpr KindSet Bit(Kind k) { return KindSet(1) << unsigned(k); }
constexpr KindSet kImages = Bit(Kind::Jpeg) | Bit(Kind::Png) | Bit(Kind::Webp) | Bit(Kind::Gif) | Bit(Kind::Bmp) | Bit(Kind::Tiff) |
                            Bit(Kind::Ico) | Bit(Kind::Svg) | Bit(Kind::Heic) | Bit(Kind::Avif);
constexpr KindSet kAnything = ~KindSet(0);
constexpr KindSet kDocs = Bit(Kind::Docx) | Bit(Kind::Markdown) | Bit(Kind::Html) | Bit(Kind::Rtf) | Bit(Kind::Txt);
constexpr KindSet kSheets = Bit(Kind::Xlsx) | Bit(Kind::Ods) | Bit(Kind::Csv) | Bit(Kind::Tsv) | Bit(Kind::Xls);
constexpr KindSet kSubtitles = Bit(Kind::Srt) | Bit(Kind::Vtt);
// What each Office app turns into a PDF (7.6): only offered when that app is installed.
constexpr KindSet kWordFiles = Bit(Kind::Docx) | Bit(Kind::Doc) | Bit(Kind::Rtf);
constexpr KindSet kExcelFiles = Bit(Kind::Xlsx) | Bit(Kind::Xls) | Bit(Kind::Ods);
constexpr KindSet kPowerPointFiles = Bit(Kind::Pptx) | Bit(Kind::Ppt);
constexpr KindSet kAudio = Bit(Kind::Mp3) | Bit(Kind::M4a) | Bit(Kind::Wav) | Bit(Kind::Flac) | Bit(Kind::Ogg) | Bit(Kind::Opus) |
                           Bit(Kind::Aiff) | Bit(Kind::Wma);
constexpr KindSet kVideo = Bit(Kind::Mp4) | Bit(Kind::Mov) | Bit(Kind::Mkv) | Bit(Kind::Webm) | Bit(Kind::Avi) | Bit(Kind::Wmv);
inline bool IsMedia(Kind k) { return (Bit(k) & (kAudio | kVideo)) != 0; }

Kind KindFromExtension(const wstring& path);
/// From the first bytes of a file; falls back to the extension when they say nothing.
Kind KindFromBytes(const uint8_t* p, size_t n, const wstring& path);
/// Magic bytes when the file is local and readable without a download (OneDrive placeholders are
/// judged by their extension), else the extension.
Kind KindOfPath(const wstring& path);
const wchar_t* KindLabel(Kind k);              // "PNG"
inline bool IsImage(Kind k) { return (Bit(k) & kImages) != 0; }

enum class Icon : uint8_t { None, Compress, Resize, Crop, Rotate, Flip, Strip, Merge, Extract, Copy, Move, Folder, Path, Terminal, Float, Share, Archive, Split, Images, Trim, Mute, Audio, Film, RemovePage };

/// Optional engines: DLLs in the "engines" folder next to Roda.exe (the Full build).
enum class Engine : uint8_t { None, Pdfium, Word, Excel, PowerPoint, Ffmpeg };   // Office apps: when installed
bool EngineAvailable(Engine e);   // the DLL is there (checked once)
wstring EnginePath(Engine e);
enum class Popup : uint8_t { None, Quality, Size, Crop, Trim, Pages };

// ---------------------------------------------------------------- jobs

/// One unit of work for a worker (or the CLI).
struct Job {
    wstring op;                       // a converter id, e.g. "to.webp"
    std::vector<wstring> inputs;
    wstring outDir;                   // empty: next to the (first) input
    wstring outPath;                  // CLI only: this exact output, replaced if it exists
    Json opts = Json::Object();       // quality, sizes, crop, metadata...

    Json ToJson() const;
    static bool FromJson(const Json& j, Job& out);
};

/// What a run reports while it works.
struct Progress {
    std::function<void(double)> report;          // 0..1
    std::function<void(const wstring&)> part;    // the partial output, so a cancel can delete it
    std::function<void(DWORD)> helper;           // a process started for this job (Office), ended with it on cancel
    void Set(double f) const { if (report) report(f); }
};

struct Result {
    wstring out;                      // the saved file or folder
    wstring error;                    // plain words, for the card
    uint64_t before = 0, after = 0;   // bytes in and out, when it means something
    Json info = Json::Object();       // anything else the UI wants back (a picture's size)
};

struct Converter {
    const wchar_t* id;
    WheelKind wheel;
    const wchar_t* label;
    KindSet inputs;                   // every dropped item must be one of these
    Icon icon;
    enum Mode : uint8_t { PerFile, AllInOne, Action } mode;
    int minItems, maxItems;           // maxItems 0: no limit
    Popup popup;
    bool (*run)(const Job&, const Progress&, Result&);   // null for actions (run by the UI)
    Engine needs = Engine::None;      // hidden when this engine is missing
};

const std::vector<Converter>& Converters();
const Converter* FindConverter(const wstring& id);

/// One segment of a wheel.
struct WheelOption {
    const Converter* conv = nullptr;
    wstring label;
    wstring arg;                      // e.g. the folder of a recent "Copy to"
    Icon icon = Icon::None;
};

/// The options valid for every item in `kinds`, for one wheel.
std::vector<WheelOption> OptionsFor(WheelKind wheel, const std::vector<Kind>& kinds, const Settings& settings);

}  // namespace rd
