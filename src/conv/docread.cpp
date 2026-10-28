// Readers for the document model: DOCX (pugixml), Markdown (md4c), HTML, RTF and plain text (our own,
// tolerant parsers: real-world files are rarely tidy).
#include "doc.h"

#include <md4c.h>
#include <pugixml.hpp>

#include <map>

namespace rd {

// ---------------------------------------------------------------- text encodings

std::string TextAsUtf8(const std::vector<uint8_t>& b)
{
    if (b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) return std::string(b.begin() + 3, b.end());
    if (b.size() >= 2 && ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF))) {
        bool be = b[0] == 0xFE;
        wstring w;
        for (size_t i = 2; i + 1 < b.size(); i += 2) w += wchar_t(be ? (b[i] << 8 | b[i + 1]) : (b[i] | b[i + 1] << 8));
        return Narrow(w);
    }
    std::string s(b.begin(), b.end());
    if (s.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0) > 0) return s;
    // Not UTF-8: an older file in this PC's own code page.
    int n = MultiByteToWideChar(CP_ACP, 0, s.data(), int(s.size()), nullptr, 0);
    wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.data(), int(s.size()), w.data(), n);
    return Narrow(w);
}

// ---------------------------------------------------------------- plain text

bool ReadText(const std::string& utf8, Doc& doc)
{
    // Paragraphs are separated by blank lines; single line breaks inside one are kept.
    wstring all = Widen(utf8);
    wstring para;
    auto flush = [&] {
        while (!para.empty() && para.back() == L'\n') para.pop_back();
        if (!para.empty()) { doc.Add(Block::Para); doc.Text(para); }
        para.clear();
    };
    size_t i = 0;
    while (i <= all.size()) {
        size_t nl = all.find(L'\n', i);
        wstring line = all.substr(i, nl == wstring::npos ? wstring::npos : nl - i);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty() && line[0] == L'\f') { flush(); doc.Add(Block::PageBreak); line.erase(0, 1); }
        if (line.find_first_not_of(L" \t") == wstring::npos) flush();
        else para += line + L"\n";
        if (nl == wstring::npos) break;
        i = nl + 1;
    }
    flush();
    return true;
}

// ---------------------------------------------------------------- DOCX

namespace {

const char* Local(const char* name)
{
    const char* c = strchr(name, ':');
    return c ? c + 1 : name;
}

bool Is(const pugi::xml_node& n, const char* local) { return strcmp(Local(n.name()), local) == 0; }

pugi::xml_node Child(const pugi::xml_node& n, const char* local)
{
    for (auto c : n.children()) if (Is(c, local)) return c;
    return {};
}

const char* Attr(const pugi::xml_node& n, const char* local)
{
    for (auto a : n.attributes()) if (strcmp(Local(a.name()), local) == 0) return a.value();
    return "";
}

bool On(const pugi::xml_node& prop)
{
    if (!prop) return false;
    std::string v = Attr(prop, "val");
    return v.empty() || (v != "0" && v != "false" && v != "none");
}

struct DocxReader {
    const std::vector<uint8_t>& zip;
    Doc& doc;
    std::map<std::string, std::string> rels;                  // id -> target
    std::map<std::string, std::pair<std::string, int>> styles; // id -> (name, outline level or -1)
    std::map<std::string, std::pair<std::string, int>> styleNum; // id -> (numId, level): lists set by the style
    std::map<std::string, std::map<int, bool>> ordered;       // numId -> level -> numbered?

    DocxReader(const std::vector<uint8_t>& z, Doc& d) : zip(z), doc(d) {}

    void LoadSide()
    {
        std::string text;
        pugi::xml_document x;
        if (ZipEntry(zip, "word/_rels/document.xml.rels", text) && x.load_string(text.c_str()))
            for (auto r : x.first_child().children()) rels[Attr(r, "Id")] = Attr(r, "Target");
        if (ZipEntry(zip, "word/styles.xml", text) && x.load_string(text.c_str()))
            for (auto s : x.first_child().children()) {
                if (!Is(s, "style")) continue;
                std::string name = Attr(Child(s, "name"), "val");
                for (auto& c : name) c = char(tolower(uint8_t(c)));
                auto ol = Child(Child(s, "pPr"), "outlineLvl");
                styles[Attr(s, "styleId")] = { name, ol ? atoi(Attr(ol, "val")) : -1 };
                auto num = Child(Child(s, "pPr"), "numPr");
                if (num) styleNum[Attr(s, "styleId")] = { Attr(Child(num, "numId"), "val"), atoi(Attr(Child(num, "ilvl"), "val")) };
            }
        if (ZipEntry(zip, "word/numbering.xml", text) && x.load_string(text.c_str())) {
            std::map<std::string, std::map<int, bool>> abstracts;
            for (auto a : x.first_child().children()) {
                if (!Is(a, "abstractNum")) continue;
                for (auto lv : a.children())
                    if (Is(lv, "lvl")) abstracts[Attr(a, "abstractNumId")][atoi(Attr(lv, "ilvl"))] = strcmp(Attr(Child(lv, "numFmt"), "val"), "bullet") != 0;
            }
            for (auto n : x.first_child().children())
                if (Is(n, "num")) ordered[Attr(n, "numId")] = abstracts[Attr(Child(n, "abstractNumId"), "val")];
        }
    }

