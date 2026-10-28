// Office documents to PDF through Office itself (when it is installed): Word for DOCX/DOC/RTF, Excel for
// XLSX/XLS/ODS, PowerPoint for PPTX/PPT. Office runs invisibly, opens the file read-only, exports, closes
// it, and quits unless it was already running with other files open. Only the worker talks to Office.
#include "conv.h"

#include <tlhelp32.h>

#include <set>

namespace rd {

namespace {

/// Calls on IDispatch by name, with named arguments: what VBScript does, without VBScript.
struct Disp {
    Com<IDispatch> d;

    static VARIANT Str(const wstring& s) { VARIANT v; VariantInit(&v); v.vt = VT_BSTR; v.bstrVal = SysAllocString(s.c_str()); return v; }
    static VARIANT Int(int i) { VARIANT v; VariantInit(&v); v.vt = VT_I4; v.lVal = i; return v; }
    static VARIANT Bool(bool b) { VARIANT v; VariantInit(&v); v.vt = VT_BOOL; v.boolVal = b ? VARIANT_TRUE : VARIANT_FALSE; return v; }

    /// name(arg=value, ...). Takes ownership of the argument variants.
    HRESULT Invoke(WORD kind, const wchar_t* name, std::vector<std::pair<const wchar_t*, VARIANT>> args, VARIANT* result, wstring& err)
    {
        if (!d) { err = L"Office stopped answering."; return E_FAIL; }
        std::vector<LPOLESTR> names{ const_cast<LPOLESTR>(name) };
        if (kind != DISPATCH_PROPERTYPUT) for (auto& a : args) names.push_back(const_cast<LPOLESTR>(a.first));   // a put's value has no name
        std::vector<DISPID> ids(names.size());
        HRESULT hr = d->GetIDsOfNames(IID_NULL, names.data(), UINT(names.size()), LOCALE_USER_DEFAULT, ids.data());
        if (FAILED(hr)) { for (auto& a : args) VariantClear(&a.second); err = wstring(L"Office doesn't know ") + name + L"."; return hr; }
        // Arguments go in reverse order, each tagged with its name's id.
        std::vector<VARIANT> v;
        std::vector<DISPID> named;
        for (size_t i = args.size(); i-- > 0;) { v.push_back(args[i].second); if (kind != DISPATCH_PROPERTYPUT) named.push_back(ids[i + 1]); }
        DISPID put = DISPID_PROPERTYPUT;
        DISPPARAMS p{ v.data(), named.data(), UINT(v.size()), UINT(named.size()) };
        if (kind == DISPATCH_PROPERTYPUT) { p.rgdispidNamedArgs = &put; p.cNamedArgs = 1; }
        EXCEPINFO ex{};
        VARIANT dummy;
        VariantInit(&dummy);
        hr = d->Invoke(ids[0], IID_NULL, LOCALE_USER_DEFAULT, kind, &p, result ? result : &dummy, &ex, nullptr);
        VariantClear(&dummy);
        for (auto& x : v) VariantClear(&x);
        if (FAILED(hr)) {
            err = ex.bstrDescription ? wstring(ex.bstrDescription) : L"Office could not do it (error " + std::to_wstring(unsigned(hr)) + L").";
            if (ex.bstrSource) SysFreeString(ex.bstrSource);
            if (ex.bstrDescription) SysFreeString(ex.bstrDescription);
            if (ex.bstrHelpFile) SysFreeString(ex.bstrHelpFile);
        }
        return hr;
    }

    bool Put(const wchar_t* name, VARIANT v, wstring& err) { return SUCCEEDED(Invoke(DISPATCH_PROPERTYPUT, name, { { L"", v } }, nullptr, err)); }
    Disp Get(const wchar_t* name, wstring& err)
    {
        VARIANT r;
        VariantInit(&r);
        Disp out;
        if (SUCCEEDED(Invoke(DISPATCH_PROPERTYGET, name, {}, &r, err)) && r.vt == VT_DISPATCH) out.d = Com<IDispatch>(r.pdispVal);
        else VariantClear(&r);
        return out;
    }
    int Count(wstring& err)
    {
        VARIANT r;
        VariantInit(&r);
        int n = -1;
        if (SUCCEEDED(Invoke(DISPATCH_PROPERTYGET, L"Count", {}, &r, err)) && SUCCEEDED(VariantChangeType(&r, &r, 0, VT_I4))) n = r.lVal;
        VariantClear(&r);
        return n;
    }
    Disp Call(const wchar_t* name, std::vector<std::pair<const wchar_t*, VARIANT>> args, wstring& err, bool* ok = nullptr)
    {
        VARIANT r;
        VariantInit(&r);
        Disp out;
        HRESULT hr = Invoke(DISPATCH_METHOD, name, std::move(args), &r, err);
        if (ok) *ok = SUCCEEDED(hr);
        if (SUCCEEDED(hr) && r.vt == VT_DISPATCH) out.d = Com<IDispatch>(r.pdispVal);
        else VariantClear(&r);
        return out;
    }
};

std::set<DWORD> ProcessesNamed(const wchar_t* exe)
{
    std::set<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{ sizeof pe };
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, exe)) out.insert(pe.th32ProcessID);
    CloseHandle(snap);
    return out;
}

}  // namespace

