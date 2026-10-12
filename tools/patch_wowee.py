#!/usr/bin/env python3
"""WoWBridge's patches to the vendored WoWee sources, applied to the pristine copies by tools/vendor_wowee.py.

Game-state patches fence upstream code with `#if WOWBRIDGE_WOWEE_GAMEHANDLER`
(never defined in WoWBridge); widget and renderer patches retain our runtime
behavior. Both entry points apply the same ordered bundle. Text anchors stop
a refresh with a diagnostic if upstream changed instead of losing a fix.
"""
import re
from pathlib import Path

WW = Path(__file__).resolve().parent.parent / 'wowcore/third_party/wowee'
ENGINE = WW / 'src/addons/lua_engine.cpp'
FONTS_GUARD = '// WoWBridge: 1.12 text metrics'
GUARD ='#if WOWBRIDGE_WOWEE_GAMEHANDLER  // WoWBridge: game-state code; WoWBridge answers it from its own API shim'


def patch_engine() -> None:
    L = ENGINE.read_text().split('\n')
    if any(GUARD in line for line in L):
        return

    def find(s, start=0):
        for i in range(start, len(L)):
            if s in L[i]:
                return i
        raise SystemExit(f'patch_wowee: anchor not found: {s}')

    def close(start, text='}'):
        return next(k for k in range(start, len(L)) if L[k] == text)

    # 1) Item/spell/unit tooltip methods -> one C entry point WoWBridge implements (wowcore/src/api). They sit in an
    #    anonymous namespace upstream, so they cannot be defined in another file; these forwarders keep the names.
    a = find('/// Fills a tooltip from an action bar slot')
    e = close(find('int lua_Tooltip_SetGuildBankItem(lua_State* L) {', a))
    block = '\n'.join(L[a:e + 1])
    names = re.findall(r'^(?:static )?int lua_Tooltip_(\w+)\(lua_State\* L\) \{', block, re.M)
    names = [n for n in names if n != 'ReturnFalse']
    alt = ['#else',
           '// WoWBridge: the same methods, answered over WoWBridge game state through one C entry point (wowcore/src/api).',
           'extern "C" int wowbridge_tooltip_method(lua_State* L, const char* method);',
           *[f'int lua_Tooltip_{n}(lua_State* L) {{ return wowbridge_tooltip_method(L, "{n}"); }}' for n in names],
           'int lua_Tooltip_ReturnFalse(lua_State* L) { lua_pushboolean(L, 0); return 1; }', '#endif']
    L[e + 1:e + 1] = alt
    L.insert(a, GUARD)
    # 2) print() also posts to the chat log upstream.
    i = find('    auto* gh = getGameHandler(L);')
    j = find('    LOG_INFO("[Lua] ", result);', i)
    L.insert(j, '#endif')
    L.insert(i, GUARD)
    # 3) Quest map blobs.
    i = find('int lua_QuestBlobFrame_DrawQuestBlob(lua_State* L) {')
    j = close(i)
    L[i + 1:j] = [GUARD, *L[i + 1:j], '#else', '    return 0;', '#endif']
    # 4) Lua errors are also posted to chat upstream (two sites).
    i = 0
    for _ in range(2):
        i = next(k for k in range(i, len(L)) if L[k] == '        if (gameHandler_) {' and 'MessageChatData' in L[k + 1])
        j = close(i, '        }')
        L[i:j + 1] = [GUARD, *L[i:j + 1], '#endif']
        i = j + 3
    ENGINE.write_text('\n'.join(L))
    print(f'patched lua_engine.cpp: {len(names)} tooltip forwarders, 5 fences')


def _replace(path: Path, pairs: list[tuple[str, str]]) -> None:
    text = path.read_text()
    for old, new in pairs:
        if old not in text:
            raise SystemExit(f'patch_wowee: anchor not found in {path.name}: {old[:60]}')
        text = text.replace(old, new)
    path.write_text(text)


FONT_HELPERS = FONTS_GUARD + r''' (tools/patch_wowee.py patch_fonts)
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
float interfaceMeasure(ImFont* font, float size, const std::string& text) {
    return font->CalcTextSizeA(size * gPixelScale, FLT_MAX, 0.0f, text.c_str()).x / gPixelScale;
}

float interfaceFontSize(float fontHeight) {
    // WoWBridge: the glyph em, in interface units (see above).
    if (fontHeight > 0.0f) return std::ceil(linePixels(fontHeight) / kAscender - 0.0001f) / gPixelScale;'''

FONT_DECLS = r'''ImFont* interfaceFaceOrDefault(const std::string& fontFace);

''' + FONTS_GUARD + r''': the effective scale text is drawn at (the renderer sets it each layout); a line's height
// (whole pixels, in interface units); the line height in pixels for a glyph em in pixels; and a width measured at the
// whole-pixel em the client draws at, in interface units.
void setInterfacePixelScale(float scale);
float interfacePixelScale();
float interfaceLineHeight(float fontHeight);
float interfaceLineFromEm(float emPixels);
float interfaceMeasure(ImFont* font, float size, const std::string& text);'''


def patch_fonts() -> None:
    """1.12 text metrics, from the client's own numbers (evidence/hudref/frames.lua at two interface scales): a font
    string is P = round(height * scale) pixels tall and lines are P apart; glyphs are drawn at ceil(P / ascender)
    pixels with hinted whole-pixel advances (FRIZQT 12 at 0.9375: 11 px lines, 12 px glyphs; 185 recorded strings
    fit this three times better than glyphs at P). WoWee drew and measured at the fractional height, lines 1.2 apart."""
    hpp, cpp = WW / 'include/ui/interface_fonts.hpp', WW / 'src/ui/interface_fonts.cpp'
    rend = WW / 'src/ui/widget_renderer.cpp'
    if FONTS_GUARD in cpp.read_text():
        return
    _replace(hpp, [('ImFont* interfaceFaceOrDefault(const std::string& fontFace);', FONT_DECLS)])
    _replace(cpp, [
        ('float interfaceFontSize(float fontHeight) {\n    if (fontHeight > 0.0f) return fontHeight;', FONT_HELPERS),
        ('        return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;',
         '        return interfaceMeasure(font, size, text);'),
        ('        if (font) return font->CalcTextSizeA(size, FLT_MAX, 0.0f, piece.c_str()).x;',
         '        if (font) return interfaceMeasure(font, size, piece);'),
        ('#include', '#include <algorithm>\n#include <cmath>\n#include'),
    ])
    _replace(rend, [
        ('void WidgetRenderer::layout(WidgetTree& tree, float screenW, float screenH) {\n    linkScreenH_ = screenH;',
         'void WidgetRenderer::layout(WidgetTree& tree, float screenW, float screenH) {\n'
         '    setInterfacePixelScale(tree.uiScale());   // WoWBridge: 1.12 text metrics\n    linkScreenH_ = screenH;'),
        ('        return runFont->CalcTextSizeA(size, FLT_MAX, 0.0f, piece.c_str()).x;',
         '        return interfaceMeasure(runFont, size, piece);'),
        ('    const ImVec2 measured =\n        runFont->CalcTextSizeA(size, FLT_MAX, 0.0f, strippedText(w->text).c_str());',
         '    const ImVec2 measured(interfaceMeasure(runFont, size, strippedText(w->text)), size);'),
        ('                total += font->CalcTextSizeA(size, FLT_MAX, 0.0f, piece.c_str()).x;',
         '                total += interfaceMeasure(font, size, piece);'),
        ('                                 return font->CalcTextSizeA(size, FLT_MAX, 0.0f,\n'
         '                                                            piece.c_str()).x;',
         '                                 return interfaceMeasure(font, size, piece);'),
        # Line heights: interface units in layout, pixels where drawing.
        ('        w->height = size * 1.2f * static_cast<float>(w->wrappedLines);',
         '        w->height = interfaceLineHeight(w->fontHeight) * static_cast<float>(w->wrappedLines);'),
        ('    const float lineH = size * 1.2f;\n    // Ten units', '    const float lineH = interfaceLineHeight(w->fontHeight);\n    // Ten units'),
        ('    const float lineH = size * 1.2f;\n    float y = at.y;', '    const float lineH = interfaceLineFromEm(size);\n    float y = at.y;'),
        ("h -= size * 1.2f;", "h -= interfaceLineFromEm(size);"),
        ('tipLineH = tipSize * 1.2f;', 'tipLineH = interfaceLineFromEm(tipSize);'),
        ('extent.y = size * 1.2f * static_cast<float>(lines.size());',
         'extent.y = interfaceLineFromEm(size) * static_cast<float>(lines.size());'),
    ])
    _replace(ENGINE, [('    return line * 1.2 * lines;', '    return wowee::ui::interfaceLineHeight(w.fontHeight) * lines;   // WoWBridge: 1.12 text metrics')])
    left = rend.read_text().count('1.2f')
    if left:
        raise SystemExit(f'patch_wowee: {left} line-height sites left unpatched in widget_renderer.cpp')
    print('patched interface text metrics')


def patch_emitter() -> None:
    """A <Texture> with <Color> and no file is a solid colour in 1.12 (GetTexture() answers "Solid Texture"):
    PlayerFrameBackground is black at half alpha behind the name. WoWee made it a vertex colour on nothing."""
    path = WW / 'src/ui/framexml_emitter.cpp'
    old = '            line(var + (isTexture ? ":SetVertexColor(" : ":SetTextColor(") + args + ")");'
    if 'WoWBridge: solid colour' in path.read_text():
        return
    _replace(path, [(old, '            // WoWBridge: solid colour when the texture names no file (tools/patch_wowee.py patch_emitter)\n'
                          '            const bool solid = isTexture && !node.attr("file");\n'
                          '            line(var + (solid ? ":SetTexture(" : isTexture ? ":SetVertexColor(" : ":SetTextColor(") + args + ")");')])
    print('patched solid colour textures')


def patch_keys() -> None:
    """Keys by WoW's name for them: the host is not SDL, and WoWee's names stop at the letters, digits and a few
    others (no NUMPAD1, no "-")."""
    hpp, cpp = WW / 'include/addons/lua_engine.hpp', WW / 'src/addons/lua_engine.cpp'
    if 'WoWBridge: keys by name' in hpp.read_text():
        return
    _replace(hpp, [('    bool dispatchFrameKey(int sdlKeycode, bool down);\n',
                    '    bool dispatchFrameKey(int sdlKeycode, bool down);\n'
                    '    // WoWBridge: keys by name (tools/patch_wowee.py patch_keys): the frame that has the keyboard, if any,\n'
                    '    // takes the key by WoW\'s name for it ("NUMPAD1"); and whether such a frame is shown.\n'
                    '    bool dispatchFrameKeyName(const std::string& key, bool down);\n'
                    '    [[nodiscard]] bool keyboardFrameShown() const { return topKeyboardFrame() != nullptr; }\n')])
    _replace(cpp, [('void LuaEngine::dispatchKey(int sdlKeycode, bool ctrlHeld) {',
                    '// WoWBridge: keys by name (tools/patch_wowee.py patch_keys)\n'
                    'bool LuaEngine::dispatchFrameKeyName(const std::string& key, bool down) {\n'
                    '    const ui::Widget* best = L_ && !key.empty() ? topKeyboardFrame() : nullptr;\n'
                    '    if (!best) return false;\n'
                    '    callFrameScript(best->id, down ? "OnKeyDown" : "OnKeyUp", key.c_str());\n'
                    '    return !best->propagateKeys;\n'
                    '}\n\n'
                    'void LuaEngine::dispatchKey(int sdlKeycode, bool ctrlHeld) {')])
    print('patched keys by name')


M2_GUARD = '// WoWBridge: vanilla effects (tools/patch_wowee.py patch_m2)'


