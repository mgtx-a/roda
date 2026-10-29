// Writers for the document model: HTML, Markdown, plain text, DOCX, EPUB, and PPTX (pictures only).
// The Office and EPUB packages are written by hand: only the parts Word, PowerPoint and e-readers need.
#include "doc.h"

#include <miniz.h>

namespace rd {

// ---------------------------------------------------------------- small helpers

void Doc::Text(const wstring& t, bool bold, bool italic, bool code, const wstring& link)
{
    if (t.empty()) return;
    if (blocks.empty()) Add(Block::Para);
    auto& spans = blocks.back().spans;
    if (!spans.empty()) {
        Span& last = spans.back();
        if (last.bold == bold && last.italic == italic && last.code == code && last.link == link) { last.text += t; return; }
    }
    spans.push_back({ t, bold, italic, code, link });
}

wstring PlainText(const Block& b)
{
    wstring s;
    for (auto& sp : b.spans) s += sp.text;
    return s;
}

std::string XmlEscape(const wstring& s)
{
    std::string u = Narrow(s), out;
    out.reserve(u.size());
    for (char c : u) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default:
                // Control characters other than tab and line breaks are not allowed in XML.
                if (uint8_t(c) < 0x20 && c != '\t' && c != '\n' && c != '\r') break;
                out += c;
        }
    }
    return out;
}

namespace {

std::string Base64(const std::vector<uint8_t>& b)
{
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((b.size() + 2) / 3 * 4);
    for (size_t i = 0; i < b.size(); i += 3) {
        uint32_t v = uint32_t(b[i]) << 16 | (i + 1 < b.size() ? uint32_t(b[i + 1]) << 8 : 0) | (i + 2 < b.size() ? b[i + 2] : 0);
        out += t[v >> 18 & 63];
        out += t[v >> 12 & 63];
        out += i + 1 < b.size() ? t[v >> 6 & 63] : '=';
        out += i + 2 < b.size() ? t[v & 63] : '=';
    }
    return out;
}

const char* Mime(const wstring& ext) { return ext == L".jpg" ? "image/jpeg" : "image/png"; }

/// Spans as HTML (or XHTML: the same, as Roda's tags are all closed).
std::string SpansHtml(const std::vector<Span>& spans, bool xhtml)
{
    std::string out;
    for (auto& s : spans) {
        std::string t;
        size_t start = 0;
        while (true) {   // line breaks inside a paragraph
            size_t nl = s.text.find(L'\n', start);
            t += XmlEscape(s.text.substr(start, nl == wstring::npos ? wstring::npos : nl - start));
            if (nl == wstring::npos) break;
            t += xhtml ? "<br />" : "<br>";
            start = nl + 1;
        }
        if (s.code) t = "<code>" + t + "</code>";
        if (s.italic) t = "<em>" + t + "</em>";
        if (s.bold) t = "<strong>" + t + "</strong>";
        if (!s.link.empty()) t = "<a href=\"" + XmlEscape(s.link) + "\">" + t + "</a>";
        out += t;
    }
    return out;
}

/// Blocks as HTML body content. `imageSrc` says where a picture's bytes are (a data: URI, or a file).
std::string BlocksHtml(const std::vector<Block>& blocks, bool xhtml, const std::function<std::string(const Block&, int)>& imageSrc)
{
    std::string out;
    std::vector<bool> open;   // list nesting: ordered?
    int picture = 0;
    auto closeLists = [&](size_t to) {
        while (open.size() > to) { out += open.back() ? "</ol>\n" : "</ul>\n"; open.pop_back(); }
    };
    for (auto& b : blocks) {
        if (b.type == Block::ListItem) {
            size_t depth = size_t(std::max(0, b.level)) + 1;
            closeLists(depth);
            if (open.size() == depth && open.back() != b.ordered) closeLists(depth - 1);
            while (open.size() < depth) { out += b.ordered ? "<ol>\n" : "<ul>\n"; open.push_back(b.ordered); }
            out += "<li>" + SpansHtml(b.spans, xhtml) + "</li>\n";
            continue;
        }
        closeLists(0);
        switch (b.type) {
            case Block::Heading: {
                std::string h = std::to_string(std::clamp(b.level, 1, 6));
                out += "<h" + h + ">" + SpansHtml(b.spans, xhtml) + "</h" + h + ">\n";
                break;
            }
            case Block::Para: out += "<p>" + SpansHtml(b.spans, xhtml) + "</p>\n"; break;
            case Block::Quote: out += "<blockquote><p>" + SpansHtml(b.spans, xhtml) + "</p></blockquote>\n"; break;
            case Block::Code: out += "<pre><code>" + XmlEscape(b.code) + "</code></pre>\n"; break;
            case Block::Rule: out += xhtml ? "<hr />\n" : "<hr>\n"; break;
            case Block::PageBreak: out += xhtml ? "<hr class=\"page\" />\n" : "<hr class=\"page\">\n"; break;
            case Block::Table: {
                out += "<table>\n";
                for (size_t r = 0; r < b.rows.size(); r++) {
                    const char* cell = r == 0 ? "th" : "td";
                    out += "<tr>";
                    for (auto& c : b.rows[r]) out += std::string("<") + cell + ">" + XmlEscape(c) + "</" + cell + ">";
                    out += "</tr>\n";
                }
                out += "</table>\n";
                break;
            }
            case Block::Picture: {
                std::string alt = XmlEscape(b.alt);
                out += "<p><img src=\"" + imageSrc(b, picture++) + "\" alt=\"" + alt + "\"" +
                       (b.imgW > 0 ? " width=\"" + std::to_string(b.imgW) + "\"" : std::string()) + (xhtml ? " /></p>\n" : "></p>\n");
                break;
            }
            default: break;
        }
    }
    closeLists(0);
    return out;
}

const char* kCss =
    "body{font-family:system-ui,'Segoe UI',sans-serif;line-height:1.55;max-width:46em;margin:2em auto;padding:0 1em;color:#1d2430}"
    "img{max-width:100%;height:auto}table{border-collapse:collapse}th,td{border:1px solid #c8d0d4;padding:.3em .6em}"
    "pre{background:#f2f5f6;padding:.8em;overflow:auto}code{font-family:Consolas,monospace}"
    "blockquote{border-left:3px solid #1f7a74;margin-left:0;padding-left:1em;color:#475360}hr.page{border:0;border-top:1px dashed #c8d0d4}";

// ---------------------------------------------------------------- Markdown

std::string MdEscape(const wstring& s)
{
    std::string u = Narrow(s), out;
    for (size_t i = 0; i < u.size(); i++) {
        char c = u[i];
        if (strchr("\\`*_[]<>#|", c)) out += '\\';
        out += c;
    }
    return out;
}

std::string SpansMd(const std::vector<Span>& spans)
{
    std::string out;
    for (auto& s : spans) {
        // Emphasis can't wrap leading or trailing spaces in Markdown: keep them outside.
        wstring t = s.text;
        size_t a = t.find_first_not_of(L" "), z = t.find_last_not_of(L" ");
        if (a == wstring::npos) { out += Narrow(t); continue; }
        std::string lead(a, ' '), trail(t.size() - 1 - z, ' ');
        wstring core = t.substr(a, z - a + 1);
        std::string m;
        if (s.code) {
            std::string c = Narrow(core);
            m = c.find('`') != std::string::npos ? "`` " + c + " ``" : "`" + c + "`";
        } else {
            m = MdEscape(core);
        }
        for (size_t nl = m.find('\n'); nl != std::string::npos; nl = m.find('\n', nl + 3)) m.replace(nl, 1, "  \n");   // a hard line break
        if (s.italic) m = "*" + m + "*";
        if (s.bold) m = "**" + m + "**";
        if (!s.link.empty()) m = "[" + m + "](" + Narrow(s.link) + ")";
        out += lead + m + trail;
    }
    return out;
}

}  // namespace

