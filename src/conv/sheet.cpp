// Spreadsheets: XLSX, ODS, CSV, TSV and old XLS (libxls) in; XLSX, ODS, CSV and TSV out. Values travel,
// formatting doesn't: text, numbers, true/false and dates. Formulas arrive as their last computed value.
// Every sheet is kept; to CSV/TSV each sheet becomes a file of its own when there are several.
#include "doc.h"

#include <pugixml.hpp>
#include <xls.h>

using namespace xls;   // libxls puts its C API in a namespace when compiled as C++

#include <map>
#include <set>

namespace rd {

namespace {

struct Cell {
    enum Type { Empty, Str, Num, Bool, Date } t = Empty;
    wstring s;
    double n = 0;             // Num, Bool (0/1), Date (Excel serial: days since 1899-12-30)
};
struct Sheet {
    wstring name;
    std::vector<std::vector<Cell>> rows;
    void Put(size_t r, size_t c, Cell v)
    {
        if (rows.size() <= r) rows.resize(r + 1);
        if (rows[r].size() <= c) rows[r].resize(c + 1);
        rows[r][c] = std::move(v);
    }
};
using Book = std::vector<Sheet>;

const char* Local(const char* name) { const char* c = strchr(name, ':'); return c ? c + 1 : name; }
bool Is(const pugi::xml_node& n, const char* local) { return strcmp(Local(n.name()), local) == 0; }
pugi::xml_node Child(const pugi::xml_node& n, const char* local) { for (auto c : n.children()) if (Is(c, local)) return c; return {}; }
const char* Attr(const pugi::xml_node& n, const char* local)
{
    for (auto a : n.attributes()) if (strcmp(Local(a.name()), local) == 0) return a.value();
    return "";
}

/// All the text below a node (rich text runs, ODS paragraphs and spaces).
wstring TextOf(const pugi::xml_node& n)
{
    wstring t;
    for (auto c : n.children()) {
        if (c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata) t += Widen(c.value());
        else if (Is(c, "rPh") || Is(c, "phoneticPr")) continue;   // pronunciation hints in Japanese files
        else if (Is(c, "s")) t += wstring(size_t(std::max(1, atoi(Attr(c, "c")))), L' ');
        else if (Is(c, "tab")) t += L"\t";
        else if (Is(c, "line-break")) t += L"\n";
        else t += TextOf(c);
    }
    return t;
}

// ---------------------------------------------------------------- dates

double SerialFromIso(const std::string& s)
{
    int y = 0, m = 0, d = 0, hh = 0, mm = 0;
    double ss = 0;
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%lf", &y, &m, &d, &hh, &mm, &ss) < 3) return 0;
    // Days from 1899-12-30, via the civil-from-days algorithm.
    auto days = [](int y, int m, int d) {
        y -= m <= 2;
        int era = (y >= 0 ? y : y - 399) / 400;
        int yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    };
    return days(y, m, d) - days(1899, 12, 30) + (hh * 3600 + mm * 60 + ss) / 86400.0;
}

wstring IsoFromSerial(double v)
{
    long long day = (long long)std::floor(v);
    double frac = v - double(day);
    long long z = day - 25569 + 719468;   // 1899-12-30 is 25569 days before 1970-01-01
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int d = int(doy - (153 * mp + 2) / 5 + 1), m = int(mp < 10 ? mp + 3 : mp - 9);
    y += m <= 2;
    wchar_t buf[40];
    int secs = int(std::lround(frac * 86400));
    if (secs >= 86400) secs = 86399;
    if (secs == 0) swprintf(buf, 40, L"%04lld-%02d-%02d", y, m, d);
    else swprintf(buf, 40, L"%04lld-%02d-%02d %02d:%02d:%02d", y, m, d, secs / 3600, secs / 60 % 60, secs % 60);
    return buf;
}

/// Whether an Excel number format shows a date or time.
bool DateFormat(int id, const std::string& code)
{
    if ((id >= 14 && id <= 22) || (id >= 45 && id <= 47)) return true;
    if (code.empty()) return false;
    bool quoted = false, bracket = false;
    for (char c : code) {
        if (c == '"') quoted = !quoted;
        else if (c == '[') bracket = true;
        else if (c == ']') bracket = false;
        else if (!quoted && !bracket && strchr("dDyYhHsS", c)) return true;
    }
    return false;
}

wstring NumberText(double v)
{
    wchar_t buf[40];
    swprintf(buf, 40, L"%.15g", v);
    return buf;
}

wstring CellText(const Cell& c)
{
    switch (c.t) {
        case Cell::Str: return c.s;
        case Cell::Num: return NumberText(c.n);
        case Cell::Bool: return c.n != 0 ? L"TRUE" : L"FALSE";
        case Cell::Date: return IsoFromSerial(c.n);
        default: return L"";
    }
}

/// A CSV field that is plainly a number becomes one ("007" and "1,234" stay text: they mean something else).
Cell Guess(const wstring& s)
{
    Cell c;
    if (s.empty()) return c;
    c.t = Cell::Str;
    c.s = s;
    if (s.size() > 20) return c;
    size_t i = s[0] == L'-' ? 1 : 0;
    if (i >= s.size() || !iswdigit(s[i])) return c;
    if (s[i] == L'0' && i + 1 < s.size() && s[i + 1] != L'.') return c;
    wchar_t* end = nullptr;
    double v = wcstod(s.c_str(), &end);
    if (end && *end == 0 && std::isfinite(v)) { c.t = Cell::Num; c.n = v; }
    return c;
}

// ---------------------------------------------------------------- readers

size_t ColumnOf(const char* ref)
{
    size_t col = 0;
    for (const char* p = ref; *p && isalpha(uint8_t(*p)); p++) col = col * 26 + size_t(toupper(uint8_t(*p)) - 'A' + 1);
    return col ? col - 1 : 0;
}

bool ReadXlsx(const std::vector<uint8_t>& zip, Book& book, wstring& err)
{
    std::string text;
    pugi::xml_document wb, rels, sst, styles;
    if (!ZipEntry(zip, "xl/workbook.xml", text) || !wb.load_string(text.c_str())) { err = L"This isn't an Excel workbook, or it is damaged."; return false; }
    std::map<std::string, std::string> targets;
    if (ZipEntry(zip, "xl/_rels/workbook.xml.rels", text) && rels.load_string(text.c_str()))
        for (auto r : rels.first_child().children()) targets[Attr(r, "Id")] = Attr(r, "Target");
    std::vector<wstring> strings;
    if (ZipEntry(zip, "xl/sharedStrings.xml", text) && sst.load_string(text.c_str()))
        for (auto si : sst.first_child().children()) if (Is(si, "si")) strings.push_back(TextOf(si));
    // Which cell styles are dates.
    std::vector<bool> dateStyle;
    if (ZipEntry(zip, "xl/styles.xml", text) && styles.load_string(text.c_str())) {
        std::map<int, std::string> codes;
        for (auto f : Child(styles.first_child(), "numFmts").children()) codes[atoi(Attr(f, "numFmtId"))] = Attr(f, "formatCode");
        for (auto xf : Child(styles.first_child(), "cellXfs").children()) {
            int id = atoi(Attr(xf, "numFmtId"));
            dateStyle.push_back(DateFormat(id, codes.count(id) ? codes[id] : ""));
        }
    }
    for (auto s : Child(wb.first_child(), "sheets").children()) {
        Sheet sheet;
        sheet.name = Widen(Attr(s, "name"));
        std::string target = targets[Attr(s, "id")];
        std::string path = target.rfind("/", 0) == 0 ? target.substr(1) : "xl/" + target;
        pugi::xml_document ws;
        if (!ZipEntry(zip, path.c_str(), text) || !ws.load_string(text.c_str())) { book.push_back(std::move(sheet)); continue; }
        size_t autoRow = 0;
        for (auto row : Child(ws.first_child(), "sheetData").children()) {
            if (!Is(row, "row")) continue;
            size_t r = *Attr(row, "r") ? size_t(atoi(Attr(row, "r")) - 1) : autoRow;
            autoRow = r + 1;
            size_t autoCol = 0;
            for (auto c : row.children()) {
                if (!Is(c, "c")) continue;
                size_t col = *Attr(c, "r") ? ColumnOf(Attr(c, "r")) : autoCol;
                autoCol = col + 1;
                std::string t = Attr(c, "t");
                pugi::xml_node v = Child(c, "v");
                Cell cell;
                if (t == "s") { size_t i = size_t(atoi(v.text().get())); if (i < strings.size()) { cell.t = Cell::Str; cell.s = strings[i]; } }
                else if (t == "inlineStr") { cell.t = Cell::Str; cell.s = TextOf(Child(c, "is")); }
                else if (t == "str" || t == "e") { cell.t = Cell::Str; cell.s = Widen(v.text().get()); }
                else if (t == "b") { cell.t = Cell::Bool; cell.n = atoi(v.text().get()); }
                else if (t == "d") { cell.t = Cell::Date; cell.n = SerialFromIso(v.text().get()); }
                else if (v && *v.text().get()) {
                    cell.t = Cell::Num;
                    cell.n = strtod(v.text().get(), nullptr);
                    size_t st = size_t(atoi(Attr(c, "s")));
                    if (st < dateStyle.size() && dateStyle[st]) cell.t = Cell::Date;
                }
                if (cell.t != Cell::Empty) sheet.Put(r, col, cell);
            }
        }
        book.push_back(std::move(sheet));
    }
    if (book.empty()) { err = L"This workbook has no sheets."; return false; }
    return true;
}

bool ReadOds(const std::vector<uint8_t>& zip, Book& book, wstring& err)
{
    std::string text;
    pugi::xml_document x;
    if (!ZipEntry(zip, "content.xml", text) || !x.load_string(text.c_str())) { err = L"This isn't an OpenDocument spreadsheet, or it is damaged."; return false; }
    pugi::xml_node ss = Child(Child(x.first_child(), "body"), "spreadsheet");
    for (auto t : ss.children()) {
        if (!Is(t, "table")) continue;
        Sheet sheet;
        sheet.name = Widen(Attr(t, "name"));
        size_t r = 0;
        // Rows and cells repeat (often a million empty rows at the end): only real content is stored.
        std::function<void(const pugi::xml_node&)> rowsIn = [&](const pugi::xml_node& parent) {
            for (auto row : parent.children()) {
                if (Is(row, "table-row-group") || Is(row, "table-header-rows") || Is(row, "table-rows")) { rowsIn(row); continue; }
                if (!Is(row, "table-row")) continue;
                size_t reps = size_t(std::max(1, atoi(Attr(row, "number-rows-repeated"))));
                std::vector<std::pair<size_t, Cell>> cells;
                size_t c = 0;
                for (auto cell : row.children()) {
                    if (!Is(cell, "table-cell") && !Is(cell, "covered-table-cell")) continue;
                    size_t cr = size_t(std::max(1, atoi(Attr(cell, "number-columns-repeated"))));
                    std::string type = Attr(cell, "value-type");
                    Cell v;
                    if (type == "float" || type == "percentage" || type == "currency") { v.t = Cell::Num; v.n = strtod(Attr(cell, "value"), nullptr); }
                    else if (type == "boolean") { v.t = Cell::Bool; v.n = strcmp(Attr(cell, "boolean-value"), "true") == 0; }
                    else if (type == "date") { v.t = Cell::Date; v.n = SerialFromIso(Attr(cell, "date-value")); }
                    else if (type == "time") { v.t = Cell::Str; v.s = Widen(Attr(cell, "time-value")); }
                    else {
                        wstring s;
                        for (auto p : cell.children()) if (Is(p, "p")) s += (s.empty() ? L"" : L"\n") + TextOf(p);
                        if (!s.empty()) { v.t = Cell::Str; v.s = s; }
                    }
                    if (v.t != Cell::Empty) for (size_t k = 0; k < std::min<size_t>(cr, 1024); k++) cells.push_back({ c + k, v });
                    c += cr;
                }
                if (!cells.empty())
                    for (size_t k = 0; k < std::min<size_t>(reps, 100000); k++)
                        for (auto& [col, v] : cells) sheet.Put(r + k, col, v);
                r += reps;
            }
        };
        rowsIn(t);
        book.push_back(std::move(sheet));
    }
    if (book.empty()) { err = L"This spreadsheet has no sheets."; return false; }
    return true;
}

bool ReadDelimited(const std::string& utf8, wchar_t sep, Book& book)
{
    wstring s = Widen(utf8);
    if (sep == 0) {
        // Comma, or semicolon (as Excel writes CSV where the comma is the decimal mark), or tab:
        // whichever the first line has most of, outside quotes.
        int counts[3] = {};
        bool q = false;
        for (wchar_t c : s) {
            if (c == L'"') q = !q;
            else if (!q && (c == L'\n' || c == L'\r')) break;
            else if (!q) counts[0] += c == L',', counts[1] += c == L';', counts[2] += c == L'\t';
        }
        sep = counts[1] > counts[0] && counts[1] >= counts[2] ? L';' : counts[2] > counts[0] ? L'\t' : L',';
    }
    Sheet sheet;
    sheet.name = L"Sheet1";
    std::vector<Cell> row;
    wstring field;
    bool quoted = false, wasQuoted = false;
    auto endField = [&] { row.push_back(wasQuoted ? Cell{ field.empty() ? Cell::Empty : Cell::Str, field, 0 } : Guess(field)); field.clear(); wasQuoted = false; };
    auto endRow = [&] { endField(); sheet.rows.push_back(std::move(row)); row.clear(); };
    for (size_t i = 0; i < s.size(); i++) {
        wchar_t c = s[i];
        if (quoted) {
            if (c == L'"') { if (i + 1 < s.size() && s[i + 1] == L'"') { field += L'"'; i++; } else quoted = false; }
            else field += c;
        } else if (c == L'"' && field.empty() && sep != L'\t') { quoted = wasQuoted = true; }
        else if (c == sep) endField();
        else if (c == L'\r') { if (i + 1 < s.size() && s[i + 1] == L'\n') i++; endRow(); }
        else if (c == L'\n') endRow();
        else field += c;
    }
    if (!field.empty() || !row.empty()) endRow();
    book.push_back(std::move(sheet));
    return true;
}

bool ReadXls(const std::vector<uint8_t>& bytes, Book& book, wstring& err)
{
    xls_error_t e = LIBXLS_OK;
    xlsWorkBook* wb = xls_open_buffer(bytes.data(), bytes.size(), "UTF-8", &e);
    if (!wb) { err = L"This old Excel file can't be read (it may be damaged or password protected)."; return false; }
    if (xls_parseWorkBook(wb) != LIBXLS_OK) { xls_close_WB(wb); err = L"This old Excel file is damaged."; return false; }
    // Which cell formats (XF records) show dates: by built-in id, or by the format's code.
    std::map<int, std::string> codes;
    for (uint32_t f = 0; f < wb->formats.count; f++)
        if (wb->formats.format[f].value) codes[wb->formats.format[f].index] = wb->formats.format[f].value;
    std::vector<bool> dateXf;
    for (uint32_t x = 0; x < wb->xfs.count; x++) {
        int id = wb->xfs.xf[x].format;
        dateXf.push_back(DateFormat(id, codes.count(id) ? codes[id] : ""));
    }
    for (uint32_t i = 0; i < wb->sheets.count; i++) {
        Sheet sheet;
        sheet.name = Widen(wb->sheets.sheet[i].name ? reinterpret_cast<const char*>(wb->sheets.sheet[i].name) : "Sheet");
        xlsWorkSheet* ws = xls_getWorkSheet(wb, int(i));
        if (ws && xls_parseWorkSheet(ws) == LIBXLS_OK) {
            for (uint32_t r = 0; r <= ws->rows.lastrow; r++) {
                xlsRow* row = xls_row(ws, uint16_t(r));
                if (!row) continue;
                for (uint32_t c = 0; c <= ws->rows.lastcol; c++) {
                    xlsCell* cell = xls_cell(ws, uint16_t(r), uint16_t(c));
                    if (!cell || cell->isHidden) continue;
                    Cell v;
                    switch (cell->id) {
                        case XLS_RECORD_NUMBER: case XLS_RECORD_RK: case XLS_RECORD_MULRK: v.t = Cell::Num; v.n = cell->d; break;
                        case XLS_RECORD_BOOLERR: v.t = Cell::Bool; v.n = cell->d; break;
                        case XLS_RECORD_FORMULA: case XLS_RECORD_FORMULA_ALT:
                            if (cell->l == 0) { v.t = Cell::Num; v.n = cell->d; }
                            else if (cell->str) { v.t = Cell::Str; v.s = Widen(cell->str); }
                            break;
                        default:
                            if (cell->str && cell->id != XLS_RECORD_BLANK && cell->id != XLS_RECORD_MULBLANK) { v.t = Cell::Str; v.s = Widen(cell->str); }
                    }
                    if (v.t == Cell::Num && cell->xf < dateXf.size() && dateXf[cell->xf]) v.t = Cell::Date;
                    if (v.t != Cell::Empty) sheet.Put(r, c, v);
                }
            }
        }
        if (ws) xls_close_WS(ws);
        book.push_back(std::move(sheet));
    }
    xls_close_WB(wb);
    if (book.empty()) { err = L"This workbook has no sheets."; return false; }
    return true;
}

// ---------------------------------------------------------------- writers

std::string Delimited(const Sheet& sheet, char sep)
{
    std::string out;
    for (auto& row : sheet.rows) {
        for (size_t c = 0; c < row.size(); c++) {
            if (c) out += sep;
            std::string f = Narrow(CellText(row[c]));
            if (sep == '\t') { for (auto& ch : f) if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' '; out += f; continue; }
            bool quote = f.find_first_of(",;\"\n\r") != std::string::npos || (!f.empty() && (f.front() == ' ' || f.back() == ' '));
            if (quote) {
                std::string q = "\"";
                for (char ch : f) { if (ch == '"') q += '"'; q += ch; }
                out += q + "\"";
            } else out += f;
        }
        out += "\r\n";
    }
    return out;
}

std::string ColumnName(size_t c)
{
    std::string s;
    c++;
    while (c) { s.insert(s.begin(), char('A' + (c - 1) % 26)); c = (c - 1) / 26; }
    return s;
}

/// Excel's rules for sheet names: 31 characters, none of []:*?/\, unique.
std::vector<wstring> SheetNames(const Book& book)
{
    std::vector<wstring> names;
    std::set<wstring> used;
    for (size_t i = 0; i < book.size(); i++) {
        wstring n = book[i].name;
        for (auto& c : n) if (wcschr(L"[]:*?/\\", c)) c = L'_';
        if (n.empty()) n = L"Sheet" + std::to_wstring(i + 1);
        n = n.substr(0, 31);
        wstring base = n;
        for (int k = 2; used.count(n); k++) n = base.substr(0, 28) + L" " + std::to_wstring(k);
        used.insert(n);
        names.push_back(n);
    }
    return names;
}

/// Each column as wide as its longest line of text (dates included), in characters.
std::vector<size_t> ColumnWidths(const Sheet& sheet)
{
    std::vector<size_t> width;
    for (auto& row : sheet.rows)
        for (size_t c = 0; c < row.size(); c++) {
            if (width.size() <= c) width.resize(c + 1, 0);
            wstring t = CellText(row[c]);
            for (size_t a = 0, b; a <= t.size(); a = b + 1) {
                b = t.find(L'\n', a);
                if (b == wstring::npos) b = t.size();
                width[c] = std::max(width[c], b - a);
            }
        }
    for (auto& w : width) w = std::clamp<size_t>(w + 2, 8, 60);
    return width;
}

bool WriteXlsx(const Book& book, std::vector<uint8_t>& out)
{
    auto names = SheetNames(book);
    std::map<wstring, size_t> index;
    std::vector<const wstring*> strings;
    std::string pre = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>";
    ZipOut z;
    std::string sheetsXml, relsXml, typesXml;
    bool ok = true;
    for (size_t i = 0; i < book.size(); i++) {
        std::string n = std::to_string(i + 1);
        std::string data;
        for (size_t r = 0; r < book[i].rows.size(); r++) {
            const auto& row = book[i].rows[r];
            if (row.empty()) continue;
            data += "<row r=\"" + std::to_string(r + 1) + "\">";
            for (size_t c = 0; c < row.size(); c++) {
                const Cell& v = row[c];
                if (v.t == Cell::Empty) continue;
                std::string ref = ColumnName(c) + std::to_string(r + 1);
                if (v.t == Cell::Str) {
                    auto it = index.find(v.s);
                    size_t id;
                    if (it == index.end()) { id = strings.size(); index[v.s] = id; strings.push_back(&index.find(v.s)->first); }
                    else id = it->second;
                    data += "<c r=\"" + ref + "\" t=\"s\"><v>" + std::to_string(id) + "</v></c>";
                } else if (v.t == Cell::Bool) data += "<c r=\"" + ref + "\" t=\"b\"><v>" + (v.n != 0 ? "1" : "0") + "</v></c>";
                else {
                    std::string style = v.t == Cell::Date ? (v.n == std::floor(v.n) ? " s=\"1\"" : " s=\"2\"") : "";
                    data += "<c r=\"" + ref + "\"" + style + "><v>" + Narrow(NumberText(v.n)) + "</v></c>";
                }
            }
            data += "</row>";
        }
        // Columns as wide as their longest value (dates included), so nothing shows as "####".
        std::vector<size_t> width = ColumnWidths(book[i]);
        std::string cols;
        for (size_t c = 0; c < width.size(); c++)
            cols += "<col min=\"" + std::to_string(c + 1) + "\" max=\"" + std::to_string(c + 1) + "\" width=\"" + std::to_string(width[c]) + "\" customWidth=\"1\"/>";
        if (!cols.empty()) cols = "<cols>" + cols + "</cols>";
        ok = ok && z.Add("xl/worksheets/sheet" + n + ".xml", pre + "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">" + cols + "<sheetData>" + data + "</sheetData></worksheet>");
        sheetsXml += "<sheet name=\"" + XmlEscape(names[i]) + "\" sheetId=\"" + n + "\" r:id=\"rId" + std::to_string(i + 10) + "\"/>";
        relsXml += "<Relationship Id=\"rId" + std::to_string(i + 10) + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet" + n + ".xml\"/>";
        typesXml += "<Override PartName=\"/xl/worksheets/sheet" + n + ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>";
    }
    std::string sst = pre + "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" count=\"" + std::to_string(strings.size()) + "\" uniqueCount=\"" + std::to_string(strings.size()) + "\">";
    for (auto* s : strings) sst += "<si><t xml:space=\"preserve\">" + XmlEscape(*s) + "</t></si>";
    sst += "</sst>";
    ok = ok && z.Add("[Content_Types].xml", pre + "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
        "<Override PartName=\"/xl/sharedStrings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml\"/>" + typesXml + "</Types>")
        && z.Add("_rels/.rels", pre + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/></Relationships>")
        && z.Add("xl/workbook.xml", pre + "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets>" + sheetsXml + "</sheets></workbook>")
        && z.Add("xl/_rels/workbook.xml.rels", pre + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings\" Target=\"sharedStrings.xml\"/>" + relsXml + "</Relationships>")
        && z.Add("xl/sharedStrings.xml", sst)
        // Styles: 0 general, 1 a date, 2 a date and time.
        && z.Add("xl/styles.xml", pre + "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        "<numFmts count=\"1\"><numFmt numFmtId=\"164\" formatCode=\"yyyy\\-mm\\-dd hh:mm:ss\"/></numFmts>"
        "<fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts><fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill></fills>"
        "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders><cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
        "<cellXfs count=\"3\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/><xf numFmtId=\"14\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>"
        "<xf numFmtId=\"164\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/></cellXfs></styleSheet>");
    return ok && z.Finish(out);
}

bool WriteOds(const Book& book, std::vector<uint8_t>& out)
{
    auto names = SheetNames(book);
    std::string tables, columnStyles;
    std::set<size_t> widths;
    for (size_t i = 0; i < book.size(); i++) {
        tables += "<table:table table:name=\"" + XmlEscape(names[i]) + "\">";
        auto width = ColumnWidths(book[i]);
        if (width.empty()) width.push_back(8);
        for (size_t w : width) {
            widths.insert(w);
            tables += "<table:table-column table:style-name=\"co" + std::to_string(w) + "\"/>";
        }
        for (auto& row : book[i].rows) {
            tables += "<table:table-row>";
            for (auto& v : row) {
                switch (v.t) {
                    case Cell::Empty: tables += "<table:table-cell/>"; break;
                    case Cell::Str: {
                        tables += "<table:table-cell office:value-type=\"string\">";
                        size_t start = 0;
                        while (true) {
                            size_t nl = v.s.find(L'\n', start);
                            tables += "<text:p>" + XmlEscape(v.s.substr(start, nl == wstring::npos ? wstring::npos : nl - start)) + "</text:p>";
                            if (nl == wstring::npos) break;
                            start = nl + 1;
                        }
                        tables += "</table:table-cell>";
                        break;
                    }
                    case Cell::Num: {
                        std::string n = Narrow(NumberText(v.n));
                        tables += "<table:table-cell office:value-type=\"float\" office:value=\"" + n + "\"><text:p>" + n + "</text:p></table:table-cell>";
                        break;
                    }
                    case Cell::Bool:
                        tables += std::string("<table:table-cell office:value-type=\"boolean\" office:boolean-value=\"") + (v.n != 0 ? "true" : "false") + "\"><text:p>" + (v.n != 0 ? "TRUE" : "FALSE") + "</text:p></table:table-cell>";
                        break;
                    case Cell::Date: {
                        std::string iso = Narrow(IsoFromSerial(v.n));
                        std::string attr = iso.size() > 10 ? iso.substr(0, 10) + "T" + iso.substr(11) : iso;
                        // ce1 / ce2: date and date-time styles, or the cell shows the day number.
                        tables += std::string("<table:table-cell table:style-name=\"") + (iso.size() > 10 ? "ce2" : "ce1") +
                                  "\" office:value-type=\"date\" office:date-value=\"" + attr + "\"><text:p>" + iso + "</text:p></table:table-cell>";
                        break;
                    }
                }
            }
            tables += "</table:table-row>";
        }
        if (book[i].rows.empty()) tables += "<table:table-row><table:table-cell/></table:table-row>";
        tables += "</table:table>";
    }
    for (size_t w : widths)   // about 0.21 cm per character of the default font
        columnStyles += "<style:style style:name=\"co" + std::to_string(w) + "\" style:family=\"table-column\"><style:table-column-properties style:column-width=\"" +
                        std::to_string(w * 21 / 100) + "." + std::to_string(w * 21 % 100 / 10) + "cm\"/></style:style>";
    std::string pre = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
    std::string content = pre + "<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
        "xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" "
        "xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" xmlns:number=\"urn:oasis:names:tc:opendocument:xmlns:datastyle:1.0\" office:version=\"1.2\">"
        "<office:automatic-styles>"
        "<number:date-style style:name=\"N1\"><number:year number:style=\"long\"/><number:text>-</number:text><number:month number:style=\"long\"/>"
        "<number:text>-</number:text><number:day number:style=\"long\"/></number:date-style>"
        "<number:date-style style:name=\"N2\"><number:year number:style=\"long\"/><number:text>-</number:text><number:month number:style=\"long\"/>"
        "<number:text>-</number:text><number:day number:style=\"long\"/><number:text> </number:text><number:hours number:style=\"long\"/><number:text>:</number:text>"
        "<number:minutes number:style=\"long\"/><number:text>:</number:text><number:seconds number:style=\"long\"/></number:date-style>"
        "<style:style style:name=\"ce1\" style:family=\"table-cell\" style:parent-style-name=\"Default\" style:data-style-name=\"N1\"/>"
        "<style:style style:name=\"ce2\" style:family=\"table-cell\" style:parent-style-name=\"Default\" style:data-style-name=\"N2\"/>"
        + columnStyles + "</office:automatic-styles>"
        "<office:body><office:spreadsheet>" + tables + "</office:spreadsheet></office:body></office:document-content>";
    ZipOut z;
    bool ok = z.Add("mimetype", std::string("application/vnd.oasis.opendocument.spreadsheet"), true) &&
              z.Add("META-INF/manifest.xml", pre + "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">"
                    "<manifest:file-entry manifest:full-path=\"/\" manifest:media-type=\"application/vnd.oasis.opendocument.spreadsheet\"/>"
                    "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/></manifest:manifest>") &&
              z.Add("content.xml", content);
    return ok && z.Finish(out);
}

}  // namespace

bool WritePageSheets(const std::vector<PageSheet>& pages, std::vector<uint8_t>& out)
{
    // Table text from a PDF: "1,234.50" and "(12.00)" are numbers there, as an accountant writes them.
    // The format keeps them looking the same: 10.00 stays 10.00, not 10.
    auto number = [](wstring s, std::string& format) {
        bool negative = s.size() > 2 && s.front() == L'(' && s.back() == L')';
        if (negative) s = s.substr(1, s.size() - 2);
        size_t dot = s.find(L'.');
        wstring whole = s.substr(0, dot);
        bool grouped = whole.size() > 4 && whole.find(L',') != wstring::npos;
        for (size_t i = whole.size(), k = 0; grouped && i-- > 0; k++)
            grouped = (k % 4 == 3) ? whole[i] == L',' : iswdigit(whole[i]) != 0;
        if (grouped) { s.erase(std::remove(s.begin(), s.begin() + long(whole.size()), L','), s.begin() + long(whole.size())); }
        Cell c = Guess(s);
        if (c.t == Cell::Num && negative) c.n = -c.n;
        format = std::string(grouped ? "#,##0" : "0") + (dot == wstring::npos ? "" : "." + std::string(s.size() - s.find(L'.') - 1, '0'));
        if (negative) format += ";(" + format + ")";
        return c;
    };
    auto hex = [](uint32_t c) { char b[9]; snprintf(b, sizeof b, "FF%06X", unsigned(c & 0xFFFFFF)); return std::string(b); };
    auto num = [](double v) { char b[32]; snprintf(b, sizeof b, "%.4g", v); return std::string(b); };
    auto fontXml = [&](const PageSheet::Piece& f, const char* nameTag) {
        return std::string(f.bold ? "<b/>" : "") + (f.italic ? "<i/>" : "") + "<sz val=\"" + num(std::round(f.size * 2) / 2) + "\"/><color rgb=\"" + hex(f.color) +
               "\"/><" + nameTag + " val=\"" + XmlEscape(f.font.empty() ? L"Arial" : f.font) + "\"/>";
    };
    // Styles, each written once: index 0 of every list is the plain default.
    std::vector<std::string> fonts{ "<font><sz val=\"11\"/><name val=\"Calibri\"/></font>" };
    std::vector<std::string> fills{ "<fill><patternFill patternType=\"none\"/></fill>", "<fill><patternFill patternType=\"gray125\"/></fill>" };
    std::vector<std::string> borders{ "<border><left/><right/><top/><bottom/><diagonal/></border>" };
    std::vector<std::string> formats, xfs{ "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>" };
    auto id = [](std::vector<std::string>& list, const std::string& x) {
        size_t i = size_t(std::find(list.begin(), list.end(), x) - list.begin());
        if (i == list.size()) list.push_back(x);
        return i;
    };

    std::string pre = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>";
    const std::string rel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
    ZipOut z;
    std::string sheetsXml, relsXml, typesXml;
    bool ok = true;
    size_t media = 0;
    for (size_t i = 0; i < pages.size(); i++) {
        const PageSheet& pg = pages[i];
        std::string n = std::to_string(i + 1);
        // What each cell holds: a piece of text, a border, a fill.
        struct Out { const PageSheet::Text* text = nullptr; PageSheet::Edge edge; uint32_t fill = 0; bool filled = false; };
        std::map<std::pair<size_t, size_t>, Out> cells;
        for (auto& t : pg.texts) cells[{ t.row, t.col }].text = &t;
        for (auto& [at, e] : pg.borders) cells[at].edge = e;
        for (auto& [at, f] : pg.fills) { cells[at].fill = f; cells[at].filled = true; }

        // Room for each piece of text, claimed cell by cell so no two overlap (Excel refuses a workbook whose
        // merged areas overlap, and text can't spill into a cell someone else holds). Text goes first: it
        // spills into the cells it reaches, or, spanning rows, is merged that far. Numbers then: Excel never
        // lets a number spill (it shows ####), so each is merged over its own width plus the free room
        // beside it, left of it when it's right-aligned, up to the next line drawn. A number with no room
        // for its own width is written as text, which can spill.
        std::map<std::pair<size_t, size_t>, std::pair<size_t, size_t>> mergedTo;   // a cell -> the last row and column it covers
        std::set<const PageSheet::Text*> numbers, numbersAsText;
        for (auto& t : pg.texts) { std::string f; if (t.pieces.size() == 1 && number(t.pieces[0].text, f).t == Cell::Num) numbers.insert(&t); }
        std::set<std::pair<size_t, size_t>> taken;
        for (auto& t : pg.texts) taken.insert({ t.row, t.col });
        auto wall = [&](size_t r, size_t c) {   // a line drawn between columns c - 1 and c
            auto a = pg.borders.find({ r, c }), b = pg.borders.find({ r, c - 1 });
            return (a != pg.borders.end() && (a->second.sides & 1)) || (b != pg.borders.end() && (b->second.sides & 2));
        };
        auto claim = [&](const PageSheet::Text& t, bool numbersNow) {
            auto free = [&](size_t r0, size_t r1, size_t c) {
                for (size_t r = r0; r <= r1; r++) if (!(r == t.row && c == t.col) && taken.count({ r, c })) return false;
                return true;
            };
            size_t a = t.col, b = t.col, rows = t.rowEnd;
            for (size_t c = t.col; c <= t.colEnd && rows > t.row; c++) if (!free(t.row, rows, c)) rows = t.row;   // in the way: one row
            while (b < t.colEnd && free(t.row, rows, b + 1)) b++;
            if (numbersNow) {
                if (b < t.colEnd) { numbersAsText.insert(&t); b = t.col; }
                else if (t.right) while (a > 0 && free(t.row, rows, a - 1) && !wall(t.row, a)) a--;
                else while (b + 1 < pg.colW.size() && free(t.row, rows, b + 1) && !wall(t.row, b + 1)) b++;
            }
            for (size_t r = t.row; r <= rows; r++) for (size_t c = a; c <= b; c++) taken.insert({ r, c });
            if (a != t.col) { cells[{ t.row, t.col }].text = nullptr; cells[{ t.row, a }].text = &t; }
            bool merged = rows > t.row || (numbersNow && !numbersAsText.count(&t) && b > a);   // one row of text spills on its own
            if (merged) mergedTo[{ t.row, a }] = { rows, b };
        };
        for (auto& t : pg.texts) if (!numbers.count(&t)) claim(t, false);
        for (auto& t : pg.texts) if (numbers.count(&t)) claim(t, true);

        std::string merges;
        size_t mergeCount = 0;
        std::vector<std::string> rowXml(pg.rowH.size());
        for (auto& [at, o] : cells) {
            if (at.first >= rowXml.size()) continue;
            std::string ref = ColumnName(at.second) + std::to_string(at.first + 1);
            std::string format, value, type;
            const PageSheet::Piece* font = o.text ? &o.text->pieces.front() : nullptr;
            bool right = false, left = false;
            // Excel sets a line a little taller than the page did: a font that would overflow its row(s) is
            // made just small enough to fit.
            double room = 0;
            auto spans = mergedTo.find(at);
            for (size_t r = at.first; r <= (spans != mergedTo.end() ? spans->second.first : at.first) && r < pg.rowH.size(); r++) room += pg.rowH[r];
            auto fit = [&](PageSheet::Piece p) { p.size = std::max(1.0, std::min(p.size, room / 1.2)); return p; };
            if (auto m = mergedTo.find(at); o.text && m != mergedTo.end()) {
                merges += "<mergeCell ref=\"" + ref + ":" + ColumnName(m->second.second) + std::to_string(m->second.first + 1) + "\"/>";
                mergeCount++;
            }
            if (o.text) {
                wstring plain;
                for (auto& piece : o.text->pieces) plain += piece.text;
                Cell c = o.text->pieces.size() == 1 && !numbersAsText.count(o.text) ? number(plain, format) : Cell{};
                right = numbersAsText.count(o.text) && o.text->right;
                if (c.t == Cell::Num) {
                    value = "<v>" + Narrow(NumberText(c.n)) + "</v>";
                    right = o.text->right;
                    left = !right;   // Excel puts numbers on the right unless told
                } else {
                    format.clear();
                    type = " t=\"inlineStr\"";
                    if (o.text->pieces.size() == 1) value = "<is><t xml:space=\"preserve\">" + XmlEscape(plain) + "</t></is>";
                    else {
                        value = "<is>";
                        for (auto& piece : o.text->pieces) value += "<r><rPr>" + fontXml(fit(piece), "rFont") + "</rPr><t xml:space=\"preserve\">" + XmlEscape(piece.text) + "</t></r>";
                        value += "</is>";
                    }
                }
            }
            PageSheet::Piece fitted;
            if (font) { fitted = fit(*font); font = &fitted; }
            size_t fontId = font ? id(fonts, "<font>" + fontXml(*font, "name") + "</font>") : 0;
            size_t fillId = o.filled ? id(fills, "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"" + hex(o.fill) + "\"/><bgColor indexed=\"64\"/></patternFill></fill>") : 0;
            size_t borderId = 0;
            if (o.edge.sides) {
                std::string b = "<border>";
                for (auto [bit, side] : { std::pair<uint8_t, const char*>{ 1, "left" }, { 2, "right" }, { 4, "top" }, { 8, "bottom" } })
                    b += o.edge.sides & bit ? std::string("<") + side + " style=\"thin\"><color rgb=\"" + hex(o.edge.color) + "\"/></" + side + ">" : std::string("<") + side + "/>";
                borderId = id(borders, b + "<diagonal/></border>");
            }
            size_t formatId = format.empty() ? 0 : 164 + id(formats, format);
            size_t xf = id(xfs, "<xf numFmtId=\"" + std::to_string(formatId) + "\" fontId=\"" + std::to_string(fontId) + "\" fillId=\"" + std::to_string(fillId) +
                                "\" borderId=\"" + std::to_string(borderId) + "\" xfId=\"0\" applyNumberFormat=\"1\" applyFont=\"1\" applyFill=\"1\" applyBorder=\"1\" applyAlignment=\"1\">"
                                "<alignment" + (right ? " horizontal=\"right\"" : left ? " horizontal=\"left\"" : "") + " vertical=\"center\"/></xf>");
            rowXml[at.first] += "<c r=\"" + ref + "\" s=\"" + std::to_string(xf) + "\"" + type + (value.empty() ? "/>" : ">" + value + "</c>");
        }
        std::string data;
        for (size_t r = 0; r < rowXml.size(); r++)
            data += "<row r=\"" + std::to_string(r + 1) + "\" ht=\"" + num(pg.rowH[r]) + "\" customHeight=\"1\">" + rowXml[r] + "</row>";
        // Column widths are in characters of the default font, 7 pixels each at 96 DPI: a point is 4/3 pixel.
        std::string cols;
        for (size_t c = 0; c < pg.colW.size(); c++)
            cols += "<col min=\"" + std::to_string(c + 1) + "\" max=\"" + std::to_string(c + 1) + "\" width=\"" + num(pg.colW[c] * 4 / 3 / 7) + "\" customWidth=\"1\"/>";
        if (!cols.empty()) cols = "<cols>" + cols + "</cols>";

        // Pictures, anchored to the cell they start in.
        std::string drawing, drawingRels;
        for (size_t k = 0; k < pg.pictures.size(); k++) {
            const auto& pic = pg.pictures[k];
            std::string file = "image" + std::to_string(++media) + (pic.ext == L".jpg" ? ".jpeg" : ".png");
            ok = ok && z.Add("xl/media/" + file, pic.bytes);   // compressed: Office refuses the stored entries miniz writes
            std::string rid = "rId" + std::to_string(k + 1), pid = std::to_string(k + 2);
            auto emu = [](double pt) { return std::to_string(std::llround(pt * 12700)); };
            drawingRels += "<Relationship Id=\"" + rid + "\" Type=\"" + rel + "/image\" Target=\"../media/" + file + "\"/>";
            drawing += "<xdr:oneCellAnchor><xdr:from><xdr:col>" + std::to_string(pic.col) + "</xdr:col><xdr:colOff>" + emu(pic.dx) + "</xdr:colOff><xdr:row>" +
                       std::to_string(pic.row) + "</xdr:row><xdr:rowOff>" + emu(pic.dy) + "</xdr:rowOff></xdr:from><xdr:ext cx=\"" + emu(pic.w) + "\" cy=\"" + emu(pic.h) + "\"/>"
                       "<xdr:pic><xdr:nvPicPr><xdr:cNvPr id=\"" + pid + "\" name=\"Picture " + pid + "\"/><xdr:cNvPicPr><a:picLocks noChangeAspect=\"1\"/></xdr:cNvPicPr></xdr:nvPicPr>"
                       "<xdr:blipFill><a:blip r:embed=\"" + rid + "\"/><a:stretch><a:fillRect/></a:stretch></xdr:blipFill>"
                       "<xdr:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"" + emu(pic.w) + "\" cy=\"" + emu(pic.h) + "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></xdr:spPr>"
                       "</xdr:pic><xdr:clientData/></xdr:oneCellAnchor>";
        }
        std::string drawingRef;
        if (!drawing.empty()) {
            ok = ok && z.Add("xl/drawings/drawing" + n + ".xml", pre + "<xdr:wsDr xmlns:xdr=\"http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing\" "
                             "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" xmlns:r=\"" + rel + "\">" + drawing + "</xdr:wsDr>")
                 && z.Add("xl/drawings/_rels/drawing" + n + ".xml.rels", pre + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">" + drawingRels + "</Relationships>")
                 && z.Add("xl/worksheets/_rels/sheet" + n + ".xml.rels", pre + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                          "<Relationship Id=\"rId1\" Type=\"" + rel + "/drawing\" Target=\"../drawings/drawing" + n + ".xml\"/></Relationships>");
            typesXml += "<Override PartName=\"/xl/drawings/drawing" + n + ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.drawing+xml\"/>";
            drawingRef = "<drawing r:id=\"rId1\"/>";
        }
        // No gridlines, and printed as one page with no margins on the same paper: it looks like the paper it came from.
        double w = 0, h = 0;
        for (double x : pg.colW) w += x;
        for (double y : pg.rowH) h += y;
        double tall = std::max(w, h), wide = std::min(w, h);
        std::string paper = std::fabs(tall - 842) < 12 && std::fabs(wide - 595) < 12 ? " paperSize=\"9\"" : std::fabs(tall - 792) < 12 ? " paperSize=\"1\"" : "";
        if (w > h) paper += " orientation=\"landscape\"";
        ok = ok && z.Add("xl/worksheets/sheet" + n + ".xml", pre + "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"" + rel + "\">"
                         "<sheetPr><pageSetUpPr fitToPage=\"1\"/></sheetPr><sheetViews><sheetView showGridLines=\"0\" workbookViewId=\"0\"/></sheetViews>" + cols +
                         "<sheetData>" + data + "</sheetData>" + (mergeCount ? "<mergeCells count=\"" + std::to_string(mergeCount) + "\">" + merges + "</mergeCells>" : "") +
                         "<pageMargins left=\"0\" right=\"0\" top=\"0\" bottom=\"0\" header=\"0\" footer=\"0\"/><pageSetup" + paper + " fitToWidth=\"1\" fitToHeight=\"1\"/>" + drawingRef + "</worksheet>");
        sheetsXml += "<sheet name=\"Page " + n + "\" sheetId=\"" + n + "\" r:id=\"rId" + std::to_string(i + 10) + "\"/>";
        relsXml += "<Relationship Id=\"rId" + std::to_string(i + 10) + "\" Type=\"" + rel + "/worksheet\" Target=\"worksheets/sheet" + n + ".xml\"/>";
        typesXml += "<Override PartName=\"/xl/worksheets/sheet" + n + ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>";
    }
    std::string numFmts;
    for (size_t k = 0; k < formats.size(); k++) numFmts += "<numFmt numFmtId=\"" + std::to_string(164 + k) + "\" formatCode=\"" + XmlEscape(Widen(formats[k])) + "\"/>";
    auto list = [](const char* tag, const std::vector<std::string>& items) {
        std::string s = std::string("<") + tag + " count=\"" + std::to_string(items.size()) + "\">";
        for (auto& x : items) s += x;
        return s + "</" + tag + ">";
    };
    ok = ok && z.Add("[Content_Types].xml", pre + "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Default Extension=\"png\" ContentType=\"image/png\"/><Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>"
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>" + typesXml + "</Types>")
        && z.Add("_rels/.rels", pre + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"" + rel + "/officeDocument\" Target=\"xl/workbook.xml\"/></Relationships>")
        && z.Add("xl/workbook.xml", pre + "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"" + rel + "\"><sheets>" + sheetsXml + "</sheets></workbook>")
        && z.Add("xl/_rels/workbook.xml.rels", pre + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"" + rel + "/styles\" Target=\"styles.xml\"/>" + relsXml + "</Relationships>")
        && z.Add("xl/styles.xml", pre + "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">" + (numFmts.empty() ? "" :
        "<numFmts count=\"" + std::to_string(formats.size()) + "\">" + numFmts + "</numFmts>") + list("fonts", fonts) + list("fills", fills) + list("borders", borders) +
        "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>" + list("cellXfs", xfs) + "</styleSheet>");
    return ok && z.Finish(out);
}

bool RunSheetTo(const Job& job, Kind kind, const wstring& target, const Progress& p, Result& r)
{
    const wstring& in = job.inputs[0];
    std::vector<uint8_t> bytes;
    if (!ReadShared(in, bytes, r.error)) return false;
    if (bytes.empty()) { r.error = L"The file is empty."; return false; }
    r.before = bytes.size();
    Book book;
    bool ok;
    switch (kind) {
        case Kind::Xlsx: ok = ReadXlsx(bytes, book, r.error); break;
        case Kind::Ods: ok = ReadOds(bytes, book, r.error); break;
        case Kind::Xls: ok = ReadXls(bytes, book, r.error); break;
        case Kind::Csv: ok = ReadDelimited(TextAsUtf8(bytes), 0, book); break;
        case Kind::Tsv: ok = ReadDelimited(TextAsUtf8(bytes), L'\t', book); break;
        default: r.error = L"This isn't a spreadsheet Roda can read."; return false;
    }
    if (!ok) return false;
    p.Set(0.5);
    wstring base = BaseName(in);
    std::vector<uint8_t> out;
    if (target == L"xlsx") { if (!WriteXlsx(book, out)) { r.error = L"Could not write the workbook."; return false; } }
    else if (target == L"ods") { if (!WriteOds(book, out)) { r.error = L"Could not write the spreadsheet."; return false; } }
    else if (target == L"csv" || target == L"tsv") {
        char sep = target == L"csv" ? ',' : '\t';
        auto bytesOf = [&](const Sheet& s) {
            std::string t = Delimited(s, sep);
            std::vector<uint8_t> b;
            if (sep == ',') b = { 0xEF, 0xBB, 0xBF };   // Excel reads a CSV as UTF-8 only with this mark
            b.insert(b.end(), t.begin(), t.end());
            return b;
        };
        wstring ext = L"." + target;
        if (book.size() == 1) out = bytesOf(book[0]);
        else {
            // One file per sheet, in a folder named after the workbook.
            OutFile o;
            if (!ReserveOutput(job, in, base, L"", true, o, r.error)) return false;
            if (p.part) p.part(o.part);
            auto names = SheetNames(book);
            for (size_t i = 0; i < book.size(); i++) {
                wstring name = names[i];
                for (auto& c : name) if (wcschr(L"<>:\"/\\|?*", c)) c = L'_';
                auto b = bytesOf(book[i]);
                if (!WriteAllBytes(LongPath(JoinPath(o.part, base + L" - " + name + ext)), b.data(), b.size())) {
                    AbandonOutput(o);
                    r.error = L"Could not write the sheets (is the disk full?).";
                    return false;
                }
            }
            if (!CommitOutput(o, r.error)) { AbandonOutput(o); return false; }
            r.out = o.final;
            return true;
        }
    } else { r.error = L"Roda can't write that spreadsheet format."; return false; }
    return WriteOutput(job, in, base, L"." + target, out, p, r);
}

}  // namespace rd