def patch_m2() -> None:
    """What the 1.12 client's effect models need and WoWee's loader did not read (layouts checked on the client's own
    files: Wrath's missile, its impact, Call Lightning, the nature precast): each colour track whole, not only its first
    key; vanilla particle emitters' 16-bit blend and emitter type (read as two bytes, the type came from the blend's high
    byte) and the fields after the colours - head and decay texture cells, tail length, twinkle, burst, drag, spin, wind,
    the enabled track; and vanilla ribbon emitters (220 bytes: the WotLK layout with 28-byte tracks)."""
    hpp, cpp = WW / 'include/pipeline/m2_loader.hpp', WW / 'src/pipeline/m2_loader.cpp'
    if M2_GUARD in cpp.read_text():
        return
    _replace(hpp, [
        ('    M2FBlock particleScale;   // float (x component of vec2) at 3 timestamps\n    bool enabled = true;\n',
         '    M2FBlock particleScale;   // float (x component of vec2) at 3 timestamps\n    bool enabled = true;\n'
         '    ' + M2_GUARD + '\n'
         '    uint8_t particleType = 0;      // 0 a quad at the particle; 1 stretched from where it was born to where it is\n'
         '    uint8_t headOrTail = 0;\n'
         '    uint16_t headCells[3] = {0, 0, 1};    // texture cells over its life: first, last, times through\n'
         '    uint16_t decayCells[3] = {0, 0, 1};\n'
         '    float tailLength = 0.0f, twinkleSpeed = 0.0f, twinklePercent = 1.0f, twinkleScaleMin = 1.0f, twinkleScaleMax = 1.0f;\n'
         '    float burstMultiplier = 1.0f, drag = 0.0f, spin = 0.0f, windTime = 0.0f;\n'
         '    glm::vec3 wind{0.0f};\n'
         '    M2AnimationTrack enabledTrack;  // 0/1\n'),
        ('    std::vector<M2AnimationTrack> colorAlphaTracks;\n',
         '    std::vector<M2AnimationTrack> colorAlphaTracks;\n'
         '    ' + M2_GUARD + ': the colour tracks whole (vec3 per sequence), parallel to colorAlphaTracks\n'
         '    std::vector<M2AnimationTrack> colorRGBTracks;\n'),
    ])
    _replace(cpp, [
        # A byte-keyed vanilla track (visibility, enabled) as 0/1 floats.
        ('// Parse a vanilla/TBC M2 animation track (version < 264).\n',
         M2_GUARD + ': a vanilla track whose keys are bytes (visibility, enabled), read as 0/1 floats.\n'
         'void parseByteTrackVanilla(const std::vector<uint8_t>& data, const M2TrackDiskVanilla& disk, M2AnimationTrack& track) {\n'
         '    track.interpolationType = disk.interpolationType;\n'
         '    track.globalSequence = disk.globalSequence;\n'
         '    if (disk.nTimestamps == 0 || disk.nKeys == 0 || disk.nTimestamps > 100000 || disk.nKeys > 100000) return;\n'
         '    if (disk.ofsTimestamps + disk.nTimestamps * sizeof(uint32_t) > data.size() || disk.ofsKeys + disk.nKeys > data.size()) return;\n'
         '    std::vector<M2Range> ranges;\n'
         '    if (disk.nRanges > 0 && disk.ofsRanges > 0 && disk.nRanges < 4096 && disk.ofsRanges + disk.nRanges * sizeof(M2Range) <= data.size())\n'
         '        ranges = readArray<M2Range>(data, disk.ofsRanges, disk.nRanges);\n'
         '    if (ranges.empty()) ranges.push_back({.start = 0, .end = disk.nTimestamps});\n'
         '    const auto times = readArray<uint32_t>(data, disk.ofsTimestamps, disk.nTimestamps);\n'
         '    track.sequences.resize(ranges.size());\n'
         '    for (size_t i = 0; i < ranges.size(); i++) {\n'
         '        const uint32_t start = ranges[i].start, end = std::min(ranges[i].end, std::min(disk.nTimestamps, disk.nKeys));\n'
         '        if (start >= end) continue;\n'
         '        auto& seq = track.sequences[i];\n'
         '        for (uint32_t k = start; k < end; k++) {\n'
         '            seq.timestamps.push_back(times[k] - times[start]);\n'
         '            seq.floatValues.push_back(data[disk.ofsKeys + k] ? 1.0f : 0.0f);\n'
         '        }\n'
         '    }\n'
         '}\n\n'
         '// Parse a vanilla/TBC M2 animation track (version < 264).\n'),
        # Emitter header: vanilla's 16-bit blend and type.
        ('            em.blendingType = readValue<uint8_t>(m2Data, base + 0x28);\n            em.emitterType  = readValue<uint8_t>(m2Data, base + 0x29);\n',
         '            em.blendingType = readValue<uint8_t>(m2Data, base + 0x28);\n            em.emitterType  = readValue<uint8_t>(m2Data, base + 0x29);\n'
         '            if (isVanilla) {   ' + M2_GUARD + ': two uint16 in vanilla\n'
         '                em.blendingType = static_cast<uint8_t>(readValue<uint16_t>(m2Data, base + 0x28));\n'
         '                em.emitterType = static_cast<uint8_t>(readValue<uint16_t>(m2Data, base + 0x2A));\n'
         '                em.particleType = readValue<uint8_t>(m2Data, base + 0x2C);\n'
         '                em.headOrTail = readValue<uint8_t>(m2Data, base + 0x2D);\n'
         '            }\n'),
        # After the vanilla scale synthesis: the remaining fields.
        ('                        if (scale < 0.001f || scale > 100.0f) scale = 1.0f;\n                        em.particleScale.floatValues[s] = scale;\n                    }\n                }\n',
         '                        if (scale < 0.001f || scale > 100.0f) scale = 1.0f;\n                        em.particleScale.floatValues[s] = scale;\n                    }\n                }\n'
         '                ' + M2_GUARD + '\n'
         '                for (int c = 0; c < 3; c++) {\n'
         '                    em.headCells[c] = readValue<uint16_t>(m2Data, base + 0x168 + c * 2);\n'
         '                    em.decayCells[c] = readValue<uint16_t>(m2Data, base + 0x16E + c * 2);\n'
         '                }\n'
         '                em.tailLength = readValue<float>(m2Data, base + 0x17C);\n'
         '                em.twinkleSpeed = readValue<float>(m2Data, base + 0x180);\n'
         '                em.twinklePercent = readValue<float>(m2Data, base + 0x184);\n'
         '                em.twinkleScaleMin = readValue<float>(m2Data, base + 0x188);\n'
         '                em.twinkleScaleMax = readValue<float>(m2Data, base + 0x18C);\n'
         '                em.burstMultiplier = readValue<float>(m2Data, base + 0x190);\n'
         '                em.drag = readValue<float>(m2Data, base + 0x194);\n'
         '                em.spin = readValue<float>(m2Data, base + 0x198);\n'
         '                em.wind = glm::vec3(readValue<float>(m2Data, base + 0x1B4), readValue<float>(m2Data, base + 0x1B8), readValue<float>(m2Data, base + 0x1BC));\n'
         '                em.windTime = readValue<float>(m2Data, base + 0x1C0);\n'
         '                if (base + 0x1DC + sizeof(M2TrackDiskVanilla) <= m2Data.size())\n'
         '                    parseByteTrackVanilla(m2Data, readValue<M2TrackDiskVanilla>(m2Data, base + 0x1DC), em.enabledTrack);\n'),
        # The colour track itself.
        ('            model.colorRGB.push_back(rgb);\n',
         '            model.colorRGB.push_back(rgb);\n'
         '            ' + M2_GUARD + ': the colour track itself\n'
         '            model.colorRGBTracks.resize(header.nColors);\n'
         '            if (wotlk) {\n'
         '                if (colorTrackOfs + sizeof(M2TrackDisk) <= m2Data.size())\n'
         '                    parseAnimTrack(m2Data, readValue<M2TrackDisk>(m2Data, colorTrackOfs), model.colorRGBTracks[ci], TrackType::VEC3, seqFlags);\n'
         '            } else if (colorTrackOfs + sizeof(M2TrackDiskVanilla) <= m2Data.size()) {\n'
         '                parseAnimTrackVanilla(m2Data, readValue<M2TrackDiskVanilla>(m2Data, colorTrackOfs), model.colorRGBTracks[ci], TrackType::VEC3);\n'
         '            }\n'),
        # Vanilla ribbons, before the collision mesh.
        ('    // Read collision mesh (bounding triangles/vertices/normals)\n',
         '    ' + M2_GUARD + ': vanilla ribbon emitters (220 bytes)\n'
         '    if (header.nRibbonEmitters > 0 && header.ofsRibbonEmitters > 0 && header.nRibbonEmitters < 64 && header.version < 264 &&\n'
         '        static_cast<size_t>(header.ofsRibbonEmitters) + static_cast<size_t>(header.nRibbonEmitters) * 0xDC <= m2Data.size()) {\n'
         '        for (uint32_t ri = 0; ri < header.nRibbonEmitters; ri++) {\n'
         '            const uint32_t base = header.ofsRibbonEmitters + ri * 0xDC;\n'
         '            M2RibbonEmitter rib;\n'
         '            rib.ribbonId = readValue<int32_t>(m2Data, base + 0x00);\n'
         '            rib.bone = readValue<uint32_t>(m2Data, base + 0x04);\n'
         '            rib.position = glm::vec3(readValue<float>(m2Data, base + 0x08), readValue<float>(m2Data, base + 0x0C), readValue<float>(m2Data, base + 0x10));\n'
         '            if (readValue<uint32_t>(m2Data, base + 0x14) > 0) rib.textureIndex = readValue<uint16_t>(m2Data, readValue<uint32_t>(m2Data, base + 0x18));\n'
         '            if (readValue<uint32_t>(m2Data, base + 0x1C) > 0) rib.materialIndex = readValue<uint16_t>(m2Data, readValue<uint32_t>(m2Data, base + 0x20));\n'
         '            auto track = [&](uint32_t off, M2AnimationTrack& t, TrackType type) {\n'
         '                parseAnimTrackVanilla(m2Data, readValue<M2TrackDiskVanilla>(m2Data, base + off), t, type);\n'
         '            };\n'
         '            track(0x24, rib.colorTrack, TrackType::VEC3);\n'
         '            track(0x40, rib.alphaTrack, TrackType::FIXED16);\n'
         '            track(0x5C, rib.heightAboveTrack, TrackType::FLOAT);\n'
         '            track(0x78, rib.heightBelowTrack, TrackType::FLOAT);\n'
         '            rib.edgesPerSecond = readValue<float>(m2Data, base + 0x94);\n'
         '            rib.edgeLifetime = readValue<float>(m2Data, base + 0x98);\n'
         '            rib.gravity = readValue<float>(m2Data, base + 0x9C);\n'
         '            rib.textureRows = std::max<uint16_t>(readValue<uint16_t>(m2Data, base + 0xA0), 1);\n'
         '            rib.textureCols = std::max<uint16_t>(readValue<uint16_t>(m2Data, base + 0xA2), 1);\n'
         '            parseByteTrackVanilla(m2Data, readValue<M2TrackDiskVanilla>(m2Data, base + 0xC0), rib.visibilityTrack);\n'
         '            if (rib.bone != 0xFFFFFFFF) model.ribbonEmitters.push_back(std::move(rib));\n'
         '        }\n'
         '    }\n\n'
         '    // Read collision mesh (bounding triangles/vertices/normals)\n'),
    ])
    print('patched vanilla effects (colour tracks, particles, ribbons)')


UNWRAP_GUARD = '// WoWBridge: an unwrapped paragraph drops its rows'


