# Vendored from WoWee

Upstream: https://github.com/Kelsidavis/WoWee @ `b8b6242d07ad87cc82743e9a9f419904ae1d6985`
License: MIT with an additional non-commercial-game restriction (LICENSE). WoWBridge is a free personal mod.

Files are copied unmodified, then `tools/patch_wowee.py` applies WoWBridge's fences (see its docstring).

- include/ui/widget_tree.hpp
- include/ui/widget_renderer.hpp
- include/ui/xml_parser.hpp
- include/ui/framexml_emitter.hpp
- include/ui/text_markup.hpp
- include/ui/text_wrap.hpp
- include/ui/simple_html.hpp
- include/ui/link_hit.hpp
- include/ui/interface_fonts.hpp
- include/ui/framexml_takeover.hpp
- include/ui/ui_colors.hpp
- include/ui/plural_escape.hpp
- src/ui/widget_tree.cpp
- src/ui/widget_renderer.cpp
- src/ui/xml_parser.cpp
- src/ui/framexml_emitter.cpp
- src/ui/interface_fonts.cpp
- src/ui/framexml_takeover.cpp
- src/ui/plural_escape.cpp
- include/addons/lua_engine.hpp
- include/addons/lua_services.hpp
- include/addons/lua_handler_globals.hpp
- include/addons/lua_api_registrations.hpp
- include/addons/toc_parser.hpp
- src/addons/lua_engine.cpp
- src/addons/lua_handler_globals.cpp
- src/addons/toc_parser.cpp
- include/pipeline/blp_loader.hpp
- include/pipeline/dxt_block.hpp
- src/pipeline/blp_loader.cpp
- include/pipeline/m2_loader.hpp
- include/pipeline/m2_color_track.hpp
- src/pipeline/m2_loader.cpp
- include/rendering/m2_track_sampler.hpp
- tests/test_widget_tree.cpp
- tests/test_framexml.cpp
- tests/test_text_markup.cpp
- tests/test_text_wrap.cpp
- tests/test_simple_html.cpp
- tests/test_lua_handler_globals.cpp
- tests/test_blp_loader.cpp
- include/core/env_flag.hpp
- LICENSE
