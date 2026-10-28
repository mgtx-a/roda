// Looks shared by the wheel, the cards and the popups: Roda's own colours, its line icons, text fitting.
#pragma once
#include "gfx.h"
#include "registry.h"

namespace rd {

struct Theme {
    uint32_t glass;      // disc and card background
    uint32_t edge;       // hairline outline
    uint32_t sep;        // separators
    uint32_t accent;     // teal: hover, progress, primary button
    uint32_t accentText; // text on the accent
    uint32_t text, dim;  // main and secondary text
    uint32_t hub, hubHot;// the wheel's centre, and its Cancel state
    uint32_t error;      // failures
    uint32_t button;     // secondary button background
    float shadow;
};
const Theme& CurrentTheme();   // light or dark, as Windows is set

uint32_t WithAlpha(uint32_t argb, double factor);

/// A line icon (24-unit design grid) centred at c, `size` DIPs across.
void DrawIcon(Surface& s, Icon icon, D2D1_POINT_2F c, float size, uint32_t argb, float stroke = 1.6f);

/// `text` shortened with "…" to fit `maxWidth` DIPs.
wstring Ellipsize(Surface& s, const wstring& text, float size, DWRITE_FONT_WEIGHT weight, float maxWidth);

/// "1.4 MB"
wstring HumanSize(uint64_t bytes);

}  // namespace rd