def patch_unwrap() -> None:
    """A paragraph's height is its wrapped rows (the measure sets it); SetWidth(0) made it a label sized by its text
    again but kept that height, and the measure only remeasures heights it set itself. GameTooltip shows it: a line
    wrapped for a newbie tip (eight rows) and then reused for "180 Mana" stayed eight rows tall, pushing the lines
    under it out of the box."""
    tree = WW / 'src/ui/widget_tree.cpp'
    if UNWRAP_GUARD in tree.read_text():
        return
    _replace(tree, [('        w->wrapsToWidth = false;\n    }\n    w->width = width;',
                     '        if (w->wrapsToWidth) w->height = 0.0f;   ' + UNWRAP_GUARD + '\n'
                     '        w->wrapsToWidth = false;\n    }\n    w->width = width;')])
    print('patched widget_tree.cpp: an unwrapped paragraph drops its rows')


EVENTS_GUARD = '// WoWBridge: every event of each sequence'


def patch_events() -> None:
    """Vanilla M2 events (44-byte records on the global timeline), every one of them per sequence, not just footfalls:
    the client acts on them - $CSS a swing's whoosh, $HIT / $CAH the blow landing, $DTH a body falling."""
    hpp, cpp = WW / 'include/pipeline/m2_loader.hpp', WW / 'src/pipeline/m2_loader.cpp'
    if EVENTS_GUARD in cpp.read_text():
        return
    _replace(hpp, [('    std::vector<std::vector<uint32_t>> footstepEventTimes;\n',
                    '    std::vector<std::vector<uint32_t>> footstepEventTimes;\n'
                    '    ' + EVENTS_GUARD + ' (vanilla): its identifier ("$CSS") and the ms into it.\n'
                    '    std::vector<std::vector<std::pair<std::string, uint32_t>>> sequenceEvents;\n')])
    _replace(cpp, [('        model.footstepEventTimes.resize(model.sequences.size());\n',
                    '        model.footstepEventTimes.resize(model.sequences.size());\n'
                    '        model.sequenceEvents.resize(model.sequences.size());\n'),
                   ('            if (std::memcmp(m2Data.data() + base, "$FSD", 4) != 0) continue;\n',
                    '            if (!wotlkEvents) {   ' + EVENTS_GUARD + '\n'
                    '                const std::string id(reinterpret_cast<const char*>(m2Data.data() + base), 4);\n'
                    '                const uint32_t n = rd32(base + trackOfs + 12), ofs = rd32(base + trackOfs + 16);\n'
                    '                if (n > 0 && n <= 100000 && ofs + size_t(n) * 4 <= m2Data.size())\n'
                    '                    for (const uint32_t ts : readArray<uint32_t>(m2Data, ofs, n))\n'
                    '                        for (size_t s = 0; s < vanillaSeqWindows.size() && s < model.sequenceEvents.size(); s++)\n'
                    '                            if (ts >= vanillaSeqWindows[s].first && ts < vanillaSeqWindows[s].second)\n'
                    '                                model.sequenceEvents[s].emplace_back(id, ts - vanillaSeqWindows[s].first);\n'
                    '            }\n'
                    '            if (std::memcmp(m2Data.data() + base, "$FSD", 4) != 0) continue;\n')])
    print('patched m2_loader: every event of each sequence')


FONT_INHERITS_GUARD = '// WoWBridge: 1.12 names a button font by inherits'


def patch_font_inherits() -> None:
    """1.12 names a button's label fonts <NormalFont inherits="GameFontHighlightSmall"/> (and HighlightFont,
    DisabledFont); later clients write style=. WoWee read only style, so 1.12's buttons kept the default font: the Key
    Bindings frame's key buttons drew GameFontHighlight instead of GameFontHighlightSmall."""
    em = WW / 'src/ui/framexml_emitter.cpp'
    if FONT_INHERITS_GUARD in em.read_text():
        return
    old = '            if (const std::string* style = child.attr("style")) {\n'
    new = ('            const std::string* style = child.attr("style");   ' + FONT_INHERITS_GUARD + '\n'
           '            if (!style) style = child.attr("inherits");\n'
           '            if (style) {\n')
    text = em.read_text()
    if text.count(old) != 2:
        raise SystemExit(f'patch_wowee: expected the two font-object anchors in {em.name}, found {text.count(old)}')
    em.write_text(text.replace(old, new))
    print('patched framexml_emitter.cpp: button fonts by inherits')


ALPHA_GUARD = '// WoWBridge: effective alpha'


def patch_alpha() -> None:
    """A frame's alpha multiplies into everything it holds, as WoW's effective alpha does (Region:GetEffectiveAlpha
    in later clients; 1.12 draws it so): FrameXML fades whole frames by their alpha (the cast bar after a cast, the Key
    Bindings frame's unbound keys at 0.8). WoWee drew each region at its own alpha only. Applied for the draw, the
    widget's own value restored after it."""
    rend = WW / 'src/ui/widget_renderer.cpp'
    if ALPHA_GUARD in rend.read_text():
        return
    _replace(rend, [('    for (const Widget* w : order) {\n'
                     '        // Anything inside a scroll frame is bounded by it.',
                     '    for (const Widget* w : order) {\n'
                     '        ' + ALPHA_GUARD + ': its own times every frame above it, for this draw.\n'
                     '        struct InheritedAlpha {\n'
                     '            Widget* w; float own;\n'
                     '            ~InheritedAlpha() { w->alpha = own; }\n'
                     '        } inheritedAlpha{ const_cast<Widget*>(w), w->alpha };\n'
                     '        for (const Widget* up = tree.get(w->parent); up; up = up->parent ? tree.get(up->parent) : nullptr)\n'
                     '            inheritedAlpha.w->alpha *= up->alpha;\n'
                     '        // Anything inside a scroll frame is bounded by it.')])
    print('patched widget_renderer.cpp: effective alpha')


FONT_ORDER_GUARD = '// WoWBridge: label fonts after the label'


def patch_font_order() -> None:
    """A button's <NormalFont>/<HighlightFont>/<DisabledFont> were emitted before its <ButtonText>, so a template that
    declares both (UIPanelButtonTemplate: GameFontNormal) set the font on a stand-in label that <ButtonText> then
    replaced - every such button drew white default text where the client draws gold. Emitted after the button's
    regions instead (needs patch_font_inherits first: it moves the block that patch rewrote)."""
    em = WW / 'src/ui/framexml_emitter.cpp'
    text = em.read_text()
    if FONT_ORDER_GUARD in text:
        return
    start = text.find('        // <NormalFont style="GameFontNormal"/> - the font a button\'s label is\n')
    end = text.find('        // <HitRectInsets><AbsInset .../></HitRectInsets>\n', start)
    anchor = '        emitButtonRegions(node, var, name);\n'
    if start < 0 or end < 0 or text.count(anchor) != 1:
        raise SystemExit('patch_wowee: font block or button regions anchor not found in framexml_emitter.cpp')
    block = text[start:end]
    text = text[:start] + text[end:]
    text = text.replace(anchor, anchor + '        ' + FONT_ORDER_GUARD + ' (<ButtonText> is made just above).\n' + block)
    em.write_text(text)
    print('patched framexml_emitter.cpp: label fonts after the label')


TEXT_GUARD = '// WoWBridge: the client\'s text metrics'

TEXT_DECLS = '''
''' + TEXT_GUARD + r''' (interface_fonts.cpp). Sizes and results in pixels; each falls back to ImGui's own
// measure for a face whose metrics were not registered.
void registerInterfaceFaceMetrics(ImFont* font, const void* ttf, size_t bytes);
float interfaceTextPx(ImFont* font, float emPx, const char* text);   // a string's width
float interfacePenPx(ImFont* font, float emPx, const char* text);    // where the pen ends after it (a caret)
// Draws one run of plain text with the client's advances at (x, y) and returns the pen's advance.
float interfaceDrawText(ImDrawList* dl, ImFont* font, float emPx, float x, float y, unsigned int col, const std::string& text);
'''

TEXT_IMPL = '''
''' + TEXT_GUARD + r''', recovered from the 1.12 client's own numbers (evidence/fontref: 282 glyph advances and 22
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
            if (gm.width > 0) g.second = static_cast<int>(gm.horiBearingX + gm.width);
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
// Pen advance and (wide) the width of the widest line, over UTF-8 text; a newline starts a line.
float measureLines(FaceMetrics& m, float emPx, const char* text, bool wide) {
    const double P = interfaceLineFromEm(emPx);
    int pen = 0, best = 0;
    uint32_t last = 0;
    const char* end = text + std::strlen(text);
    for (const char* s = text; s < end;) {
        unsigned int c = 0;
        s += ImTextCharFromUtf8(&c, s, end);
        if (c == '\n') { best = std::max(best, wide && last ? pen - penOf(m, P, last) + ownWidthOf(m, P, last) : pen); pen = 0; last = 0; continue; }
        pen += penOf(m, P, c);
        last = c;
    }
    best = std::max(best, wide && last ? pen - penOf(m, P, last) + ownWidthOf(m, P, last) : pen);
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

float interfaceTextPx(ImFont* font, float emPx, const char* text) {
    if (FaceMetrics* m = metricsOf(font)) return measureLines(*m, emPx, text, true);
    return font->CalcTextSizeA(emPx, FLT_MAX, 0.0f, text).x;
}

float interfacePenPx(ImFont* font, float emPx, const char* text) {
    if (FaceMetrics* m = metricsOf(font)) return measureLines(*m, emPx, text, false);
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
        if (c != ' ') font->RenderChar(dl, emPx, ImVec2(pen, y), col, static_cast<ImWchar>(c));
        pen += static_cast<float>(penOf(*m, P, c));
    }
    return pen - std::floor(x);
}
'''


