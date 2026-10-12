# Wraps CommonLibSSE-NG's Linux clang-cl/xwin toolchain and adds project-wide defines that must
# also reach every vcpkg dependency (this file is the chainload toolchain for our overlay triplet).
include("$ENV{COMMONLIB_DIR}/cmake/toolchain-linux-clangcl.cmake")

# MSVC STL >= 14.40 made std::mutex constexpr; with the older msvcp140.dll that ships in Proton
# prefixes (14.0, 2016) the first lock() is an access violation. Opt out everywhere.
set(_wb_defines "-D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR")
if(NOT CMAKE_CXX_FLAGS MATCHES "_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR")
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} ${_wb_defines}" CACHE STRING "" FORCE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${_wb_defines}" CACHE STRING "" FORCE)
endif()
