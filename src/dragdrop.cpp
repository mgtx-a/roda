#include "dragdrop.h"

namespace rd {

static bool g_ownDrag = false;

void SetOwnDrag(bool on) { g_ownDrag = on; }
bool OwnDrag() { return g_ownDrag; }

bool HasFiles(IDataObject* data)
{
    FORMATETC fmt{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    return data && data->QueryGetData(&fmt) == S_OK;
}

bool ReadDroppedPaths(IDataObject* data, std::vector<wstring>& out)
{
    out.clear();
    if (!data) return false;
    FORMATETC fmt{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM med{};
    if (FAILED(data->GetData(&fmt, &med))) return false;
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

DWORD CopyEffect(DWORD allowed)
{
    if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
    return DROPEFFECT_NONE;
}

}  // namespace rd
