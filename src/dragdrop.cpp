#include "dragdrop.h"

namespace rd {

static bool g_ownDrag = false;

void SetOwnDrag(bool on) { g_ownDrag = on; }
bool OwnDrag() { return g_ownDrag; }

static CLIPFORMAT Descriptor() { static CLIPFORMAT f = CLIPFORMAT(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW)); return f; }
static CLIPFORMAT Contents() { static CLIPFORMAT f = CLIPFORMAT(RegisterClipboardFormatW(CFSTR_FILECONTENTS)); return f; }

bool HasVirtualFiles(IDataObject* data)
{
    FORMATETC fmt{ Descriptor(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    return data && data->QueryGetData(&fmt) == S_OK;
}

bool HasFilesOnDisk(IDataObject* data)
{
    FORMATETC fmt{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    return data && data->QueryGetData(&fmt) == S_OK;
}

bool HasFiles(IDataObject* data) { return HasFilesOnDisk(data) || HasVirtualFiles(data); }

/// The names in a virtual-file drop (relative paths, "folder\file.txt"), with their attributes.
static std::vector<std::pair<wstring, DWORD>> VirtualNames(IDataObject* data)
{
    std::vector<std::pair<wstring, DWORD>> out;
    FORMATETC fmt{ Descriptor(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM med{};
    if (FAILED(data->GetData(&fmt, &med))) return out;
    if (auto* g = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(med.hGlobal))) {
        for (UINT i = 0; i < g->cItems; i++) {
            const FILEDESCRIPTORW& d = g->fgd[i];
            out.push_back({ d.cFileName, (d.dwFlags & FD_ATTRIBUTES) ? d.dwFileAttributes : 0 });
        }
        GlobalUnlock(med.hGlobal);
    }
    ReleaseStgMedium(&med);
    return out;
}

bool ReadDroppedPaths(IDataObject* data, std::vector<wstring>& out)
{
    out.clear();
    if (!data) return false;
    FORMATETC fmt{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM med{};
    if (FAILED(data->GetData(&fmt, &med))) {
        // Virtual files: their top-level names, under the temp folder they will be written to.
        for (auto& [name, attrs] : VirtualNames(data))
            if (name.find_first_of(L"\\/") == wstring::npos) out.push_back(JoinPath(JoinPath(TempDir(), L"pending"), name));
        return !out.empty();
    }
    if (auto drop = static_cast<HDROP>(GlobalLock(med.hGlobal))) {
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            UINT len = DragQueryFileW(drop, i, nullptr, 0);   // no MAX_PATH limit: long paths come through whole
            wstring p(len, L'\0');
            DragQueryFileW(drop, i, p.data(), len + 1);
            if (!p.empty()) out.push_back(std::move(p));
        }
        GlobalUnlock(med.hGlobal);
    }
    ReleaseStgMedium(&med);
    return !out.empty();
}

bool ExtractVirtualFiles(IDataObject* data, std::vector<wstring>& out, wstring& err)
{
    out.clear();
    auto names = VirtualNames(data);
    if (names.empty()) { err = L"Nothing could be read from what was dropped."; return false; }
    static int drop = 0;
    wstring dir = JoinPath(TempDir(), L"drop-" + std::to_wstring(++drop));
    EnsureDir(dir);
    for (size_t i = 0; i < names.size(); i++) {
        // A safe relative name: no "..", no drive, no characters Windows refuses.
        wstring rel;
        for (wchar_t c : names[i].first) rel += (c == L'/') ? L'\\' : (wcschr(L"<>:\"|?*", c) || c < 32) ? L'_' : c;
        if (rel.empty() || rel.find(L"..") != wstring::npos) continue;
        wstring path = JoinPath(dir, rel);
        if (names[i].second & FILE_ATTRIBUTE_DIRECTORY) SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
        else {
            SHCreateDirectoryExW(nullptr, ParentDir(path).c_str(), nullptr);
            FORMATETC fmt{ Contents(), nullptr, DVASPECT_CONTENT, LONG(i), TYMED_ISTREAM | TYMED_HGLOBAL };
            STGMEDIUM med{};
            if (FAILED(data->GetData(&fmt, &med))) { err = L"Could not read " + rel + L" from the source."; return false; }
            HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            bool ok = h != INVALID_HANDLE_VALUE;
            DWORD put = 0;
            if (ok && med.tymed == TYMED_ISTREAM) {
                std::vector<uint8_t> buf(1 << 20);
                ULONG got = 0;
                while (ok && SUCCEEDED(med.pstm->Read(buf.data(), ULONG(buf.size()), &got)) && got) ok = WriteFile(h, buf.data(), got, &put, nullptr) && put == got;
            } else if (ok && med.tymed == TYMED_HGLOBAL) {
                SIZE_T n = GlobalSize(med.hGlobal);
                void* p = GlobalLock(med.hGlobal);
                ok = p && WriteFile(h, p, DWORD(n), &put, nullptr);
                GlobalUnlock(med.hGlobal);
            }
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            ReleaseStgMedium(&med);
            if (!ok) { err = L"Could not copy " + rel + L" (is the disk full?)."; return false; }
        }
        if (rel.find(L'\\') == wstring::npos) out.push_back(path);   // the items themselves, not what is inside a folder
    }
    Log(L"drop: %d virtual item(s) copied to %s", int(out.size()), dir.c_str());
    return !out.empty();
}

DWORD CopyEffect(DWORD allowed)
{
    if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
    return DROPEFFECT_NONE;
}

}  // namespace rd
