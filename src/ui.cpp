#include "ui.h"

namespace rd {

const Theme& CurrentTheme()
{
    // Our own colours: a deep teal accent on pale (or dark) glass.
    static const Theme light{ 0xF4FAFBFC, 0x2410202A, 0x1A10202A, 0xFF1F7A74, 0xFFFFFFFF, 0xFF1D2430, 0xFF6A7480,
                              0xFFE8EEF0, 0xFFF3D9D9, 0xFFB3261E, 0xFFE4EAEC, 0.28f };
    static const Theme dark{ 0xF2252A30, 0x30FFFFFF, 0x26FFFFFF, 0xFF2E9E96, 0xFFFFFFFF, 0xFFE8EBEE, 0xFF9AA3AD,
                             0xFF1B1F24, 0xFF4A2A2C, 0xFFF2B8B5, 0xFF343A41, 0.50f };
    return DarkMode() ? dark : light;
}

uint32_t WithAlpha(uint32_t argb, double factor)
{
    uint32_t a = uint32_t(std::clamp(((argb >> 24) & 255) * factor, 0.0, 255.0) + 0.5);
    return (argb & 0x00FFFFFF) | (a << 24);
}

namespace {

/// Path data on a 24-unit grid: M x y, L x y, C x1 y1 x2 y2 x y, Z, and O cx cy r (a circle).
const char* IconPath(Icon i)
{
    switch (i) {
        case Icon::Compress: return "M12 2 L12 9 M8.5 5.5 L12 9 L15.5 5.5 M12 22 L12 15 M8.5 18.5 L12 15 L15.5 18.5 M4 12 L20 12";
        case Icon::Resize: return "M5 19 L19 5 M13 5 L19 5 L19 11 M5 13 L5 19 L11 19";
        case Icon::Crop: return "M7 2 L7 17 L22 17 M2 7 L17 7 L17 22";
        case Icon::Rotate: return "M20 12 C20 16.4 16.4 20 12 20 C7.6 20 4 16.4 4 12 C4 7.6 7.6 4 12 4 C14.8 4 17.2 5.4 18.6 7.6 M19 2.5 L19 8 L13.5 8";
        case Icon::Flip: return "M12 2 L12 22 M9 6 L3 18 L9 18 Z M15 6 L21 18 L15 18 Z";
        case Icon::Strip: return "O12 12 8.5 M12 11 L12 16.5 M12 7.5 L12 8 M4 20 L20 4";
        case Icon::Merge: return "M8 3 L16.5 3 L20 6.5 L20 17 L8 17 Z M4 7 L4 21 L16 21";
        case Icon::Extract: return "M4 11 L4 20 L20 20 L20 11 M12 3 L12 15 M8 7 L12 3 L16 7";
        case Icon::Copy: return "M9 9 L20 9 L20 20 L9 20 Z M5 15 L4 15 L4 4 L15 4 L15 5";
        case Icon::Move: return "M3 6 L9 6 L11 8.5 L21 8.5 L21 19 L3 19 Z M8 14 L16 14 M13 11 L16 14 L13 17";
        case Icon::Folder: return "M3 6 L9 6 L11 8.5 L21 8.5 L21 19 L3 19 Z";
        case Icon::Path: return "M10 14 L14 10 M9.5 6.5 L11 5 C13 3 16 3 18 5 C20 7 20 10 18 12 L16.5 13.5 M14.5 17.5 L13 19 C11 21 8 21 6 19 C4 17 4 14 6 12 L7.5 10.5";
        case Icon::Terminal: return "M3 5 L21 5 L21 19 L3 19 Z M7 9.5 L10 12 L7 14.5 M12.5 15 L17 15";
        case Icon::Float: return "M9 3 L15 3 M10 3 L10 9 L7 13 L17 13 L14 9 L14 3 M12 13 L12 21";
        case Icon::Share: return "O18 5 2.5 O6 12 2.5 O18 19 2.5 M8.2 10.8 L15.8 6.2 M8.2 13.2 L15.8 17.8";
        case Icon::Split: return "M7 3 L14 3 L17 6 L17 11 L7 11 Z M7 14 L17 14 L17 21 L7 21 Z M3 12.5 L21 12.5";
        case Icon::Images: return "M3 5 L21 5 L21 19 L3 19 Z M3 16 L9 10 L14 15 L17 12 L21 16 O16 9 1.6";
        case Icon::Trim: return "O6 6 3 O6 18 3 M8.5 7.5 L20 18 M8.5 16.5 L20 6";
        case Icon::Mute: return "M3 9 L7 9 L12 5 L12 19 L7 15 L3 15 Z M16 9 L21 15 M21 9 L16 15";
        case Icon::Audio: return "M9 18 L9 5 L20 3 L20 16 O6.5 18 2.5 O17.5 16 2.5";
        case Icon::Film: return "M3 5 L21 5 L21 19 L3 19 Z M7 5 L7 19 M17 5 L17 19 M3 9 L7 9 M3 15 L7 15 M17 9 L21 9 M17 15 L21 15";
        case Icon::Sign: return "M3 19 C6 13 8 11 9 13 C10 15 8 18 10 18 C12 18 13 14 15 14 C16 14 16 17 18 17 L21 17 M14 4 L19 9 L12 16 L8 17 L9 13 Z";
        case Icon::Picture: return "M3 6 L15 6 M3 6 L3 19 L21 19 L21 12 M3 16 L8 11 L12 15 L14 13 L21 19 M18 3 L18 9 M15 6 L21 6";
        case Icon::RemovePage: return "M6 3 L14 3 L18 7 L18 21 L6 21 Z M9 13 L15 13";
        case Icon::Archive: return "M4 4 L20 4 L20 20 L4 20 Z M12 4 L12 6 M11 7 L13 7 M12 8 L12 10 M11 11 L13 11 M10 13 L14 13 L14 16.5 L10 16.5 Z";
        default: return nullptr;
    }
}

Com<ID2D1PathGeometry> BuildIcon(const char* d, D2D1_POINT_2F c, float size)
{
    Com<ID2D1PathGeometry> geo;
    Com<ID2D1GeometrySink> sink;
    if (FAILED(D2D()->CreatePathGeometry(geo.Put())) || FAILED(geo->Open(sink.Put()))) return {};
    float k = size / 24.0f;
    auto P = [&](float x, float y) { return D2D1::Point2F(c.x + (x - 12) * k, c.y + (y - 12) * k); };
    bool open = false;
    const char* p = d;
    auto num = [&]() { while (*p == ' ') p++; char* e; float v = strtof(p, &e); p = e; return v; };
    auto end = [&](bool closed) { if (open) { sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN); open = false; } };
    while (*p) {
        char cmd = *p++;
        switch (cmd) {
            case 'M': { end(false); float x = num(), y = num(); sink->BeginFigure(P(x, y), D2D1_FIGURE_BEGIN_HOLLOW); open = true; break; }
            case 'L': { float x = num(), y = num(); sink->AddLine(P(x, y)); break; }
            case 'C': {
                float a = num(), b = num(), cc = num(), dd = num(), e = num(), f = num();
                sink->AddBezier(D2D1::BezierSegment(P(a, b), P(cc, dd), P(e, f)));
                break;
            }
            case 'Z': end(true); break;
            case 'O': {
                end(false);
                float x = num(), y = num(), r = num();
                D2D1_SIZE_F rs = D2D1::SizeF(r * k, r * k);
                sink->BeginFigure(P(x + r, y), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddArc(D2D1::ArcSegment(P(x - r, y), rs, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sink->AddArc(D2D1::ArcSegment(P(x + r, y), rs, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                break;
            }
            default: break;
        }
    }
    end(false);
    sink->Close();
    return geo;
}

Com<ID2D1StrokeStyle> RoundStroke()
{
    Com<ID2D1StrokeStyle> s;
    D2D()->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                             nullptr, 0, s.Put());
    return s;
}

}  // namespace

void DrawIcon(Surface& s, Icon icon, D2D1_POINT_2F c, float size, uint32_t argb, float stroke)
{
    const char* d = IconPath(icon);
    if (!d || !s.ready()) return;
    // Built per draw: geometries belong to the factory, which GfxTrim replaces between wheels.
    auto geo = BuildIcon(d, c, size);
    auto style = RoundStroke();
    if (geo) s.Target()->DrawGeometry(geo.Get(), s.Solid(argb), stroke * size / 24.0f * 1.25f, style.Get());   // the caller is mid-frame
}

wstring Ellipsize(Surface& s, const wstring& text, float size, DWRITE_FONT_WEIGHT weight, float maxWidth)
{
    if (s.MeasureText(text.c_str(), size, weight) <= maxWidth) return text;
    size_t lo = 0, hi = text.size();
    while (lo < hi) {   // the longest prefix that fits with the ellipsis
        size_t mid = (lo + hi + 1) / 2;
        wstring t = text.substr(0, mid) + L"…";
        if (s.MeasureText(t.c_str(), size, weight) <= maxWidth) lo = mid; else hi = mid - 1;
    }
    return text.substr(0, lo) + L"…";
}

wstring HumanSize(uint64_t b)
{
    wchar_t buf[32];
    if (b < 1024) swprintf(buf, 32, L"%llu bytes", (unsigned long long)b);
    else if (b < 1024 * 1024) swprintf(buf, 32, L"%.0f KB", b / 1024.0);
    else if (b < 1024ull * 1024 * 1024) swprintf(buf, 32, L"%.1f MB", b / 1048576.0);
    else swprintf(buf, 32, L"%.2f GB", b / 1073741824.0);
    return buf;
}

}  // namespace rd