    int HeadingLevel(const std::string& styleId, const pugi::xml_node& ppr)
    {
        auto ol = Child(ppr, "outlineLvl");
        if (ol && atoi(Attr(ol, "val")) < 9) return atoi(Attr(ol, "val")) + 1;
        auto it = styles.find(styleId);
        if (it == styles.end()) return 0;
        const std::string& name = it->second.first;
        if (name == "title") return 1;
        if (name.rfind("heading ", 0) == 0) return std::clamp(atoi(name.c_str() + 8), 1, 6);
        if (it->second.second >= 0 && it->second.second < 9) return it->second.second + 1;
        return 0;
    }

    void Picture(const pugi::xml_node& drawing)
    {
        // a:blip r:embed, anywhere below the drawing; the size from wp:extent (EMUs).
        pugi::xml_node blip, extent, docPr;
        std::function<void(const pugi::xml_node&)> find = [&](const pugi::xml_node& n) {
            for (auto c : n.children()) {
                if (Is(c, "blip") && !blip) blip = c;
                if (Is(c, "extent") && !extent) extent = c;
                if (Is(c, "docPr") && !docPr) docPr = c;
                find(c);
            }
        };
        find(drawing);
        if (!blip) return;
        auto it = rels.find(Attr(blip, "embed"));
        if (it == rels.end()) return;
        std::string path = it->second;
        if (path.rfind("/", 0) == 0) path = path.substr(1); else path = "word/" + path;
        std::string data;
        if (!ZipEntry(zip, path.c_str(), data)) return;
        Block b;
        b.type = Block::Picture;
        b.image.assign(data.begin(), data.end());
        Kind k = KindFromBytes(b.image.data(), b.image.size(), Widen(path));
        if (k == Kind::Jpeg) b.imageExt = L".jpg";
        else if (k == Kind::Png) b.imageExt = L".png";
        else return;   // EMF, WMF and friends: no portable way to show them
        b.imgW = int(atoll(Attr(extent, "cx")) / 9525);
        b.imgH = int(atoll(Attr(extent, "cy")) / 9525);
        b.alt = Widen(Attr(docPr, "descr"));
        doc.blocks.push_back(std::move(b));
    }

    /// Runs of a paragraph (or hyperlink, or field), into the last block.
    void Runs(const pugi::xml_node& parent, const wstring& link, std::vector<Block>& pictures, bool& pageBreak)
    {
        for (auto n : parent.children()) {
            if (Is(n, "hyperlink")) {
                std::string id = Attr(n, "id");
                auto it = rels.find(id);
                wstring url = it != rels.end() ? Widen(it->second) : L"";
                std::string anchor = Attr(n, "anchor");
                Runs(n, url.empty() && !anchor.empty() ? L"" : url, pictures, pageBreak);
                continue;
            }
            if (Is(n, "fldSimple") || Is(n, "smartTag") || Is(n, "ins") || Is(n, "customXml") || Is(n, "sdt") || Is(n, "sdtContent")) { Runs(n, link, pictures, pageBreak); continue; }
            if (!Is(n, "r")) continue;
            auto rpr = Child(n, "rPr");
            bool bold = On(Child(rpr, "b")), italic = On(Child(rpr, "i"));
            std::string rstyle = Attr(Child(rpr, "rStyle"), "val");
            for (auto& c : rstyle) c = char(tolower(uint8_t(c)));
            bool code = rstyle.find("code") != std::string::npos;
            for (auto c : n.children()) {
                if (Is(c, "t")) doc.Text(Widen(c.text().get()), bold, italic, code, link);
                else if (Is(c, "tab")) doc.Text(L"\t", bold, italic, code, link);
                else if (Is(c, "br") || Is(c, "cr")) {
                    if (strcmp(Attr(c, "type"), "page") == 0) pageBreak = true;
                    else doc.Text(L"\n", bold, italic, code, link);
                } else if (Is(c, "drawing") || Is(c, "pict")) {
                    Doc tmp;
                    std::swap(tmp.blocks, doc.blocks);
                    Picture(c);
                    std::swap(tmp.blocks, doc.blocks);
                    for (auto& b : tmp.blocks) pictures.push_back(std::move(b));
                } else if (Is(c, "noBreakHyphen")) doc.Text(L"-", bold, italic, code, link);
            }
        }
    }