std::string WriteHtml(const Doc& doc)
{
    std::string title = XmlEscape(doc.title.empty() ? L"Document" : doc.title);
    std::string out = "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width\">\n<title>" +
                      title + "</title>\n<style>" + kCss + "</style>\n</head>\n<body>\n";
    out += BlocksHtml(doc.blocks, false, [](const Block& b, int) { return std::string("data:") + Mime(b.imageExt) + ";base64," + Base64(b.image); });
    out += "</body>\n</html>\n";
    return out;
}

std::string WriteMarkdown(const Doc& doc)
{
    std::string out;
    std::vector<int> counters(8, 0);
    Block::Type prev = Block::Para;
    bool prevOrdered = false;
    for (auto& b : doc.blocks) {
        bool prevList = prev == Block::ListItem;
        if (b.type != Block::ListItem && prev == Block::ListItem) { out += "\n"; std::fill(counters.begin(), counters.end(), 0); }
        switch (b.type) {
            case Block::Heading: {
                std::vector<Span> plain = b.spans;   // a heading is bold already
                for (auto& s : plain) s.bold = false;
                out += std::string(size_t(std::clamp(b.level, 1, 6)), '#') + " " + SpansMd(plain) + "\n\n";
                break;
            }
            case Block::Para: { std::string t = SpansMd(b.spans); if (!t.empty()) out += t + "\n\n"; break; }
            case Block::ListItem: {
                int lv = std::clamp(b.level, 0, 7);
                for (int i = lv + 1; i < 8; i++) counters[size_t(i)] = 0;
                // A different kind of list right after another: a blank line, or Markdown runs them together.
                if (prevList && lv == 0 && prevOrdered != b.ordered) { out += "\n"; counters[0] = 0; }
                prevOrdered = b.ordered;
                std::string marker = b.ordered ? std::to_string(++counters[size_t(lv)]) + ". " : "- ";
                out += std::string(size_t(lv) * 3, ' ') + marker + SpansMd(b.spans) + "\n";
                break;
            }
            case Block::Quote: out += "> " + SpansMd(b.spans) + "\n\n"; break;
            case Block::Code: out += "```\n" + Narrow(b.code) + (b.code.empty() || b.code.back() == L'\n' ? "" : "\n") + "```\n\n"; break;
            case Block::Rule: out += "---\n\n"; break;
            case Block::Table: {
                if (b.rows.empty()) break;
                size_t cols = 0;
                for (auto& r : b.rows) cols = std::max(cols, r.size());
                auto cell = [](const wstring& c) { std::string s = MdEscape(c); for (auto& ch : s) if (ch == '\n') ch = ' '; return s; };
                for (size_t r = 0; r < b.rows.size(); r++) {
                    out += "|";
                    for (size_t c = 0; c < cols; c++) out += " " + (c < b.rows[r].size() ? cell(b.rows[r][c]) : std::string()) + " |";
                    out += "\n";
                    if (r == 0) { out += "|"; for (size_t c = 0; c < cols; c++) out += " --- |"; out += "\n"; }
                }
                out += "\n";
                break;
            }
            case Block::Picture: if (!b.alt.empty()) out += "*(picture: " + MdEscape(b.alt) + ")*\n\n"; break;   // pictures stay in the original
            default: break;
        }
        prev = b.type;
    }
    while (out.size() > 1 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n') out.pop_back();
    return out;
}

std::string WriteText(const Doc& doc)
{
    std::string out;
    std::vector<int> counters(8, 0);
    Block::Type prev = Block::Para;
    for (auto& b : doc.blocks) {
        if (b.type != Block::ListItem && prev == Block::ListItem) { out += "\r\n"; std::fill(counters.begin(), counters.end(), 0); }
        auto lines = [](const wstring& t) { std::string s = Narrow(t), r; for (char c : s) { if (c == '\n') r += "\r\n"; else r += c; } return r; };
        switch (b.type) {
            case Block::Heading: case Block::Para: case Block::Quote: {
                std::string t = lines(PlainText(b));
                if (!t.empty()) out += t + "\r\n\r\n";
                break;
            }
            case Block::ListItem: {
                int lv = std::clamp(b.level, 0, 7);
                for (int i = lv + 1; i < 8; i++) counters[size_t(i)] = 0;
                out += std::string(size_t(lv) * 2, ' ') + (b.ordered ? std::to_string(++counters[size_t(lv)]) + ". " : "- ") + lines(PlainText(b)) + "\r\n";
                break;
            }
            case Block::Code: out += lines(b.code) + "\r\n\r\n"; break;
            case Block::Table:
                for (auto& r : b.rows) {
                    for (size_t c = 0; c < r.size(); c++) out += (c ? "\t" : "") + lines(r[c]);
                    out += "\r\n";
                }
                out += "\r\n";
                break;
            case Block::PageBreak: out += "\f"; break;   // form feed: printers and editors know it as a new page
            default: break;
        }
        prev = b.type;
    }
    while (out.size() > 2 && out.compare(out.size() - 4, 4, "\r\n\r\n") == 0) out.resize(out.size() - 2);
    return out;
}

// ---------------------------------------------------------------- ZIP in memory

// Our own small writer rather than miniz's: miniz marks every in-memory entry "sizes follow the data",
// which Word, Excel and PowerPoint refuse, and which the ODS and EPUB rules forbid for "mimetype".
// Here every header carries the real sizes and checksum. Deflate itself is still miniz's.
struct ZipOut::Impl {
    struct Entry { std::string name; uint32_t crc, csize, usize, offset; uint16_t method; };
    std::vector<uint8_t> out;
    std::vector<Entry> entries;
    uint16_t time = 0, date = 0;
    bool ok = true;

    void U16(uint16_t v) { out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8)); }
    void U32(uint32_t v) { U16(uint16_t(v)); U16(uint16_t(v >> 16)); }

    bool Add(const std::string& name, const uint8_t* data, size_t size, bool store)
    {
        if (!ok || size > 0x7FFFFFFF || out.size() > 0x7FFFFFFF) return ok = false;   // in-memory packages: no ZIP64
        Entry e{ name, uint32_t(mz_crc32(MZ_CRC32_INIT, data, size)), 0, uint32_t(size), uint32_t(out.size()), 0 };
        const uint8_t* body = data;
        size_t bodySize = size;
        void* packed = nullptr;
        if (!store && size > 0) {
            size_t n = 0;
            packed = tdefl_compress_mem_to_heap(data, size, &n, tdefl_create_comp_flags_from_zip_params(6, -15, MZ_DEFAULT_STRATEGY));
            if (packed && n < size) { body = static_cast<const uint8_t*>(packed); bodySize = n; e.method = 8; }
        }
        e.csize = uint32_t(bodySize);
        U32(0x04034b50); U16(20); U16(0); U16(e.method); U16(time); U16(date);
        U32(e.crc); U32(e.csize); U32(e.usize); U16(uint16_t(name.size())); U16(0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), body, body + bodySize);
        if (packed) mz_free(packed);
        entries.push_back(std::move(e));
        return true;
    }
};

