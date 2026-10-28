// The Convert wheel's rows ("to.png", "to.docx", ...) all land here; the input's kind decides who does it.
#include "doc.h"

#include <md4c-html.h>

namespace rd {

bool RunPdfTo(const Job& job, const wstring& target, const Progress& p, Result& r);
bool RunSheetTo(const Job& job, Kind kind, const wstring& target, const Progress& p, Result& r);
bool RunSubtitleTo(const Job& job, Kind kind, const wstring& target, const Progress& p, Result& r);

namespace {

/// Markdown straight to HTML with md4c's own renderer: tables, task lists and all.
std::string MarkdownPage(const std::string& md, const wstring& title)
{
    std::string body;
    md_html(md.data(), MD_SIZE(md.size()), [](const MD_CHAR* t, MD_SIZE n, void* u) { static_cast<std::string*>(u)->append(t, n); },
            &body, MD_DIALECT_GITHUB, 0);
    Doc styled;
    std::string page = WriteHtml(styled);   // the page around it: the same look as Roda's other HTML
    size_t at = page.find("<body>\n") + 7;
    page.insert(at, body);
    size_t t0 = page.find("<title>") + 7, t1 = page.find("</title>");
    page.replace(t0, t1 - t0, XmlEscape(title));
    return page;
}

bool RunDocTo(const Job& job, Kind kind, const wstring& target, const Progress& p, Result& r)
{
    const wstring& in = job.inputs[0];
    std::vector<uint8_t> bytes;
    if (!ReadShared(in, bytes, r.error)) return false;
    if (bytes.empty()) { r.error = L"The file is empty."; return false; }
    r.before = bytes.size();
    wstring base = BaseName(in);
    std::string text;
    std::vector<uint8_t> out;
    if (kind == Kind::Markdown && target == L"html") {
        Doc d;
        wstring err;
        std::string md = TextAsUtf8(bytes);
        ReadMarkdown(md, d, err);
        text = MarkdownPage(md, d.title.empty() ? base : d.title);
    } else {
        Doc doc;
        bool ok = true;
        switch (kind) {
            case Kind::Docx: ok = ReadDocx(bytes, doc, r.error); break;
            case Kind::Markdown: ok = ReadMarkdown(TextAsUtf8(bytes), doc, r.error); break;
            case Kind::Html: ok = ReadHtml(TextAsUtf8(bytes), doc, r.error); break;
            case Kind::Rtf: ok = ReadRtf(std::string(bytes.begin(), bytes.end()), doc, r.error); break;
            case Kind::Txt: ok = ReadText(TextAsUtf8(bytes), doc); break;
            default: r.error = L"This isn't a document Roda can read."; return false;
        }
        if (!ok) return false;
        if (doc.title.empty()) doc.title = base;
        p.Set(0.5);
        if (target == L"html") text = WriteHtml(doc);
        else if (target == L"md") text = WriteMarkdown(doc);
        else if (target == L"txt") text = "\xEF\xBB\xBF" + WriteText(doc);
        else if (target == L"docx") { if (!WriteDocx(doc, out)) { r.error = L"Could not write the Word document."; return false; } }
        else if (target == L"epub") { if (!WriteEpub(doc, out)) { r.error = L"Could not write the e-book."; return false; } }
        else { r.error = L"Roda can't write that format."; return false; }
    }
    if (out.empty()) out.assign(text.begin(), text.end());
    return WriteOutput(job, in, base, L"." + target, out, p, r);
}

}  // namespace

bool RunConvert(const Job& job, const Progress& p, Result& r)
{
    wstring target = job.op.substr(job.op.find(L'.') + 1);   // "to.png" -> "png"
    Kind kind = KindOfPath(job.inputs[0]);
    if (IsImage(kind)) return RunToImage(job, p, r);
    if (kind == Kind::Pdf) return RunPdfTo(job, target, p, r);
    if (Bit(kind) & kDocs) return RunDocTo(job, kind, target, p, r);
    if (Bit(kind) & kSheets) return RunSheetTo(job, kind, target, p, r);
    if (Bit(kind) & kSubtitles) return RunSubtitleTo(job, kind, target, p, r);
    // The bytes didn't say: an image format by name still goes to the image decoder, which explains.
    return RunToImage(job, p, r);
}

}  // namespace rd