def patch_text_metrics() -> None:
    """The client's text metrics (evidence/fontref): measuring and drawing with the 1.12 client's own glyph advances and
    string widths instead of ImGui's - which were the face's hinted advances at the drawn em, a pixel off on 10-17 of
    every 94 glyphs (our "[PH[ Combat Tester" fit its 100-unit box; the client's is 103.47 wide and is cut). Needs
    patch_fonts (interfaceLineFromEm) and the faces' bytes (wowcore ui_session registers them)."""
    hpp, cpp = WW / 'include/ui/interface_fonts.hpp', WW / 'src/ui/interface_fonts.cpp'
    rend = WW / 'src/ui/widget_renderer.cpp'
    if TEXT_GUARD in cpp.read_text():
        return
    _replace(hpp, [('float interfaceMeasure(ImFont* font, float size, const std::string& text);',
                    'float interfaceMeasure(ImFont* font, float size, const std::string& text);' + TEXT_DECLS),
                   ('struct ImFont;\n', 'struct ImFont;\nstruct ImDrawList;\n')])
    _replace(cpp, [('#include "ui/interface_fonts.hpp"\n',
                    '#include "ui/interface_fonts.hpp"\n#include <imgui_internal.h>\n#include <cstring>\n#include <unordered_map>\n'
                    '#include <ft2build.h>\n#include FT_FREETYPE_H\n'),
                   ('float interfaceMeasure(ImFont* font, float size, const std::string& text) {\n'
                    '    return font->CalcTextSizeA(size * gPixelScale, FLT_MAX, 0.0f, text.c_str()).x / gPixelScale;\n}\n',
                    'float interfaceMeasure(ImFont* font, float size, const std::string& text) {\n'
                    '    return interfaceTextPx(font, size * gPixelScale, text.c_str()) / gPixelScale;\n}\n' + TEXT_IMPL)])
    text = rend.read_text()
    # The edit box's caret sits where the pen stops, not past the last glyph's ink.
    caret = 'font->CalcTextSizeA(size, FLT_MAX, 0.0f, upTo.c_str()).x'
    extent = ('font ? font->CalcTextSizeA(size, FLT_MAX, 0.0f, measured.c_str())\n'
              '                     : ImGui::CalcTextSize(measured.c_str());')
    draw = ('            dl->AddText(font, size, ImVec2(x, y), col, run.text.c_str());\n'
            '            const float runW = font->CalcTextSizeA(size, FLT_MAX, 0.0f,\n'
            '                                                   run.text.c_str()).x;\n')
    line_w = ('        float lineW = 0.0f;\n'
              '        for (const TextRun& run : line) lineW += runWidth(run);\n')
    for a in (caret, extent, draw, line_w):
        if text.count(a) != 1:
            raise SystemExit(f'patch_wowee: text anchor not found once in widget_renderer.cpp: {a[:50]}')
    text = text.replace(caret, 'interfacePenPx(font, size, upTo.c_str())')
    text = text.replace(extent, 'font ? ImVec2(interfaceTextPx(font, size, measured.c_str()),   ' + TEXT_GUARD + '\n'
                        '                            font->CalcTextSizeA(size, FLT_MAX, 0.0f, measured.c_str()).y)\n'
                        '                     : ImGui::CalcTextSize(measured.c_str());')
    text = text.replace(draw, '            const float runW = interfaceDrawText(dl, font, size, x, y, col, run.text);   '
                        + TEXT_GUARD + '\n')
    # A line is as wide as its runs' advances and its last glyph's own width past where the pen stops.
    text = text.replace(line_w, '        float lineW = 0.0f;   ' + TEXT_GUARD + '\n'
                        '        for (const TextRun& run : line)\n'
                        '            lineW += run.texture.empty() ? interfacePenPx(font, size, run.text.c_str()) : runWidth(run);\n'
                        '        if (!line.empty() && line.back().texture.empty())\n'
                        '            lineW += interfaceTextPx(font, size, line.back().text.c_str()) -\n'
                        '                     interfacePenPx(font, size, line.back().text.c_str());\n')
    # Every other width: the client's.
    text, n = re.subn(r'(\w+)->CalcTextSizeA\(\s*(\w+),\s*FLT_MAX,\s*0\.0f,\s*([^;{}]*?\.c_str\(\))\)\.x',
                      r'interfaceTextPx(\1, \2, \3)', text, flags=re.S)
    if n < 6:
        raise SystemExit(f'patch_wowee: expected the renderer\'s text measures, replaced {n}')
    rend.write_text(text)
    print(f'patched the client\'s text metrics ({n} measures)')

MODEL_GUARD = '// WoWBridge: Model frames the host runs'

MODEL_DECLS = '''
''' + MODEL_GUARD + r''' (wowcore: the 1.12 cooldown's M2, a sweep and a flash). For a frame, what to draw
// in its rect (pixels): batches of triangles, each with its texture and whether it adds light. Set once; called for
// every frame drawn, so it answers nothing for frames that are not its own.
struct HostModelVertex { float x, y, u, v; uint32_t col; };
struct HostModelBatch { std::string texture; bool add = false; std::vector<HostModelVertex> triangles; };
using HostModelHook = std::function<void(const Widget& w, float x0, float y0, float x1, float y1,
                                         std::vector<HostModelBatch>& out)>;
void setHostModelHook(HostModelHook hook);
'''


def patch_model_frames() -> None:
    """Model frames run by the host: 1.12's cooldown is <Model file="Interface\\Cooldown\\UI-Cooldown-Indicator.mdx">,
    not a Cooldown frame, and WoWee drew nothing for it (SetSequence and SetSequenceTime were no-ops). The renderer
    now asks a hook (wowcore evaluates the M2) for each frame's triangles and draws them, clipped to the frame; a
    texture not yet resident is asked for on the next frame. And a Model's file="" reaches SetModel."""
    hpp, rend = WW / 'include/ui/widget_renderer.hpp', WW / 'src/ui/widget_renderer.cpp'
    em, tree_h, tree = WW / 'src/ui/framexml_emitter.cpp', WW / 'include/ui/widget_tree.hpp', WW / 'src/ui/widget_tree.cpp'
    if MODEL_GUARD in rend.read_text():
        return
    # A frame the host draws a model in is drawn even with nothing of its own (no backdrop, bar or text).
    _replace(tree_h, [('    bool  isCooldown = false;', '    bool  hostModel = false;   ' + MODEL_GUARD + ' (the host sets it)\n    bool  isCooldown = false;')])
    _replace(tree, [('        if (w.kind == WidgetKind::Frame && !w.hasBackdrop && !w.isStatusBar &&\n',
                     '        if (w.kind == WidgetKind::Frame && !w.hasBackdrop && !w.isStatusBar && !w.hostModel &&\n')])
    text = hpp.read_text()
    ns = text.find('namespace wowee')
    cls = text.find('class WidgetRenderer', ns)
    if ns < 0 or cls < 0:
        raise SystemExit('patch_wowee: WidgetRenderer declaration not found')
    text = text[:cls] + MODEL_DECLS.lstrip('\n') + '\n' + text[cls:]
    text = text.replace('#include <cstdint>\n', '#include <cstdint>\n#include <functional>\n', 1)
    hpp.write_text(text)
    _replace(rend, [
        ('constexpr VkDescriptorSet kMissing = VK_NULL_HANDLE;\n',
         'constexpr VkDescriptorSet kMissing = VK_NULL_HANDLE;\n' + MODEL_GUARD + ': the hook, and the textures it named\n'
         '// that were not resident yet (asked for before the next frame draws).\n'
         'HostModelHook& hostModelHook() { static HostModelHook hook; return hook; }\n'
         'std::set<std::pair<std::string, bool>>& hostModelPending() { static std::set<std::pair<std::string, bool>> s; return s; }\n'),
        ('        wanted.emplace_back(&path, add);\n    };\n',
         '        wanted.emplace_back(&path, add);\n    };\n'
         '    for (const auto& p : hostModelPending()) want(p.first, p.second);   ' + MODEL_GUARD + '\n'),
        ('            if (w->isCooldown) drawCooldown(dl, *w, x0, y0, x1, y1);\n',
         '            if (w->isCooldown) drawCooldown(dl, *w, x0, y0, x1, y1);\n'
         '            if (hostModelHook()) {   ' + MODEL_GUARD + '\n'
         '                std::vector<HostModelBatch> batches;\n'
         '                hostModelHook()(*w, x0, y0, x1, y1, batches);\n'
         '                if (!batches.empty()) dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);\n'
         '                for (const HostModelBatch& b : batches) {\n'
         '                    const VkDescriptorSet tex = resident(b.texture, b.add);\n'
         '                    if (tex == kMissing) { hostModelPending().insert({b.texture, b.add}); continue; }\n'
         '                    dl->PushTexture(ImTextureRef(reinterpret_cast<ImTextureID>(tex)));\n'
         '                    const int n = static_cast<int>(b.triangles.size());\n'
         '                    dl->PrimReserve(n, n);\n'
         '                    for (const HostModelVertex& v : b.triangles) {\n'
         '                        const uint32_t a = static_cast<uint32_t>(((v.col >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(w->alpha, 0.0f, 1.0f));\n'
         '                        dl->PrimVtx(ImVec2(v.x, v.y), ImVec2(v.u, v.v), (v.col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT));\n'
         '                    }\n'
         '                    dl->PopTexture();\n'
         '                }\n'
         '                if (!batches.empty()) dl->PopClipRect();\n'
         '            }\n'),
        ('void WidgetRenderer::drawCooldown(ImDrawList* dl, const Widget& w,',
         'void setHostModelHook(HostModelHook hook) { hostModelHook() = std::move(hook); }   ' + MODEL_GUARD + '\n\n'
         'void WidgetRenderer::drawCooldown(ImDrawList* dl, const Widget& w,'),
    ])
    text = rend.read_text()
    up = '        vkCtx_->endUploadBatchSync();\n    }\n'
    if text.count(up) != 1:
        raise SystemExit('patch_wowee: upload batch anchor not found')
    text = text.replace(up, up + '    hostModelPending().clear();   ' + MODEL_GUARD + ': asked for; still missing ones ask again\n')
    if '#include <set>' not in text:
        text = text.replace('#include "ui/widget_renderer.hpp"\n', '#include "ui/widget_renderer.hpp"\n#include <set>\n', 1)
    rend.write_text(text)
    # <Model file="..."> names its model: SetModel, which the host's Model frames read.
    _replace(em, [('        if (const std::string* strata = node.attr("frameStrata")) {\n',
                   '        if (const std::string* file = node.attr("file");   ' + MODEL_GUARD + '\n'
                   '            file && (node.name == "Model" || node.name == "PlayerModel" || node.name == "DressUpModel" ||\n'
                   '                     node.name == "TabardModel")) {\n'
                   '            line(var + ":SetModel(" + quote(*file) + ")");\n'
                   '        }\n'
                   '        if (const std::string* strata = node.attr("frameStrata")) {\n')])
    print('patched Model frames run by the host')

HEADER_GUARD = '// WoWBridge: texture_flipbooks before the transforms'


def patch_vanilla_header() -> None:
    """A vanilla (and TBC) M2 header has texture_flipbooks between transparency and texture_transforms; WoWee skipped an
    "unknown extra M2Array" after the replaceable-texture lookup instead, so it read the transforms' count from the
    (empty) flipbooks and every vanilla model lost its texture animations - the cooldown's turning sweep among them
    (UI-Cooldown-Indicator: transforms 4 at 5808, replace lookup 1 at 7680)."""
    m2 = WW / 'src/pipeline/m2_loader.cpp'
    if HEADER_GUARD in m2.read_text():
        return
    _replace(m2, [('        header.nTransparency = r32();\n        header.ofsTransparency = r32();\n'
                   '        header.nUVAnimation = r32();\n',
                   '        header.nTransparency = r32();\n        header.ofsTransparency = r32();\n'
                   '        c += 8;   ' + HEADER_GUARD + '\n'
                   '        header.nUVAnimation = r32();\n'),
                  ('        header.nTexReplace = r32();\n        header.ofsTexReplace = r32();\n\n'
                   '        // Skip unknown extra M2Array (8 bytes)\n        c += 8;\n',
                   '        header.nTexReplace = r32();\n        header.ofsTexReplace = r32();\n')])
    print('patched m2_loader: the vanilla header\'s flipbooks')


RANGE_GUARD = '// WoWBridge: vanilla key ranges are inclusive'


def patch_vanilla_ranges() -> None:
    """A vanilla track's per-sequence key ranges name their first and last key (M2Range minimum/maximum, inclusive);
    WoWee read the last as one past the end, so every sequence lost its last key and a one-key sequence vanished. The
    cooldown shows it: its quads' alpha is (0, 1) (1, 1) over two keys - 1 in the sweep, 0 in the flash - and the
    flash's star fades over (2, 5); read exclusively the quads stayed dark through the flash and the star never faded."""
    m2 = WW / 'src/pipeline/m2_loader.cpp'
    if RANGE_GUARD in m2.read_text():
        return
    _replace(m2, [('        ranges = readArray<M2Range>(data, disk.ofsRanges, disk.nRanges);\n    }\n\n'
                   '    // If no ranges, treat entire array as one sequence\n',
                   '        ranges = readArray<M2Range>(data, disk.ofsRanges, disk.nRanges);\n'
                   '        for (auto& r : ranges) r.end += 1;   ' + RANGE_GUARD + '\n    }\n\n'
                   '    // If no ranges, treat entire array as one sequence\n')])
    print('patched m2_loader: vanilla key ranges inclusive')


BLEND_GUARD = '// WoWBridge: light that adds'