    void Paragraph(const pugi::xml_node& p)
    {
        auto ppr = Child(p, "pPr");
        std::string styleId = Attr(Child(ppr, "pStyle"), "val");
        std::string styleName = styles.count(styleId) ? styles[styleId].first : "";
        int heading = HeadingLevel(styleId, ppr);
        auto num = Child(ppr, "numPr");
        Block::Type type = Block::Para;
        int level = 0;
        bool numbered = false;
        // A list item by its own numbering, or by its style's ("List Bullet", "List Number").
        std::string numId = num ? Attr(Child(num, "numId"), "val") : "";
        int ilvl = num ? atoi(Attr(Child(num, "ilvl"), "val")) : 0;
        if (!num && styleNum.count(styleId)) { numId = styleNum[styleId].first; ilvl = styleNum[styleId].second; }
        if (heading) { type = Block::Heading; level = heading; }
        else if (!numId.empty() && numId != "0") {
            type = Block::ListItem;
            level = ilvl;
            numbered = ordered[numId][level];
        } else if (styleName.find("quote") != std::string::npos) type = Block::Quote;
        bool code = styleName.find("code") != std::string::npos || styleName.find("preformatted") != std::string::npos;

        size_t before = doc.blocks.size();
        doc.Add(type, level).ordered = numbered;
        std::vector<Block> pictures;
        bool pageBreak = false;
        Runs(p, L"", pictures, pageBreak);
        Block& b = doc.blocks[before];
        if (code) {
            // Consecutive code paragraphs make one code block.
            wstring line = PlainText(b);
            doc.blocks.pop_back();
            if (before > 0 && doc.blocks.back().type == Block::Code) doc.blocks.back().code += L"\n" + line;
            else { doc.Add(Block::Code).code = line; }
        } else if (b.spans.empty() && type == Block::Para) {
            doc.blocks.pop_back();   // empty paragraphs are only spacing
        }
        for (auto& pic : pictures) doc.blocks.push_back(std::move(pic));
        if (pageBreak) doc.Add(Block::PageBreak);
    }

    wstring CellText(const pugi::xml_node& tc)
    {
        wstring t;
        std::function<void(const pugi::xml_node&)> walk = [&](const pugi::xml_node& n) {
            for (auto c : n.children()) {
                if (Is(c, "t")) t += Widen(c.text().get());
                else if (Is(c, "tab")) t += L"\t";
                else if (Is(c, "p") && !t.empty()) { t += L"\n"; walk(c); }
                else walk(c);
            }
        };
        walk(tc);
        return t;
    }

    void Table(const pugi::xml_node& tbl)
    {
        Block& b = doc.Add(Block::Table);
        for (auto tr : tbl.children()) {
            if (!Is(tr, "tr")) continue;
            std::vector<wstring> row;
            for (auto tc : tr.children()) if (Is(tc, "tc")) row.push_back(CellText(tc));
            b.rows.push_back(std::move(row));
        }
        if (b.rows.empty()) doc.blocks.pop_back();
    }

    void Body(const pugi::xml_node& body)
    {
        for (auto n : body.children()) {
            if (Is(n, "p")) Paragraph(n);
            else if (Is(n, "tbl")) Table(n);
            else if (Is(n, "sdt")) Body(Child(n, "sdtContent"));
            else if (Is(n, "customXml")) Body(n);
        }
    }
};

}  // namespace

bool ReadDocx(const std::vector<uint8_t>& bytes, Doc& doc, wstring& err)
{
    std::string text;
    if (!ZipEntry(bytes, "word/document.xml", text)) { err = L"This isn't a Word document, or it is damaged."; return false; }
    pugi::xml_document x;
    if (!x.load_string(text.c_str())) { err = L"This Word document is damaged."; return false; }
    DocxReader r(bytes, doc);
    r.LoadSide();
    pugi::xml_node body = Child(x.first_child(), "body");
    if (!body) { err = L"This Word document has no text."; return false; }
    r.Body(body);
    std::string core;
    pugi::xml_document cx;
    if (ZipEntry(bytes, "docProps/core.xml", core) && cx.load_string(core.c_str())) {
        auto t = Child(cx.first_child(), "title");
        if (t && *t.text().get()) doc.title = Widen(t.text().get());
    }
    return true;
}

// ---------------------------------------------------------------- Markdown (md4c)

namespace {

struct MdState {
    Doc* doc;
    std::vector<bool> lists;         // ordered?
    int bold = 0, italic = 0, code = 0, quote = 0;
    std::vector<wstring> links;
    bool inCode = false, inItem = false, inTable = false, inCell = false;
    std::vector<wstring> row;
    wstring cell;
};

wstring Entity(const std::string& e)
{
    // Latin-1's named entities in code point order (160..255), then common typography.
    static const char* latin1[] = { "nbsp", "iexcl", "cent", "pound", "curren", "yen", "brvbar", "sect", "uml", "copy", "ordf", "laquo", "not", "shy",
        "reg", "macr", "deg", "plusmn", "sup2", "sup3", "acute", "micro", "para", "middot", "cedil", "sup1", "ordm", "raquo", "frac14", "frac12",
        "frac34", "iquest", "Agrave", "Aacute", "Acirc", "Atilde", "Auml", "Aring", "AElig", "Ccedil", "Egrave", "Eacute", "Ecirc", "Euml", "Igrave",
        "Iacute", "Icirc", "Iuml", "ETH", "Ntilde", "Ograve", "Oacute", "Ocirc", "Otilde", "Ouml", "times", "Oslash", "Ugrave", "Uacute", "Ucirc",
        "Uuml", "Yacute", "THORN", "szlig", "agrave", "aacute", "acirc", "atilde", "auml", "aring", "aelig", "ccedil", "egrave", "eacute", "ecirc",
        "euml", "igrave", "iacute", "icirc", "iuml", "eth", "ntilde", "ograve", "oacute", "ocirc", "otilde", "ouml", "divide", "oslash", "ugrave",
        "uacute", "ucirc", "uuml", "yacute", "thorn", "yuml" };
    static const std::pair<const char*, wchar_t> extra[] = { { "euro", 0x20AC }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "ldquo", 0x201C },
        { "rdquo", 0x201D }, { "bull", 0x2022 }, { "trade", 0x2122 }, { "larr", 0x2190 }, { "rarr", 0x2192 }, { "minus", 0x2212 }, { "times", 0xD7 } };
    if (e.size() > 2 && e[1] != '#') {
        std::string name = e.substr(1, e.size() - 2);
        for (size_t i = 0; i < sizeof latin1 / sizeof latin1[0]; i++) if (name == latin1[i]) return wstring(1, wchar_t(160 + i));
        for (auto& [n, c] : extra) if (name == n) return wstring(1, c);
    }
    if (e == "&amp;") return L"&";
    if (e == "&lt;") return L"<";
    if (e == "&gt;") return L">";
    if (e == "&quot;") return L"\"";
    if (e == "&apos;") return L"'";
    if (e == "&nbsp;") return L" ";
    if (e == "&copy;") return L"©";
    if (e == "&mdash;") return L"—";
    if (e == "&ndash;") return L"–";
    if (e == "&hellip;") return L"…";
    if (e.size() > 3 && e[1] == '#') {
        unsigned long cp = e[2] == 'x' || e[2] == 'X' ? strtoul(e.c_str() + 3, nullptr, 16) : strtoul(e.c_str() + 2, nullptr, 10);
        if (cp && cp < 0x110000) {
            if (cp < 0x10000) return wstring(1, wchar_t(cp));
            cp -= 0x10000;
            return { wchar_t(0xD800 + (cp >> 10)), wchar_t(0xDC00 + (cp & 0x3FF)) };
        }
    }
    return Widen(e);
}

