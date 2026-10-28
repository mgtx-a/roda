#include "json.h"

namespace rd {

const Json& Json::operator[](const wchar_t* key) const
{
    static const Json none;
    if (type != Type::Object) return none;
    auto it = obj.find(key);
    return it == obj.end() ? none : it->second;
}

double Json::Num(double fallback) const
{
    if (type == Type::Number) return num;
    if (type == Type::String && !str.empty()) return wcstod(str.c_str(), nullptr);   // options arrive as text from the CLI
    return fallback;
}

static void DumpString(std::string& out, const wstring& s)
{
    out += '"';
    std::string u = Narrow(s);
    for (unsigned char c : u) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                else out += char(c);   // UTF-8 passes through
        }
    }
    out += '"';
}

static void DumpTo(std::string& out, const Json& j)
{
    switch (j.type) {
        case Json::Type::Null: out += "null"; break;
        case Json::Type::Bool: out += j.b ? "true" : "false"; break;
        case Json::Type::Number: {
            char b[40];
            if (j.num == std::floor(j.num) && std::fabs(j.num) < 1e15) snprintf(b, sizeof b, "%lld", (long long)j.num);
            else snprintf(b, sizeof b, "%.6g", j.num);
            out += b;
            break;
        }
        case Json::Type::String: DumpString(out, j.str); break;
        case Json::Type::Array: {
            out += '[';
            for (size_t i = 0; i < j.arr.size(); i++) { if (i) out += ','; DumpTo(out, j.arr[i]); }
            out += ']';
            break;
        }
        case Json::Type::Object: {
            out += '{';
            bool first = true;
            for (auto& [k, v] : j.obj) {
                if (!first) out += ',';
                first = false;
                DumpString(out, k);
                out += ':';
                DumpTo(out, v);
            }
            out += '}';
            break;
        }
    }
}

std::string Json::Dump() const
{
    std::string s;
    DumpTo(s, *this);
    return s;
}

namespace {

struct Reader {
    std::string_view t;
    size_t i = 0;
    int depth = 0;

    void Skip() { while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) i++; }

    bool String(wstring& out)
    {
        if (i >= t.size() || t[i] != '"') return false;
        i++;
        std::string u;
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c != '\\') { u += c; continue; }
            if (i >= t.size()) return false;
            char e = t[i++];
            switch (e) {
                case 'n': u += '\n'; break;
                case 'r': u += '\r'; break;
                case 't': u += '\t'; break;
                case 'b': u += '\b'; break;
                case 'f': u += '\f'; break;
                case 'u': {
                    if (i + 4 > t.size()) return false;
                    unsigned cp = unsigned(strtoul(std::string(t.substr(i, 4)).c_str(), nullptr, 16));
                    i += 4;
                    // Surrogate pairs come as two escapes.
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                        unsigned lo = unsigned(strtoul(std::string(t.substr(i + 2, 4)).c_str(), nullptr, 16));
                        i += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    if (cp < 0x80) u += char(cp);
                    else if (cp < 0x800) { u += char(0xC0 | (cp >> 6)); u += char(0x80 | (cp & 63)); }
                    else if (cp < 0x10000) { u += char(0xE0 | (cp >> 12)); u += char(0x80 | ((cp >> 6) & 63)); u += char(0x80 | (cp & 63)); }
                    else { u += char(0xF0 | (cp >> 18)); u += char(0x80 | ((cp >> 12) & 63)); u += char(0x80 | ((cp >> 6) & 63)); u += char(0x80 | (cp & 63)); }
                    break;
                }
                default: u += e;
            }
        }
        if (i >= t.size()) return false;
        i++;
        out = Widen(u);
        return true;
    }

    bool Value(Json& out)
    {
        if (++depth > 64) return false;
        Skip();
        if (i >= t.size()) return false;
        char c = t[i];
        bool ok = true;
        if (c == '{') {
            out = Json::Object();
            i++;
            Skip();
            if (i < t.size() && t[i] == '}') { i++; depth--; return true; }
            for (;;) {
                Skip();
                wstring key;
                if (!String(key)) return false;
                Skip();
                if (i >= t.size() || t[i] != ':') return false;
                i++;
                Json v;
                if (!Value(v)) return false;
                out.obj[key] = std::move(v);
                Skip();
                if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == '}') { i++; break; }
                return false;
            }
        } else if (c == '[') {
            out = Json::Array();
            i++;
            Skip();
            if (i < t.size() && t[i] == ']') { i++; depth--; return true; }
            for (;;) {
                Json v;
                if (!Value(v)) return false;
                out.arr.push_back(std::move(v));
                Skip();
                if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == ']') { i++; break; }
                return false;
            }
        } else if (c == '"') {
            out.type = Json::Type::String;
            ok = String(out.str);
        } else if (t.substr(i, 4) == "true") { out = Json(true); i += 4; }
        else if (t.substr(i, 5) == "false") { out = Json(false); i += 5; }
        else if (t.substr(i, 4) == "null") { out = Json(); i += 4; }
        else {
            char* end = nullptr;
            std::string num(t.substr(i, std::min<size_t>(40, t.size() - i)));
            double v = strtod(num.c_str(), &end);
            if (end == num.c_str()) return false;
            i += size_t(end - num.c_str());
            out = Json(v);
        }
        depth--;
        return ok;
    }
};

}  // namespace

bool Json::Parse(std::string_view text, Json& out)
{
    Reader r{ text };
    if (!r.Value(out)) return false;
    r.Skip();
    return r.i == text.size();
}

}  // namespace rd
