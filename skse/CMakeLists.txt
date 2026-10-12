cmake_minimum_required(VERSION 3.25)
project(WoWBridge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded")

# Remove build-machine prefixes from __FILE__ and embedded debug information.
get_filename_component(WOWBRIDGE_WORKSPACE "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options("/clang:-ffile-prefix-map=${WOWBRIDGE_WORKSPACE}=/wowbridge")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options("-ffile-prefix-map=${WOWBRIDGE_WORKSPACE}=/wowbridge")
endif()

# CommonLibSSE-NG (alandtse ng): AE-only build -> covers runtime 1.7.104.
set(COMMONLIB_DIR "$ENV{COMMONLIB_DIR}" CACHE PATH "CommonLibSSE-NG checkout")
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_SE OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK ON CACHE BOOL "" FORCE)
add_subdirectory("${COMMONLIB_DIR}" CommonLibSSE EXCLUDE_FROM_ALL)
if(NOT COMMAND add_commonlibsse_plugin)
    include("${COMMONLIB_DIR}/cmake/CommonLibSSE.cmake")
endif()

file(GLOB_RECURSE WOWBRIDGE_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
add_commonlibsse_plugin(WoWBridge
    NAME "WoWBridge"
    AUTHOR "WoWBridge"
    USE_ADDRESS_LIBRARY
    SOURCES ${WOWBRIDGE_SOURCES})
target_include_directories(WoWBridge PRIVATE src)
target_precompile_headers(WoWBridge PRIVATE src/PCH.h)
target_compile_definitions(WoWBridge PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(WoWBridge PRIVATE d3d11.lib dxgi.lib ws2_32.lib psapi.lib)
target_link_options(WoWBridge PRIVATE /DEBUG /PDBALTPATH:WoWBridge.pdb)   # WoWBridge.pdb: names for a fault's offsets (main.cpp OnFault)

# HLSL -> DXBC headers at build time with fxc2 (Microsoft's d3dcompiler_47 under Wine), so the game
# never depends on Proton's runtime HLSL compiler.
set(FXC2 "$ENV{FXC2}" CACHE FILEPATH "fxc2.exe")
set(SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/shaders")
function(wb_shader FILE ENTRY PROFILE)
    get_filename_component(stem "${FILE}" NAME_WE)
    set(out "${SHADER_OUT}/${stem}_${ENTRY}.h")
    add_custom_command(OUTPUT "${out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT}"
        COMMAND ${CMAKE_COMMAND} -E env WINEDEBUG=-all wine "${FXC2}" /nologo /T${PROFILE} /E${ENTRY} /Vng_${stem}_${ENTRY} /Fh${out} "${FILE}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"  # fxc2 would parse an absolute /path as an option
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${FILE}" VERBATIM)
    target_sources(WoWBridge PRIVATE "${out}")
endfunction()
wb_shader(shaders/Debug.hlsl VSMain vs_5_0)
wb_shader(shaders/Debug.hlsl PSMain ps_5_0)
wb_shader(shaders/WowModel.hlsl VSMain vs_5_0)
wb_shader(shaders/WowModel.hlsl PSMain ps_5_0)
wb_shader(shaders/WowQuad.hlsl VSMain vs_5_0)
wb_shader(shaders/WowQuad.hlsl PSMain ps_5_0)
wb_shader(shaders/WowDecal.hlsl VSMain vs_5_0)
wb_shader(shaders/WowDecal.hlsl PSMain ps_5_0)
wb_shader(shaders/ImGui.hlsl VSMain vs_4_0)
wb_shader(shaders/ImGui.hlsl PSMain ps_4_0)
find_package(imgui CONFIG REQUIRED)
target_link_libraries(WoWBridge PRIVATE imgui::imgui)
# The WoW runtime: MPQ, DBC, BLP, the 1.12 interface (FrameXML on WoWee's engine) and WoWBridge's API.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../wowcore" wowcore)
target_link_libraries(WoWBridge PRIVATE wowcore)
# vcpkg's StormLib (clang-cl) divides 128-bit integers: __udivti3 is compiler-rt's, not in MSVC's runtime.
file(GLOB CLANG_RT_BUILTINS "$ENV{LLVM_MINGW_BIN}/../lib/clang/*/lib/windows/libclang_rt.builtins-x86_64.a")
target_link_libraries(WoWBridge PRIVATE ${CLANG_RT_BUILTINS})
# The reference character's state (generated from the recorded client), until the gameplay bridge writes it live.
set(SLICE_STATE ${CMAKE_CURRENT_SOURCE_DIR}/../wowcore/tests/fixtures/elunara.lua)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/slice_state.cpp
    COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/slice_state.cpp -DSYMBOL=kSliceState "-DFILES=${SLICE_STATE}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/../wowcore/cmake/embed.cmake
    DEPENDS ${SLICE_STATE} VERBATIM)
target_sources(WoWBridge PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/slice_state.cpp)
find_path(Stb_INCLUDE_DIR stb_image_write.h REQUIRED)
target_include_directories(WoWBridge PRIVATE ${Stb_INCLUDE_DIR})
target_include_directories(WoWBridge PRIVATE "${SHADER_OUT}")