int MdEnterBlock(MD_BLOCKTYPE t, void* detail, void* user)
{
    auto* s = static_cast<MdState*>(user);
    Doc& d = *s->doc;
    switch (t) {
        case MD_BLOCK_H: d.Add(Block::Heading, int(static_cast<MD_BLOCK_H_DETAIL*>(detail)->level)); break;
        case MD_BLOCK_P:
            // Loose list items wrap their text in a paragraph: it still belongs to the item.
            if (!(s->inItem && !d.blocks.empty() && d.blocks.back().type == Block::ListItem && d.blocks.back().spans.empty()) && !s->inCell)
                d.Add(s->quote ? Block::Quote : Block::Para);
            break;
        case MD_BLOCK_UL: s->lists.push_back(false); break;
        case MD_BLOCK_OL: s->lists.push_back(true); break;
        case MD_BLOCK_LI: d.Add(Block::ListItem, int(s->lists.size()) - 1).ordered = !s->lists.empty() && s->lists.back(); s->inItem = true; break;
        case MD_BLOCK_QUOTE: s->quote++; break;
        case MD_BLOCK_CODE: d.Add(Block::Code); s->inCode = true; break;
        case MD_BLOCK_HR: d.Add(Block::Rule); break;
        case MD_BLOCK_TABLE: d.Add(Block::Table); s->inTable = true; break;
        case MD_BLOCK_TR: s->row.clear(); break;
        case MD_BLOCK_TH: case MD_BLOCK_TD: s->cell.clear(); s->inCell = true; break;
        default: break;
    }
    return 0;
}

int MdLeaveBlock(MD_BLOCKTYPE t, void*, void* user)
{
    auto* s = static_cast<MdState*>(user);
    Doc& d = *s->doc;
    switch (t) {
        case MD_BLOCK_UL: case MD_BLOCK_OL: if (!s->lists.empty()) s->lists.pop_back(); break;
        case MD_BLOCK_LI: s->inItem = false; break;
        case MD_BLOCK_QUOTE: s->quote--; break;
        case MD_BLOCK_CODE:
            s->inCode = false;
            if (!d.blocks.empty() && !d.blocks.back().code.empty() && d.blocks.back().code.back() == L'\n') d.blocks.back().code.pop_back();
            break;
        case MD_BLOCK_TH: case MD_BLOCK_TD: s->row.push_back(s->cell); s->inCell = false; break;
        case MD_BLOCK_TR: if (!d.blocks.empty() && d.blocks.back().type == Block::Table) d.blocks.back().rows.push_back(s->row); break;
        case MD_BLOCK_TABLE: s->inTable = false; break;
        default: break;
    }
    return 0;
}

int MdEnterSpan(MD_SPANTYPE t, void* detail, void* user)
{
    auto* s = static_cast<MdState*>(user);
    switch (t) {
        case MD_SPAN_EM: s->italic++; break;
        case MD_SPAN_STRONG: s->bold++; break;
        case MD_SPAN_CODE: s->code++; break;
        case MD_SPAN_A: {
            auto* a = static_cast<MD_SPAN_A_DETAIL*>(detail);
            s->links.push_back(Widen(std::string_view(a->href.text, a->href.size)));
            break;
        }
        default: break;
    }
    return 0;
}

int MdLeaveSpan(MD_SPANTYPE t, void*, void* user)
{
    auto* s = static_cast<MdState*>(user);
    switch (t) {
        case MD_SPAN_EM: s->italic--; break;
        case MD_SPAN_STRONG: s->bold--; break;
        case MD_SPAN_CODE: s->code--; break;
        case MD_SPAN_A: if (!s->links.empty()) s->links.pop_back(); break;
        default: break;
    }
    return 0;
}

