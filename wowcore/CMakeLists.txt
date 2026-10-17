# wowcore: portable WoW 1.12 runtime (MPQ, BLP, DBC, FrameXML UI). Builds natively on Linux for tests and with
# clang-cl into WoWBridge.dll (skse/CMakeLists.txt adds this directory).
cmake_minimum_required(VERSION 3.24)
project(wowcore LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set(WW ${TP}/wowee)

# Lua 5.1.5 (compat: vararg `arg`, math.mod, string.gfind, [[ ]] nesting -- what 1.12 FrameXML is written in)
file(GLOB LUA_SRC ${TP}/lua-5.1.5/src/*.c)
list(FILTER LUA_SRC EXCLUDE REGEX "/(lua|luac|print)\\.c$")
add_library(wowcore_lua STATIC ${LUA_SRC})
target_include_directories(wowcore_lua PUBLIC ${TP}/lua-5.1.5/src)
if(NOT MSVC)
    target_compile_definitions(wowcore_lua PRIVATE LUA_USE_POSIX)
endif()

# StormLib: vcpkg's port inside the DLL build; the locally built static library for native Linux tests.
set(WOWCORE_STORMLIB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../reference/StormLib" CACHE PATH "StormLib source+build-linux")
find_package(StormLib CONFIG QUIET)
if(StormLib_FOUND)
    set(WOWCORE_STORM StormLib::storm)
else()
    add_library(wowcore_storm STATIC IMPORTED)
    set_target_properties(wowcore_storm PROPERTIES IMPORTED_LOCATION ${WOWCORE_STORMLIB_ROOT}/build-linux/libstorm.a
        INTERFACE_INCLUDE_DIRECTORIES ${WOWCORE_STORMLIB_ROOT}/src INTERFACE_LINK_LIBRARIES "z;bz2")
    set(WOWCORE_STORM wowcore_storm)
endif()

# ImGui 1.92.8 + FreeType: vcpkg's imgui[freetype] in the DLL; the same release's sources for native tests.
find_package(imgui CONFIG QUIET)
if(imgui_FOUND)
    set(WOWCORE_IMGUI imgui::imgui)
else()
    set(WOWCORE_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../toolchain/vcpkg/buildtrees/imgui/src/v1.92.8-8e9c05eb59.clean"
        CACHE PATH "ImGui 1.92.8 source tree")
    find_package(Freetype REQUIRED)
    add_library(wowcore_imgui STATIC ${WOWCORE_IMGUI_DIR}/imgui.cpp ${WOWCORE_IMGUI_DIR}/imgui_draw.cpp
        ${WOWCORE_IMGUI_DIR}/imgui_tables.cpp ${WOWCORE_IMGUI_DIR}/imgui_widgets.cpp
        ${WOWCORE_IMGUI_DIR}/misc/freetype/imgui_freetype.cpp)
    target_include_directories(wowcore_imgui PUBLIC ${WOWCORE_IMGUI_DIR})
    target_compile_definitions(wowcore_imgui PUBLIC IMGUI_ENABLE_FREETYPE)
    target_link_libraries(wowcore_imgui PUBLIC Freetype::Freetype)
    set(WOWCORE_IMGUI wowcore_imgui)
endif()

# WoWBridge's WoW API (Lua), compiled in, in load order.
set(WOWAPI_LUA ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi.lua ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_tooltip.lua
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_layout.lua
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_live.lua
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_defaults.lua ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_login.lua
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_recorded.lua ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_model.lua
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_party.lua
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_nameplates.lua
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_bubbles.lua
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_tracking.lua
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_minimap.lua
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_worldmap.lua
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_quests.lua
    ${CMAKE_CURRENT_SOURCE_DIR}/../data/bag-icons.lua ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_bags.lua
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_character.lua
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_customize.lua
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_merchant.lua
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_loot.lua
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_trainer.lua
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_taxi.lua
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_testing.lua
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/wowapi_mounts.lua)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/wowapi_lua.cpp
    COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/wowapi_lua.cpp -DSYMBOL=kWowApiLua
            "-DFILES=${WOWAPI_LUA}" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${WOWAPI_LUA} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake VERBATIM)

add_library(wowcore STATIC
    src/mpq.cpp
    src/dbc.cpp
    src/api/wowapi.cpp
    src/api/apiref.cpp
    src/event_log.cpp
    src/soft_raster.cpp
    src/model.cpp
    src/minimap.cpp
    src/worldmap.cpp
    src/character.cpp
    src/player_model.cpp
    src/effect.cpp
    src/ui_session.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/wowapi_lua.cpp
    src/compat.cpp
    src/framexml.cpp
    src/api/shim.cpp
    ${WW}/src/ui/plural_escape.cpp
    ${WW}/src/ui/widget_tree.cpp
    ${WW}/src/ui/widget_renderer.cpp
    ${WW}/src/ui/xml_parser.cpp
    ${WW}/src/ui/framexml_emitter.cpp
    ${WW}/src/ui/framexml_takeover.cpp
    ${WW}/src/ui/interface_fonts.cpp
    ${WW}/src/addons/lua_engine.cpp
    ${WW}/src/addons/lua_handler_globals.cpp
    ${WW}/src/addons/toc_parser.cpp
    ${WW}/src/pipeline/blp_loader.cpp
    ${WW}/src/pipeline/m2_loader.cpp)
# compat/ first: our replacements shadow WoWee headers of the same name.
target_include_directories(wowcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/compat ${WW}/include)
find_package(ZLIB REQUIRED)
find_package(glm CONFIG REQUIRED)
target_link_libraries(wowcore PUBLIC wowcore_lua ${WOWCORE_IMGUI} glm::glm PRIVATE ${WOWCORE_STORM} ZLIB::ZLIB)

# Native tools (the probe, the API and HUD checks, MPQ extraction); not part of the DLL build.
option(WOWCORE_TOOLS "Build native tools" ${PROJECT_IS_TOP_LEVEL})
if(WOWCORE_TOOLS)
    add_executable(framexml_probe tools/framexml_probe.cpp)
    target_link_libraries(framexml_probe PRIVATE wowcore)
    target_compile_definitions(framexml_probe PRIVATE WOWBRIDGE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
    add_executable(mpq_extract tools/mpq_extract.cpp)
    add_executable(model_render tools/model_render.cpp)
    target_link_libraries(model_render PRIVATE wowcore)
    target_compile_definitions(model_render PRIVATE WOWBRIDGE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
    add_executable(hud_render tools/hud_render.cpp)
    target_link_libraries(hud_render PRIVATE wowcore)
    target_compile_definitions(hud_render PRIVATE WOWBRIDGE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
    add_executable(api_check tools/api_check.cpp)
    target_link_libraries(api_check PRIVATE wowcore)
    target_compile_definitions(api_check PRIVATE WOWBRIDGE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_link_libraries(mpq_extract PRIVATE wowcore)
endif()

option(WOWCORE_TESTS "Build native unit tests" ${PROJECT_IS_TOP_LEVEL})
if(WOWCORE_TESTS)
    add_library(wowcore_catch2 STATIC ${TP}/catch2/catch_amalgamated.cpp)
    target_include_directories(wowcore_catch2 PUBLIC ${TP}/catch2)
    add_executable(wowcore_tests
        ${WW}/tests/test_widget_tree.cpp ${WW}/tests/test_framexml.cpp ${WW}/tests/test_text_markup.cpp
        ${WW}/tests/test_text_wrap.cpp ${WW}/tests/test_simple_html.cpp ${WW}/tests/test_blp_loader.cpp
        ${WW}/tests/test_lua_handler_globals.cpp
        tests/test_client_data.cpp tests/test_wowapi.cpp tests/test_minimap.cpp tests/test_worldmap.cpp
        tests/test_bags.cpp tests/test_merchant.cpp tests/test_character.cpp tests/test_party.cpp tests/test_nameplates.cpp
        tests/test_integration.cpp tests/test_ui_profiles.cpp tests/test_resources.cpp tests/test_cast_persistence.cpp tests/test_native_text.cpp tests/test_loot.cpp tests/test_trainer.cpp tests/test_talent_wipe.cpp tests/test_selftest.cpp tests/test_taxi.cpp tests/test_testing.cpp tests/test_menus.cpp tests/test_customize.cpp tests/test_tooltip_audit.cpp)
    target_link_libraries(wowcore_tests PRIVATE wowcore wowcore_catch2)
    # Upstream tests that read WoWee's extracted 3.3.5a interface skip when it is absent; ours is 1.12 from MPQ.
    target_compile_definitions(wowcore_tests PRIVATE WOWEE_SOURCE_DIR="${CMAKE_CURRENT_BINARY_DIR}/no-wowee-data"
                                                    WOWBRIDGE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
    enable_testing()
    add_test(NAME wowcore_tests COMMAND wowcore_tests)
endif()
