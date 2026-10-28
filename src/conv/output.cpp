// Where results go. Never over a source file, never over anything that already exists (except the
// CLI's explicit output): a unique name is reserved by creating "<name>.part" exclusively, the work is
// written there, and only a finished result is renamed to its final name.
#include "conv.h"

namespace rd {

wstring LongPath(const wstring& path)
{
    if (path.size() < MAX_PATH - 12 || path.rfind(L"\\\\?\\", 0) == 0) return path;
    if (path.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

wstring BaseName(const wstring& path)
{
    wstring name = FileName(path);
    size_t dot = name.find_last_of(L'.');
    return dot == wstring::npos || dot == 0 ? name : name.substr(0, dot);
}

uint64_t FileSize(const wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(LongPath(path).c_str(), GetFileExInfoStandard, &fa)) return 0;
    return (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
}

static bool Exists(const wstring& p) { return GetFileAttributesW(LongPath(p).c_str()) != INVALID_FILE_ATTRIBUTES; }

/// Creates `part` only if nothing has that name: two workers can never pick the same output.
static bool Claim(const wstring& part, bool folder)
{
    if (folder) return CreateDirectoryW(LongPath(part).c_str(), nullptr) != FALSE;
    HANDLE h = CreateFileW(LongPath(part).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

bool ReserveOutput(const Job& job, const wstring& nextTo, const wstring& base, const wstring& ext, bool folder, OutFile& o, wstring& err)
{
    o = {};
    o.folder = folder;
    if (!job.outPath.empty()) {
        o.exact = true;
        o.final = job.outPath;
        o.part = job.outPath + L".part";
        for (auto& in : job.inputs)
            if (SamePath(in, o.final)) { err = L"The output would replace the original file."; return false; }
        if (Exists(o.part) && !folder) DeleteFileW(LongPath(o.part).c_str());   // left by an earlier, killed run
        if (!Claim(o.part, folder)) { err = L"Could not write " + o.part + L"."; return false; }
        return true;
    }
    wstring dir = job.outDir.empty() ? ParentDir(nextTo) : job.outDir;
    if (!job.outDir.empty()) EnsureDir(dir);
    for (int n = 0; n < 10000; n++) {
        wstring name = base + (n ? L" (" + std::to_wstring(n) + L")" : L"") + ext;
        wstring final = JoinPath(dir, name);
        if (Exists(final) || Exists(final + L".part")) continue;
        if (Claim(final + L".part", folder)) { o.final = final; o.part = final + L".part"; return true; }
        DWORD e = GetLastError();
        if (e != ERROR_FILE_EXISTS && e != ERROR_ALREADY_EXISTS) {
            err = e == ERROR_ACCESS_DENIED ? L"Roda can't write in " + dir + L" (no permission)." : L"Could not write in " + dir + L".";
            return false;
        }
    }
    err = L"Could not find a free file name in " + dir + L".";
    return false;
}

bool CommitOutput(OutFile& o, wstring& err)
{
    if (o.exact) {
        if (o.folder) {
            if (Exists(o.final)) { err = L"The output folder already exists."; return false; }
        }
        if (MoveFileExW(LongPath(o.part).c_str(), LongPath(o.final).c_str(), (o.folder ? 0 : MOVEFILE_REPLACE_EXISTING) | MOVEFILE_WRITE_THROUGH)) return true;
        err = L"Could not save " + o.final + L".";
        return false;
    }
    // Something may have taken the name while we worked: then use the next free one.
    wstring dir = ParentDir(o.final);
    wstring name = FileName(o.final);
    wstring ext = o.folder ? L"" : Extension(o.final);
    wstring base = name.substr(0, name.size() - ext.size());
    for (int n = 0; n < 10000; n++) {
        wstring final = n == 0 ? o.final : JoinPath(dir, base + L" (" + std::to_wstring(n) + L")" + ext);
        if (Exists(final)) continue;
        if (MoveFileExW(LongPath(o.part).c_str(), LongPath(final).c_str(), MOVEFILE_WRITE_THROUGH)) { o.final = final; return true; }
        DWORD e = GetLastError();
        if (e != ERROR_FILE_EXISTS && e != ERROR_ALREADY_EXISTS) break;
    }
    err = L"Could not save the result in " + dir + L".";
    return false;
}

void AbandonOutput(OutFile& o)
{
    if (o.part.empty()) return;
    if (o.folder) DeleteTree(LongPath(o.part));
    else DeleteFileW(LongPath(o.part).c_str());
    o.part.clear();
}

bool WriteOutput(const Job& job, const wstring& nextTo, const wstring& base, const wstring& ext, const std::vector<uint8_t>& data,
                 const Progress& p, Result& r)
{
    OutFile o;
    if (!ReserveOutput(job, nextTo, base, ext, false, o, r.error)) return false;
    if (p.part) p.part(o.part);
    if (!WriteAllBytes(LongPath(o.part), data.data(), data.size())) {
        AbandonOutput(o);
        r.error = L"Could not write the result (is the disk full?).";
        return false;
    }
    if (!CommitOutput(o, r.error)) { AbandonOutput(o); return false; }
    r.out = o.final;
    r.after = data.size();
    return true;
}

bool ReadShared(const wstring& path, std::vector<uint8_t>& out, wstring& err)
{
    HANDLE h = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        err = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? L"The file is gone." :
              e == ERROR_SHARING_VIOLATION ? L"The file is in use by another app." :
              e == ERROR_ACCESS_DENIED ? L"Roda isn't allowed to read this file." : L"Could not open the file.";
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    if (size.QuadPart > (1LL << 31)) { CloseHandle(h); err = L"The file is too big (over 2 GB)."; return false; }
    out.resize(size_t(size.QuadPart));
    DWORD got = 0;
    size_t done = 0;
    while (done < out.size()) {
        DWORD want = DWORD(std::min<size_t>(out.size() - done, 1 << 24));
        if (!ReadFile(h, out.data() + done, want, &got, nullptr) || got == 0) break;
        done += got;
    }
    CloseHandle(h);
    if (done != out.size()) { err = L"Could not read the whole file."; return false; }
    return true;
}

}  // namespace rd