ZipOut::ZipOut() : z_(new Impl)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    z_->time = uint16_t(t.wHour << 11 | t.wMinute << 5 | t.wSecond / 2);
    z_->date = uint16_t((t.wYear - 1980) << 9 | t.wMonth << 5 | t.wDay);
}

ZipOut::~ZipOut() = default;

bool ZipOut::Add(const std::string& name, const std::string& data, bool store)
{
    return z_->Add(name, reinterpret_cast<const uint8_t*>(data.data()), data.size(), store);
}

bool ZipOut::Add(const std::string& name, const std::vector<uint8_t>& data, bool store)
{
    return z_->Add(name, data.data(), data.size(), store);
}

bool ZipOut::Finish(std::vector<uint8_t>& out)
{
    Impl& z = *z_;
    if (!z.ok) return false;
    uint32_t cd = uint32_t(z.out.size());
    for (auto& e : z.entries) {
        z.U32(0x02014b50); z.U16(20); z.U16(20); z.U16(0); z.U16(e.method); z.U16(z.time); z.U16(z.date);
        z.U32(e.crc); z.U32(e.csize); z.U32(e.usize); z.U16(uint16_t(e.name.size())); z.U16(0); z.U16(0); z.U16(0); z.U16(0); z.U32(0); z.U32(e.offset);
        z.out.insert(z.out.end(), e.name.begin(), e.name.end());
    }
    uint32_t cdSize = uint32_t(z.out.size()) - cd;
    z.U32(0x06054b50); z.U16(0); z.U16(0); z.U16(uint16_t(z.entries.size())); z.U16(uint16_t(z.entries.size()));
    z.U32(cdSize); z.U32(cd); z.U16(0);
    out.swap(z.out);
    z.ok = false;
    return true;
}

bool ZipEntry(const std::vector<uint8_t>& zip, const char* name, std::string& out)
{
    mz_zip_archive z{};
    if (!mz_zip_reader_init_mem(&z, zip.data(), zip.size(), 0)) return false;
    size_t size = 0;
    void* p = mz_zip_reader_extract_file_to_heap(&z, name, &size, 0);
    bool ok = p != nullptr;
    if (ok) { out.assign(static_cast<char*>(p), size); mz_free(p); }
    mz_zip_reader_end(&z);
    return ok;
}

// ---------------------------------------------------------------- DOCX

namespace {

struct DocxWriter {
    std::string body, rels;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> media;
    int nextRel = 10, nextPic = 1, nextNum = 3;
    std::string numberingExtra;          // one <w:num> per ordered list, so each starts at 1
    int currentOrderedNum = 0;

    std::string Rel(const std::string& type, const std::string& target, bool external)
    {
        std::string id = "rId" + std::to_string(nextRel++);
        rels += "<Relationship Id=\"" + id + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/" + type +
                "\" Target=\"" + target + "\"" + (external ? " TargetMode=\"External\"" : "") + "/>";
        return id;
    }

    std::string Runs(const std::vector<Span>& spans, const char* extraRunProps = "")
    {
        std::string out;
        for (auto& s : spans) {
            std::string props = extraRunProps;
            if (s.bold) props += "<w:b/>";
            if (s.italic) props += "<w:i/>";
            if (s.code) props += "<w:rStyle w:val=\"CodeChar\"/>";
            if (!s.link.empty()) props = "<w:rStyle w:val=\"Hyperlink\"/>" + props;
            std::string runs;
            size_t start = 0;
            while (true) {
                size_t nl = s.text.find(L'\n', start);
                wstring part = s.text.substr(start, nl == wstring::npos ? wstring::npos : nl - start);
                runs += "<w:r>" + (props.empty() ? std::string() : "<w:rPr>" + props + "</w:rPr>") + "<w:t xml:space=\"preserve\">" + XmlEscape(part) + "</w:t>" +
                        (nl != wstring::npos ? "<w:br/>" : "") + "</w:r>";
                if (nl == wstring::npos) break;
                start = nl + 1;
            }
            if (!s.link.empty()) out += "<w:hyperlink r:id=\"" + Rel("hyperlink", XmlEscape(s.link), true) + "\">" + runs + "</w:hyperlink>";
            else out += runs;
        }
        return out;
    }

    void Para(const std::string& style, const std::string& content, const std::string& extraPPr = "")
    {
        std::string ppr = (style.empty() ? std::string() : "<w:pStyle w:val=\"" + style + "\"/>") + extraPPr;
        body += "<w:p>" + (ppr.empty() ? std::string() : "<w:pPr>" + ppr + "</w:pPr>") + content + "</w:p>";
    }

