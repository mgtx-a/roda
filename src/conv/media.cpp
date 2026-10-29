// Audio and video through FFmpeg (engines\ffmpeg\ffmpeg.exe and ffprobe.exe, an LGPL build): the worker
// probes the file, picks codecs, and runs ffmpeg with progress on a pipe. Streams are copied untouched
// whenever only the container changes and the codecs fit; everything else is re-encoded.
// FFmpeg only ever reads and writes local files here (protocol whitelist "file,pipe").
#include "conv.h"

namespace rd {

namespace {

wstring Tool(const wchar_t* exe) { return JoinPath(JoinPath(ExeDir(), L"engines\\ffmpeg"), exe); }

/// Windows command-line quoting (backslashes before a quote are doubled).
wstring Quote(const wstring& a)
{
    if (!a.empty() && a.find_first_of(L" \t\"") == wstring::npos) return a;
    wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { slashes++; continue; }
        if (c == L'"') { out += wstring(slashes * 2 + 1, L'\\'); out += L'"'; slashes = 0; continue; }
        out += wstring(slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out += wstring(slashes * 2, L'\\');
    return out + L"\"";
}

struct Run {
    DWORD code = 1;
    std::string out;          // stdout (lines, or the raw bytes of a frame)
    std::string errTail;      // the end of stderr: FFmpeg's reason when it fails
};

/// Runs an FFmpeg tool. `line` gets each stdout line as it comes (progress); without it stdout is kept whole.
bool RunTool(const wchar_t* exe, const std::vector<wstring>& args, Run& run, const Progress* prog, const std::function<void(const std::string&)>& line)
{
    wstring cmd = Quote(Tool(exe));
    for (auto& a : args) cmd += L" " + Quote(a);
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE outR, outW, errR, errW;
    if (!CreatePipe(&outR, &outW, &sa, 0)) return false;
    if (!CreatePipe(&errR, &errW, &sa, 0)) { CloseHandle(outR); CloseHandle(outW); return false; }
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{ sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr;
    si.hStdOutput = outW;
    si.hStdError = errW;
    PROCESS_INFORMATION pi{};
    wstring dir = JoinPath(ExeDir(), L"engines\\ffmpeg");
    BOOL ok = CreateProcessW(Tool(exe).c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, dir.c_str(), &si, &pi);
    CloseHandle(outW);
    CloseHandle(errW);
    if (!ok) { CloseHandle(outR); CloseHandle(errR); return false; }
    // Cancelling the job ends FFmpeg too (the UI is told which process it is).
    if (prog && prog->helper) prog->helper(pi.dwProcessId);
    CloseHandle(pi.hThread);
    // stderr on a thread of its own, so neither pipe can fill up and stall FFmpeg.
    std::string err;
    HANDLE errThread = CreateThread(nullptr, 0, [](void* p) -> DWORD {
        auto* ctx = static_cast<std::pair<HANDLE, std::string*>*>(p);
        char buf[4096];
        DWORD got;
        while (ReadFile(ctx->first, buf, sizeof buf, &got, nullptr) && got) {
            ctx->second->append(buf, got);
            if (ctx->second->size() > 16384) ctx->second->erase(0, ctx->second->size() - 8192);
        }
        return 0;
    }, new std::pair<HANDLE, std::string*>(errR, &err), 0, nullptr);
    char buf[65536];
    DWORD got;
    std::string pending;
    while (ReadFile(outR, buf, sizeof buf, &got, nullptr) && got) {
        if (!line) { run.out.append(buf, got); continue; }
        pending.append(buf, got);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string l = pending.substr(0, nl);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            pending.erase(0, nl + 1);
            line(l);
        }
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &run.code);
    CloseHandle(pi.hProcess);
    WaitForSingleObject(errThread, 5000);
    CloseHandle(errThread);
    CloseHandle(outR);
    CloseHandle(errR);
    run.errTail = err;
    return true;
}

struct Probe {
    double duration = 0, fps = 30;
    double bitrate = 0;       // the whole file's, bits per second (0: unknown)
    int w = 0, h = 0;
    bool video = false, audio = false;
    std::string vcodec, acodec;
};

bool ProbeFile(const wstring& path, Probe& pr, wstring& err)
{
    Run run;
    if (!RunTool(L"ffprobe.exe", { L"-v", L"error", L"-protocol_whitelist", L"file", L"-print_format", L"json", L"-show_format", L"-show_streams", L"file:" + path },
                 run, nullptr, nullptr)) { err = L"Could not start FFmpeg."; return false; }
    Json j;
    if (run.code != 0 || !Json::Parse(run.out, j)) { err = L"This file is damaged, or not something FFmpeg can read."; return false; }
    pr.duration = j[L"format"][L"duration"].Num(0);
    pr.bitrate = j[L"format"][L"bit_rate"].Num(0);
    for (auto& s : j[L"streams"].arr) {
        wstring type = s[L"codec_type"].Str();
        // Cover art in music files is a "video" stream of one picture: not a video.
        bool picture = s[L"disposition"][L"attached_pic"].Num(0) != 0;
        if (type == L"video" && !pr.video && !picture) {
            pr.video = true;
            pr.vcodec = Narrow(s[L"codec_name"].Str());
            pr.w = int(s[L"width"].Num(0));
            pr.h = int(s[L"height"].Num(0));
            int a = 0, b = 1;
            if (swscanf(s[L"avg_frame_rate"].Str().c_str(), L"%d/%d", &a, &b) == 2 && a > 0 && b > 0) pr.fps = double(a) / b;
            if (pr.duration <= 0) pr.duration = s[L"duration"].Num(0);
        } else if (type == L"audio" && !pr.audio) {
            pr.audio = true;
            pr.acodec = Narrow(s[L"codec_name"].Str());
            if (pr.duration <= 0) pr.duration = s[L"duration"].Num(0);
        }
    }
    if (!pr.video && !pr.audio) { err = L"There is no sound or picture in this file."; return false; }
    return true;
}

bool In(const std::string& v, std::initializer_list<const char*> set)
{
    for (auto* s : set) if (v == s) return true;
    return false;
}

const wchar_t* Muxer(const wstring& ext)
{
    static const std::pair<const wchar_t*, const wchar_t*> m[] = { { L"mp4", L"mp4" }, { L"mov", L"mov" }, { L"mkv", L"matroska" }, { L"webm", L"webm" },
        { L"avi", L"avi" }, { L"wmv", L"asf" }, { L"gif", L"gif" }, { L"mp3", L"mp3" }, { L"m4a", L"ipod" }, { L"wav", L"wav" }, { L"flac", L"flac" },
        { L"ogg", L"ogg" }, { L"opus", L"opus" }, { L"aiff", L"aiff" }, { L"wma", L"asf" } };
    for (auto& [e, f] : m) if (ext == e) return f;
    return nullptr;
}

bool AudioTarget(const wstring& ext) { return ext == L"mp3" || ext == L"m4a" || ext == L"wav" || ext == L"flac" || ext == L"ogg" || ext == L"opus" || ext == L"aiff" || ext == L"wma"; }

wstring ExtOfKind(Kind k)
{
    wstring label = KindLabel(k);
    for (auto& c : label) c = wchar_t(towlower(c));
    return label;
}

/// Audio codec options for an audio file of type `ext`, or for the sound of a video container.
std::vector<wstring> AudioCodec(const wstring& ext, const std::string& src, bool canCopy, bool smaller)
{
    auto copyIf = [&](std::initializer_list<const char*> ok) { return canCopy && In(src, ok); };
    if (ext == L"mp3") return copyIf({ "mp3" }) ? std::vector<wstring>{ L"-c:a", L"copy" } : std::vector<wstring>{ L"-c:a", L"libmp3lame", smaller ? L"-b:a" : L"-q:a", smaller ? L"96k" : L"2" };
    if (ext == L"m4a" || ext == L"mp4" || ext == L"mov" || ext == L"mkv")
        return copyIf({ "aac", "alac" }) || (ext == L"mkv" && copyIf({ "mp3", "opus", "vorbis", "flac", "ac3", "eac3" })) || ((ext == L"mp4" || ext == L"mov") && copyIf({ "mp3" }))
                   ? std::vector<wstring>{ L"-c:a", L"copy" } : std::vector<wstring>{ L"-c:a", L"aac", L"-b:a", smaller ? L"96k" : L"192k" };
    if (ext == L"webm") return copyIf({ "opus", "vorbis" }) ? std::vector<wstring>{ L"-c:a", L"copy" } : std::vector<wstring>{ L"-c:a", L"libopus", L"-b:a", smaller ? L"64k" : L"128k" };
    if (ext == L"wav") return copyIf({ "pcm_s16le" }) ? std::vector<wstring>{ L"-c:a", L"copy" } : std::vector<wstring>{ L"-c:a", L"pcm_s16le" };
    if (ext == L"aiff") return copyIf({ "pcm_s16be" }) ? std::vector<wstring>{ L"-c:a", L"copy" } : std::vector<wstring>{ L"-c:a", L"pcm_s16be" };
    if (ext == L"flac") return copyIf({ "flac" }) ? std::vector<wstring>{ L"-c:a", L"copy" } : std::vector<wstring>{ L"-c:a", L"flac" };
    if (ext == L"ogg") return copyIf({ "vorbis" }) ? std::vector<wstring>{ L"-c:a", L"copy" } : std::vector<wstring>{ L"-c:a", L"libvorbis", L"-q:a", smaller ? L"2" : L"5" };
    if (ext == L"opus") return copyIf({ "opus" }) ? std::vector<wstring>{ L"-c:a", L"copy" } : std::vector<wstring>{ L"-c:a", L"libopus", L"-b:a", smaller ? L"64k" : L"128k" };
    if (ext == L"wma" || ext == L"wmv") return copyIf({ "wmav2" }) ? std::vector<wstring>{ L"-c:a", L"copy" } : std::vector<wstring>{ L"-c:a", L"wmav2", L"-b:a", smaller ? L"96k" : L"192k" };
    if (ext == L"avi") return copyIf({ "mp3" }) ? std::vector<wstring>{ L"-c:a", L"copy" } : std::vector<wstring>{ L"-c:a", L"libmp3lame", L"-q:a", L"2" };
    return { L"-c:a", L"aac" };
}

bool VideoFits(const wstring& ext, const std::string& v)
{
    if (ext == L"mp4" || ext == L"mov") return In(v, { "h264", "hevc", "mpeg4", "av1" });
    if (ext == L"mkv") return In(v, { "h264", "hevc", "mpeg4", "vp8", "vp9", "av1", "mpeg2video", "theora" });
    if (ext == L"webm") return In(v, { "vp8", "vp9", "av1" });
    if (ext == L"avi") return In(v, { "mpeg4", "mjpeg" });
    if (ext == L"wmv") return In(v, { "wmv1", "wmv2", "wmv3" });
    return false;
}

/// How to encode the picture for a video container. `h264` false: the fallback when Windows has no H.264 encoder.
std::vector<wstring> VideoCodec(const wstring& ext, const Probe& pr, double bpp, bool h264)
{
    int w = std::max(pr.w, 2), h = std::max(pr.h, 2);
    long long rate = std::clamp<long long>((long long)(w * double(h) * std::clamp(pr.fps, 5.0, 120.0) * bpp), 250'000, 25'000'000);
    // Never more than the source itself used (a simple, low-detail video would otherwise grow), and for
    // Compress clearly less: about 60% of it.
    if (pr.bitrate > 0) rate = std::min(rate, std::max<long long>(100'000, (long long)(pr.bitrate * (bpp < 0.08 ? 0.6 : 1.2))));
    wstring br = std::to_wstring(rate / 1000) + L"k";
    if (ext == L"webm") return { L"-c:v", L"libvpx-vp9", L"-b:v", br, L"-deadline", L"good", L"-cpu-used", L"4", L"-row-mt", L"1", L"-pix_fmt", L"yuv420p" };
    if (ext == L"avi") return { L"-c:v", L"mpeg4", L"-vtag", L"xvid", L"-q:v", bpp < 0.08 ? L"6" : L"3" };
    if (ext == L"wmv") return { L"-c:v", L"wmv2", L"-b:v", br };
    // MP4, MOV, MKV: H.264 through Windows' own encoder (Media Foundation), MPEG-4 where Windows has none.
    if (h264) return { L"-c:v", L"h264_mf", L"-b:v", br, L"-pix_fmt", L"nv12" };
    return { L"-c:v", L"mpeg4", L"-q:v", bpp < 0.08 ? L"6" : L"3" };
}

struct Plan {
    std::vector<wstring> args;   // between the input and the output
    std::vector<wstring> pre;    // before the input (seeking)
    wstring ext, suffix;
    double length = 0;           // seconds of output, for the progress bar
};

}  // namespace

bool MediaFrame(const wstring& path, std::vector<uint8_t>& png, wstring& err)
{
    Probe pr;
    if (!ProbeFile(path, pr, err)) return false;
    if (!pr.video) { err = L"This file has no picture."; return false; }
    double at = pr.duration > 3 ? 1.0 : pr.duration / 3;
    Run run;
    if (!RunTool(L"ffmpeg.exe", { L"-nostdin", L"-v", L"error", L"-protocol_whitelist", L"file,pipe", L"-ss", std::to_wstring(at), L"-i", L"file:" + path,
                                  L"-frames:v", L"1", L"-f", L"image2pipe", L"-c:v", L"png", L"pipe:1" }, run, nullptr, nullptr) || run.code != 0 || run.out.empty()) {
        err = L"Could not take a picture from this video.";
        return false;
    }
    png.assign(run.out.begin(), run.out.end());
    return true;
}

bool FfmpegImage(const wstring& path, std::vector<uint8_t>& png, wstring& err)
{
    Run run;
    if (!RunTool(L"ffmpeg.exe", { L"-nostdin", L"-v", L"error", L"-protocol_whitelist", L"file,pipe", L"-i", L"file:" + path,
                                  L"-frames:v", L"1", L"-f", L"image2pipe", L"-c:v", L"png", L"pipe:1" }, run, nullptr, nullptr) || run.code != 0 || run.out.empty()) {
        err = L"This picture can't be read (damaged, or a kind FFmpeg doesn't know).";
        return false;
    }
    png.assign(run.out.begin(), run.out.end());
    return true;
}

bool EncodeAvif(const Image& img, int quality, std::vector<uint8_t>& out, wstring& err)
{
    if (GetFileAttributesW(Tool(L"ffmpeg.exe").c_str()) == INVALID_FILE_ATTRIBUTES) { err = L"AVIF needs the Full build (engines\\ffmpeg)."; return false; }
    // Through a lossless PNG in the worker's temp folder: FFmpeg reads that, AV1 (libaom) writes the still.
    std::vector<uint8_t> png;
    if (!EncodeImage(img, Kind::Png, 0, true, png, err)) return false;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wstring dir = JoinPath(JoinPath(tmp, L"Roda"), L"avif-" + std::to_wstring(GetCurrentProcessId()));
    EnsureDir(dir);
    wstring in = JoinPath(dir, L"in.png"), outPath = JoinPath(dir, L"out.avif");
    bool ok = WriteAllBytes(in, png.data(), png.size());
    int crf = std::clamp(63 - quality * 63 / 100, 10, 50);   // AV1 counts the other way (lower is better): quality 82 -> 12
    Run run;
    ok = ok && RunTool(L"ffmpeg.exe", { L"-nostdin", L"-v", L"error", L"-protocol_whitelist", L"file", L"-y", L"-i", L"file:" + in, L"-c:v", L"libaom-av1",
                                        L"-still-picture", L"1", L"-crf", std::to_wstring(crf), L"-cpu-used", L"6", L"-pix_fmt", L"yuv420p",
                                        L"-vf", L"scale=trunc(iw/2)*2:trunc(ih/2)*2", L"-f", L"avif", L"file:" + outPath }, run, nullptr, nullptr) && run.code == 0;
    wstring readErr;
    ok = ok && ReadShared(outPath, out, readErr) && !out.empty();
    DeleteFileW(in.c_str());
    DeleteFileW(outPath.c_str());
    RemoveDirectoryW(dir.c_str());
    if (!ok) err = L"Could not write the AVIF picture.";
    return ok;
}

bool RunMedia(const Job& job, const Progress& p, Result& r)
{
    if (GetFileAttributesW(Tool(L"ffmpeg.exe").c_str()) == INVALID_FILE_ATTRIBUTES) { r.error = L"Audio and video need the Full build (engines\\ffmpeg is missing)."; return false; }
    const wstring& in = job.inputs[0];
    Probe pr;
    if (!ProbeFile(in, pr, r.error)) return false;
    r.before = FileSize(in);
    Kind kind = KindOfPath(in);
    wstring srcExt = kind == Kind::Gif ? L"gif" : ExtOfKind(kind);
    const wstring& op = job.op;
    Plan plan;
    plan.length = pr.duration;
    bool h264 = true;

    auto videoInto = [&](const wstring& ext, bool forceEncode, double bpp, const wstring& filter) {
        // Picture: copied when it fits and nothing changes it, else encoded (sizes made even for H.264).
        if (pr.video && ext != L"gif") {
            plan.args.insert(plan.args.end(), { L"-map", L"0:v:0" });
            if (!forceEncode && filter.empty() && VideoFits(ext, pr.vcodec)) plan.args.insert(plan.args.end(), { L"-c:v", L"copy" });
            else {
                wstring vf = filter.empty() ? L"" : filter + L",";
                vf += L"scale=trunc(iw/2)*2:trunc(ih/2)*2";
                plan.args.insert(plan.args.end(), { L"-vf", vf });
                auto vc = VideoCodec(ext, pr, bpp, h264);
                plan.args.insert(plan.args.end(), vc.begin(), vc.end());
            }
        }
        if (pr.audio && op != L"media.mute") {
            plan.args.insert(plan.args.end(), { L"-map", L"0:a:0" });
            auto ac = AudioCodec(ext, pr.acodec, !forceEncode, bpp < 0.08);
            plan.args.insert(plan.args.end(), ac.begin(), ac.end());
        }
        if (ext == L"mp4" || ext == L"mov") plan.args.insert(plan.args.end(), { L"-movflags", L"+faststart" });   // plays while it downloads
    };

    if (op.rfind(L"to.", 0) == 0) {
        plan.ext = op.substr(3);
        if (AudioTarget(plan.ext)) {
            if (!pr.audio) { r.error = L"This video has no sound to keep."; return false; }
            plan.args = { L"-map", L"0:a:0", L"-vn" };
            auto ac = AudioCodec(plan.ext, pr.acodec, true, false);
            plan.args.insert(plan.args.end(), ac.begin(), ac.end());
        } else if (plan.ext == L"gif") {
            if (!pr.video) { r.error = L"There is no picture to make a GIF from."; return false; }
            // Its own 256-colour palette, 12 frames a second, at most 480 pixels wide: small and clean.
            plan.args = { L"-map", L"0:v:0", L"-vf", L"fps=12,scale='min(480,iw)':-2:flags=lanczos,split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=4",
                          L"-loop", L"0" };
        } else {
            if (!pr.video) { r.error = L"There is no picture in this file."; return false; }
            videoInto(plan.ext, kind == Kind::Gif, 0.1, L"");
        }
    } else if (op == L"media.trim") {
        double start = job.opts[L"start"].Num(0), end = job.opts[L"end"].Num(0);
        if (end <= 0 || end > pr.duration) end = pr.duration;
        if (start < 0 || start >= end) { r.error = L"The start must come before the end."; return false; }
        plan.ext = srcExt == L"gif" ? L"mp4" : srcExt;
        plan.suffix = L"trimmed";
        plan.pre = { L"-ss", std::to_wstring(start) };
        plan.length = end - start;
        if (pr.video) videoInto(plan.ext, true, 0.1, L"");
        else { plan.args = { L"-map", L"0:a:0", L"-vn" }; auto ac = AudioCodec(plan.ext, pr.acodec, false, false); plan.args.insert(plan.args.end(), ac.begin(), ac.end()); }
        plan.args.insert(plan.args.end(), { L"-t", std::to_wstring(end - start) });
    } else if (op == L"media.compress") {
        plan.suffix = L"compressed";
        if (pr.video) {
            plan.ext = srcExt == L"webm" ? L"webm" : L"mp4";
            videoInto(plan.ext, true, 0.05, L"");
        } else {
            // Lossless sound becomes AAC; lossy stays itself, at a lower bitrate.
            plan.ext = In(Narrow(srcExt), { "wav", "flac", "aiff" }) ? L"m4a" : srcExt;
            plan.args = { L"-map", L"0:a:0", L"-vn" };
            auto ac = AudioCodec(plan.ext, pr.acodec, false, true);
            plan.args.insert(plan.args.end(), ac.begin(), ac.end());
        }
    } else if (op == L"media.audio") {
        if (!pr.audio) { r.error = L"This video has no sound."; return false; }
        // The sound as it is when a plain audio file can hold it, else AAC.
        const std::string& a = pr.acodec;
        plan.ext = a == "aac" || a == "alac" ? L"m4a" : a == "mp3" ? L"mp3" : a == "opus" ? L"opus" : a == "vorbis" ? L"ogg" : a == "flac" ? L"flac" : L"m4a";
        plan.suffix = L"audio";
        plan.args = { L"-map", L"0:a:0", L"-vn" };
        auto ac = AudioCodec(plan.ext, a, true, false);
        plan.args.insert(plan.args.end(), ac.begin(), ac.end());
    } else if (op == L"media.mute") {
        plan.ext = srcExt;
        plan.suffix = L"muted";
        videoInto(plan.ext, false, 0.1, L"");
    } else if (op == L"media.1080" || op == L"media.720" || op == L"media.480") {
        int target = _wtoi(op.c_str() + 6);
        if (pr.h <= target) { r.error = L"This video is already " + std::to_wstring(pr.h) + L" pixels tall."; return false; }
        plan.ext = srcExt;
        plan.suffix = std::to_wstring(target) + L"p";
        Probe scaled = pr;
        scaled.w = pr.w * target / std::max(1, pr.h);
        scaled.h = target;
        std::swap(pr, scaled);   // the bitrate follows the new size
        videoInto(plan.ext, true, 0.1, L"scale=-2:" + std::to_wstring(target));
        std::swap(pr, scaled);
    } else if (op == L"media.crop") {
        int c[4];
        if (swscanf(job.opts[L"crop"].Str().c_str(), L"%d,%d,%d,%d", &c[0], &c[1], &c[2], &c[3]) != 4 || c[2] < 2 || c[3] < 2) { r.error = L"No crop area was given."; return false; }
        plan.ext = srcExt;
        plan.suffix = L"cropped";
        wchar_t f[96];
        swprintf(f, 96, L"crop=%d:%d:%d:%d", c[2] & ~1, c[3] & ~1, c[0], c[1]);
        videoInto(plan.ext, true, 0.1, f);
    } else if (op == L"media.merge") {
        // Sound files one after another, in the order dropped, as the first one's format.
        plan.ext = srcExt;
        plan.suffix = L"joined";
        std::vector<wstring> args;
        wstring graph;
        double total = 0;
        for (size_t i = 0; i < job.inputs.size(); i++) {
            Probe pi;
            if (!ProbeFile(job.inputs[i], pi, r.error) || !pi.audio) { if (r.error.empty()) r.error = FileName(job.inputs[i]) + L" has no sound."; return false; }
            total += pi.duration;
            graph += L"[" + std::to_wstring(i) + L":a:0]aresample=48000,aformat=channel_layouts=stereo[a" + std::to_wstring(i) + L"];";
        }
        for (size_t i = 0; i < job.inputs.size(); i++) graph += L"[a" + std::to_wstring(i) + L"]";
        graph += L"concat=n=" + std::to_wstring(job.inputs.size()) + L":v=0:a=1[out]";
        plan.args = { L"-filter_complex", graph, L"-map", L"[out]" };
        auto ac = AudioCodec(plan.ext, "", false, false);
        plan.args.insert(plan.args.end(), ac.begin(), ac.end());
        plan.length = total;
    } else {
        r.error = L"Unknown audio or video job.";
        return false;
    }

    const wchar_t* muxer = Muxer(plan.ext);
    if (!muxer) { r.error = L"Roda can't write that format."; return false; }
    OutFile o;
    if (!ReserveOutput(job, in, BaseName(in) + (plan.suffix.empty() ? L"" : L" " + plan.suffix), L"." + plan.ext, false, o, r.error)) return false;
    if (p.part) p.part(o.part);

    for (int attempt = 0; attempt < 2; attempt++) {
        std::vector<wstring> args = { L"-nostdin", L"-hide_banner", L"-v", L"error", L"-protocol_whitelist", L"file,pipe", L"-y" };
        args.insert(args.end(), plan.pre.begin(), plan.pre.end());
        for (auto& f : (op == L"media.merge" ? job.inputs : std::vector<wstring>{ in })) args.insert(args.end(), { L"-i", L"file:" + f });
        args.insert(args.end(), plan.args.begin(), plan.args.end());
        args.insert(args.end(), { L"-map_metadata", L"0", L"-progress", L"pipe:1", L"-nostats", L"-f", muxer, L"file:" + o.part });
        Run run;
        double length = std::max(0.1, plan.length);
        bool started = RunTool(L"ffmpeg.exe", args, run, &p, [&](const std::string& l) {
            if (l.rfind("out_time_us=", 0) == 0) p.Set(std::clamp(atof(l.c_str() + 12) / 1e6 / length, 0.0, 0.99));
        });
        if (!started) { AbandonOutput(o); r.error = L"Could not start FFmpeg."; return false; }
        if (run.code == 0 && FileSize(o.part) > 0) break;
        // Windows without its H.264 encoder (the "N" editions): try once more with MPEG-4.
        bool mf = std::find(plan.args.begin(), plan.args.end(), L"h264_mf") != plan.args.end();
        if (attempt == 0 && mf) {
            Log(L"media: Windows' H.264 encoder failed (%s); trying MPEG-4", Widen(run.errTail.substr(0, 200)).c_str());
            h264 = false;
            for (size_t i = 0; i + 1 < plan.args.size(); i++)
                if (plan.args[i] == L"-c:v" && plan.args[i + 1] == L"h264_mf") {
                    auto vc = VideoCodec(plan.ext, pr, 0.1, false);
                    plan.args.erase(plan.args.begin() + long(i), plan.args.begin() + long(i + 6));   // -c:v h264_mf -b:v X -pix_fmt nv12
                    plan.args.insert(plan.args.begin() + long(i), vc.begin(), vc.end());
                    break;
                }
            continue;
        }
        AbandonOutput(o);
        std::string tail = run.errTail;
        while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r')) tail.pop_back();
        size_t nl = tail.find_last_of('\n');
        std::string last = nl == std::string::npos ? tail : tail.substr(nl + 1);
        r.error = L"FFmpeg could not convert this file" + (last.empty() ? wstring(L".") : L": " + Widen(last));
        return false;
    }
    // Compress that didn't make it smaller is no use to anyone: say so instead of saving it.
    if (op == L"media.compress" && FileSize(o.part) >= r.before) {
        AbandonOutput(o);
        r.error = L"This file is already well compressed: Roda couldn't make it smaller.";
        return false;
    }
    if (!CommitOutput(o, r.error)) { AbandonOutput(o); return false; }
    r.out = o.final;
    r.after = FileSize(o.final);
    return true;
}

}  // namespace rd
