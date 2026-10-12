#pragma once
#include <cstdint>
#include <vector>

namespace wowcore {
// An RGBA8 image, top row first.
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    Image() = default;
    Image(int width, int height) : w(width), h(height), rgba(static_cast<size_t>(width) * height * 4, 0) {}
};
}  // namespace wowcore