int MdText(MD_TEXTTYPE t, const MD_CHAR* text, MD_SIZE size, void* user)
{
    auto* s = static_cast<MdState*>(user);
    Doc& d = *s->doc;
    wstring w;
    switch (t) {
        case MD_TEXT_NORMAL: case MD_TEXT_CODE: case MD_TEXT_LATEXMATH: w = Widen(std::string_view(text, size)); break;
        case MD_TEXT_ENTITY: w = Entity(std::string(text, size)); break;
        case MD_TEXT_BR: w = s->inCode ? L"" : L"\n"; break;
        case MD_TEXT_SOFTBR: w = L" "; break;
        case MD_TEXT_NULLCHAR: w = L"�"; break;
        default: return 0;   // raw HTML is left out
    }
    if (s->inCode && !d.blocks.empty()) { d.blocks.back().code += w; return 0; }
    if (s->inCell) { s->cell += w; return 0; }
    d.Text(w, s->bold > 0, s->italic > 0, s->code > 0, s->links.empty() ? L"" : s->links.back());
    return 0;
}

}  // namespace

bool ReadMarkdown(const std::string& utf8, Doc& doc, wstring& err)
{
    MdState s;
    s.doc = &doc;
    MD_PARSER p{};
    p.flags = MD_DIALECT_GITHUB;
    p.enter_block = MdEnterBlock;
    p.leave_block = MdLeaveBlock;
    p.enter_span = MdEnterSpan;
    p.leave_span = MdLeaveSpan;
    p.text = MdText;
    if (md_parse(utf8.data(), MD_SIZE(utf8.size()), &p, &s) != 0) { err = L"This Markdown file can't be read."; return false; }
    // Empty paragraphs (from blocks md4c reports but that held only raw HTML) go.
    doc.blocks.erase(std::remove_if(doc.blocks.begin(), doc.blocks.end(), [](const Block& b) {
        return (b.type == Block::Para || b.type == Block::Quote) && b.spans.empty();
    }), doc.blocks.end());
    for (auto& b : doc.blocks) if (b.type == Block::Heading) { doc.title = PlainText(b); break; }
    return true;
}

// ---------------------------------------------------------------- HTML

namespace {

std::vector<uint8_t> Unbase64(const std::string& s)
{
    std::vector<uint8_t> out;
    uint32_t v = 0;
    int bits = 0;
    for (char c : s) {
        int d = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (d < 0) continue;
        v = v << 6 | uint32_t(d);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(uint8_t(v >> bits)); }
    }
    return out;
}

wstring DecodeEntities(const std::string& s)
{
    std::string out;
    wstring w;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '&') {
            size_t semi = s.find(';', i);
            if (semi != std::string::npos && semi - i < 12) {
                w += Widen(out);
                out.clear();
                w += Entity(s.substr(i, semi - i + 1));
                i = semi + 1;
                continue;
            }
        }
        out += s[i++];
    }
    return w + Widen(out);
}

struct HtmlReader {
    Doc& doc;
    int bold = 0, italic = 0, code = 0, pre = 0, quote = 0, skip = 0;
    std::vector<wstring> links;
    std::vector<bool> lists;
    bool open = false;               // a text block is open
    bool inTitle = false, inCell = false;
    std::vector<wstring> row;
    wstring cell;

    explicit HtmlReader(Doc& d) : doc(d) {}

    void Close() { open = false; }

    void Ensure()
    {
        if (open) return;
        doc.Add(quote ? Block::Quote : Block::Para);
        open = true;
    }

    void Text(std::string raw)
    {
        if (skip) return;
        wstring t = DecodeEntities(raw);
        if (inTitle) { doc.title += t; return; }
        if (pre) { if (!doc.blocks.empty() && doc.blocks.back().type == Block::Code) doc.blocks.back().code += t; return; }
        // Outside <pre>, runs of whitespace are one space.
        wstring c;
        for (wchar_t ch : t) {
            bool ws = ch == L' ' || ch == L'\n' || ch == L'\r' || ch == L'\t';
            if (ws) { if (c.empty() || c.back() != L' ') c += L' '; }
            else c += ch;
        }
        if (inCell) { cell += c; return; }
        if (!open) {
            size_t a = c.find_first_not_of(L' ');
            if (a == wstring::npos) return;
            c = c.substr(a);
        }
        Ensure();
        doc.Text(c, bold > 0, italic > 0, code > 0, links.empty() ? L"" : links.back());
    }

    static std::string AttrOf(const std::string& tag, const char* name)
    {
        std::string lower = tag;
        for (auto& ch : lower) ch = char(tolower(uint8_t(ch)));
        size_t at = 0;
        std::string key = name;
        while ((at = lower.find(key, at)) != std::string::npos) {
            size_t j = at + key.size();
            bool boundary = at > 0 && (isspace(uint8_t(lower[at - 1])));
            while (j < tag.size() && isspace(uint8_t(tag[j]))) j++;
            if (boundary && j < tag.size() && tag[j] == '=') {
                j++;
                while (j < tag.size() && isspace(uint8_t(tag[j]))) j++;
                if (j < tag.size() && (tag[j] == '"' || tag[j] == '\'')) {
                    char q = tag[j];
                    size_t e = tag.find(q, j + 1);
                    return tag.substr(j + 1, e == std::string::npos ? std::string::npos : e - j - 1);
                }
                size_t e = j;
                while (e < tag.size() && !isspace(uint8_t(tag[e])) && tag[e] != '>') e++;
                return tag.substr(j, e - j);
            }
            at += key.size();
        }
        return {};
    }