bool RunOfficePdf(const Job& job, Kind kind, const Progress& p, Result& r)
{
    const wstring& in = job.inputs[0];
    bool word = Bit(kind) & kWordFiles, excel = Bit(kind) & kExcelFiles;
    const wchar_t* prog = word ? L"Word.Application" : excel ? L"Excel.Application" : L"PowerPoint.Application";
    const wchar_t* exe = word ? L"WINWORD.EXE" : excel ? L"EXCEL.EXE" : L"POWERPNT.EXE";
    const wchar_t* appName = word ? L"Word" : excel ? L"Excel" : L"PowerPoint";
    r.before = FileSize(in);

    // Office writes the PDF itself, to a path with a .pdf name; it goes to the real output afterwards.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wstring dir = JoinPath(JoinPath(tmp, L"Roda"), L"office-" + std::to_wstring(GetCurrentProcessId()));
    EnsureDir(dir);
    wstring pdf = JoinPath(dir, L"out.pdf");
    DeleteFileW(pdf.c_str());

    CLSID clsid;
    if (FAILED(CLSIDFromProgID(prog, &clsid))) { r.error = wstring(appName) + L" isn't installed on this PC."; return false; }
    auto before = ProcessesNamed(exe);
    Disp app;
    if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(app.d.Put())))) {
        r.error = wstring(L"Could not start ") + appName + L".";
        return false;
    }
    // An Office we started ourselves: the UI is told, so a cancel can end it too.
    for (DWORD pid : ProcessesNamed(exe))
        if (!before.count(pid)) { if (p.helper) p.helper(pid); break; }
    p.Set(0.2);

    wstring err;
    bool ok = false;
    // A wrong password is passed on purpose: a protected file then fails at once instead of showing a
    // password prompt nobody can see.
    const wchar_t* noPassword = L"roda-has-no-password";
    if (word) {
        app.Put(L"DisplayAlerts", Disp::Int(0), err);   // wdAlertsNone
        app.Put(L"Visible", Disp::Bool(false), err);
        Disp docs = app.Get(L"Documents", err);
        Disp doc = docs.Call(L"Open", { { L"FileName", Disp::Str(in) }, { L"ConfirmConversions", Disp::Bool(false) }, { L"ReadOnly", Disp::Bool(true) },
                                       { L"AddToRecentFiles", Disp::Bool(false) }, { L"PasswordDocument", Disp::Str(noPassword) },
                                       { L"Visible", Disp::Bool(false) }, { L"NoEncodingDialog", Disp::Bool(true) } }, err);
        if (doc.d) {
            p.Set(0.5);
            doc.Call(L"ExportAsFixedFormat", { { L"OutputFileName", Disp::Str(pdf) }, { L"ExportFormat", Disp::Int(17) } }, err, &ok);   // wdExportFormatPDF
            doc.Call(L"Close", { { L"SaveChanges", Disp::Int(0) } }, err);
        }
        if (docs.d && docs.Count(err) == 0) app.Call(L"Quit", { { L"SaveChanges", Disp::Int(0) } }, err);
    } else if (excel) {
        app.Put(L"DisplayAlerts", Disp::Bool(false), err);
        app.Put(L"Visible", Disp::Bool(false), err);
        Disp books = app.Get(L"Workbooks", err);
        Disp wb = books.Call(L"Open", { { L"Filename", Disp::Str(in) }, { L"UpdateLinks", Disp::Int(0) }, { L"ReadOnly", Disp::Bool(true) },
                                        { L"Password", Disp::Str(noPassword) }, { L"AddToMru", Disp::Bool(false) } }, err);
        if (wb.d) {
            p.Set(0.5);
            wb.Call(L"ExportAsFixedFormat", { { L"Type", Disp::Int(0) }, { L"Filename", Disp::Str(pdf) } }, err, &ok);   // xlTypePDF
            wb.Call(L"Close", { { L"SaveChanges", Disp::Bool(false) } }, err);
        }
        if (books.d && books.Count(err) == 0) app.Call(L"Quit", {}, err);
    } else {
        Disp pres = app.Get(L"Presentations", err);
        // ReadOnly, not Untitled, no window. The password trick works through the file name for PowerPoint.
        Disp pr = pres.Call(L"Open", { { L"FileName", Disp::Str(in + L"::" + noPassword + L"::") }, { L"ReadOnly", Disp::Int(-1) },
                                        { L"Untitled", Disp::Int(0) }, { L"WithWindow", Disp::Int(0) } }, err);
        if (pr.d) {
            p.Set(0.5);
            pr.Call(L"SaveAs", { { L"FileName", Disp::Str(pdf) }, { L"FileFormat", Disp::Int(32) } }, err, &ok);   // ppSaveAsPDF
            pr.Call(L"Close", {}, err);
        }
        if (pres.d && pres.Count(err) == 0) app.Call(L"Quit", {}, err);
    }
    app.d.Reset();

    std::vector<uint8_t> out;
    wstring readErr;
    if (!ok || !ReadShared(pdf, out, readErr) || out.empty()) {
        DeleteFileW(pdf.c_str());
        RemoveDirectoryW(dir.c_str());
        wstring lower = err;
        for (auto& c : lower) c = wchar_t(towlower(c));
        if (lower.find(L"password") != wstring::npos) r.error = L"This file is password protected.";
        else r.error = wstring(appName) + L" could not turn this file into a PDF" + (err.empty() ? L"." : L": " + err);
        return false;
    }
    DeleteFileW(pdf.c_str());
    RemoveDirectoryW(dir.c_str());
    p.Set(0.9);
    return WriteOutput(job, in, BaseName(in), L".pdf", out, p, r);
}

}  // namespace rd
