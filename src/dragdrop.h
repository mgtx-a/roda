// OLE drag and drop plumbing shared by the probe and the wheel.
#pragma once
#include "common.h"

#include <oleidl.h>

namespace rd {

/// IDropTarget with the COM bookkeeping done; owners override the four calls.
/// Owned by the window that registered it: RegisterDragDrop takes its own reference.
class DropTarget : public IDropTarget {
    LONG ref_ = 1;
public:
    virtual ~DropTarget() = default;
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) { *ppv = static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ULONG(InterlockedIncrement(&ref_)); }
    STDMETHODIMP_(ULONG) Release() override { ULONG r = ULONG(InterlockedDecrement(&ref_)); if (!r) delete this; return r; }
};

/// The file and folder paths in a drag (CF_HDROP). For virtual files (below) only their names, as
/// paths that don't exist yet: enough to choose the wheel. False when the drag carries no files.
bool ReadDroppedPaths(IDataObject* data, std::vector<wstring>& out);

/// Whether the drag carries files at all (real or virtual), without reading them.
bool HasFiles(IDataObject* data);
bool HasFilesOnDisk(IDataObject* data);   // a CF_HDROP: real paths

/// Virtual files: things that aren't files on disk until dropped (Outlook attachments, files inside a
/// ZIP opened in Explorer). They arrive as a list of names plus a stream each.
bool HasVirtualFiles(IDataObject* data);
/// Writes the virtual files of a drop into a new folder in Roda's temp folder; their paths in `out`.
/// Must run during the drop: the source only promises the contents until then.
bool ExtractVirtualFiles(IDataObject* data, std::vector<wstring>& out, wstring& err);

/// Roda never moves what you drop: copy when the source allows it, else link, else nothing.
DWORD CopyEffect(DWORD allowed);

/// Set while Roda itself runs a drag, so the probe stays out of the way.
void SetOwnDrag(bool on);
bool OwnDrag();

}  // namespace rd
