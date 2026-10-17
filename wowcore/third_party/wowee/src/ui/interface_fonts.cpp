#include <algorithm>
#include <cmath>
#include "ui/interface_fonts.hpp"
#include <imgui_internal.h>
#include <cstring>
#include <unordered_map>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cfloat>

#include <algorithm>
#include <cmath>
#include "ui/text_markup.hpp"
#include <algorithm>
#include <cmath>
#include "ui/text_wrap.hpp"

#include <algorithm>
#include <cmath>
#include <algorithm>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <vector>

namespace wowee::ui {

namespace {

/// The lower-case stem of a path, with any directory and extension removed.
/// "Fonts\\MORPHEUS.ttf" and "morpheus.ttf" both come out as "morpheus", which
/// is what lets a font object written one way find a file named the other.
std::string faceKey(const std::string& pathOrName) {
    const size_t slash = pathOrName.find_last_of("\\/");
    std::string stem = (slash == std::string::npos) ? pathOrName
                                                    : pathOrName.substr(slash + 1);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return stem;
}

std::unordered_map<std::string, ImFont*>& faces() {
    static std::unordered_map<std::string, ImFont*> map;
    return map;
}

/// A big-endian field out of a font file, or zero past the end.
///
/// Bounds-checked on every read rather than once at the top: a truncated font
/// is a file this client did not write, and the tables it names are at offsets
/// the file itself supplies.
uint32_t beAt(const uint8_t* d, size_t n, size_t at, int bytes) {
    if (at + static_cast<size_t>(bytes) > n) return 0;
    uint32_t v = 0;
    for (int i = 0; i < bytes; ++i) v = (v << 8) | d[at + static_cast<size_t>(i)];
    return v;
}

} // namespace

void registerInterfaceFace(const std::string& pathOrName, ImFont* font) {
    if (!font) return;
    faces()[faceKey(pathOrName)] = font;
}

ImFont* interfaceFace(const std::string& pathOrName) {
    if (pathOrName.empty()) return nullptr;
    const auto it = faces().find(faceKey(pathOrName));
    return (it == faces().end()) ? nullptr : it->second;
}

ImFont* interfaceFaceOrDefault(const std::string& fontFace) {
    // The order the renderer uses. The interface's own default rather than this
    // client's, because ImGui draws with whatever face was added first and that
    // is deliberately the built-in one.
    if (ImFont* f = interfaceFace(fontFace)) return f;
    if (ImFont* f = interfaceFace("frizqt__")) return f;
    return (ImGui::GetCurrentContext() != nullptr) ? ImGui::GetFont() : nullptr;
}

// WoWBridge: 1.12 text metrics (tools/patch_wowee.py patch_fonts)
// A font's height becomes P = round(height * scale) whole pixels: the line height, and a font string's height. The
// glyphs are drawn at the smallest whole em whose ascender fits P, ceil(P / ascender) - FRIZQT's ascender is 0.965
// em, so 11 px lines carry 12 px glyphs - with the hinted whole-pixel advances that go with it.
static float gPixelScale = 1.0f;
constexpr float kAscender = 0.965f;   // FRIZQT__.TTF hhea; the face nearly all of FrameXML uses
void setInterfacePixelScale(float scale) { gPixelScale = scale > 0.0f ? scale : 1.0f; }
float interfacePixelScale() { return gPixelScale; }
static float linePixels(float fontHeight) { return std::max(1.0f, std::floor(fontHeight * gPixelScale + 0.5f)); }
float interfaceLineHeight(float fontHeight) { return linePixels(fontHeight) / gPixelScale; }
float interfaceLineFromEm(float emPixels) { return std::floor(emPixels * kAscender + 0.001f); }
float interfaceMeasure(ImFont* font, float size, const std::string& text, float advancePx) {
    return interfaceTextPx(font, size * gPixelScale, text.c_str(), advancePx) / gPixelScale;
}

// WoWBridge: the client's text metrics, recovered from the 1.12 client's own numbers (evidence/fontref: 282 glyph advances and 22
// strings, FRIZQT at 10, 12 and 16): the pen advances by floor(round26(advance * P / unitsPerEm)) + 1 pixels - in the
// face's font units at the line's pixel height P, not at the larger em the glyphs are drawn at - and a string is as
// wide as the advances of all but its last glyph plus that glyph's own width, ceil26(xMax * P / unitsPerEm) + 2 (a
// glyph with no outline: its advance + 1). FreeType's unscaled metrics over the face's bytes.
namespace {
struct FaceMetrics {
    FT_Face face = nullptr;
    double upem = 1000.0;
    std::unordered_map<uint32_t, std::pair<int, int>> glyphs;   // advance, xMax (font units); xMax < 0: no outline
};
std::unordered_map<const ImFont*, FaceMetrics>& faceMetrics() {
    static std::unordered_map<const ImFont*, FaceMetrics> m;
    return m;
}
const std::pair<int, int>& glyphUnits(FaceMetrics& m, uint32_t cp) {
    auto it = m.glyphs.find(cp);
    if (it == m.glyphs.end()) {
        std::pair<int, int> g{0, -1};
        if (FT_Load_Char(m.face, cp, FT_LOAD_NO_SCALE) == 0) {
            const FT_Glyph_Metrics& gm = m.face->glyph->metrics;
            g.first = static_cast<int>(gm.horiAdvance);
            // WoWBridge: outlines below the baseline use advance width (the
            // recorded underscore samples at all three sizes, evidence/fontref).
            if (gm.width > 0 && gm.horiBearingY > 0) g.second = static_cast<int>(gm.horiBearingX + gm.width);
        }
        it = m.glyphs.emplace(cp, g).first;
    }
    return it->second;
}
long round26(double px) { return static_cast<long>(std::floor(px * 64.0 + 0.5)); }
int penOf(FaceMetrics& m, double P, uint32_t cp) { return static_cast<int>(round26(glyphUnits(m, cp).first * P / m.upem) >> 6) + 1; }
int ownWidthOf(FaceMetrics& m, double P, uint32_t cp) {
    const auto& g = glyphUnits(m, cp);
    if (g.second < 0) return penOf(m, P, cp) + 1;
    return static_cast<int>((round26(g.second * P / m.upem) + 63) >> 6) + 2;
}
FaceMetrics* metricsOf(ImFont* font) {
    auto it = faceMetrics().find(font);
    return it == faceMetrics().end() ? nullptr : &it->second;
}
// Pen advance and (wide) the width of the widest line, over UTF-8 text; a newline starts a line. advancePx widens
// every glyph advance (interfaceFontStyleAdvance): all of them for the pen, all but the last glyph's for a width.
float measureLines(FaceMetrics& m, float emPx, const char* text, bool wide, float advancePx) {
    const double P = interfaceLineFromEm(emPx);
    int pen = 0, glyphs = 0;
    double best = 0.0;
    uint32_t last = 0;
    const auto line = [&] {
        const bool own = wide && last;
        const double drawn = own ? pen - penOf(m, P, last) + ownWidthOf(m, P, last) : pen;
        return drawn + double(advancePx) * (own ? glyphs - 1 : glyphs);
    };
    const char* end = text + std::strlen(text);
    for (const char* s = text; s < end;) {
        unsigned int c = 0;
        s += ImTextCharFromUtf8(&c, s, end);
        if (c == '\n') { best = std::max(best, line()); pen = 0; glyphs = 0; last = 0; continue; }
        pen += penOf(m, P, c);
        ++glyphs;
        last = c;
    }
    best = std::max(best, line());
    return static_cast<float>(best);
}
}  // namespace

void registerInterfaceFaceMetrics(ImFont* font, const void* ttf, size_t bytes) {
    static FT_Library library = [] { FT_Library l = nullptr; return FT_Init_FreeType(&l) == 0 ? l : nullptr; }();
    FT_Face face = nullptr;
    if (!font || !library ||
        FT_New_Memory_Face(library, static_cast<const FT_Byte*>(ttf), static_cast<FT_Long>(bytes), 0, &face) != 0)
        return;
    faceMetrics()[font] = FaceMetrics{face, face->units_per_EM ? double(face->units_per_EM) : 1000.0, {}};
}

float interfaceTextPx(ImFont* font, float emPx, const char* text, float advancePx) {
    if (FaceMetrics* m = metricsOf(font)) return measureLines(*m, emPx, text, true, advancePx);
    return font->CalcTextSizeA(emPx, FLT_MAX, 0.0f, text).x;
}

float interfacePenPx(ImFont* font, float emPx, const char* text) {
    if (FaceMetrics* m = metricsOf(font)) return measureLines(*m, emPx, text, false, 0.0f);
    return font->CalcTextSizeA(emPx, FLT_MAX, 0.0f, text).x;
}

float interfaceDrawText(ImDrawList* dl, ImFont* font, float emPx, float x, float y, unsigned int col, const std::string& text) {
    FaceMetrics* m = metricsOf(font);
    if (!m) {
        dl->AddText(font, emPx, ImVec2(x, y), col, text.c_str());
        return font->CalcTextSizeA(emPx, FLT_MAX, 0.0f, text.c_str()).x;
    }
    const double P = interfaceLineFromEm(emPx);
    float pen = std::floor(x);
    const char* end = text.c_str() + text.size();
    for (const char* s = text.c_str(); s < end;) {
        unsigned int c = 0;
        s += ImTextCharFromUtf8(&c, s, end);
        // Captures place the glyph origin about two pixels below ImGui's.
        // Smaller UI scales reduce it; do not double this raster offset in
        // 2560 captures. Shadow and outline use the same measured origin.
        if (c != ' ') font->RenderChar(dl, emPx, ImVec2(pen, y + 2.0f * std::min(gPixelScale, 1.0f)), col, static_cast<ImWchar>(c));
        pen += static_cast<float>(penOf(*m, P, c));
    }
    return pen - std::floor(x);
}

float interfaceFontSize(float fontHeight) {
    // WoWBridge: the glyph em, in interface units (see above).
    if (fontHeight > 0.0f) return std::ceil(linePixels(fontHeight) / kAscender - 0.0001f) / gPixelScale;
    // What the renderer falls back to: the current size, not a flat twelve.
    return (ImGui::GetCurrentContext() != nullptr) ? ImGui::GetFontSize() : 12.0f;
}


// WoWBridge: native font style width padding (evidence/fontref/client_style_widths).
// The legacy face measure includes the standard one-pixel shadow. Native plain
// strings omit it; OUTLINE/THICKOUTLINE add4/8pixels, and positive shadow X adds
// its rounded physical extent. Negative X does not extend GetStringWidth.
// THICKOUTLINE also widens every glyph advance by one physical pixel: "45", "1000"
// and "Elunara" measure 1, 3 and 6 px wider than the padding alone, in both faces
// at every captured height; OUTLINE advances are unchanged.
float interfaceFontStyleAdvance(const std::string& outline) { return outline == "THICK" ? 1.0f : 0.0f; }
float interfaceFontStylePadding(const std::string& outline, bool shadow, float shadowX) {
    float padding = -1.0f;
    if (!outline.empty()) padding += outline == "THICK" ? 8.0f : 4.0f;
    if (shadow) padding += std::max(0.0f, std::floor(shadowX * gPixelScale + 0.5f));
    return padding / gPixelScale;
}
float interfaceStyleTextWidth(float measured, const std::string& outline, bool shadow, float shadowX) {
    if (measured <= 0.0f) return 0.0f;
    return std::max(0.0f, measured + interfaceFontStylePadding(outline, shadow, shadowX));
}

float interfaceTextWidth(const std::string& text, const std::string& fontFace,
                         float fontHeight, float advancePx) {
    if (text.empty()) return 0.0f;
    const float size = interfaceFontSize(fontHeight);
    if (ImFont* font = interfaceFaceOrDefault(fontFace)) {
        return interfaceMeasure(font, size, text, advancePx);
    }
    // No context yet - during the FrameXML load there may be no frame in
    // flight to ask. An estimate is far better than nothing: the alternative
    // is answering zero, and MoneyFrame does SetWidth(GetTextWidth() +
    // iconWidth), which then places three buttons on top of each other.
    return static_cast<float>(text.size()) * size * 0.5f;
}

int interfaceTextLines(const std::string& text, const std::string& fontFace,
                       float fontHeight, float wrapWidth, bool nonSpaceWrap) {
    if (text.empty()) return 0;
    const float size = interfaceFontSize(fontHeight);
    ImFont* font = interfaceFaceOrDefault(fontFace);
    const auto measure = [&](const std::string& piece) {
        if (font) return interfaceMeasure(font, size, piece);
        // The same estimate interfaceTextWidth falls back on when there is no
        // context yet, so a measure taken during the load does not answer that
        // everything fits on one line.
        return static_cast<float>(piece.size()) * size * 0.5f;
    };
    const auto lines = wrapText(parseMarkup(text), wrapWidth, nonSpaceWrap, measure);
    return lines.empty() ? 1 : static_cast<int>(lines.size());
}

float fontEmSizeScale(const void* ttfData, size_t byteCount) {
    const auto* d = static_cast<const uint8_t*>(ttfData);
    if (!d || byteCount < 12) return 1.0f;

    // A collection points at its first face; a plain font is already there.
    size_t base = 0;
    if (beAt(d, byteCount, 0, 4) == 0x74746366u /* 'ttcf' */) {
        base = beAt(d, byteCount, 12, 4);
        if (base + 12 > byteCount) return 1.0f;
    }

    const uint32_t tables = beAt(d, byteCount, base + 4, 2);
    size_t head = 0, hhea = 0;
    for (uint32_t i = 0; i < tables; ++i) {
        const size_t rec = base + 12 + static_cast<size_t>(i) * 16;
        const uint32_t tag = beAt(d, byteCount, rec, 4);
        if (tag == 0x68656164u /* 'head' */) head = beAt(d, byteCount, rec + 8, 4);
        else if (tag == 0x68686561u /* 'hhea' */) hhea = beAt(d, byteCount, rec + 8, 4);
    }
    if (head == 0 || hhea == 0) return 1.0f;

    const float upem = static_cast<float>(beAt(d, byteCount, head + 18, 2));
    // Signed, and the descender is negative in every font that has one.
    const auto s16 = [&](size_t at) {
        return static_cast<float>(static_cast<int16_t>(beAt(d, byteCount, at, 2)));
    };
    const float span = s16(hhea + 4) - s16(hhea + 6);
    if (upem <= 0.0f || span <= 0.0f) return 1.0f;

    // A face whose span is wildly different from its em is more likely a
    // misread than a real design, and scaling by it would ruin the text rather
    // than improve it. FRIZQT__ is 1.215 and the other four are near it.
    const float scale = span / upem;
    return (scale >= 0.5f && scale <= 3.0f) ? scale : 1.0f;
}

float fontEmSizeScaleOfFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return 1.0f;
    // The tables this reads are in the first few hundred bytes of every font,
    // but the directory can name them in any order, so the file is read whole
    // rather than guessed at. These are a few hundred kilobytes each.
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    return fontEmSizeScale(bytes.data(), bytes.size());
}

} // namespace wowee::ui
