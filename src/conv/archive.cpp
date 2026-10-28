// Archives: ZIP (miniz), TAR and TAR.GZ (our own TAR, miniz's deflate), extraction of ZIP, TAR,
// TAR.GZ and GZIP. Everything streams: a 10 GB folder never sits in memory.
// RAR needs libarchive, which isn't in yet.
#include "conv.h"

#include <miniz.h>

namespace rd {

namespace {

// ---------------------------------------------------------------- what goes in

struct Entry {
    wstring path;          // on disk
    std::string name;      // in the archive, UTF-8, '/' separated; folders end in '/'
    uint64_t size = 0;
    FILETIME mtime{};
    bool dir = false;
};

void Walk(const wstring& dir, const std::string& prefix, std::vector<Entry>& out)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(LongPath(JoinPath(dir, L"*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        // Junctions and symbolic links are left out: following them can loop or reach far outside the folder.
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        Entry e;
        e.path = JoinPath(dir, fd.cFileName);
        e.name = prefix + Narrow(fd.cFileName);
        e.mtime = fd.ftLastWriteTime;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            e.dir = true;
            e.name += '/';
            out.push_back(e);
            Walk(e.path, e.name, out);
        } else {
            e.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            out.push_back(e);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::vector<Entry> Collect(const std::vector<wstring>& inputs, uint64_t& total)
{
    std::vector<Entry> out;
    total = 0;
    for (auto& in : inputs) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(LongPath(in).c_str(), GetFileExInfoStandard, &fa)) continue;
        Entry e;
        e.path = in;
        e.name = Narrow(FileName(in));
        e.mtime = fa.ftLastWriteTime;
        if (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            e.dir = true;
            e.name += '/';
            out.push_back(e);
            Walk(in, e.name, out);
        } else {
            e.size = (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
            out.push_back(e);
        }
    }
    for (auto& e : out) total += e.size;
    return out;
}

HANDLE OpenRead(const wstring& p)
{
    return CreateFileW(LongPath(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

HANDLE OpenWrite(const wstring& p)
{
    return CreateFileW(LongPath(p).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool WriteAll(HANDLE h, const void* p, size_t n)
{
    const uint8_t* b = static_cast<const uint8_t*>(p);
    while (n) {
        DWORD put = 0, want = DWORD(std::min<size_t>(n, 1 << 24));
        if (!WriteFile(h, b, want, &put, nullptr) || put == 0) return false;
        b += put;
        n -= put;
    }
    return true;
}

/// Files that are already compressed gain nothing from deflate: store them, it's much faster.
bool AlreadyCompressed(const wstring& path)
{
    static const wchar_t* exts[] = { L".jpg", L".jpeg", L".png", L".gif", L".webp", L".heic", L".avif", L".mp4", L".mov", L".mkv", L".webm",
                                     L".mp3", L".aac", L".m4a", L".ogg", L".opus", L".flac", L".zip", L".gz", L".tgz", L".7z", L".rar",
                                     L".xz", L".bz2", L".docx", L".xlsx", L".pptx" };
    wstring e = Extension(path);
    for (auto* x : exts) if (e == x) return true;
    return false;
}

wstring ArchiveBase(const std::vector<wstring>& inputs)
{
    if (inputs.size() == 1) {
        DWORD a = GetFileAttributesW(LongPath(inputs[0]).c_str());
        return (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? FileName(inputs[0]) : BaseName(inputs[0]);
    }
    return L"Archive";
}

// ---------------------------------------------------------------- sinks: a file, or gzip on top of a file

struct Sink {
    virtual ~Sink() = default;
    virtual bool Write(const void* p, size_t n) = 0;
    virtual bool Finish() = 0;
};

struct FileSink : Sink {
    HANDLE h;
    explicit FileSink(HANDLE f) : h(f) {}
    bool Write(const void* p, size_t n) override { return WriteAll(h, p, n); }
    bool Finish() override { return true; }
};

struct GzipSink : Sink {
    Sink& out;
    std::unique_ptr<tdefl_compressor> comp{ new tdefl_compressor };
    mz_ulong crc = MZ_CRC32_INIT;
    uint64_t size = 0;
    bool ok = true;

    explicit GzipSink(Sink& o) : out(o)
    {
        static const uint8_t header[10] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0x0b };   // deflate, no name, made on NTFS
        ok = out.Write(header, 10);
        tdefl_init(comp.get(), Put, this, int(tdefl_create_comp_flags_from_zip_params(6, -15, MZ_DEFAULT_STRATEGY)));
    }
    static mz_bool Put(const void* p, int n, void* user)
    {
        auto* self = static_cast<GzipSink*>(user);
        self->ok = self->ok && self->out.Write(p, size_t(n));
        return self->ok;
    }
    bool Write(const void* p, size_t n) override
    {
        crc = mz_crc32(crc, static_cast<const uint8_t*>(p), n);
        size += n;
        return ok && tdefl_compress_buffer(comp.get(), p, n, TDEFL_NO_FLUSH) == TDEFL_STATUS_OKAY && ok;
    }
    bool Finish() override
    {
        if (!ok || tdefl_compress_buffer(comp.get(), nullptr, 0, TDEFL_FINISH) != TDEFL_STATUS_DONE) return false;
        uint8_t tail[8];
        for (int i = 0; i < 4; i++) { tail[i] = uint8_t(crc >> (8 * i)); tail[4 + i] = uint8_t(size >> (8 * i)); }
        return ok && out.Write(tail, 8);
    }
};

// ---------------------------------------------------------------- TAR writing (POSIX ustar, pax for long or non-ASCII names)

void Octal(char* field, size_t len, uint64_t v)
{
    snprintf(field, len, "%0*llo", int(len - 1), (unsigned long long)v);
}

uint64_t UnixTime(FILETIME ft)
{
    uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return t < 116444736000000000ULL ? 0 : (t - 116444736000000000ULL) / 10000000ULL;
}

bool TarHeader(Sink& out, const std::string& name, uint64_t size, uint64_t mtime, char type)
{
    char h[512] = {};
    memcpy(h, name.data(), std::min<size_t>(name.size(), 100));
    Octal(h + 100, 8, type == '5' ? 0755 : 0644);
    Octal(h + 108, 8, 0);
    Octal(h + 116, 8, 0);
    Octal(h + 124, 12, size);
    Octal(h + 136, 12, mtime);
    memset(h + 148, ' ', 8);
    h[156] = type;
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += uint8_t(h[i]);
    snprintf(h + 148, 8, "%06o", sum);
    h[155] = ' ';
    return out.Write(h, 512);
}

bool Pad(Sink& out, uint64_t size)
{
    static const char zero[512] = {};
    size_t pad = size_t((512 - size % 512) % 512);
    return pad == 0 || out.Write(zero, pad);
}

bool TarEntry(Sink& out, const Entry& e)
{
    uint64_t mt = UnixTime(e.mtime);
    bool ascii = true;
    for (unsigned char c : e.name) ascii = ascii && c < 0x80;
    bool big = e.size >= 077777777777ULL;   // over 8 GB doesn't fit the octal field
    if (e.name.size() > 99 || !ascii || big) {
        // A pax header carries what ustar can't: "<len> path=<utf-8>\n", "<len> size=<n>\n".
        std::string recs;
        auto rec = [&](const std::string& kv) {
            size_t n = kv.size() + 3;
            for (;;) { std::string s = std::to_string(n) + " " + kv + "\n"; if (s.size() == n) { recs += s; return; } n = s.size(); }
        };
        rec("path=" + e.name);
        if (big) rec("size=" + std::to_string(e.size));
        if (!TarHeader(out, "PaxHeader", recs.size(), mt, 'x') || !out.Write(recs.data(), recs.size()) || !Pad(out, recs.size())) return false;
    }
    std::string shortName = e.name.substr(0, 99);
    for (auto& c : shortName) if (uint8_t(c) >= 0x80) c = '_';
    return TarHeader(out, shortName, e.dir || big ? 0 : e.size, mt, e.dir ? '5' : '0');
}

// ---------------------------------------------------------------- sources: a file, or gunzip on top of a file

struct Source {
    virtual ~Source() = default;
    virtual size_t Read(void* p, size_t n) = 0;    // 0: end (or error, see failed)
    bool failed = false;
    wstring error;
};

struct FileSource : Source {
    HANDLE h;
    uint64_t done = 0;
    explicit FileSource(HANDLE f) : h(f) {}
    size_t Read(void* p, size_t n) override
    {
        DWORD got = 0;
        if (!ReadFile(h, p, DWORD(std::min<size_t>(n, 1 << 24)), &got, nullptr)) { failed = true; error = L"Could not read the archive."; return 0; }
        done += got;
        return got;
    }
};

struct GunzipSource : Source {
    Source& in;
    std::vector<uint8_t> inBuf = std::vector<uint8_t>(1 << 16);
    size_t inPos = 0, inLen = 0;
    std::unique_ptr<tinfl_decompressor> dec{ new tinfl_decompressor };
    std::vector<uint8_t> dict = std::vector<uint8_t>(TINFL_LZ_DICT_SIZE);
    size_t dictPos = 0, ready = 0, readyAt = 0;
    bool started = false, ended = false;

    explicit GunzipSource(Source& s) : in(s) {}

    bool Fill()
    {
        if (inPos < inLen) return true;
        inLen = in.Read(inBuf.data(), inBuf.size());
        inPos = 0;
        return inLen > 0;
    }
    int Byte() { return Fill() ? inBuf[inPos++] : -1; }

    bool Header()
    {
        if (Byte() != 0x1f || Byte() != 0x8b || Byte() != 8) return false;
        int flags = Byte();
        for (int i = 0; i < 6; i++) Byte();
        if (flags & 4) { int lo = Byte(), hi = Byte(); for (int n = lo | hi << 8; n > 0; n--) Byte(); }
        if (flags & 8) while (Byte() > 0) {}
        if (flags & 16) while (Byte() > 0) {}
        if (flags & 2) { Byte(); Byte(); }
        tinfl_init(dec.get());
        return true;
    }

    size_t Read(void* p, size_t n) override
    {
        uint8_t* out = static_cast<uint8_t*>(p);
        size_t given = 0;
        while (given < n) {
            if (ready) {
                size_t k = std::min(ready, n - given);
                memcpy(out + given, &dict[readyAt], k);
                given += k; readyAt += k; ready -= k;
                continue;
            }
            if (ended) break;
            if (!started) {
                if (!Header()) { failed = true; error = L"This isn't a gzip file, or it is damaged."; break; }
                started = true;
            }
            Fill();
            size_t inBytes = inLen - inPos, outBytes = dict.size() - dictPos;
            int flags = (inPos < inLen ? TINFL_FLAG_HAS_MORE_INPUT : 0);
            tinfl_status st = tinfl_decompress(dec.get(), &inBuf[inPos], &inBytes, dict.data(), &dict[dictPos], &outBytes, flags | TINFL_FLAG_HAS_MORE_INPUT);
            inPos += inBytes;
            readyAt = dictPos;
            ready = outBytes;
            dictPos = (dictPos + outBytes) & (dict.size() - 1);
            if (st == TINFL_STATUS_DONE) {
                for (int i = 0; i < 8; i++) Byte();   // CRC and size: trusted, as tar has its own structure
                // Another member may follow (concatenated .gz files).
                if (Fill() && inBuf[inPos] == 0x1f) started = false;
                else ended = true;
            } else if (st < 0) {
                failed = true;
                error = L"The compressed data is damaged.";
                break;
            } else if (st == TINFL_STATUS_NEEDS_MORE_INPUT && inLen == 0 && !ready) {
                failed = true;
                error = L"The archive ends too early (it may be incomplete).";
                break;
            }
        }
        return given;
    }
};

bool ReadExact(Source& s, void* p, size_t n)
{
    uint8_t* b = static_cast<uint8_t*>(p);
    while (n) {
        size_t got = s.Read(b, n);
        if (!got) return false;
        b += got;
        n -= got;
    }
    return true;
}

bool SkipBytes(Source& s, uint64_t n)
{
    uint8_t buf[1 << 14];
    while (n) {
        size_t k = size_t(std::min<uint64_t>(n, sizeof buf));
        if (!ReadExact(s, buf, k)) return false;
        n -= k;
    }
    return true;
}

// ---------------------------------------------------------------- safe names on the way out

/// An archive entry name made safe for this disk, relative, or empty to skip it ("../" and friends).
wstring SafeRelative(const wstring& raw)
{
    wstring out;
    size_t i = 0;
    while (i <= raw.size()) {
        size_t j = raw.find_first_of(L"/\\", i);
        if (j == wstring::npos) j = raw.size();
        wstring part = raw.substr(i, j - i);
        i = j + 1;
        if (part.empty() || part == L".") continue;
        if (part == L"..") return L"";
        if (out.empty() && part.size() == 2 && part[1] == L':') continue;   // "C:"
        for (auto& c : part) if (c < 32 || wcschr(L"<>:\"|?*", c)) c = L'_';
        while (!part.empty() && (part.back() == L' ' || part.back() == L'.')) part.pop_back();
        if (part.empty()) part = L"_";
        wstring upper = part.substr(0, part.find(L'.'));
        for (auto& c : upper) c = wchar_t(towupper(c));
        static const wchar_t* reserved[] = { L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"LPT1", L"LPT2", L"LPT3" };
        for (auto* r : reserved) if (upper == r) { part = L"_" + part; break; }
        out += (out.empty() ? L"" : L"\\") + part;
    }
    return out;
}

void MakeDirs(const wstring& root, const wstring& rel)
{
    size_t i = 0;
    while ((i = rel.find(L'\\', i)) != wstring::npos) {
        CreateDirectoryW(LongPath(JoinPath(root, rel.substr(0, i))).c_str(), nullptr);
        i++;
    }
    CreateDirectoryW(LongPath(JoinPath(root, rel)).c_str(), nullptr);
}

wstring ArchiveName(const char* name, bool utf8)
{
    std::string s(name);
    if (utf8 || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0) > 0) return Widen(s);
    int n = MultiByteToWideChar(437, 0, s.data(), int(s.size()), nullptr, 0);   // old zips: the DOS code page
    wstring w(size_t(n), L'\0');
    MultiByteToWideChar(437, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// ---------------------------------------------------------------- packing

bool PackZip(const std::vector<Entry>& entries, uint64_t total, HANDLE h, const Progress& p, wstring& err)
{
    struct Ctx { HANDLE h; uint64_t done = 0, total = 0; const Progress* p; HANDLE in = INVALID_HANDLE_VALUE; };
    Ctx ctx{ h, 0, total, &p };
    mz_zip_archive zip{};
    zip.m_pWrite = [](void* o, mz_uint64 ofs, const void* buf, size_t n) -> size_t {
        auto* c = static_cast<Ctx*>(o);
        LARGE_INTEGER at;
        at.QuadPart = LONGLONG(ofs);
        if (!SetFilePointerEx(c->h, at, nullptr, FILE_BEGIN) || !WriteAll(c->h, buf, n)) return 0;
        return n;
    };
    zip.m_pIO_opaque = &ctx;
    bool z64 = total > 0xF0000000ULL || entries.size() > 65000;
    if (!mz_zip_writer_init_v2(&zip, 0, z64 ? MZ_ZIP_FLAG_WRITE_ZIP64 : 0)) { err = L"Could not start the ZIP file."; return false; }
    bool ok = true;
    for (auto& e : entries) {
        if (e.dir) { ok = mz_zip_writer_add_mem(&zip, e.name.c_str(), nullptr, 0, 0); }
        else {
            ctx.in = OpenRead(e.path);
            if (ctx.in == INVALID_HANDLE_VALUE) { err = L"Could not read " + FileName(e.path) + L" (in use, or no permission)."; ok = false; break; }
            time_t mt = time_t(UnixTime(e.mtime));
            unsigned level = AlreadyCompressed(e.path) ? MZ_NO_COMPRESSION : MZ_DEFAULT_LEVEL;
            ok = mz_zip_writer_add_read_buf_callback(&zip, e.name.c_str(),
                [](void* o, mz_uint64 ofs, void* buf, size_t n) -> size_t {
                    auto* c = static_cast<Ctx*>(o);
                    LARGE_INTEGER at;
                    at.QuadPart = LONGLONG(ofs);
                    DWORD got = 0;
                    if (!SetFilePointerEx(c->in, at, nullptr, FILE_BEGIN) || !ReadFile(c->in, buf, DWORD(n), &got, nullptr)) return 0;
                    c->done += got;
                    if (c->total) c->p->Set(0.98 * double(c->done) / double(c->total));
                    return got;
                }, &ctx, e.size, &mt, nullptr, 0, level, nullptr, 0, nullptr, 0);
            CloseHandle(ctx.in);
            ctx.in = INVALID_HANDLE_VALUE;
        }
        if (!ok) { if (err.empty()) err = L"Could not add " + FileName(e.path) + L" to the ZIP."; break; }
    }
    ok = ok && mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    if (!ok && err.empty()) err = L"Could not finish the ZIP file (is the disk full?).";
    return ok;
}

bool PackTar(const std::vector<Entry>& entries, uint64_t total, Sink& out, const Progress& p, wstring& err)
{
    std::vector<uint8_t> buf(1 << 20);
    uint64_t done = 0;
    for (auto& e : entries) {
        if (!TarEntry(out, e)) { err = L"Could not write the archive (is the disk full?)."; return false; }
        if (e.dir) continue;
        HANDLE in = OpenRead(e.path);
        if (in == INVALID_HANDLE_VALUE) { err = L"Could not read " + FileName(e.path) + L" (in use, or no permission)."; return false; }
        uint64_t left = e.size;
        while (left) {
            DWORD got = 0;
            if (!ReadFile(in, buf.data(), DWORD(std::min<uint64_t>(left, buf.size())), &got, nullptr) || got == 0) break;
            if (!out.Write(buf.data(), got)) { CloseHandle(in); err = L"Could not write the archive (is the disk full?)."; return false; }
            left -= got;
            done += got;
            if (total) p.Set(0.98 * double(done) / double(total));
        }
        CloseHandle(in);
        if (left) { err = FileName(e.path) + L" changed while it was being packed."; return false; }
        if (!Pad(out, e.size)) return false;
    }
    static const char zero[1024] = {};
    return out.Write(zero, 1024);
}

// ---------------------------------------------------------------- extracting

bool ExtractZip(const wstring& path, const wstring& root, const Progress& p, wstring& err)
{
    HANDLE h = OpenRead(path);
    if (h == INVALID_HANDLE_VALUE) { err = L"Could not open the archive."; return false; }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    mz_zip_archive zip{};
    zip.m_pRead = [](void* o, mz_uint64 ofs, void* buf, size_t n) -> size_t {
        LARGE_INTEGER at;
        at.QuadPart = LONGLONG(ofs);
        DWORD got = 0;
        HANDLE f = static_cast<HANDLE>(o);
        if (!SetFilePointerEx(f, at, nullptr, FILE_BEGIN) || !ReadFile(f, buf, DWORD(n), &got, nullptr)) return 0;
        return got;
    };
    zip.m_pIO_opaque = h;
    if (!mz_zip_reader_init(&zip, mz_uint64(size.QuadPart), 0)) { CloseHandle(h); err = L"This ZIP file is damaged."; return false; }
    bool ok = true;
    mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n && ok; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) { err = L"This ZIP file is damaged."; ok = false; break; }
        if (st.m_is_encrypted) { err = L"This ZIP is password protected."; ok = false; break; }
        wstring rel = SafeRelative(ArchiveName(st.m_filename, (st.m_bit_flag & (1 << 11)) != 0));
        if (rel.empty()) continue;
        if (st.m_is_directory) { MakeDirs(root, rel); continue; }
        size_t slash = rel.find_last_of(L'\\');
        if (slash != wstring::npos) MakeDirs(root, rel.substr(0, slash));
        wstring dest = JoinPath(root, rel);
        HANDLE out = OpenWrite(dest);
        if (out == INVALID_HANDLE_VALUE) { err = L"Could not write " + rel + L"."; ok = false; break; }
        ok = mz_zip_reader_extract_to_callback(&zip, i, [](void* o, mz_uint64, const void* buf, size_t len) -> size_t {
            return WriteAll(static_cast<HANDLE>(o), buf, len) ? len : 0;
        }, out, 0);
        // Keep the time the file was last changed, as Explorer does.
        if (ok && st.m_time) {
            uint64_t t = uint64_t(st.m_time) * 10000000ULL + 116444736000000000ULL;
            FILETIME ft{ DWORD(t), DWORD(t >> 32) };
            SetFileTime(out, nullptr, nullptr, &ft);
        }
        CloseHandle(out);
        if (!ok) err = L"Could not extract " + rel + L" (the archive may be damaged, or the disk full).";
        p.Set(0.98 * (i + 1) / n);
    }
    mz_zip_reader_end(&zip);
    CloseHandle(h);
    return ok;
}

uint64_t TarNumber(const char* f, size_t len)
{
    if (uint8_t(f[0]) & 0x80) {   // GNU base-256 for big values
        uint64_t v = 0;
        for (size_t i = 1; i < len; i++) v = (v << 8) | uint8_t(f[i]);
        return v;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < len && f[i]; i++) if (f[i] >= '0' && f[i] <= '7') v = v * 8 + uint64_t(f[i] - '0');
    return v;
}

bool ExtractTar(Source& in, const wstring& root, const std::function<void()>& tick, wstring& err)
{
    std::vector<uint8_t> buf(1 << 20);
    std::string longName;
    uint64_t paxSize = 0;
    bool havePaxSize = false;
    int zeros = 0;
    for (;;) {
        char h[512];
        if (!ReadExact(in, h, 512)) { if (in.failed) { err = in.error; return false; } break; }
        bool empty = true;
        for (char c : h) if (c) { empty = false; break; }
        if (empty) { if (++zeros == 2) break; continue; }
        zeros = 0;
        unsigned sum = 0;
        for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : uint8_t(h[i]);
        if (sum != TarNumber(h + 148, 8)) { err = L"This TAR archive is damaged."; return false; }
        char type = h[156];
        uint64_t size = havePaxSize ? paxSize : TarNumber(h + 124, 12);
        std::string name;
        if (!longName.empty()) name = longName;
        else {
            std::string prefix(h + 345, strnlen(h + 345, 155));
            name = std::string(h, strnlen(h, 100));
            if (!prefix.empty() && memcmp(h + 257, "ustar", 5) == 0) name = prefix + "/" + name;
        }
        longName.clear();
        havePaxSize = false;
        uint64_t padded = (size + 511) / 512 * 512;
        if (type == 'x' || type == 'L') {
            // Pax ("path=", "size=") or GNU long name: applies to the next entry.
            std::string data(size_t(size), '\0');
            if (!ReadExact(in, data.data(), data.size()) || !SkipBytes(in, padded - size)) { err = L"This TAR archive ends too early."; return false; }
            if (type == 'L') longName = data.c_str();
            else {
                size_t i = 0;
                while (i < data.size()) {
                    size_t sp = data.find(' ', i);
                    if (sp == std::string::npos) break;
                    size_t len = size_t(strtoull(data.c_str() + i, nullptr, 10));
                    if (len == 0 || i + len > data.size()) break;
                    std::string kv = data.substr(sp + 1, i + len - sp - 2);
                    if (kv.rfind("path=", 0) == 0) longName = kv.substr(5);
                    if (kv.rfind("size=", 0) == 0) { paxSize = strtoull(kv.c_str() + 5, nullptr, 10); havePaxSize = true; }
                    i += len;
                }
            }
            continue;
        }
        wstring rel = SafeRelative(Widen(name));
        if (type == '5') { if (!rel.empty()) MakeDirs(root, rel); continue; }
        if ((type != '0' && type != 0 && type != '7') || rel.empty()) {
            // Links, devices, unsafe names: skipped.
            if (!SkipBytes(in, padded)) { err = L"This TAR archive ends too early."; return false; }
            continue;
        }
        size_t slash = rel.find_last_of(L'\\');
        if (slash != wstring::npos) MakeDirs(root, rel.substr(0, slash));
        HANDLE out = OpenWrite(JoinPath(root, rel));
        if (out == INVALID_HANDLE_VALUE) { err = L"Could not write " + rel + L"."; return false; }
        uint64_t left = size;
        while (left) {
            size_t k = size_t(std::min<uint64_t>(left, buf.size()));
            if (!ReadExact(in, buf.data(), k)) { CloseHandle(out); err = in.failed ? in.error : L"This TAR archive ends too early."; return false; }
            if (!WriteAll(out, buf.data(), k)) { CloseHandle(out); err = L"Could not write " + rel + L" (is the disk full?)."; return false; }
            left -= k;
            tick();
        }
        uint64_t mt = TarNumber(h + 136, 12);
        if (mt) {
            uint64_t t = mt * 10000000ULL + 116444736000000000ULL;
            FILETIME ft{ DWORD(t), DWORD(t >> 32) };
            SetFileTime(out, nullptr, nullptr, &ft);
        }
        CloseHandle(out);
        if (!SkipBytes(in, padded - size)) { err = L"This TAR archive ends too early."; return false; }
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------- public

bool RunPack(const Job& job, const Progress& p, Result& r)
{
    uint64_t total = 0;
    auto entries = Collect(job.inputs, total);
    if (entries.empty()) { r.error = L"Nothing to pack (the files are gone)."; return false; }
    r.before = total;
    const wchar_t* ext = job.op == L"pack.zip" ? L".zip" : job.op == L"pack.tar" ? L".tar" : L".tar.gz";
    OutFile o;
    if (!ReserveOutput(job, job.inputs[0], ArchiveBase(job.inputs), ext, false, o, r.error)) return false;
    if (p.part) p.part(o.part);
    HANDLE h = OpenWrite(o.part);
    if (h == INVALID_HANDLE_VALUE) { AbandonOutput(o); r.error = L"Could not write the archive."; return false; }
    bool ok;
    if (job.op == L"pack.zip") ok = PackZip(entries, total, h, p, r.error);
    else {
        FileSink file(h);
        if (job.op == L"pack.tar") ok = PackTar(entries, total, file, p, r.error);
        else {
            GzipSink gz(file);
            ok = PackTar(entries, total, gz, p, r.error) && gz.Finish();
            if (!ok && r.error.empty()) r.error = L"Could not write the archive (is the disk full?).";
        }
    }
    CloseHandle(h);
    if (!ok || !CommitOutput(o, r.error)) { AbandonOutput(o); return false; }
    r.out = o.final;
    r.after = FileSize(o.final);
    return true;
}

bool RunExtract(const Job& job, const Progress& p, Result& r)
{
    const wstring& in = job.inputs[0];
    uint8_t head[512] = {};
    DWORD got = 0;
    HANDLE h = OpenRead(in);
    if (h == INVALID_HANDLE_VALUE) { r.error = L"Could not open the archive."; return false; }
    ReadFile(h, head, sizeof head, &got, nullptr);
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    r.before = uint64_t(size.QuadPart);
    Kind k = KindFromBytes(head, got, in);
    if (k == Kind::Rar) { CloseHandle(h); r.error = L"RAR archives can't be opened yet."; return false; }
    if (k != Kind::Zip && k != Kind::Tar && k != Kind::Gzip) { CloseHandle(h); r.error = L"This isn't an archive Roda can open."; return false; }
    SetFilePointer(h, 0, nullptr, FILE_BEGIN);

    // For a .gz, look inside: a tarball becomes a folder, anything else a single file.
    bool tarInside = false;
    if (k == Kind::Gzip) {
        FileSource probeFile(h);
        GunzipSource probe(probeFile);
        uint8_t inner[512];
        size_t n = 0;
        while (n < sizeof inner) { size_t g = probe.Read(inner + n, sizeof inner - n); if (!g) break; n += g; }
        tarInside = n >= 262 && memcmp(inner + 257, "ustar", 5) == 0;
        if (n == 0 && probe.failed) { CloseHandle(h); r.error = probe.error; return false; }
        SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    }

    wstring base = BaseName(in);
    if (Extension(base) == L".tar") base = BaseName(base);   // "x.tar.gz" -> "x"

    if (k == Kind::Gzip && !tarInside) {
        wstring name = Extension(in) == L".tgz" ? base + L".tar" : base;
        wstring ext = Extension(name);
        OutFile o;
        if (!ReserveOutput(job, in, name.substr(0, name.size() - ext.size()), ext, false, o, r.error)) { CloseHandle(h); return false; }
        if (p.part) p.part(o.part);
        HANDLE out = OpenWrite(o.part);
        FileSource file(h);
        GunzipSource gz(file);
        std::vector<uint8_t> buf(1 << 20);
        bool ok = out != INVALID_HANDLE_VALUE;
        while (ok) {
            size_t n = gz.Read(buf.data(), buf.size());
            if (!n) break;
            ok = WriteAll(out, buf.data(), n);
            p.Set(0.98 * double(file.done) / double(std::max<LONGLONG>(1, size.QuadPart)));
        }
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        CloseHandle(h);
        if (gz.failed) { r.error = gz.error; ok = false; }
        if (!ok || !CommitOutput(o, r.error)) { if (r.error.empty()) r.error = L"Could not write the file."; AbandonOutput(o); return false; }
        r.out = o.final;
        r.after = FileSize(o.final);
        return true;
    }

    OutFile o;
    if (!ReserveOutput(job, in, base, L"", true, o, r.error)) { CloseHandle(h); return false; }
    if (p.part) p.part(o.part);
    bool ok;
    if (k == Kind::Zip) {
        CloseHandle(h);
        ok = ExtractZip(in, o.part, p, r.error);
    } else {
        FileSource file(h);
        auto tick = [&] { p.Set(0.98 * double(file.done) / double(std::max<LONGLONG>(1, size.QuadPart))); };
        if (k == Kind::Gzip) {
            GunzipSource gz(file);
            ok = ExtractTar(gz, o.part, tick, r.error);
        } else {
            ok = ExtractTar(file, o.part, tick, r.error);
        }
        CloseHandle(h);
    }
    if (!ok || !CommitOutput(o, r.error)) { AbandonOutput(o); return false; }
    r.out = o.final;
    return true;
}

}  // namespace rd