    void Tag(const std::string& tag)
    {
        bool closing = tag.size() > 1 && tag[1] == '/';
        size_t s = closing ? 2 : 1, e = s;
        while (e < tag.size() && (isalnum(uint8_t(tag[e])))) e++;
        std::string name = tag.substr(s, e - s);
        for (auto& c : name) c = char(tolower(uint8_t(c)));
        if (name == "script" || name == "style" || name == "noscript" || name == "template" || name == "svg") { skip += closing ? -1 : 1; skip = std::max(0, skip); return; }
        if (skip) return;
        if (name == "title" && !doc.title.size()) { inTitle = !closing; return; }
        if (name == "title") { inTitle = false; return; }
        if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
            Close();
            if (!closing) { doc.Add(Block::Heading, name[1] - '0'); open = true; }
            else Close();
            return;
        }
        if (name == "p" || name == "div" || name == "section" || name == "article" || name == "header" || name == "footer" || name == "main" ||
            name == "nav" || name == "aside" || name == "figure" || name == "figcaption" || name == "dt" || name == "dd" || name == "address") { Close(); return; }
        if (name == "br") { if (inCell) cell += L"\n"; else if (open) doc.Text(L"\n", bold > 0, italic > 0, code > 0); return; }
        if (name == "b" || name == "strong") { bold += closing ? -1 : 1; return; }
        if (name == "i" || name == "em" || name == "cite") { italic += closing ? -1 : 1; return; }
        if (name == "code" || name == "kbd" || name == "samp" || name == "tt") { if (!pre) code += closing ? -1 : 1; return; }
        if (name == "a") {
            if (closing) { if (!links.empty()) links.pop_back(); }
            else links.push_back(DecodeEntities(AttrOf(tag, "href")));
            return;
        }
        if (name == "ul" || name == "ol") { Close(); if (closing) { if (!lists.empty()) lists.pop_back(); } else lists.push_back(name == "ol"); return; }
        if (name == "li") {
            Close();
            if (!closing) { doc.Add(Block::ListItem, std::max(0, int(lists.size()) - 1)).ordered = !lists.empty() && lists.back(); open = true; }
            return;
        }
        if (name == "blockquote") { Close(); quote += closing ? -1 : 1; quote = std::max(0, quote); return; }
        if (name == "pre") {
            Close();
            if (!closing) { doc.Add(Block::Code); pre++; }
            else if (pre) {
                pre--;
                auto& c = doc.blocks.back().code;
                while (!c.empty() && (c.back() == L'\n' || c.back() == L'\r')) c.pop_back();
                if (!c.empty() && c[0] == L'\n') c.erase(0, 1);
            }
            return;
        }
        if (name == "hr") { Close(); doc.Add(Block::Rule); return; }
        if (name == "table") { Close(); if (!closing) doc.Add(Block::Table); return; }
        if (name == "tr") {
            if (!closing) row.clear();
            else if (!doc.blocks.empty() && doc.blocks.back().type == Block::Table && !row.empty()) doc.blocks.back().rows.push_back(row);
            return;
        }
        if (name == "td" || name == "th") {
            if (!closing) { cell.clear(); inCell = true; }
            else if (inCell) {
                size_t a = cell.find_first_not_of(L' '), z = cell.find_last_not_of(L' ');
                row.push_back(a == wstring::npos ? L"" : cell.substr(a, z - a + 1));
                inCell = false;
            }
            return;
        }
        if (name == "img" && !closing) {
            std::string src = AttrOf(tag, "src");
            wstring alt = DecodeEntities(AttrOf(tag, "alt"));
            if (src.rfind("data:image/", 0) == 0 && src.find(";base64,") != std::string::npos) {
                Close();
                Block& b = doc.Add(Block::Picture);
                b.image = Unbase64(src.substr(src.find(";base64,") + 8));
                Kind k = KindFromBytes(b.image.data(), b.image.size(), L"");
                b.imageExt = k == Kind::Jpeg ? L".jpg" : L".png";
                if (k != Kind::Jpeg && k != Kind::Png) { doc.blocks.pop_back(); return; }
                b.alt = alt;
                b.imgW = atoi(AttrOf(tag, "width").c_str());
                b.imgH = atoi(AttrOf(tag, "height").c_str());
            } else if (!alt.empty()) {
                Ensure();
                doc.Text(L"[" + alt + L"]", false, true);
            }
        }
    }

    void Parse(const std::string& h)
    {
        size_t i = 0, text = 0;
        while (i < h.size()) {
            if (h[i] != '<') { i++; continue; }
            if (h.compare(i, 4, "<!--") == 0) {
                Text(h.substr(text, i - text));
                size_t e = h.find("-->", i + 4);
                i = text = e == std::string::npos ? h.size() : e + 3;
                continue;
            }
            bool tagStart = i + 1 < h.size() && (isalpha(uint8_t(h[i + 1])) || h[i + 1] == '/' || h[i + 1] == '!' || h[i + 1] == '?');
            if (!tagStart) { i++; continue; }
            Text(h.substr(text, i - text));
            // The tag ends at the first '>' outside quotes.
            size_t j = i + 1;
            char q = 0;
            while (j < h.size()) {
                if (q) { if (h[j] == q) q = 0; }
                else if (h[j] == '"' || h[j] == '\'') q = h[j];
                else if (h[j] == '>') break;
                j++;
            }
            std::string tag = h.substr(i, j - i + 1);
            if (h[i + 1] != '!' && h[i + 1] != '?') Tag(tag);
            i = text = j + 1;
            // Inside script/style everything up to the closing tag is skipped as text anyway.
        }
        Text(h.substr(std::min(text, h.size())));
    }
};

}  // namespace