    void Picture(const Block& b)
    {
        std::string ext = b.imageExt == L".jpg" ? "jpeg" : "png";
        std::string name = "media/image" + std::to_string(nextPic) + (ext == "jpeg" ? ".jpg" : ".png");
        std::string rid = Rel("image", name, false);
        media.push_back({ "word/" + name, b.image });
        // Pixels at 96 DPI, fitted to a 6.3-inch text column.
        double wEmu = std::max(1, b.imgW) * 9525.0, hEmu = std::max(1, b.imgH) * 9525.0;
        double k = std::min(1.0, 5760720.0 / wEmu);
        long long cx = (long long)(wEmu * k), cy = (long long)(hEmu * k);
        int id = nextPic++;
        std::string sz = "cx=\"" + std::to_string(cx) + "\" cy=\"" + std::to_string(cy) + "\"";
        body += "<w:p><w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\"><wp:extent " + sz + "/>"
                "<wp:effectExtent l=\"0\" t=\"0\" r=\"0\" b=\"0\"/>"   // the optional parts Word itself writes: it refuses some files without them
                "<wp:docPr id=\"" + std::to_string(id) + "\" name=\"Picture " + std::to_string(id) + "\" descr=\"" + XmlEscape(b.alt) + "\"/>"
                "<wp:cNvGraphicFramePr><a:graphicFrameLocks xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" noChangeAspect=\"1\"/></wp:cNvGraphicFramePr>"
                "<a:graphic xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
                "<pic:pic xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><pic:nvPicPr><pic:cNvPr id=\"0\" name=\"" + name.substr(6) + "\"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill><a:blip r:embed=\"" + rid +
                "\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill><pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext " + sz +
                "/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>";
    }

    void Table(const Block& b)
    {
        size_t cols = 0;
        for (auto& r : b.rows) cols = std::max(cols, r.size());
        if (!cols) return;
        body += "<w:tbl><w:tblPr><w:tblStyle w:val=\"Grid\"/><w:tblW w:w=\"0\" w:type=\"auto\"/></w:tblPr><w:tblGrid>";
        for (size_t c = 0; c < cols; c++) body += "<w:gridCol/>";
        body += "</w:tblGrid>";
        for (size_t r = 0; r < b.rows.size(); r++) {
            body += r == 0 ? "<w:tr><w:trPr><w:tblHeader/></w:trPr>" : "<w:tr>";
            for (size_t c = 0; c < cols; c++) {
                wstring t = c < b.rows[r].size() ? b.rows[r][c] : L"";
                body += "<w:tc><w:p><w:r>" + std::string(r == 0 ? "<w:rPr><w:b/></w:rPr>" : "") + "<w:t xml:space=\"preserve\">" + XmlEscape(t) + "</w:t></w:r></w:p></w:tc>";
            }
            body += "</w:tr>";
        }
        body += "</w:tbl><w:p/>";
    }
};

const char* kDocxStyles =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
    "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Calibri\" w:hAnsi=\"Calibri\" w:eastAsia=\"Calibri\" w:cs=\"Calibri\"/><w:sz w:val=\"22\"/></w:rPr></w:rPrDefault>"
    "<w:pPrDefault><w:pPr><w:spacing w:after=\"160\" w:line=\"264\" w:lineRule=\"auto\"/></w:pPr></w:pPrDefault></w:docDefaults>"
    "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:qFormat/></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Title\"><w:name w:val=\"Title\"/><w:basedOn w:val=\"Normal\"/><w:qFormat/><w:rPr><w:sz w:val=\"52\"/></w:rPr></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/><w:pPr><w:keepNext/><w:spacing w:before=\"360\" w:after=\"120\"/><w:outlineLvl w:val=\"0\"/></w:pPr><w:rPr><w:b/><w:sz w:val=\"36\"/></w:rPr></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading2\"><w:name w:val=\"heading 2\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/><w:pPr><w:keepNext/><w:spacing w:before=\"280\" w:after=\"100\"/><w:outlineLvl w:val=\"1\"/></w:pPr><w:rPr><w:b/><w:sz w:val=\"30\"/></w:rPr></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading3\"><w:name w:val=\"heading 3\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/><w:pPr><w:keepNext/><w:spacing w:before=\"240\" w:after=\"80\"/><w:outlineLvl w:val=\"2\"/></w:pPr><w:rPr><w:b/><w:sz w:val=\"26\"/></w:rPr></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading4\"><w:name w:val=\"heading 4\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/><w:pPr><w:keepNext/><w:outlineLvl w:val=\"3\"/></w:pPr><w:rPr><w:b/><w:i/><w:sz w:val=\"24\"/></w:rPr></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading5\"><w:name w:val=\"heading 5\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/><w:pPr><w:keepNext/><w:outlineLvl w:val=\"4\"/></w:pPr><w:rPr><w:b/></w:rPr></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading6\"><w:name w:val=\"heading 6\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/><w:pPr><w:keepNext/><w:outlineLvl w:val=\"5\"/></w:pPr><w:rPr><w:i/></w:rPr></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Quote\"><w:name w:val=\"Quote\"/><w:basedOn w:val=\"Normal\"/><w:qFormat/><w:pPr><w:ind w:left=\"720\"/></w:pPr><w:rPr><w:i/><w:color w:val=\"475360\"/></w:rPr></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Code\"><w:name w:val=\"Code\"/><w:basedOn w:val=\"Normal\"/><w:pPr><w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/><w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"F2F5F6\"/></w:pPr><w:rPr><w:rFonts w:ascii=\"Consolas\" w:hAnsi=\"Consolas\"/><w:sz w:val=\"19\"/></w:rPr></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"ListParagraph\"><w:name w:val=\"List Paragraph\"/><w:basedOn w:val=\"Normal\"/><w:pPr><w:spacing w:after=\"40\"/><w:ind w:left=\"720\"/></w:pPr></w:style>"
    "<w:style w:type=\"character\" w:styleId=\"Hyperlink\"><w:name w:val=\"Hyperlink\"/><w:rPr><w:color w:val=\"1F7A74\"/><w:u w:val=\"single\"/></w:rPr></w:style>"
    "<w:style w:type=\"character\" w:styleId=\"CodeChar\"><w:name w:val=\"Code Char\"/><w:rPr><w:rFonts w:ascii=\"Consolas\" w:hAnsi=\"Consolas\"/></w:rPr></w:style>"
    "<w:style w:type=\"table\" w:styleId=\"Grid\"><w:name w:val=\"Table Grid\"/><w:tblPr><w:tblBorders>"
    "<w:top w:val=\"single\" w:sz=\"4\" w:color=\"C8D0D4\"/><w:left w:val=\"single\" w:sz=\"4\" w:color=\"C8D0D4\"/><w:bottom w:val=\"single\" w:sz=\"4\" w:color=\"C8D0D4\"/>"
    "<w:right w:val=\"single\" w:sz=\"4\" w:color=\"C8D0D4\"/><w:insideH w:val=\"single\" w:sz=\"4\" w:color=\"C8D0D4\"/><w:insideV w:val=\"single\" w:sz=\"4\" w:color=\"C8D0D4\"/>"
    "</w:tblBorders><w:tblCellMar><w:left w:w=\"108\" w:type=\"dxa\"/><w:right w:w=\"108\" w:type=\"dxa\"/></w:tblCellMar></w:tblPr></w:style>"
    "</w:styles>";

std::string NumberingLevels(bool ordered)
{
    std::string out;
    static const char* bullets[] = { "\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA" };
    for (int lv = 0; lv < 9; lv++) {
        std::string ind = std::to_string(720 * (lv + 1));
        out += "<w:lvl w:ilvl=\"" + std::to_string(lv) + "\"><w:start w:val=\"1\"/><w:numFmt w:val=\"" + (ordered ? std::string(lv % 3 == 1 ? "lowerLetter" : lv % 3 == 2 ? "lowerRoman" : "decimal") : "bullet") +
               "\"/><w:lvlText w:val=\"" + (ordered ? "%" + std::to_string(lv + 1) + "." : std::string(bullets[lv % 3])) +
               "\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"" + ind + "\" w:hanging=\"360\"/></w:pPr></w:lvl>";
    }
    return out;
}

}  // namespace

