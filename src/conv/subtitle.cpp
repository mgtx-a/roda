// Subtitles: SRT and WebVTT into each other, and either into plain text (just what is said).
#include "doc.h"

namespace rd {

namespace {

struct Cue {
    double start = 0, end = 0;   // seconds
    wstring text;                // lines joined with '\n'; tags kept (both formats know <i> and <b>)
};

bool ParseTime(const wstring& s, double& out)
{
    // "01:02:03,456", "01:02:03.456" or "02:03.456" (WebVTT may leave out the hours).
    int h = 0, m = 0, sec = 0, ms = 0;
    wchar_t sep;
    if (swscanf(s.c_str(), L"%d:%d:%d%lc%d", &h, &m, &sec, &sep, &ms) == 5) {}
    else if (swscanf(s.c_str(), L"%d:%d%lc%d", &m, &sec, &sep, &ms) == 4) h = 0;
    else return false;
    out = h * 3600.0 + m * 60.0 + sec + ms / 1000.0;
    return true;
}

wstring FormatTime(double t, wchar_t sep)
{
    long long ms = std::llround(std::max(0.0, t) * 1000);
    wchar_t buf[32];
    swprintf(buf, 32, L"%02lld:%02lld:%02lld%lc%03lld", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, sep, ms % 1000);
    return buf;
}

std::vector<Cue> Parse(const std::string& utf8, bool vtt)
{
    std::vector<Cue> cues;
    wstring all = Widen(utf8);
    std::vector<wstring> lines;
    size_t i = 0;
    while (i <= all.size()) {
        size_t nl = all.find(L'\n', i);
        wstring l = all.substr(i, nl == wstring::npos ? wstring::npos : nl - i);
        if (!l.empty() && l.back() == L'\r') l.pop_back();
        lines.push_back(l);
        if (nl == wstring::npos) break;
        i = nl + 1;
    }
    for (size_t k = 0; k < lines.size(); k++) {
        size_t arrow = lines[k].find(L"-->");
        if (arrow == wstring::npos) continue;
        Cue c;
        wstring a = lines[k].substr(0, arrow), b = lines[k].substr(arrow + 3);
        a.erase(0, a.find_first_not_of(L" \t"));
        b.erase(0, b.find_first_not_of(L" \t"));
        size_t sp = b.find_first_of(L" \t");   // WebVTT cue settings after the end time
        if (sp != wstring::npos) b = b.substr(0, sp);
        if (!ParseTime(a, c.start) || !ParseTime(b, c.end)) continue;
        for (k++; k < lines.size() && !lines[k].empty(); k++) c.text += (c.text.empty() ? L"" : L"\n") + lines[k];
        cues.push_back(std::move(c));
    }
    (void)vtt;
    return cues;
}

wstring WithoutTags(const wstring& t)
{
    wstring out;
    bool tag = false;
    for (wchar_t c : t) {
        if (c == L'<') tag = true;
        else if (c == L'>' && tag) tag = false;
        else if (!tag) out += c;
    }
    // A few entities WebVTT uses.
    for (auto [e, ch] : { std::pair<const wchar_t*, const wchar_t*>{ L"&amp;", L"&" }, { L"&lt;", L"<" }, { L"&gt;", L">" }, { L"&nbsp;", L" " } }) {
        size_t p;
        while ((p = out.find(e)) != wstring::npos) out.replace(p, wcslen(e), ch);
    }
    return out;
}

}  // namespace

bool RunSubtitleTo(const Job& job, Kind kind, const wstring& target, const Progress& p, Result& r)
{
    const wstring& in = job.inputs[0];
    std::vector<uint8_t> bytes;
    if (!ReadShared(in, bytes, r.error)) return false;
    r.before = bytes.size();
    auto cues = Parse(TextAsUtf8(bytes), kind == Kind::Vtt);
    if (cues.empty()) { r.error = L"No subtitles were found in this file."; return false; }
    std::string out;
    if (target == L"srt") {
        for (size_t i = 0; i < cues.size(); i++) {
            // Cue settings are dropped (SRT has none); <i> and <b> mean the same in both.
            out += std::to_string(i + 1) + "\r\n" + Narrow(FormatTime(cues[i].start, L',') + L" --> " + FormatTime(cues[i].end, L',')) + "\r\n";
            wstring t = cues[i].text;
            if (kind == Kind::Vtt)
                for (auto [e, ch] : { std::pair<const wchar_t*, const wchar_t*>{ L"&lt;", L"<" }, { L"&gt;", L">" }, { L"&nbsp;", L"\u00A0" }, { L"&amp;", L"&" } }) {
                    size_t at;
                    while ((at = t.find(e)) != wstring::npos) t.replace(at, wcslen(e), ch);
                }
            for (char ch : Narrow(t)) { if (ch == '\n') out += '\r'; out += ch; }
            out += "\r\n\r\n";
        }
    } else if (target == L"vtt") {
        out = "WEBVTT\n\n";
        for (auto& c : cues) {
            std::string body = Narrow(c.text);
            // "-->" inside the text would end the cue early in WebVTT.
            size_t a;
            while ((a = body.find("-->")) != std::string::npos) body.replace(a, 3, "->");
            out += Narrow(FormatTime(c.start, L'.') + L" --> " + FormatTime(c.end, L'.')) + "\n" + body + "\n\n";
        }
    } else if (target == L"txt") {
        out = "\xEF\xBB\xBF";
        for (auto& c : cues) {
            wstring t = WithoutTags(c.text);
            for (auto& ch : t) if (ch == L'\n') ch = L' ';
            out += Narrow(t) + "\r\n";
        }
    } else { r.error = L"Roda can't write that subtitle format."; return false; }
    p.Set(0.9);
    std::vector<uint8_t> data(out.begin(), out.end());
    return WriteOutput(job, in, BaseName(in), L"." + target, data, p, r);
}

}  // namespace rd