bool ReadHtml(const std::string& utf8, Doc& doc, wstring& err)
{
    (void)err;
    HtmlReader r(doc);
    r.Parse(utf8);
    // Trim the ends of paragraphs and drop empty ones.
    for (auto& b : doc.blocks) {
        if (b.spans.empty()) continue;
        auto& first = b.spans.front().text;
        size_t a = first.find_first_not_of(L' ');
        first.erase(0, a == wstring::npos ? first.size() : a);
        auto& last = b.spans.back().text;
        while (!last.empty() && last.back() == L' ') last.pop_back();
    }
    doc.blocks.erase(std::remove_if(doc.blocks.begin(), doc.blocks.end(), [](const Block& b) {
        return (b.type == Block::Para || b.type == Block::Quote || b.type == Block::Heading) && PlainText(b).empty();
    }), doc.blocks.end());
    size_t a = doc.title.find_first_not_of(L' ');
    doc.title = a == wstring::npos ? L"" : doc.title.substr(a);
    return true;
}

// ---------------------------------------------------------------- RTF

namespace {

struct RtfState {
    bool bold = false, italic = false;
    int skip = 0;              // inside a destination we don't show
    int uc = 1;                // characters to skip after \uN
    bool fldinst = false, listtext = false, pict = false;
    std::string pictKind;
};

}  // namespace