bool WriteDocx(const Doc& doc, std::vector<uint8_t>& out)
{
    DocxWriter w;
    bool inOrdered = false;
    for (auto& b : doc.blocks) {
        if (b.type != Block::ListItem || !b.ordered) inOrdered = false;
        switch (b.type) {
            case Block::Heading: w.Para("Heading" + std::to_string(std::clamp(b.level, 1, 6)), w.Runs(b.spans)); break;
            case Block::Para: w.Para("", w.Runs(b.spans)); break;
            case Block::Quote: w.Para("Quote", w.Runs(b.spans)); break;
            case Block::ListItem: {
                int numId = 1;
                if (b.ordered) {
                    // A new numbered list restarts at 1: it gets a numbering instance of its own.
                    if (!inOrdered) {
                        w.currentOrderedNum = w.nextNum++;
                        w.numberingExtra += "<w:num w:numId=\"" + std::to_string(w.currentOrderedNum) + "\"><w:abstractNumId w:val=\"1\"/>"
                                            "<w:lvlOverride w:ilvl=\"0\"><w:startOverride w:val=\"1\"/></w:lvlOverride></w:num>";
                        inOrdered = true;
                    }
                    numId = w.currentOrderedNum;
                }
                w.Para("ListParagraph", w.Runs(b.spans), "<w:numPr><w:ilvl w:val=\"" + std::to_string(std::clamp(b.level, 0, 8)) + "\"/><w:numId w:val=\"" + std::to_string(numId) + "\"/></w:numPr>");
                break;
            }
            case Block::Code: {
                std::vector<Span> one{ { b.code } };
                w.Para("Code", w.Runs(one));
                break;
            }
            case Block::Table: w.Table(b); break;
            case Block::Picture: if (!b.image.empty()) w.Picture(b); break;
            case Block::Rule: w.Para("", "", "<w:pBdr><w:bottom w:val=\"single\" w:sz=\"6\" w:space=\"1\" w:color=\"C8D0D4\"/></w:pBdr>"); break;
            case Block::PageBreak: w.body += "<w:p><w:r><w:br w:type=\"page\"/></w:r></w:p>"; break;
        }
    }
    std::string document =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
        "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\"><w:body>" + w.body +
        "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/><w:pgMar w:top=\"1134\" w:right=\"1134\" w:bottom=\"1134\" w:left=\"1134\" w:header=\"709\" w:footer=\"709\" w:gutter=\"0\"/></w:sectPr>"
        "</w:body></w:document>";
    std::string numbering =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:abstractNum w:abstractNumId=\"0\"><w:multiLevelType w:val=\"hybridMultilevel\"/>" + NumberingLevels(false) + "</w:abstractNum>"
        "<w:abstractNum w:abstractNumId=\"1\"><w:multiLevelType w:val=\"hybridMultilevel\"/>" + NumberingLevels(true) + "</w:abstractNum>"
        "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num><w:num w:numId=\"2\"><w:abstractNumId w:val=\"1\"/></w:num>" + w.numberingExtra +
        "</w:numbering>";
    std::string docRels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering\" Target=\"numbering.xml\"/>" +
        w.rels + "</Relationships>";
    std::string core =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<dc:title>" + XmlEscape(doc.title) + "</dc:title><dc:creator>Roda</dc:creator></cp:coreProperties>";
    ZipOut z;
    bool ok = z.Add("[Content_Types].xml",
                    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                    "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                    "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                    "<Default Extension=\"png\" ContentType=\"image/png\"/><Default Extension=\"jpg\" ContentType=\"image/jpeg\"/>"
                    "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
                    "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
                    "<Override PartName=\"/word/numbering.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>"
                    "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
                    "</Types>") &&
              z.Add("_rels/.rels",
                    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
                    "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
                    "</Relationships>") &&
              z.Add("word/document.xml", document) && z.Add("word/styles.xml", std::string(kDocxStyles)) && z.Add("word/numbering.xml", numbering) &&
              z.Add("word/_rels/document.xml.rels", docRels) && z.Add("docProps/core.xml", core);
    for (auto& [name, data] : w.media) ok = ok && z.Add(name, data);   // compressed: Office refuses the stored entries miniz writes
    return ok && z.Finish(out);
}