def patch_host_blend() -> None:
    """M2 batches that add light (blend 3, 4: the cooldown's end flash) drawn additively rather than as WoWee draws
    additive art (alpha taken from brightness, which paints a pale shape over what is beneath instead of lighting it).
    HostAdditiveBlend and HostAlphaBlend are callback identifiers, as ImGui's own are: each backend (the in-game D3D11
    one, the native CPU one) switches its blend on them."""
    hpp, rend = WW / 'include/ui/widget_renderer.hpp', WW / 'src/ui/widget_renderer.cpp'
    if BLEND_GUARD in rend.read_text():
        return
    _replace(hpp, [('struct ImDrawList;   // global, as ImGui declares it\n',
                    'struct ImDrawList;   // global, as ImGui declares it\nstruct ImDrawCmd;\n'),
                   ('void setHostModelHook(HostModelHook hook);\n',
                    'void setHostModelHook(HostModelHook hook);\n' + BLEND_GUARD + ' (M2 blend 3, 4): the backend blends additively after\n'
                    '// HostAdditiveBlend and as usual again after HostAlphaBlend. Identifiers only, as ImGui\'s own callbacks are.\n'
                    'void HostAdditiveBlend(const ImDrawList*, const ImDrawCmd*);\n'
                    'void HostAlphaBlend(const ImDrawList*, const ImDrawCmd*);\n')])
    _replace(rend, [('                    const VkDescriptorSet tex = resident(b.texture, b.add);\n'
                     '                    if (tex == kMissing) { hostModelPending().insert({b.texture, b.add}); continue; }\n',
                     '                    const VkDescriptorSet tex = resident(b.texture);   ' + BLEND_GUARD + ': its own colours, added\n'
                     '                    if (tex == kMissing) { hostModelPending().insert({b.texture, false}); continue; }\n'
                     '                    if (b.add) dl->AddCallback(HostAdditiveBlend, nullptr);\n'),
                    ('                    dl->PopTexture();\n                }\n                if (!batches.empty()) dl->PopClipRect();\n',
                     '                    dl->PopTexture();\n                    if (b.add) dl->AddCallback(HostAlphaBlend, nullptr);\n'
                     '                }\n                if (!batches.empty()) dl->PopClipRect();\n'),
                    ('void setHostModelHook(HostModelHook hook) { hostModelHook() = std::move(hook); }',
                     'void setHostModelHook(HostModelHook hook) { hostModelHook() = std::move(hook); }\n'
                     'void HostAdditiveBlend(const ImDrawList*, const ImDrawCmd*) {}   ' + BLEND_GUARD + '\n'
                     'void HostAlphaBlend(const ImDrawList*, const ImDrawCmd*) {}')])
    print('patched additive host blending')


MESSAGES_GUARD = '// WoWBridge: a message frame inserting at the top'


SCROLL_GUARD = '// WoWBridge: scrolling relights'


def patch_message_scroll() -> None:
    """Chat lines fade after their time and came back only while the frame was scrolled away from its newest line, so
    scrolling back down - or the bottom button - left it empty again at once (the user: once faded, the arrows did
    nothing). The client restarts the lines' time when the frame is scrolled: they show again, and fade later."""
    eng = WW / 'src/addons/lua_engine.cpp'
    text = eng.read_text()
    if SCROLL_GUARD in text:
        return
    relight = ('        ' + SCROLL_GUARD + ' its lines: they show for their whole time again.\n'
               '        for (auto& m : w->messages) m.age = 0.0f;\n')
    pairs = [
        ('        if (static_cast<size_t>(w->messageScroll) + 1 < w->messages.size())\n            ++w->messageScroll;\n',
         '        if (static_cast<size_t>(w->messageScroll) + 1 < w->messages.size())\n            ++w->messageScroll;\n' + relight),
        ('        if (w->messageScroll > 0) --w->messageScroll;\n',
         '        if (w->messageScroll > 0) --w->messageScroll;\n' + relight),
        ('    if (auto* w = widgetOf(L, 1)) w->messageScroll = 0;\n',
         '    if (auto* w = widgetOf(L, 1)) {\n        w->messageScroll = 0;\n' + relight + '    }\n'),
    ]
    for old, new in pairs:
        if text.count(old) != 1:
            raise SystemExit('patch_wowee: message scroll not found')
        text = text.replace(old, new)
    eng.write_text(text)


def patch_message_frames() -> None:
    """A message frame drew every line from its bottom upward and left-aligned. UIErrorsFrame says insertMode="TOP" and
    <FontString justifyH="CENTER"/>: the newest line at the top, older ones below, each centred - so "You have no
    target." sat at the bottom-left of a 512-unit frame, up and left of where the client centres it (the user's
    playtest)."""
    rend = WW / 'src/ui/widget_renderer.cpp'
    text = rend.read_text()
    if MESSAGES_GUARD in text:
        return
    start = text.find('                float y = y1 - lineH;\n                int painted = 0;\n')
    loop = text.find('                for (int i = static_cast<int>(w->messages.size()) - 1 - scroll;\n', start)
    if start < 0 or loop < 0:
        raise SystemExit('patch_wowee: message frame loop not found')
    text = text[:loop] + ('                ' + MESSAGES_GUARD + ' (UIErrorsFrame): the newest first, from the\n'
                          '                // top down; the loop below walks the same lines in its own order.\n'
                          '                const bool fromTop = w->messagesInsertTop;\n'
                          '                if (fromTop) y = y0 + lineH * 0.0f;\n') + text[loop:]
    old_for = ('                for (int i = static_cast<int>(w->messages.size()) - 1 - scroll;\n'
               '                     i >= 0 && y >= y0; --i) {\n'
               '                    const auto& m = w->messages[static_cast<size_t>(i)];\n')
    new_for = ('                for (int n = 0; n < static_cast<int>(w->messages.size()) - scroll &&\n'
               '                                (fromTop ? y < y1 : y >= y0); ++n) {\n'
               '                    const int i = fromTop ? scroll + n : static_cast<int>(w->messages.size()) - 1 - scroll - n;\n'
               '                    const auto& m = w->messages[static_cast<size_t>(i)];\n')
    old_top = ('                    const size_t rows = lineCount(m.text);\n'
               '                    const float top = y - static_cast<float>(rows - 1) * lineH;\n')
    new_top = ('                    const size_t rows = lineCount(m.text);\n'
               '                    const float top = fromTop ? y : y - static_cast<float>(rows - 1) * lineH;\n'
               '                    if (fromTop && top + static_cast<float>(rows) * lineH > y1 + 0.5f) break;\n')
    old_shadow = ('                                       w->alpha, m.text, wrapW, false, nullptr, true);\n')
    new_shadow = ('                                       w->alpha, m.text, wrapW, false, w->justifyH.c_str(), true);\n')
    old_draw = ('                                   wrapW, false, nullptr, false, &tree, w->id);\n'
                '                    y -= lineH * static_cast<float>(rows);\n')
    new_draw = ('                                   wrapW, false, w->justifyH.c_str(), false, &tree, w->id);\n'
                '                    y += (fromTop ? 1.0f : -1.0f) * lineH * static_cast<float>(rows);\n')
    for a, b in ((old_for, new_for), (old_top, new_top), (old_shadow, new_shadow), (old_draw, new_draw)):
        if text.count(a) != 1:
            raise SystemExit(f'patch_wowee: message frame anchor not found once: {a[:60]}')
        text = text.replace(a, b)
    rend.write_text(text)
    print('patched message frames: insertMode TOP, justifyH')


def patch_layout_dependencies() -> None:
    """Visible dock tabs anchor to backgrounds under hidden chat frames. Resolve those
    dependencies before placing the tab; the hidden-subtree walk leaves their rects stale."""
    path = WW / 'src/ui/widget_tree.cpp'
    guard = '// WoWBridge: resolve hidden anchor dependencies'
    text = path.read_text()
    if guard in text:
        return
    old = ('void WidgetTree::layoutWidget(uint32_t id, float screenW, float screenH) {\n'
           '    layoutWidgetSelf(id, screenW, screenH);')
    new = ('void WidgetTree::layoutWidget(uint32_t id, float screenW, float screenH) {\n'
           '    ' + guard + ' (tools/patch_wowee.py patch_layout_dependencies).\n'
           '    // A shown chat tab still uses the background of its hidden docked window.\n'
           '    int depth = 0;\n'
           '    resolveChain(id, screenW, screenH, depth);\n'
           '    layoutWidgetSelf(id, screenW, screenH);')
    if text.count(old) != 1:
        raise SystemExit('patch_wowee: layoutWidget anchor not found once')
    path.write_text(text.replace(old, new))
    print('patched layout: resolve hidden anchor dependencies')


def patch_tooltip_anchors() -> None:
    """1.12 tooltip owner corners extend outwards; bottom-left puts a buff's tooltip to its left.
    FrameXML also passes x/y offsets on target aura buttons."""
    guard = '// WoWBridge: 1.12 owner anchors extend away from the named owner corner.'
    if guard in ENGINE.read_text():
        return
    old = '''        {"ANCHOR_TOPLEFT",     "BOTTOMLEFT",  "TOPLEFT"},
        {"ANCHOR_TOPRIGHT",    "BOTTOMRIGHT", "TOPRIGHT"},
        {"ANCHOR_BOTTOMLEFT",  "TOPLEFT",     "BOTTOMLEFT"},
        {"ANCHOR_BOTTOMRIGHT", "TOPRIGHT",    "BOTTOMRIGHT"},
        {"ANCHOR_LEFT",        "RIGHT",       "LEFT"},
        {"ANCHOR_RIGHT",       "LEFT",        "RIGHT"},'''
    new = '''        ''' + guard + '''
        {"ANCHOR_TOPLEFT",     "BOTTOMRIGHT", "TOPLEFT"},
        {"ANCHOR_TOPRIGHT",    "BOTTOMLEFT",  "TOPRIGHT"},
        {"ANCHOR_BOTTOMLEFT",  "TOPRIGHT",    "BOTTOMLEFT"},
        {"ANCHOR_BOTTOMRIGHT", "TOPLEFT",     "BOTTOMRIGHT"},
        {"ANCHOR_LEFT",        "BOTTOMRIGHT", "TOPLEFT"},
        {"ANCHOR_RIGHT",       "BOTTOMLEFT",  "TOPRIGHT"},'''
    _replace(ENGINE, [(old, new),
                     ('    a.relativePoint = rel;\n    tree->clearPoints(id);',
                      '    a.relativePoint = rel;\n'
                      '    a.x = static_cast<float>(luaL_optnumber(L, 4, 0));\n'
                      '    a.y = static_cast<float>(luaL_optnumber(L, 5, 0));\n'
                      '    tree->clearPoints(id);')])
    print('patched tooltip owner anchors and offsets')


