// The other modes of Roda.exe:
//   --worker               one job as JSON on stdin; progress lines on stdout; then exit
//   --convert <in>... <out> [--op <id>] [--option k=v]...
//                          runs a job in this process and exits 0 on success (tests use this)
//   --info <file>          what Roda reads in a file (kind, size); used to check outputs
// Decoders live only in these modes, so a malformed file can only take down its own process.
#include "worker.h"

#include "conv/conv.h"
#include "conv/pdfium.h"
#include "json.h"
#include "registry.h"

#include <miniz.h>

namespace rd {

namespace {

HANDLE g_out = INVALID_HANDLE_VALUE;

void Print(const std::string& utf8)
{
    if (g_out == INVALID_HANDLE_VALUE || !g_out) return;
    DWORD put;
    DWORD mode;
    if (GetConsoleMode(g_out, &mode)) {
        wstring w = Widen(utf8);
        WriteConsoleW(g_out, w.c_str(), DWORD(w.size()), &put, nullptr);
    } else {
        WriteFile(g_out, utf8.data(), DWORD(utf8.size()), &put, nullptr);
    }
}

void PrintLine(const Json& j) { Print(j.Dump() + "\n"); }

void SetUpStdout(bool console)
{
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    // Started from a console with nothing redirected: a GUI-subsystem exe has to borrow it.
    if (console && (g_out == nullptr || g_out == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS))
        g_out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

bool Run(const Job& job, const Progress& p, Result& r)
{
    const Converter* c = FindConverter(job.op);
    if (!c || !c->run) { r.error = L"Unknown job: " + job.op; return false; }
    for (auto& in : job.inputs)
        if (GetFileAttributesW(LongPath(in).c_str()) == INVALID_FILE_ATTRIBUTES) { r.error = FileName(in) + L" is gone."; return false; }
    return c->run(job, p, r);
}

/// Defaults a UI job would carry, for the CLI.
void DefaultOptions(Job& job)
{
    Settings defaults;
    if (!job.opts.has(L"jpegQ")) job.opts.Set(L"jpegQ", defaults.jpegQuality);
    if (!job.opts.has(L"webpQ")) job.opts.Set(L"webpQ", defaults.webpQuality);
    if (!job.opts.has(L"keepMeta")) job.opts.Set(L"keepMeta", defaults.keepMetadata);
    if (job.op == L"tool.compress" && !job.opts.has(L"q")) job.opts.Set(L"q", defaults.compressQuality);
}

wstring GuessOp(const std::vector<wstring>& inputs, const wstring& out, bool outIsDir)
{
    wstring lower = out;
    for (auto& c : lower) c = wchar_t(towlower(c));
    auto ends = [&](const wchar_t* s) { size_t n = wcslen(s); return lower.size() >= n && lower.compare(lower.size() - n, n, s) == 0; };
    if (outIsDir) return L"tool.extract";
    if (ends(L".tar.gz") || ends(L".tgz")) return L"pack.targz";
    if (ends(L".tar")) return L"pack.tar";
    if (ends(L".zip")) return L"pack.zip";
    if (ends(L".pdf") && inputs.size() > 1) return L"tool.mergepdf";
    wstring ext = Extension(out);
    if (ext == L".jpeg") ext = L".jpg";
    if (ext == L".tif") ext = L".tiff";
    return ext.empty() ? L"" : L"to" + ext;
}

int Info(const wstring& path)
{
    std::vector<uint8_t> bytes;
    wstring err;
    if (!ReadShared(path, bytes, err)) { Print("error: " + Narrow(err) + "\n"); return 1; }
    Kind k = KindFromBytes(bytes.data(), bytes.size(), path);
    Json j = Json::Object();
    j.Set(L"kind", KindLabel(k));
    j.Set(L"bytes", double(bytes.size()));
    if (IsImage(k)) {
        Image img;
        if (!DecodeImage(path, img, bytes, err)) { Print("error: " + Narrow(err) + "\n"); return 1; }
        j.Set(L"w", img.w).Set(L"h", img.h).Set(L"alpha", img.alpha);
    } else if (k == Kind::Zip || (bytes.size() > 4 && bytes[0] == 'P' && bytes[1] == 'K')) {   // DOCX, XLSX, PPTX, EPUB, ODS are ZIPs too
        mz_zip_archive zip{};
        if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0)) { Print("error: damaged zip\n"); return 1; }
        j.Set(L"entries", int(mz_zip_reader_get_num_files(&zip)));
        mz_zip_reader_end(&zip);
    } else if (k == Kind::Pdf) {
        std::string s(bytes.begin(), bytes.end());
        bool ok = s.rfind("%%EOF") != std::string::npos && s.find("startxref") != std::string::npos;
        PdfDoc pdf;
        if (EngineAvailable(Engine::Pdfium)) {
            // PDFium opens it: the real page count, and proof the file reads back.
            if (!pdf.OpenMemory(bytes, err)) { Print("error: " + Narrow(err) + "\n"); return 1; }
            j.Set(L"pages", pdf.pages()).Set(L"wellFormed", ok);
        } else {
            int pages = 0;
            for (size_t i = 0; (i = s.find("/Type /Page ", i)) != std::string::npos; i++) pages++;
            j.Set(L"pages", pages).Set(L"wellFormed", ok);
        }
    }
    PrintLine(j);
    return 0;
}

int Convert(int argc, wchar_t** argv, int first)
{
    std::vector<wstring> files;
    Job job;
    for (int i = first; i < argc; i++) {
        wstring a = argv[i];
        if (a == L"--op" && i + 1 < argc) job.op = argv[++i];
        else if (a == L"--option" && i + 1 < argc) {
            wstring kv = argv[++i];
            size_t eq = kv.find(L'=');
            if (eq == wstring::npos) { Print("error: --option needs key=value\n"); return 2; }
            wstring k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if (v == L"true" || v == L"false") job.opts.Set(k.c_str(), v == L"true");
            else job.opts.Set(k.c_str(), v);
        } else files.push_back(a);
    }
    if (files.size() < 2) { Print("usage: Roda.exe --convert <input>... <output> [--op <id>] [--option key=value]...\n"); return 2; }
    wstring out = files.back();
    files.pop_back();
    for (auto& f : files) {
        wchar_t full[32768];
        if (GetFullPathNameW(f.c_str(), 32768, full, nullptr)) f = full;
    }
    wchar_t fullOut[32768];
    if (GetFullPathNameW(out.c_str(), 32768, fullOut, nullptr)) out = fullOut;
    DWORD a = GetFileAttributesW(LongPath(out).c_str());
    bool outIsDir = (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) || out.back() == L'\\' || out.back() == L'/';
    if (job.op.empty()) job.op = GuessOp(files, out, outIsDir);
    job.inputs = files;
    if (outIsDir || job.op == L"tool.extract") job.outDir = out;
    else job.outPath = out;
    DefaultOptions(job);
    if (!FindConverter(job.op)) { Print("error: can't tell what to do; name the output's format or pass --op\n"); return 2; }

    Result r;
    Progress p;
    double t0 = Now();
    if (!Run(job, p, r)) { Print("error: " + Narrow(r.error) + "\n"); return 1; }
    char line[128];
    snprintf(line, sizeof line, " (%llu -> %llu bytes, %.0f ms)\n", (unsigned long long)r.before, (unsigned long long)r.after, (Now() - t0) * 1000);
    Print("saved " + Narrow(r.out) + line);
    return 0;
}

int Worker()
{
    // Read the whole job, then work.
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    std::string text;
    char buf[4096];
    DWORD got;
    while (ReadFile(in, buf, sizeof buf, &got, nullptr) && got) text.append(buf, got);
    Json j;
    Job job;
    if (!Json::Parse(text, j) || !Job::FromJson(j, job)) { PrintLine(Json::Object().Set(L"error", L"The job could not be read.")); return 2; }

    Progress p;
    double last = -1;
    p.report = [&](double f) {
        if (f - last < 0.01 && f < 1) return;   // enough for a progress bar
        last = f;
        PrintLine(Json::Object().Set(L"progress", f));
    };
    p.part = [](const wstring& path) { PrintLine(Json::Object().Set(L"part", path)); };
    p.helper = [](DWORD pid) { PrintLine(Json::Object().Set(L"helper", double(pid))); };
    Result r;
    if (!Run(job, p, r)) { PrintLine(Json::Object().Set(L"error", r.error)); return 1; }
    PrintLine(Json::Object().Set(L"done", r.out).Set(L"before", double(r.before)).Set(L"after", double(r.after)).Set(L"info", r.info));
    return 0;
}

}  // namespace

int RunOtherMode(int argc, wchar_t** argv, bool& handled)
{
    handled = false;
    if (argc < 2) return 0;
    wstring mode = argv[1];
    if (mode != L"--worker" && mode != L"--convert" && mode != L"--info") return 0;
    handled = true;
    // No crash dialogs from a worker: it just ends, and the UI says the file could not be converted.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetUpStdout(mode != L"--worker");
    int rc;
    if (mode == L"--worker") rc = Worker();
    else if (mode == L"--info") rc = argc > 2 ? Info(argv[2]) : 2;
    else rc = Convert(argc, argv, 2);
    CoUninitialize();
    return rc;
}

}  // namespace rd