bool WriteLaidDocx(const std::vector<LaidPage>& pages, const wstring& title, std::vector<uint8_t>& out)
{
    const std::string pre = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>";
    const std::string rel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
    auto twips = [](double pt) { return std::to_string(std::lround(pt * 20)); };
    auto emu = [](double pt) { return std::to_string(std::llround(pt * 12700)); };
    // A paragraph one point high, out of the way: holds the page's drawing, or ends its section.
    const std::string tiny = "<w:spacing w:before=\"0\" w:after=\"0\" w:line=\"20\" w:lineRule=\"exact\"/>";
    std::string body, rels;
    ZipOut z;
    bool ok = true;
    for (size_t i = 0; i < pages.size(); i++) {
        const LaidPage& pg = pages[i];
        std::string n = std::to_string(i + 1);
        body += "<w:p><w:pPr>" + tiny + "</w:pPr>";
        if (!pg.drawing.empty()) {
            // The drawing fills the page, behind the text and locked, so clicking the text edits the text.
            std::string rid = "rIdP" + n, cx = emu(pg.w), cy = emu(pg.h);
            ok = ok && z.Add("word/media/page" + n + ".png", pg.drawing);
            rels += "<Relationship Id=\"" + rid + "\" Type=\"" + rel + "/image\" Target=\"media/page" + n + ".png\"/>";
            body += "<w:r><w:drawing><wp:anchor distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\" simplePos=\"0\" relativeHeight=\"0\" behindDoc=\"1\" locked=\"1\" layoutInCell=\"1\" allowOverlap=\"1\">"
                    "<wp:simplePos x=\"0\" y=\"0\"/><wp:positionH relativeFrom=\"page\"><wp:posOffset>0</wp:posOffset></wp:positionH>"
                    "<wp:positionV relativeFrom=\"page\"><wp:posOffset>0</wp:posOffset></wp:positionV><wp:extent cx=\"" + cx + "\" cy=\"" + cy + "\"/>"
                    "<wp:effectExtent l=\"0\" t=\"0\" r=\"0\" b=\"0\"/><wp:wrapNone/><wp:docPr id=\"" + n + "\" name=\"Page " + n + "\"/><wp:cNvGraphicFramePr/>"
                    "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><pic:pic><pic:nvPicPr><pic:cNvPr id=\"" + n +
                    "\" name=\"page" + n + ".png\"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill><a:blip r:embed=\"" + rid + "\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
                    "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"" + cx + "\" cy=\"" + cy + "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr>"
                    "</pic:pic></a:graphicData></a:graphic></wp:anchor></w:drawing></w:r>";
        }
        body += "</w:p>";
        // Each piece of text in a frame at its place on the page, one line exactly as high as on the page.
        for (auto& t : pg.texts) {
            body += "<w:p><w:pPr><w:framePr w:wrap=\"around\" w:hAnchor=\"page\" w:vAnchor=\"page\" w:x=\"" + twips(t.x) + "\" w:y=\"" + twips(t.y) +
                    "\" w:hSpace=\"0\" w:vSpace=\"0\"/><w:spacing w:before=\"0\" w:after=\"0\" w:line=\"" + twips(std::max(1.0, t.h)) + "\" w:lineRule=\"exact\"/>"
                    "<w:ind w:left=\"0\" w:right=\"0\"/></w:pPr>";
            for (auto& p : t.pieces) {
                std::string font = XmlEscape(p.font.empty() ? L"Arial" : p.font);
                char color[8];
                snprintf(color, sizeof color, "%06X", unsigned(p.color & 0xFFFFFF));
                std::string size = std::to_string(std::max(2L, std::lround(p.size * 2)));   // half points
                long spacing = t.stretch != 1 ? 0 : std::lround(t.spacing * 20);   // twentieths of a point; stretched letters need none
                body += "<w:r><w:rPr><w:rFonts w:ascii=\"" + font + "\" w:hAnsi=\"" + font + "\" w:cs=\"" + font + "\"/>" + (p.bold ? "<w:b/>" : "") +
                        (p.italic ? "<w:i/>" : "") + "<w:color w:val=\"" + color + "\"/>" + (spacing ? "<w:spacing w:val=\"" + std::to_string(spacing) + "\"/>" : "") +
                        (t.stretch != 1 ? "<w:w w:val=\"" + std::to_string(std::lround(t.stretch * 100)) + "\"/>" : "") +
                        "<w:sz w:val=\"" + size + "\"/><w:szCs w:val=\"" + size + "\"/></w:rPr>"
                        "<w:t xml:space=\"preserve\">" + XmlEscape(p.text) + "</w:t></w:r>";
            }
            body += "</w:p>";
        }
        // The page's own size, no margins: a section per page, as pages may differ.
        std::string sect = "<w:sectPr><w:pgSz w:w=\"" + twips(pg.w) + "\" w:h=\"" + twips(pg.h) + "\"" + (pg.w > pg.h ? " w:orient=\"landscape\"" : "") +
                           "/><w:pgMar w:top=\"0\" w:right=\"0\" w:bottom=\"0\" w:left=\"0\" w:header=\"0\" w:footer=\"0\" w:gutter=\"0\"/></w:sectPr>";
        body += i + 1 < pages.size() ? "<w:p><w:pPr>" + tiny + sect + "</w:pPr></w:p>" : sect;
    }
    std::string document = pre + "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" xmlns:r=\"" + rel + "\" "
        "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
        "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><w:body>" + body + "</w:body></w:document>";
    ok = ok && z.Add("[Content_Types].xml", pre + "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                     "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                     "<Default Extension=\"xml\" ContentType=\"application/xml\"/><Default Extension=\"png\" ContentType=\"image/png\"/>"
                     "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
                     "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
                     "<Override PartName=\"/word/settings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml\"/>"
                     "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/></Types>")
         && z.Add("_rels/.rels", pre + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                  "<Relationship Id=\"rId1\" Type=\"" + rel + "/officeDocument\" Target=\"word/document.xml\"/>"
                  "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/></Relationships>")
         && z.Add("word/document.xml", document)
         && z.Add("word/_rels/document.xml.rels", pre + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                  "<Relationship Id=\"rId1\" Type=\"" + rel + "/styles\" Target=\"styles.xml\"/><Relationship Id=\"rId2\" Type=\"" + rel + "/settings\" Target=\"settings.xml\"/>" +
                  rels + "</Relationships>")
         // No space around paragraphs unless asked: the frames carry their own.
         && z.Add("word/styles.xml", pre + "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:docDefaults>"
                  "<w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Arial\" w:hAnsi=\"Arial\" w:cs=\"Arial\"/><w:sz w:val=\"20\"/></w:rPr></w:rPrDefault>"
                  "<w:pPrDefault><w:pPr><w:spacing w:before=\"0\" w:after=\"0\"/></w:pPr></w:pPrDefault></w:docDefaults></w:styles>")
         // Today's Word, not "Compatibility Mode".
         && z.Add("word/settings.xml", pre + "<w:settings xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:compat>"
                  "<w:compatSetting w:name=\"compatibilityMode\" w:uri=\"http://schemas.microsoft.com/office/word\" w:val=\"15\"/></w:compat></w:settings>")
         && z.Add("docProps/core.xml", pre + "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
                  "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>" + XmlEscape(title) + "</dc:title><dc:creator>Roda</dc:creator></cp:coreProperties>");
    return ok && z.Finish(out);
}

// ---------------------------------------------------------------- EPUB