def patch_desaturated() -> None:
    """Implement 1.12 per-region desaturation and its support result on every host backend."""
    if "WoWBridge: per-region desaturation" in (WW / "include/ui/widget_tree.hpp").read_text():
        return
    _replace(WW / 'include/ui/widget_tree.hpp', [
        (
            '    std::string texturePath;\n',
            '    std::string texturePath;\n'
            '    // WoWBridge: per-region desaturation, independent of the shared texture and vertex tint.\n'
            '    bool desaturated = false;\n',
        ),
    ])
    _replace(WW / 'src/addons/lua_engine.cpp', [
        (
            'int lua_Texture_SetTexCoord(lua_State* L) {',
            '// WoWBridge: 1.12 SetDesaturated reports shader support, including when resetting it.\n'
            'int lua_Texture_SetDesaturated(lua_State* L) {\n'
            '    if (auto* w = widgetOf(L, 1)) w->desaturated = lua_toboolean(L, 2) != 0;\n'
            '    lua_pushboolean(L, 1);\n'
            '    return 1;\n'
            '}\n'
            'int lua_Texture_SetTexCoord(lua_State* L) {',
        ),
        (
            '        set("SetTexCoord", lua_Texture_SetTexCoord);',
            '        set("SetDesaturated", lua_Texture_SetDesaturated);\n'
            '        set("SetTexCoord", lua_Texture_SetTexCoord);',
        ),
    ])
    _replace(WW / 'include/ui/widget_renderer.hpp', [
        (
            'VkDescriptorSet texture(const std::string& path, bool add = false);',
            'VkDescriptorSet texture(const std::string& path, bool add = false, bool desaturated = false);',
        ),
        (
            'bool add) const;',
            'bool add, bool desaturated = false) const;',
        ),
    ])
    _replace(WW / 'src/ui/widget_renderer.cpp', [
        (
            'static std::string cacheKey(const std::string& path, bool add) {\n'
            '    return add ? path + "|add" : path;\n'
            '}',
            '// WoWBridge: desaturation is a cached RGBA variant so every host backend draws it identically.\n'
            'static std::string cacheKey(const std::string& path, bool add, bool desaturated = false) {\n'
            '    return path + (add ? "|add" : "") + (desaturated ? "|gray" : "");\n'
            '}',
        ),
        (
            '                                                     bool add) const {',
            '                                                     bool add, bool desaturated) const {',
        ),
        (
            'auto it = add ? textures_.find(cacheKey(path, true)) : textures_.find(path);',
            'auto it = (add || desaturated) ? textures_.find(cacheKey(path, add, desaturated)) : textures_.find(path);',
        ),
        (
            'VkDescriptorSet WidgetRenderer::texture(const std::string& path, bool add) {\n'
            '    const std::string key = cacheKey(path, add);',
            'VkDescriptorSet WidgetRenderer::texture(const std::string& path, bool add, bool desaturated) {\n'
            '    const std::string key = cacheKey(path, add, desaturated);',
        ),
        (
            '    if (add) {\n'
            '        // Additive blending',
            '    if (desaturated) {\n'
            '        for (size_t i = 0; i + 3 < image.data.size(); i += 4) {\n'
            '            const auto gray = static_cast<uint8_t>((299u * image.data[i] + 587u * image.data[i + 1] +\n'
            '                                                   114u * image.data[i + 2] + 500u) / 1000u);\n'
            '            image.data[i] = image.data[i + 1] = image.data[i + 2] = gray;\n'
            '        }\n'
            '    }\n'
            '    if (add) {\n'
            '        // Additive blending',
        ),
        (
            '    std::vector<std::pair<const std::string*, bool>> wanted;',
            '    struct WantedTexture { const std::string* path; bool add, desaturated; };\n'
            '    std::vector<WantedTexture> wanted;',
        ),
        (
            'auto want = [&](const std::string& path, bool add = false) {',
            'auto want = [&](const std::string& path, bool add = false, bool desaturated = false) {',
        ),
        (
            '        if (cachedTexture(path, add)) return;\n'
            '        for (const auto& p : wanted) if (*p.first == path && p.second == add) return;\n'
            '        wanted.emplace_back(&path, add);',
            '        if (cachedTexture(path, add, desaturated)) return;\n'
            '        for (const auto& p : wanted)\n'
            '            if (*p.path == path && p.add == add && p.desaturated == desaturated) return;\n'
            '        wanted.push_back({&path, add, desaturated});',
        ),
        (
            '            want(w->texturePath, w->blendAdd);',
            '            want(w->texturePath, w->blendAdd, w->desaturated);',
        ),
        (
            '        for (const auto& p : wanted) texture(*p.first, p.second);',
            '        for (const auto& p : wanted) texture(*p.path, p.add, p.desaturated);',
        ),
        (
            '                    cachedTexture(w->texturePath, w->blendAdd);',
            '                    cachedTexture(w->texturePath, w->blendAdd, w->desaturated);',
        ),
    ])
    print("patched texture desaturation")


def patch_desaturated_tint() -> None:
    """The 1.12 desaturation path ignores RGB vertex tint; alpha still applies."""
    path = WW / 'src/ui/widget_renderer.cpp'
    if 'packColor(textureColor, w->alpha)' in path.read_text():
        return
    if 'WoWBridge: desaturation ignores RGB vertex tint' not in path.read_text():
        _replace(path, [('            if (tex == VK_NULL_HANDLE) continue;\n            // SetTexCoord',
                     '            if (tex == VK_NULL_HANDLE) continue;\n'
                     '            // WoWBridge: desaturation ignores RGB vertex tint (1.12 client captures).\n'
                     '            const float textureColor[4] = {\n'
                     '                w->desaturated ? 1.0f : w->color[0],\n'
                     '                w->desaturated ? 1.0f : w->color[1],\n'
                     '                w->desaturated ? 1.0f : w->color[2], w->color[3]};\n'
                     '            // SetTexCoord')])
    text = path.read_text()
    start = text.index('            // WoWBridge: desaturation ignores RGB vertex tint')
    end = text.index('        } else if (w->kind == WidgetKind::FontString)', start)
    block = text[start:end]
    if block.count('packColor(w->color, w->alpha)') != 3:
        raise SystemExit('patch_wowee: texture colour anchors changed')
    path.write_text(text[:start] + block.replace('packColor(w->color, w->alpha)',
                                               'packColor(textureColor, w->alpha)') + text[end:])
    print('patched desaturated texture tint')


def patch_region_layout_fidelity() -> None:
    """Preserve inherited texture anchors and explicit button/slider art sizes."""
    path = WW / 'src/ui/framexml_emitter.cpp'
    marker = '// Do not overwrite anchors inherited from a texture template.'
    if marker in path.read_text():
        return
    old = '        if (node.attrBool("setAllPoints")) {'
    # Accept the earlier stack-split correction too. Filling every derived
    # region overwrote TalentBranchTemplate's anchors and slider knob sizes.
    earlier = '        if (node.attrBool("setAllPoints") || (isTexture && !node.child("Anchors"))) {'
    if earlier in path.read_text():
        old = earlier
    _replace(path, [(old,
        '        // Do not overwrite anchors inherited from a texture template.\n'
        '        // Button art and slider thumbs retain their explicit sizes.\n'
        '        if (node.attrBool("setAllPoints") ||\n'
        '            (isTexture && node.name == "Texture" && !node.attrBool("virtual") &&\n'
        '             !node.attr("inherits") && !node.child("Anchors"))) {')])
    print('patched inherited region and slider layout')


def patch_glyph_origin_fidelity() -> None:
    """Align the glyph origin with measured 1.12 HUD and buff captures."""
    path = WW / 'src/ui/interface_fonts.cpp'
    if 'ImVec2(pen, y + 2.0f * std::min(gPixelScale, 1.0f))' in path.read_text():
        return
    old = 'ImVec2(pen, y + 2.0f * gPixelScale)' if 'ImVec2(pen, y + 2.0f * gPixelScale)' in path.read_text() else 'ImVec2(pen, y)'
    _replace(path, [(old, 'ImVec2(pen, y + 2.0f * std::min(gPixelScale, 1.0f))')])
    print('patched client glyph origin')


def patch_checked_string_fidelity() -> None:
    """Vanilla SpellButton_UpdateSelection uses SetChecked('false')."""
    if '// Vanilla SpellButton_UpdateSelection passes these strings.' in ENGINE.read_text():
        return
    _replace(ENGINE, [('                   : lua_isnumber(L, 2)  ? (lua_tonumber(L, 2) != 0)\n',
        '                   : lua_isnumber(L, 2)  ? (lua_tonumber(L, 2) != 0)\n'
        '                   // Vanilla SpellButton_UpdateSelection passes these strings.\n'
        '                   : lua_isstring(L, 2) && std::strcmp(lua_tostring(L, 2), "false") == 0 ? false\n')])
    print('patched vanilla checked strings')


def patch_below_baseline_width() -> None:
    """The client's recorded underscore width is its advance plus padding."""
    path = WW / 'src/ui/interface_fonts.cpp'
    if 'gm.width > 0 && gm.horiBearingY > 0' in path.read_text():
        return
    _replace(path, [('if (gm.width > 0) g.second',
                     'if (gm.width > 0 && gm.horiBearingY > 0) g.second')])
    print('patched below-baseline glyph widths')


SHOW_DESCENDANTS_HELPER = r'''// WoWBridge: children become visible before the parent's OnShow finalizes layout.
// Talent tab OnShow removes padding; TalentFrame_OnShow then adds its ten units.
// Deferring the children until the next frame silently undid that final sizing.
static void showDescendantsNow(lua_State* L, uint32_t id) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const auto* parent = tree ? tree->get(id) : nullptr;
    if (!parent) return;
    const auto children = parent->children; // handlers can create more widgets
    for (const auto child : children) {
        auto* w = tree->get(child);
        if (!w || !w->shown || !ancestorsShown(L, w)) continue;
        showDescendantsNow(L, child);
        w = tree->get(child);
        if (!w || !w->shown || w->onShowFired || !ancestorsShown(L, w)) continue;
        lua_getglobal(L, "__WoweeFramesByWid");
        lua_pushinteger(L, child);
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) fireOnShowNow(L, -1, w);
        lua_pop(L, 2);
    }
}

'''


def patch_show_descendants_fidelity() -> None:
    """Child OnShow must precede the parent's final tab sizing."""
    if 'static void showDescendantsNow' in ENGINE.read_text():
        return
    _replace(ENGINE, [
        ('int lua_Region_Show(lua_State* L) {',
         SHOW_DESCENDANTS_HELPER + 'int lua_Region_Show(lua_State* L) {'),
        ('        fireOnShowNow(L, 1, w);',
         '        showDescendantsNow(L, w->id);\n        fireOnShowNow(L, 1, w);'),
        ('        const uint8_t toggles = w->shownToggles;\n        w->shownToggles = 0;',
         '        const uint8_t toggles = w->shownToggles;\n        w->shownToggles = 0;\n'
         '        // A synchronous handler may hide the frame before visibility observes it.\n'
         '        if (!w->visibleChain) w->onShowFired = false;'),
    ])
    print('patched child OnShow before parent final layout')


def patch_compass_art_ready():
    path = WW / 'include/ui/widget_renderer.hpp'
    text = path.read_text()
    if 'bool artResident(' in text: return
    anchor = 'private:\n    /// Descriptor set'
    assert anchor in text
    text = text.replace(anchor, '    // WoWBridge: hide replaced native HUD only after the glyph atlas is uploaded.\n'
                        '    bool artResident(const std::string& path) const { return resident(path) != VK_NULL_HANDLE; }\n\n' + anchor, 1)
    path.write_text(text)



