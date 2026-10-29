// Documents: one simple model every reader fills and every writer reads, so N formats need N readers
// and N writers rather than N x N converters. Headings, paragraphs (with bold, italic, code and links),
// lists, quotes, code blocks, tables, pictures and page breaks: what survives a trip between DOCX,
// Markdown, HTML, RTF, plain text, EPUB and a PDF's text.
#pragma once
#include "conv.h"

namespace rd {

struct Span {
    wstring text;                 // may contain '\n': a line break inside the paragraph
    bool bold = false, italic = false, code = false;
    wstring link;                 // a URL, or empty
};

struct Block {
    enum Type { Para, Heading, ListItem, Quote, Code, Table, Picture, Rule, PageBreak } type = Para;
    int level = 0;                // heading 1..6; list nesting 0..
    bool ordered = false;         // list item: numbered
    std::vector<Span> spans;      // Para, Heading, ListItem, Quote
    wstring code;                 // Code
    std::vector<std::vector<wstring>> rows;   // Table: first row is the header
    std::vector<uint8_t> image;   // Picture: PNG or JPEG bytes
    wstring imageExt;             // ".png" / ".jpg"
    int imgW = 0, imgH = 0;       // pixels
    wstring alt;
};

struct Doc {
    wstring title;
    std::vector<Block> blocks;

    Block& Add(Block::Type t, int level = 0) { blocks.push_back({}); blocks.back().type = t; blocks.back().level = level; return blocks.back(); }
    /// Adds plain text to the last block (merging into its last span when the styles match).
    void Text(const wstring& t, bool bold = false, bool italic = false, bool code = false, const wstring& link = L"");
};

/// Plain text of a block's spans.
wstring PlainText(const Block& b);

// docread.cpp
bool ReadDocx(const std::vector<uint8_t>& bytes, Doc& doc, wstring& err);
bool ReadMarkdown(const std::string& utf8, Doc& doc, wstring& err);
bool ReadHtml(const std::string& utf8, Doc& doc, wstring& err);
bool ReadRtf(const std::string& bytes, Doc& doc, wstring& err);
bool ReadText(const std::string& utf8, Doc& doc);
/// Bytes of a text file as UTF-8 (BOMs and UTF-16 handled; other text taken as the PC's code page).
std::string TextAsUtf8(const std::vector<uint8_t>& bytes);

// docwrite.cpp
std::string WriteHtml(const Doc& doc);
std::string WriteMarkdown(const Doc& doc);
std::string WriteText(const Doc& doc);
bool WriteDocx(const Doc& doc, std::vector<uint8_t>& out);
bool WriteEpub(const Doc& doc, std::vector<uint8_t>& out);
/// A PDF page for a DOCX that looks like it: the page's drawing (all but its text) as a picture behind,
/// and each piece of text typed over it where it was. Points, y down from the page's top.
struct LaidPage {
    double w = 595, h = 842;
    // spacing: points after each letter; stretch: letters this many times their width, instead of the spacing (Word only)
    struct Text { double x = 0, y = 0, h = 0, spacing = 0; std::vector<PageSheet::Piece> pieces; double w = 0, stretch = 1; };
    std::vector<Text> texts;
    std::vector<uint8_t> drawing;   // PNG of the page without its text; empty when nothing else is on it
};
bool WriteLaidDocx(const std::vector<LaidPage>& pages, const wstring& title, std::vector<uint8_t>& out);
/// A presentation with a slide per page, laid out like it: the page's drawing behind (locked), its text
/// in text boxes over it where it was.
bool WritePptx(const std::vector<LaidPage>& pages, std::vector<uint8_t>& out, wstring& err);

/// XML text escaping.
std::string XmlEscape(const wstring& s);

// A ZIP built in memory (DOCX, EPUB, PPTX, XLSX and ODS are all ZIPs).
class ZipOut {
public:
    ZipOut();
    ~ZipOut();
    bool Add(const std::string& name, const std::string& data, bool store = false);
    bool Add(const std::string& name, const std::vector<uint8_t>& data, bool store = false);
    bool Finish(std::vector<uint8_t>& out);
private:
    struct Impl;
    std::unique_ptr<Impl> z_;
};

/// One file out of a ZIP held in memory; false if it isn't there.
bool ZipEntry(const std::vector<uint8_t>& zip, const char* name, std::string& out);

}  // namespace rd