bool WriteEpub(const Doc& doc, std::vector<uint8_t>& out)
{
    // Chapters start at each top-level heading (and every ~150 blocks, so no file gets huge).
    int top = 6;
    for (auto& b : doc.blocks) if (b.type == Block::Heading) top = std::min(top, b.level);
    std::vector<std::vector<Block>> chapters(1);
    std::vector<std::pair<wstring, int>> toc;   // title, chapter
    for (auto& b : doc.blocks) {
        bool split = (b.type == Block::Heading && b.level == top) || chapters.back().size() >= 150;
        if (split && !chapters.back().empty()) chapters.emplace_back();
        if (b.type == Block::Heading && b.level <= top + 1) toc.push_back({ PlainText(b), int(chapters.size()) - 1 });
        if (b.type != Block::PageBreak) chapters.back().push_back(b);
    }
    wstring title = doc.title.empty() ? L"Document" : doc.title;
    std::string t = XmlEscape(title);
    ZipOut z;
    bool ok = z.Add("mimetype", std::string("application/epub+zip"), true);   // first and uncompressed, as readers expect
    ok = ok && z.Add("META-INF/container.xml",
                     "<?xml version=\"1.0\" encoding=\"UTF-8\"?><container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
                     "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>");
    std::string manifest, spine;
    int pic = 0;
    for (size_t c = 0; c < chapters.size(); c++) {
        std::string name = "chapter" + std::to_string(c + 1) + ".xhtml";
        std::string body = BlocksHtml(chapters[c], true, [&](const Block& b, int) {
            std::string file = "images/picture" + std::to_string(++pic) + (b.imageExt == L".jpg" ? ".jpg" : ".png");
            z.Add("OEBPS/" + file, b.image);
            manifest += "<item id=\"pic" + std::to_string(pic) + "\" href=\"" + file + "\" media-type=\"" + Mime(b.imageExt) + "\"/>";
            return file;
        });
        std::string page = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE html>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">"
                           "<head><title>" + t + "</title><link rel=\"stylesheet\" href=\"style.css\" type=\"text/css\"/></head><body>\n" + body + "</body></html>\n";
        ok = ok && z.Add("OEBPS/" + name, page);
        manifest += "<item id=\"c" + std::to_string(c + 1) + "\" href=\"" + name + "\" media-type=\"application/xhtml+xml\"/>";
        spine += "<itemref idref=\"c" + std::to_string(c + 1) + "\"/>";
    }
    std::string nav = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE html>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">"
                      "<head><title>" + t + "</title></head><body><nav epub:type=\"toc\" id=\"toc\"><h1>Contents</h1><ol>";
    if (toc.empty()) nav += "<li><a href=\"chapter1.xhtml\">" + t + "</a></li>";
    for (auto& [name, c] : toc) nav += "<li><a href=\"chapter" + std::to_string(c + 1) + ".xhtml\">" + XmlEscape(name.empty() ? L"Untitled" : name) + "</a></li>";
    nav += "</ol></nav></body></html>\n";
    // A stable identifier from the title and content size: the same input gives the same book.
    uint32_t h = 2166136261u;
    for (wchar_t ch : title) h = (h ^ uint32_t(ch)) * 16777619u;
    h ^= uint32_t(doc.blocks.size());
    char uid[64];
    snprintf(uid, sizeof uid, "urn:roda:%08x", h);
    SYSTEMTIME st;
    GetSystemTime(&st);
    char modified[32];
    snprintf(modified, sizeof modified, "%04d-%02d-%02dT%02d:%02d:%02dZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::string opf = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"uid\">"
                      "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"uid\">" + std::string(uid) + "</dc:identifier>"
                      "<dc:title>" + t + "</dc:title><dc:language>und</dc:language><meta property=\"dcterms:modified\">" + modified + "</meta></metadata>"
                      "<manifest><item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>"
                      "<item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>" + manifest + "</manifest><spine>" + spine + "</spine></package>";
    ok = ok && z.Add("OEBPS/nav.xhtml", nav) && z.Add("OEBPS/style.css", std::string(kCss)) && z.Add("OEBPS/content.opf", opf);
    return ok && z.Finish(out);
}

// ---------------------------------------------------------------- PPTX (one picture per slide)

namespace {

const char* kPptTheme =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"Roda\"><a:themeElements>"
    "<a:clrScheme name=\"Roda\"><a:dk1><a:srgbClr val=\"000000\"/></a:dk1><a:lt1><a:srgbClr val=\"FFFFFF\"/></a:lt1>"
    "<a:dk2><a:srgbClr val=\"1D2430\"/></a:dk2><a:lt2><a:srgbClr val=\"E8EEF0\"/></a:lt2><a:accent1><a:srgbClr val=\"1F7A74\"/></a:accent1>"
    "<a:accent2><a:srgbClr val=\"2E9E96\"/></a:accent2><a:accent3><a:srgbClr val=\"6A7480\"/></a:accent3><a:accent4><a:srgbClr val=\"B3261E\"/></a:accent4>"
    "<a:accent5><a:srgbClr val=\"F2B8B5\"/></a:accent5><a:accent6><a:srgbClr val=\"475360\"/></a:accent6><a:hlink><a:srgbClr val=\"1F7A74\"/></a:hlink>"
    "<a:folHlink><a:srgbClr val=\"475360\"/></a:folHlink></a:clrScheme>"
    "<a:fontScheme name=\"Roda\"><a:majorFont><a:latin typeface=\"Calibri\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:majorFont>"
    "<a:minorFont><a:latin typeface=\"Calibri\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:minorFont></a:fontScheme>"
    "<a:fmtScheme name=\"Roda\"><a:fillStyleLst><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
    "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:fillStyleLst><a:lnStyleLst><a:ln w=\"6350\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln>"
    "<a:ln w=\"12700\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln><a:ln w=\"19050\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln></a:lnStyleLst>"
    "<a:effectStyleLst><a:effectStyle><a:effectLst/></a:effectStyle><a:effectStyle><a:effectLst/></a:effectStyle><a:effectStyle><a:effectLst/></a:effectStyle></a:effectStyleLst>"
    "<a:bgFillStyleLst><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
    "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:bgFillStyleLst></a:fmtScheme></a:themeElements></a:theme>";

const char* kPptNs = "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
                     "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"";

std::string EmptyTree()
{
    return "<p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
           "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>"
           "</p:spTree></p:cSld>";
}

}  // namespace

bool WritePptx(const std::vector<LaidPage>& slides, std::vector<uint8_t>& out, wstring& err)
{
    if (slides.empty()) { err = L"Nothing to put on slides."; return false; }
    // The slide takes the first page's shape, 10 inches wide.
    const long long cx = 9144000;
    long long cy = std::clamp((long long)(cx * slides[0].h / std::max(1.0, slides[0].w)), 914400LL, 51206400LL);
    std::string pre = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>";
    std::string types = pre + "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Default Extension=\"png\" ContentType=\"image/png\"/>"
        "<Override PartName=\"/ppt/presentation.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml\"/>"
        "<Override PartName=\"/ppt/slideMasters/slideMaster1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml\"/>"
        "<Override PartName=\"/ppt/slideLayouts/slideLayout1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml\"/>"
        "<Override PartName=\"/ppt/theme/theme1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>";
    std::string presRels = pre + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster\" Target=\"slideMasters/slideMaster1.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme\" Target=\"theme/theme1.xml\"/>";
    std::string sldIds;
    ZipOut z;
    bool ok = true;
    for (size_t i = 0; i < slides.size(); i++) {
        const LaidPage& pg = slides[i];
        std::string n = std::to_string(i + 1);
        // The page fills the slide, keeping its shape (centred when the pages differ in size): k is EMUs per
        // point of the page, s points on the slide per point of the page.
        double k = std::min(double(cx) / std::max(1.0, pg.w), double(cy) / std::max(1.0, pg.h)), s = k / 12700;
        long long w = (long long)(pg.w * k), h = (long long)(pg.h * k);
        long long x = (cx - w) / 2, y = (cy - h) / 2;
        auto emu = [](double v) { return std::to_string(std::llround(v)); };
        std::string shapes, picRel;
        if (!pg.drawing.empty()) {
            // The drawing behind, locked: clicking the slide picks the text, not the picture.
            ok = ok && z.Add("ppt/media/page" + n + ".png", pg.drawing);
            picRel = "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/page" + n + ".png\"/>";
            shapes += "<p:pic><p:nvPicPr><p:cNvPr id=\"2\" name=\"Page " + n + "\"/><p:cNvPicPr><a:picLocks noGrp=\"1\" noSelect=\"1\" noMove=\"1\" noResize=\"1\" noChangeAspect=\"1\"/></p:cNvPicPr><p:nvPr/></p:nvPicPr>"
                      "<p:blipFill><a:blip r:embed=\"rId2\"/><a:stretch><a:fillRect/></a:stretch></p:blipFill>"
                      "<p:spPr><a:xfrm><a:off x=\"" + emu(x) + "\" y=\"" + emu(y) + "\"/><a:ext cx=\"" + emu(w) + "\" cy=\"" + emu(h) +
                      "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>";
        }
        // Each piece of text in a box of its own, where it was: no wrapping, no padding, one line as high as on the page.
        int id = 3;
        for (auto& t : pg.texts) {
            std::string sid = std::to_string(id++);
            shapes += "<p:sp><p:nvSpPr><p:cNvPr id=\"" + sid + "\" name=\"Text " + sid + "\"/><p:cNvSpPr txBox=\"1\"/><p:nvPr/></p:nvSpPr>"
                      "<p:spPr><a:xfrm><a:off x=\"" + emu(x + t.x * k) + "\" y=\"" + emu(y + t.y * k) + "\"/><a:ext cx=\"" + emu(std::max(1.0, t.w) * k) + "\" cy=\"" +
                      emu(std::max(1.0, t.h) * k) + "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:noFill/></p:spPr>"
                      "<p:txBody><a:bodyPr wrap=\"none\" lIns=\"0\" tIns=\"0\" rIns=\"0\" bIns=\"0\" anchor=\"t\"><a:noAutofit/></a:bodyPr><a:lstStyle/>"
                      "<a:p><a:pPr><a:lnSpc><a:spcPts val=\"" + std::to_string(std::max(100L, std::lround(t.h * s * 100))) + "\"/></a:lnSpc>"
                      "<a:spcBef><a:spcPts val=\"0\"/></a:spcBef><a:spcAft><a:spcPts val=\"0\"/></a:spcAft></a:pPr>";
            for (auto& p : t.pieces) {
                std::string font = XmlEscape(p.font.empty() ? L"Arial" : p.font);
                char color[8];
                snprintf(color, sizeof color, "%06X", unsigned(p.color & 0xFFFFFF));
                long spacing = std::lround(t.spacing * s * 100);   // hundredths of a point
                shapes += "<a:r><a:rPr lang=\"en-US\" sz=\"" + std::to_string(std::max(100L, std::lround(p.size * s * 100))) + "\"" + (p.bold ? " b=\"1\"" : "") +
                          (p.italic ? " i=\"1\"" : "") + (spacing ? " spc=\"" + std::to_string(spacing) + "\"" : "") + " dirty=\"0\"><a:solidFill><a:srgbClr val=\"" + color +
                          "\"/></a:solidFill><a:latin typeface=\"" + font + "\"/><a:cs typeface=\"" + font + "\"/></a:rPr><a:t>" + XmlEscape(p.text) + "</a:t></a:r>";
            }
            shapes += "</a:p></p:txBody></p:sp>";
        }
        std::string slide = pre + "<p:sld " + kPptNs + "><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
            "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>" +
            shapes + "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>";
        std::string rels = pre + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>" +
            picRel + "</Relationships>";
        ok = ok && z.Add("ppt/slides/slide" + n + ".xml", slide) && z.Add("ppt/slides/_rels/slide" + n + ".xml.rels", rels);
        types += "<Override PartName=\"/ppt/slides/slide" + n + ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slide+xml\"/>";
        presRels += "<Relationship Id=\"rId" + std::to_string(i + 10) + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide\" Target=\"slides/slide" + n + ".xml\"/>";
        sldIds += "<p:sldId id=\"" + std::to_string(256 + i) + "\" r:id=\"rId" + std::to_string(i + 10) + "\"/>";
    }
    types += "</Types>";
    presRels += "</Relationships>";
    std::string pres = pre + "<p:presentation " + kPptNs + "><p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" r:id=\"rId1\"/></p:sldMasterIdLst>"
        "<p:sldIdLst>" + sldIds + "</p:sldIdLst><p:sldSz cx=\"" + std::to_string(cx) + "\" cy=\"" + std::to_string(cy) + "\"/><p:notesSz cx=\"6858000\" cy=\"9144000\"/>"
        "<p:defaultTextStyle/></p:presentation>";
    std::string master = pre + "<p:sldMaster " + kPptNs + ">" + EmptyTree() +
        "<p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/>"
        "<p:sldLayoutIdLst><p:sldLayoutId id=\"2147483649\" r:id=\"rId1\"/></p:sldLayoutIdLst>"
        "<p:txStyles><p:titleStyle/><p:bodyStyle/><p:otherStyle/></p:txStyles></p:sldMaster>";   // PowerPoint won't open a file without these
    std::string masterRels = pre + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme\" Target=\"../theme/theme1.xml\"/></Relationships>";
    std::string layout = pre + "<p:sldLayout " + kPptNs + " type=\"blank\" preserve=\"1\">" + EmptyTree() + "<p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>";
    std::string layoutRels = pre + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster\" Target=\"../slideMasters/slideMaster1.xml\"/></Relationships>";
    ok = ok && z.Add("[Content_Types].xml", types) &&
         z.Add("_rels/.rels", pre + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
               "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"ppt/presentation.xml\"/></Relationships>") &&
         z.Add("ppt/presentation.xml", pres) && z.Add("ppt/_rels/presentation.xml.rels", presRels) &&
         z.Add("ppt/slideMasters/slideMaster1.xml", master) && z.Add("ppt/slideMasters/_rels/slideMaster1.xml.rels", masterRels) &&
         z.Add("ppt/slideLayouts/slideLayout1.xml", layout) && z.Add("ppt/slideLayouts/_rels/slideLayout1.xml.rels", layoutRels) &&
         z.Add("ppt/theme/theme1.xml", std::string(kPptTheme));
    if (!ok || !z.Finish(out)) { if (err.empty()) err = L"Could not write the presentation."; return false; }
    return true;
}

}  // namespace rd