def patch_font_style_widths() -> None:
    """Native font probes: shadow contributes its positive X extent; outlines add4/8px."""
    cpp = WW / 'src/ui/interface_fonts.cpp'
    marker = '// WoWBridge: native font style width padding'
    if marker in cpp.read_text():
        return
    hpp = WW / 'include/ui/interface_fonts.hpp'
    rend = WW / 'src/ui/widget_renderer.cpp'
    widget = WW / 'include/ui/widget_tree.hpp'
    decl = 'float interfaceFontStylePadding(const std::string& outline, bool shadow, float shadowX);\nfloat interfaceStyleTextWidth(float measured, const std::string& outline, bool shadow, float shadowX);\n'
    impl = r'''
// WoWBridge: native font style width padding (evidence/fontref/client_style_widths).
// The legacy face measure includes the standard one-pixel shadow. Native plain
// strings omit it; OUTLINE/THICKOUTLINE add4/8pixels, and positive shadow X adds
// its rounded physical extent. Negative X does not extend GetStringWidth.
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
'''
    _replace(hpp, [('float interfaceTextWidth(const std::string& text, const std::string& fontFace,\n',
                    decl + '\nfloat interfaceTextWidth(const std::string& text, const std::string& fontFace,\n')])
    _replace(cpp, [('float interfaceTextWidth(const std::string& text, const std::string& fontFace,\n',
                    impl + '\nfloat interfaceTextWidth(const std::string& text, const std::string& fontFace,\n')])
    _replace(ENGINE, [('w ? measureTextWidth(w->text, w->fontFace, w->fontHeight) : 0.0',
        'w ? wowee::ui::interfaceStyleTextWidth(measureTextWidth(w->text, w->fontFace, w->fontHeight),\n'
        '        w->fontOutline, w->hasShadow, w->shadowX) : 0.0'),
        ('lua_pushnumber(L, measureTextWidth(w->text, w->fontFace, w->fontHeight));',
         'lua_pushnumber(L, wowee::ui::interfaceStyleTextWidth(measureTextWidth(w->text, w->fontFace, w->fontHeight),\n'
         '            w->fontOutline, w->hasShadow, w->shadowX));'),
        ('        !w->text.empty() && w->text != w->measuredText;', '        !w->text.empty() && (w->text != w->measuredText || w->fontFace != w->measuredFace ||\n        wowee::ui::interfaceFontSize(w->fontHeight) != w->measuredSize ||\n        wowee::ui::interfaceFontStylePadding(w->fontOutline,w->hasShadow,w->shadowX) != w->measuredStylePadding);')])
    _replace(widget, [('    std::string measuredFace;\n', '    std::string measuredFace;\n    float measuredStylePadding = 0.0f;\n')])
    _replace(rend, [
        ('    const auto measureRun = [&](const std::string& piece) {\n        return interfaceMeasure(runFont, size, piece);\n    };',
         '    const float stylePadding = interfaceFontStylePadding(w->fontOutline, w->hasShadow, w->shadowX);\n'
         '    const auto measureRun = [&](const std::string& piece) {\n'
         '        return interfaceStyleTextWidth(interfaceMeasure(runFont, size, piece), w->fontOutline, w->hasShadow, w->shadowX);\n    };'),
        ('w->measuredFace == w->fontFace;', 'w->measuredFace == w->fontFace && w->measuredStylePadding == stylePadding;'),
        ('        w->measuredFace = w->fontFace;\n        return;', '        w->measuredFace = w->fontFace;\n        w->measuredStylePadding = stylePadding;\n        return;'),
        ('    w->measuredFace = w->fontFace;\n}', '    w->measuredFace = w->fontFace;\n    w->measuredStylePadding = stylePadding;\n}'),
        ('    const ImVec2 measured(interfaceMeasure(runFont, size, strippedText(w->text)), size);',
         '    const ImVec2 measured(measureRun(strippedText(w->text)), size);'),
        ('            const float boxW = x1 - x0, boxH = y1 - y0;\n',
         '            if (wrapW <= 0.0f) extent.x = interfaceStyleTextWidth(extent.x / s, w->fontOutline, w->hasShadow, w->shadowX) * s;\n'
         '            const float boxW = x1 - x0, boxH = y1 - y0;\n'),
    ])
    print('patched native font outline/shadow width padding')


def patch_retained_widget_runtime() -> None:
    """Preserve deployed host textures, taxi clipping and interrupted input gestures.

    These changes already exist in the committed vendor tree. Keep exact unique
    anchors so a changed upstream implementation fails instead of losing a fix.
    """
    patches = {
        'include/ui/widget_renderer.hpp': [
            (r'''// in its rect (pixels): batches of triangles, each with its texture and whether it adds light. Set once; called for
// every frame drawn, so it answers nothing for frames that are not its own.
struct HostModelVertex { float x, y, u, v; uint32_t col; };
struct HostModelBatch { std::string texture; bool add = false; std::vector<HostModelVertex> triangles; };
using HostModelHook = std::function<void(const Widget& w, float x0, float y0, float x1, float y1,
                                         std::vector<HostModelBatch>& out)>;
void setHostModelHook(HostModelHook hook);
''', r'''// in its rect (pixels): batches of triangles, each with its texture and whether it adds light. Set once; called for
// every frame drawn, so it answers nothing for frames that are not its own.
struct HostModelVertex { float x, y, u, v; uint32_t col; };
struct HostModelBatch { uint64_t externalTexture = 0; std::string texture; bool add = false; std::vector<HostModelVertex> triangles; };
using HostModelHook = std::function<void(const Widget& w, float x0, float y0, float x1, float y1,
                                         std::vector<HostModelBatch>& out)>;
void setHostModelHook(HostModelHook hook);
'''),
        ],
        'src/ui/widget_renderer.cpp': [
            (r'''                hostModelHook()(*w, x0, y0, x1, y1, batches);
                if (!batches.empty()) dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
                for (const HostModelBatch& b : batches) {
                    const VkDescriptorSet tex = resident(b.texture);   // WoWBridge: light that adds: its own colours, added
                    if (tex == kMissing) { hostModelPending().insert({b.texture, false}); continue; }
                    if (b.add) dl->AddCallback(HostAdditiveBlend, nullptr);
                    dl->PushTexture(ImTextureRef(reinterpret_cast<ImTextureID>(tex)));
''', r'''                hostModelHook()(*w, x0, y0, x1, y1, batches);
                if (!batches.empty()) dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
                for (const HostModelBatch& b : batches) {
                    const VkDescriptorSet tex = b.externalTexture ? reinterpret_cast<VkDescriptorSet>(b.externalTexture) : resident(b.texture);   // WoWBridge: light that adds: its own colours, added
                    if (tex == kMissing) { hostModelPending().insert({b.texture, false}); continue; }
                    if (b.add) dl->AddCallback(HostAdditiveBlend, nullptr);
                    dl->PushTexture(ImTextureRef(reinterpret_cast<ImTextureID>(tex)));
'''),
            (r'''                // angle. WoW's order is upper-left, lower-left, upper-right,
                // lower-right; the quad wants them going round the rect.
                const float* q = w->texCoordQuad;
                dl->AddImageQuad(reinterpret_cast<ImTextureID>(tex),
                                 ImVec2(x0, y0), ImVec2(x1, y0),
                                 ImVec2(x1, y1), ImVec2(x0, y1),
                                 ImVec2(q[0], q[1]),   // upper-left
''', r'''                // angle. WoW's order is upper-left, lower-left, upper-right,
                // lower-right; the quad wants them going round the rect.
                const float* q = w->texCoordQuad;
                if (w->texturePath == "Interface\\TaxiFrame\\UI-Taxi-Line") {
                    // 1.12 DrawRouteLine rotates the texture inside a bounding
                    // rectangle using UVs beyond [0,1]. Outside its source is
                    // transparent; our shared CPU/GPU sampler repeats instead.
                    // Clip geometry in UV space so both backends draw exactly
                    // one route, without repeated parallel lines.
                    struct Vertex { ImVec2 p, uv; };
                    std::vector<Vertex> polygon = {
                        {{x0,y0},{q[0],q[1]}}, {{x1,y0},{q[4],q[5]}},
                        {{x1,y1},{q[6],q[7]}}, {{x0,y1},{q[2],q[3]}}
                    };
                    for (int edge=0; edge<4 && !polygon.empty(); ++edge) {
                        const auto distance=[&](const Vertex& v) {
                            const float value=edge<2 ? v.uv.x : v.uv.y;
                            return edge%2 ? 1-value : value;
                        };
                        std::vector<Vertex> clipped;
                        for (size_t i=0;i<polygon.size();++i) {
                            const auto a=polygon[i], b=polygon[(i+1)%polygon.size()];
                            const float da=distance(a), db=distance(b);
                            if (da>=0) clipped.push_back(a);
                            if ((da<0 && db>0) || (da>0 && db<0)) {
                                const float t=da/(da-db);
                                clipped.push_back({{a.p.x+t*(b.p.x-a.p.x),a.p.y+t*(b.p.y-a.p.y)},
                                    {a.uv.x+t*(b.uv.x-a.uv.x),a.uv.y+t*(b.uv.y-a.uv.y)}});
                            }
                        }
                        polygon=std::move(clipped);
                    }
                    for (size_t i=1;i+1<polygon.size();++i) {
                        const auto a=polygon[0], b=polygon[i], c=polygon[i+1];
                        dl->AddImageQuad(reinterpret_cast<ImTextureID>(tex),a.p,b.p,c.p,c.p,
                            a.uv,b.uv,c.uv,c.uv,packColor(textureColor,w->alpha));
                    }
                } else dl->AddImageQuad(reinterpret_cast<ImTextureID>(tex),
                                 ImVec2(x0, y0), ImVec2(x1, y0),
                                 ImVec2(x1, y1), ImVec2(x0, y1),
                                 ImVec2(q[0], q[1]),   // upper-left
'''),
        ],
        'src/ui/framexml_emitter.cpp': [
            (r'''        if (const std::string* j = node.attr("justifyH")) {
            line(var + ":SetJustifyH(" + quote(*j) + ")");
        }
        // Do not overwrite anchors inherited from a texture template.
        // Button art and slider thumbs retain their explicit sizes.
        if (node.attrBool("setAllPoints") ||
            (isTexture && node.name == "Texture" && !node.attrBool("virtual") &&
             !node.attr("inherits") && !node.child("Anchors"))) {
''', r'''        if (const std::string* j = node.attr("justifyH")) {
            line(var + ":SetJustifyH(" + quote(*j) + ")");
        }
        // XML layer textures without anchors cover their owner. StackSplitFrame
        // declares a 256x32 texture but samples 172x96 art into its 172x96 frame;
        // centring the stated size instead leaves the arrows outside the background.
        // Do not overwrite anchors inherited from a texture template. Button
        // art and slider thumbs also retain their explicit sizes; their owner
        // places them. Only ordinary unanchored layer textures fill the owner.
        if (node.attrBool("setAllPoints") ||
            (isTexture && node.name == "Texture" && !node.attrBool("virtual") &&
             !node.attr("inherits") && !node.child("Anchors"))) {
'''),
            (r'''        // with no size, so its centre is the screen's: PlayerFrame's name sat
        // in the middle of the world because two frames above it said
        // setAllPoints and were heard by nobody.
        // Do not overwrite anchors inherited from a texture template.
        // Button art and slider thumbs retain their explicit sizes.
        if (node.attrBool("setAllPoints") ||
            (isTexture && node.name == "Texture" && !node.attrBool("virtual") &&
             !node.attr("inherits") && !node.child("Anchors"))) {
            line(var + ":SetAllPoints(" + parentVar + ")");
        }
        if (const XmlNode* anchors = node.child("Anchors")) {
''', r'''        // with no size, so its centre is the screen's: PlayerFrame's name sat
        // in the middle of the world because two frames above it said
        // setAllPoints and were heard by nobody.
        if (node.attrBool("setAllPoints")) {
            line(var + ":SetAllPoints(" + parentVar + ")");
        }
        if (const XmlNode* anchors = node.child("Anchors")) {
'''),
        ],
        'src/ui/interface_fonts.cpp': [
            (r'''        if (FT_Load_Char(m.face, cp, FT_LOAD_NO_SCALE) == 0) {
            const FT_Glyph_Metrics& gm = m.face->glyph->metrics;
            g.first = static_cast<int>(gm.horiAdvance);
            if (gm.width > 0 && gm.horiBearingY > 0) g.second = static_cast<int>(gm.horiBearingX + gm.width);
        }
        it = m.glyphs.emplace(cp, g).first;
''', r'''        if (FT_Load_Char(m.face, cp, FT_LOAD_NO_SCALE) == 0) {
            const FT_Glyph_Metrics& gm = m.face->glyph->metrics;
            g.first = static_cast<int>(gm.horiAdvance);
            // WoWBridge: outlines below the baseline use advance width (the
            // recorded underscore samples at all three sizes, evidence/fontref).
            if (gm.width > 0 && gm.horiBearingY > 0) g.second = static_cast<int>(gm.horiBearingX + gm.width);
        }
        it = m.glyphs.emplace(cp, g).first;
'''),
            (r'''    for (const char* s = text.c_str(); s < end;) {
        unsigned int c = 0;
        s += ImTextCharFromUtf8(&c, s, end);
        if (c != ' ') font->RenderChar(dl, emPx, ImVec2(pen, y + 2.0f * std::min(gPixelScale, 1.0f)), col, static_cast<ImWchar>(c));
        pen += static_cast<float>(penOf(*m, P, c));
    }
''', r'''    for (const char* s = text.c_str(); s < end;) {
        unsigned int c = 0;
        s += ImTextCharFromUtf8(&c, s, end);
        // Captures place the glyph origin about two pixels below ImGui's.
        // Smaller UI scales reduce it; do not double this raster offset in
        // 2560 captures. Shadow and outline use the same measured origin.
        if (c != ' ') font->RenderChar(dl, emPx, ImVec2(pen, y + 2.0f * std::min(gPixelScale, 1.0f)), col, static_cast<ImWchar>(c));
        pen += static_cast<float>(penOf(*m, P, c));
    }
'''),
        ],
        'include/addons/lua_engine.hpp': [
            (r'''    /// highlight and its tooltip for as long as the cursor stays away - the
    /// tree's last word on the subject is that the cursor is still on it.
    void releaseMouseHover();

    /// The wheel, to the frame under the cursor that asked for it.
    ///
''', r'''    /// highlight and its tooltip for as long as the cursor stays away - the
    /// tree's last word on the subject is that the cursor is still on it.
    void releaseMouseHover();

    /// A native menu or focus change took the release. Stop the gesture without
    /// synthesizing a click or a drop on whatever is under the returning cursor.
    /// The carried item/spell remains the cursor API's responsibility.
    void cancelMousePress();

    /// The wheel, to the frame under the cursor that asked for it.
    ///
'''),
        ],
        'src/addons/lua_engine.cpp': [
            (r'''    const auto* w = widgetOf(L, 1);
    using F = wowee::ui::Widget::Forced;
    const F f = w ? w->forcedState : F::None;
    lua_pushstring(L, f == F::Pushed ? "PUSHED"
                    : f == F::Disabled ? "DISABLED" : "NORMAL");
    return 1;
}
int lua_Button_LockHighlight(lua_State* L) {
''', r'''    const auto* w = widgetOf(L, 1);
    using F = wowee::ui::Widget::Forced;
    const F f = w ? w->forcedState : F::None;
    const auto* tree = wowee::addons::getWidgetTree(L);
    const bool pressed = w && tree && tree->pressedWidget() == w->id;
    lua_pushstring(L, w && (!w->enabled || f == F::Disabled) ? "DISABLED"
                    : f == F::Pushed || (f == F::None && pressed) ? "PUSHED" : "NORMAL");
    return 1;
}
int lua_Button_LockHighlight(lua_State* L) {
'''),
            (r'''           widgets_.sizingWidget() != 0;
}

void LuaEngine::releaseMouseHover() {
    if (!L_) return;
    if (hoverWid_ != 0) {
''', r'''           widgets_.sizingWidget() != 0;
}

void LuaEngine::cancelMousePress() {
    if (L_ && draggingWid_ != 0)
        callFrameScript(draggingWid_, "OnDragStop", draggingButton_ == 1 ? "RightButton" : "LeftButton");
    draggingWid_ = 0;
    draggingButton_ = -1;
    for (int b = 0; b < kMouseButtons; ++b) {
        pressedWid_[b] = 0;
        buttonDown_[b] = false;
    }
    pickerPart_ = 0;
    lastClickWid_ = 0;
    widgets_.setMovingWidget(0);
    widgets_.setSizingWidget(0, "");
    releaseMouseHover();
}

void LuaEngine::releaseMouseHover() {
    if (!L_) return;
    if (hoverWid_ != 0) {
'''),
            (r'''    // draws the same line: below the threshold it is a click, above it the
    // frame's OnDragStart runs and the click is abandoned.
    if (draggingWid_ == 0) {
        constexpr float kDragThreshold = 4.0f;   // interface units
        for (int i = 0; i < kMouseButtons; ++i) {
            if (!buttonDown_[i] || pressedWid_[i] == 0) continue;
            // Up through the parents until something is registered for this
''', r'''    // draws the same line: below the threshold it is a click, above it the
    // frame's OnDragStart runs and the click is abandoned.
    if (draggingWid_ == 0) {
        const float kDragThreshold = 4.0f / widgets_.uiScale();   // window pixels, independent of UI scale
        for (int i = 0; i < kMouseButtons; ++i) {
            if (!buttonDown_[i] || pressedWid_[i] == 0) continue;
            // Up through the parents until something is registered for this
'''),
            (r'''                        // back. An item off an action bar has no such prompt -
                        // the bar held a reference, not the item - so it still
                        // clears here, which is what takes it off the bar.
                        if (onWorld && !cursorItemCameFromInventory()) {
                            lua_getglobal(L_, "ClearCursor");
                            if (lua_isfunction(L_, -1)) {
                                if (lua_pcall(L_, 0, 0, 0) != 0) lua_pop(L_, 1);
''', r'''                        // back. An item off an action bar has no such prompt -
                        // the bar held a reference, not the item - so it still
                        // clears here, which is what takes it off the bar.
                        // The host routes inventory releases after dispatch. Clearing
                        // here cancels the held pile before that route can drop it.
                        bool inventoryCursor = false;
                        lua_getglobal(L_, "__WBBagCursorHeld");
                        if (lua_isfunction(L_, -1)) {
                            if (lua_pcall(L_, 0, 1, 0) == 0)
                                inventoryCursor = lua_toboolean(L_, -1);
                            lua_pop(L_, 1);
                        } else lua_pop(L_, 1);
                        if (onWorld && !inventoryCursor) {
                            lua_getglobal(L_, "ClearCursor");
                            if (lua_isfunction(L_, -1)) {
                                if (lua_pcall(L_, 0, 0, 0) != 0) lua_pop(L_, 1);
'''),
            (r'''                    }
                }
            }
            pressedWid_[i] = 0;
        }
    }
}

/// The frame a click on `wid` belongs to: the nearest one, itself or above it,
''', r'''                    }
                }
            }
            // A swapped spell remains held after its drag has ended. A later world
            // click must release it too, even though that click has no drag owner.
            const auto* under = hit ? widgets_.get(hit) : nullptr;
            const auto* origin = pressedWid_[i] ? widgets_.get(pressedWid_[i]) : nullptr;
            const auto isWorld = [](const ui::Widget* w) {
                return !w || w->name == "UIParent" || w->name == "WorldFrame";
            };
            if (i < 2 && !draggingThisButton && !tookLink && isWorld(under) && isWorld(origin)) {
                lua_getglobal(L_, "CursorHasSpell");
                bool heldSpell = false;
                if (lua_isfunction(L_, -1)) {
                    if (lua_pcall(L_, 0, 1, 0) == 0) heldSpell = lua_toboolean(L_, -1);
                    lua_pop(L_, 1);
                } else lua_pop(L_, 1);
                if (heldSpell) {
                    lua_getglobal(L_, "ClearCursor");
                    if (lua_isfunction(L_, -1)) {
                        if (lua_pcall(L_, 0, 0, 0) != 0) lua_pop(L_, 1);
                    } else lua_pop(L_, 1);
                }
            }
            pressedWid_[i] = 0;
        }
    }
    // Lua's GetButtonState and model OnUpdate see the current press/release in this frame.
    widgets_.setInteraction(hit, buttonDown_[0] ? pressedWid_[0] : 0);
}

/// The frame a click on `wid` belongs to: the nearest one, itself or above it,
'''),
        ],
    }
    for relative, pairs in patches.items():
        path = WW / relative
        text = path.read_text()
        for old, new in pairs:
            if new in text:
                continue
            if text.count(old) != 1:
                raise SystemExit(f'patch_wowee: unique retained anchor not found in {relative}: {old[:60]}')
            text = text.replace(old, new, 1)
        path.write_text(text)


