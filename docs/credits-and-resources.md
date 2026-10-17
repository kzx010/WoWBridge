# Credits and resources

The ledger of every outside work WoWBridge uses, ports from, or consulted, kept current as work lands so the README's
Credits section and the release bundle's `licenses/` directory can be regenerated from it. Add a row the moment a
feature borrows a technique, a table, a constant or a file; say what was taken, not just the name.

Legend: **Bundled** = code or data shipped in WoWBridge or the tester bundle (notice required in `licenses/`, see
`tools/release.py`). **Ported** = a technique or function reimplemented from the project's source (attribution in the
README Credits and in a comment at the port). **Consulted** = read for understanding or verification; nothing copied.

## Shipped or ported

| Work | Licence | Relationship | What WoWBridge takes | Where |
|---|---|---|---|---|
| WoWee (Wowee Contributors) | MIT with an additional restriction (no commercial game products) | Bundled (vendored, patched) | M2/BLP/DBC/MPQ loading pipeline, model rendering primitives | `wowcore/third_party/wowee` (PROVENANCE.md, `tools/vendor_wowee.py`, `tools/patch_wowee.py`) |
| CMaNGOS Classic (mangos-classic, classic-db) | GPL-2.0 | Bundled extracts (data), consulted (rules) | Spell, item, creature and resistance tables extracted at setup; combat/aura/threat formulas verified against its source | `data/` builders, `service/`, `docs/*` fidelity tables; licence copied by `tools/release.py` |
| CommonLibSSE-NG | MIT (hde64 notice) | Bundled (linked) | SKSE plugin framework, RE:: engine types, offsets | `skse/` |
| SKSE | MIT | Bundled (runtime dependency) | Plugin loader, trampoline | `skse/`, setup |
| StormLib (Ladislav Zezula) | MIT | Bundled (linked) | MPQ archive reading | `wowcore` |
| Dear ImGui | MIT | Bundled (linked) | Diagnostic overlay | `skse/src/ui` |
| Lua 5.1.5 | MIT | Bundled | FrameXML/addon interpreter | `wowcore/third_party/lua-5.1.5` |
| Catch2 | BSL-1.0 | Bundled (tests only) | wowcore unit tests | `wowcore/third_party/catch2` |
| vcpkg ports used by the plugin | each port's own | Bundled (linked) | per `build/skse/vcpkg_installed/*/share/*/copyright`, collected by `tools/release.py` | |
| SkyCraft (chasmlol) | MIT | Ported | The in-frame D3D11 world pass technique: the RenderWorld hook site (REL::ID(36559)+0x85E), camera-relative view from `worldToCam`, drawing into kMAIN + motion vectors depth-tested against the engine's depth, state backup/restore; the app-culled-native-body approach for the player (Surrogate); M10 shadows (October 16): the sun-shadow cascade capture (`BSShadowDirectionalLight::Render` vfunc hook, `CaptureSunShadows`, `DepthReadFormat`, `SetSkyrimShadows` with its runtime row/column and depth-direction detection, `SkyrimSunShadow` PCF, the probe shader) and the shadow-mask ground overlay with blob AO | `skse/src/render/WorldPass.cpp` (header comment), `skse/src/Surrogate.h`, `skse/src/render/Shadows.cpp` + `skse/shaders/WowShadow.hlsl` (function names cited in comments), `docs/shadows.md` |
| SmoothCam | MIT | Ported (small) | Havok `hkpRayHitCollector` vtable layout and the cursor-pick collision layer set | `skse/src/Picker.cpp` |
| TrueDirectionalMovement (Ersh) | GPL-3.0 | Consulted (no code copied) | `hkpWorld::CastRay` under the world's read lock (the call sequence); how vanilla's `ThirdPersonState` vfunc 0xD folds free rotation into the heading while weapons are drawn, which shaped the self-test-only front-view hook (independent implementation; GPL, so keep it consulted only) | `skse/src/Picker.cpp`, `skse/src/Hooks.cpp` |
| WoWDBDefs (wowdev) | CC BY-SA 4.0 (definitions) / BSD-3-Clause (code) | Bundled (definitions, as our column layouts) | DBC column layouts for client build 1.12.1.5875, incl. HelmetGeosetVisData (which hair, beard and ears a helm hides on NPCs) | `data/dbc.py`, `wowcore/src/dbc*`, docs |
| wow-ui-source (Gethe) | Blizzard's FrameXML, no licence (Blizzard-owned); the repository is a reference copy | Consulted / compared | The 1.12 FrameXML the client's own files are compared against (line-ending-insensitive match); nothing shipped from it: the user's installed client provides the FrameXML at runtime | `docs/*` (FrameXML match), `wowcore/tools/framexml_probe.cpp` |

## Consulted only

| Work | Licence | What was looked at |
|---|---|---|
| wheeler (dTry) | BSD-3-Clause | ImGui-in-SKSE rendering/input conventions |
| SpellHotbar2 (pWn3d1337) | MIT | SKSE hotbar input and SWF conventions |
| SKSE-Menu-Framework-3 | GPL-3.0 | Menu hooking conventions (GPL: never copy) |
| Address Library for SKSE (meh321) | — | Runtime address resolution; `tools/addrlib.py` reads its database |
| Community Shaders (training knowledge only, no local copy) | GPL-3.0 | Named in the M10 recon for how the engine's lighting consumes the shadow mask; nothing used |

## Game data (not licensable by us)

World of Warcraft 1.12.1 (5875) client data and Skyrim Special Edition belong to Blizzard Entertainment and Bethesda
Softworks. WoWBridge ships none of it; setup reads the user's own installs. The README's disclaimer covers this.
