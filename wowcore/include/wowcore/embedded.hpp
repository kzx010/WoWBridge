// Text files compiled into wowcore (wowcore/cmake/embed.cmake).
#pragma once
#include <cstddef>

namespace wowcore {
struct EmbeddedFile {
    const char* name;
    const char* text;
    std::size_t size;
};
extern const EmbeddedFile kWowApiLua[];   // wowcore/lua, in load order; ends with a null entry
}  // namespace wowcore