def patch_font_readback() -> None:
    """Keep native requested heights and API flags through GetFont/SetFont."""
    if '// Native GetFont returns the requested height' in ENGINE.read_text():
        return
    _replace(ENGINE, [(r'''    lua_pushnumber(L, wowee::ui::interfaceFontSize(w ? w->fontHeight : 0.0f));
    lua_pushstring(L, (w && !w->fontOutline.empty()) ? w->fontOutline.c_str() : "");
''', r'''    // Native GetFont returns the requested height, independent of raster scale.
    // FrameXML reads this back into SetFont when resizing chat and quest text.
    lua_pushnumber(L, w && w->fontHeight > 0.0f ? w->fontHeight : wowee::ui::interfaceFontSize(0.0f));
    // Internal renderer names must not leak into SetFont's public flags.
    lua_pushstring(L, !w ? "" : w->fontOutline == "NORMAL" ? "OUTLINE"
        : w->fontOutline == "THICK" ? "THICKOUTLINE" : w->fontOutline.c_str());
''')])
    print('patched native font readback')


def patch_classic_interface_probe() -> None:
    """The automatic character-sheet diagnostic must work with the 1.12 API."""
    if '// WoWBridge: classic has GetPlayerBuff' in ENGINE.read_text():
        return
    _replace(ENGINE, [(r'''        "for i = 1, 40 do if not UnitAura('player', i) then break end auras = i end\n"
''', r'''        // WoWBridge: classic has GetPlayerBuff, not the later UnitAura API.
        "if type(UnitAura) == 'function' then\n"
        "  for i = 1, 40 do if not UnitAura('player', i) then break end auras = i end\n"
        "else\n"
        "  for _, filter in ipairs({'HELPFUL','HARMFUL'}) do\n"
        "    for i = 0, 31 do\n"
        "      local index = GetPlayerBuff(i, filter)\n"
        "      if not index or index < 0 then break end\n"
        "      auras = auras + 1\n"
        "    end\n"
        "  end\n"
        "end\n"
''')])
    print('patched classic interface aura diagnostic')


def apply_all(root: Path | None = None) -> None:
    """Apply the same ordered patch bundle for refreshes and direct invocation."""
    global WW, ENGINE
    previous = WW, ENGINE
    if root is not None:
        WW = Path(root)
        ENGINE = WW / 'src/addons/lua_engine.cpp'
    try:
        patch_engine()
        patch_fonts()
        patch_emitter()
        patch_keys()
        patch_m2()
        patch_unwrap()
        patch_events()
        patch_font_inherits()
        patch_alpha()
        patch_font_order()
        patch_text_metrics()
        patch_model_frames()
        patch_vanilla_header()
        patch_vanilla_ranges()
        patch_host_blend()
        patch_message_frames()
        patch_message_scroll()
        patch_layout_dependencies()
        patch_tooltip_anchors()
        patch_desaturated()
        patch_desaturated_tint()
        patch_region_layout_fidelity()
        patch_glyph_origin_fidelity()
        patch_checked_string_fidelity()
        patch_below_baseline_width()
        patch_show_descendants_fidelity()
        patch_compass_art_ready()
        patch_font_style_widths()
        patch_retained_widget_runtime()
        patch_font_readback()
        patch_classic_interface_probe()
    finally:
        WW, ENGINE = previous


if __name__ == '__main__':
    apply_all()