bool ReadRtf(const std::string& r, Doc& doc, wstring& err)
{
    if (r.compare(0, 5, "{\\rtf") != 0) { err = L"This isn't an RTF file."; return false; }
    std::vector<RtfState> stack(1);
    UINT codepage = 1252;
    std::string bytes;            // \'hh bytes waiting for the code page
    std::string hex;              // picture data
    int pendingSkip = 0;
    int outline = -1;             // \outlinelevelN of this paragraph
    bool listItem = false, numbered = false, inTable = false;
    std::vector<std::vector<wstring>> rows;
    std::vector<wstring> row;
    wstring cell;
    bool ignorable = false;

    auto flushBytes = [&] {
        if (bytes.empty()) return;
        int n = MultiByteToWideChar(codepage, 0, bytes.data(), int(bytes.size()), nullptr, 0);
        wstring w(size_t(n), L'\0');
        MultiByteToWideChar(codepage, 0, bytes.data(), int(bytes.size()), w.data(), n);
        bytes.clear();
        RtfState& s = stack.back();
        if (s.skip || s.fldinst) return;
        if (s.listtext) { for (wchar_t c : w) if (iswdigit(c)) numbered = true; return; }
        if (inTable) cell += w;
        else doc.Text(w, s.bold, s.italic);
    };
    auto put = [&](const wstring& w) {
        flushBytes();
        RtfState& s = stack.back();
        if (s.skip || s.fldinst) return;
        if (s.listtext) { for (wchar_t c : w) if (iswdigit(c)) numbered = true; return; }
        if (inTable) { cell += w; return; }
        if (doc.blocks.empty()) doc.Add(Block::Para);
        doc.Text(w, s.bold, s.italic);
    };
    auto endPara = [&] {
        flushBytes();
        if (doc.blocks.empty()) doc.Add(Block::Para);
        Block& b = doc.blocks.back();
        if (outline >= 0 && outline < 6) { b.type = Block::Heading; b.level = outline + 1; }
        else if (listItem) { b.type = Block::ListItem; b.ordered = numbered; }
        doc.Add(Block::Para);
        outline = -1;
        listItem = numbered = false;
    };

    size_t i = 0;
    while (i < r.size()) {
        char c = r[i];
        if (pendingSkip > 0 && c != '{' && c != '}') {
            // Fallback characters after a \u: skip one (a \'hh counts as one).
            if (c == '\\' && i + 1 < r.size() && r[i + 1] == '\'') i += 4; else if (c == '\\') { i++; while (i < r.size() && isalpha(uint8_t(r[i]))) i++; if (i < r.size() && (r[i] == '-' || isdigit(uint8_t(r[i])))) { i++; while (i < r.size() && isdigit(uint8_t(r[i]))) i++; } if (i < r.size() && r[i] == ' ') i++; }
            else i++;
            pendingSkip--;
            continue;
        }
        if (c == '{') { flushBytes(); stack.push_back(stack.back()); stack.back().pict = false; i++; ignorable = false; continue; }
        if (c == '}') {
            flushBytes();
            if (stack.back().pict && !hex.empty()) {
                Block p;
                p.type = Block::Picture;
                for (size_t k = 0; k + 1 < hex.size(); k += 2) p.image.push_back(uint8_t(strtoul(hex.substr(k, 2).c_str(), nullptr, 16)));
                p.imageExt = stack.back().pictKind == "jpeg" ? L".jpg" : L".png";
                if (stack.back().pictKind == "png" || stack.back().pictKind == "jpeg") { doc.blocks.push_back(std::move(p)); doc.Add(Block::Para); }
                hex.clear();
            }
            if (stack.size() > 1) stack.pop_back();
            i++;
            continue;
        }
        if (c == '\r' || c == '\n') { i++; continue; }
        if (c != '\\') {
            if (stack.back().pict) { if (isxdigit(uint8_t(c))) hex += c; i++; continue; }
            if (stack.back().skip) { i++; continue; }
            size_t j = i;
            while (j < r.size() && r[j] != '\\' && r[j] != '{' && r[j] != '}' && r[j] != '\r' && r[j] != '\n') j++;
            bytes += r.substr(i, j - i);
            i = j;
            continue;
        }
        // A control word or symbol.
        i++;
        if (i >= r.size()) break;
        char n = r[i];
        if (n == '\'') { if (i + 2 < r.size()) bytes += char(strtoul(r.substr(i + 1, 2).c_str(), nullptr, 16)); i += 3; continue; }
        if (n == '*') { ignorable = true; i++; continue; }
        if (!isalpha(uint8_t(n))) {
            if (n == '\\' || n == '{' || n == '}') bytes += n;
            else if (n == '~') put(L" ");
            else if (n == '_') put(L"‑");
            else if (n == '\r' || n == '\n') endPara();
            i++;
            continue;
        }
        size_t s = i;
        while (i < r.size() && isalpha(uint8_t(r[i]))) i++;
        std::string word = r.substr(s, i - s);
        bool hasNum = false;
        long num = 0;
        if (i < r.size() && (r[i] == '-' || isdigit(uint8_t(r[i])))) {
            size_t ns = i;
            i++;
            while (i < r.size() && isdigit(uint8_t(r[i]))) i++;
            num = strtol(r.substr(ns, i - ns).c_str(), nullptr, 10);
            hasNum = true;
        }
        if (i < r.size() && r[i] == ' ') i++;
        RtfState& st = stack.back();
        if (word == "par") endPara();
        else if (word == "line") put(L"\n");
        else if (word == "tab") put(L"\t");
        else if (word == "page") { endPara(); doc.blocks.back().type = Block::PageBreak; doc.Add(Block::Para); }
        else if (word == "b") { flushBytes(); st.bold = !hasNum || num != 0; }
        else if (word == "i") { flushBytes(); st.italic = !hasNum || num != 0; }
        else if (word == "plain") { flushBytes(); st.bold = st.italic = false; }
        else if (word == "pard") { outline = -1; listItem = false; }
        else if (word == "outlinelevel") outline = int(num);
        else if (word == "ls" || word == "ilvl") listItem = true;
        else if (word == "listtext" || word == "pntext") { st.listtext = true; listItem = true; }
        else if (word == "ansicpg") codepage = UINT(num);
        else if (word == "uc") st.uc = int(num);
        else if (word == "u") { flushBytes(); long v = num < 0 ? num + 65536 : num; put(wstring(1, wchar_t(v))); pendingSkip = st.uc; }
        else if (word == "emdash") put(L"—");
        else if (word == "endash") put(L"–");
        else if (word == "bullet") put(L"•");
        else if (word == "lquote") put(L"‘");
        else if (word == "rquote") put(L"’");
        else if (word == "ldblquote") put(L"“");
        else if (word == "rdblquote") put(L"”");
        else if (word == "fldinst") st.fldinst = true;
        else if (word == "pict") { st.pict = true; st.skip = 0; }
        else if (word == "pngblip") st.pictKind = "png";
        else if (word == "jpegblip") st.pictKind = "jpeg";
        else if (word == "intbl") inTable = true;
        else if (word == "cell") { flushBytes(); row.push_back(cell); cell.clear(); }
        else if (word == "row") {
            flushBytes();
            if (!row.empty()) rows.push_back(row);
            row.clear();
            inTable = false;
            // The table ends when the next paragraph isn't in one; collect rows until then.
            if (!(doc.blocks.size() && doc.blocks.back().type == Block::Table)) { doc.Add(Block::Table); }
            doc.blocks.back().rows = rows;
        } else if (word == "fonttbl" || word == "colortbl" || word == "stylesheet" || word == "info" || word == "header" || word == "footer" ||
                   word == "headerl" || word == "headerr" || word == "footerl" || word == "footerr" || word == "footnote" || word == "listtable" ||
                   word == "listoverridetable" || word == "rsidtbl" || word == "generator" || word == "themedata" || word == "colorschememapping" ||
                   word == "latentstyles" || word == "datastore" || word == "xmlnstbl" || word == "pgdsctbl" || word == "object" || word == "nonshppict" ||
                   word == "shppict" || ignorable) {
            if (word == "shppict") { ignorable = false; continue; }   // the picture inside is the one to keep
            st.skip = 1;
        }
        if (word != "pard" && word != "ls" && rows.size() && !inTable && word == "par") rows.clear();
        ignorable = false;
    }
    flushBytes();
    // Empty paragraphs (the last one, spacing) go.
    doc.blocks.erase(std::remove_if(doc.blocks.begin(), doc.blocks.end(), [](const Block& b) {
        return (b.type == Block::Para || b.type == Block::Heading || b.type == Block::ListItem) && PlainText(b).find_first_not_of(L" \t\n") == wstring::npos;
    }), doc.blocks.end());
    return true;
}

}  // namespace rd
