// CPU renderer for ImGui draw data: the same triangles, textures and blending a GPU backend gets, rasterised to an
// RGBA image so native tests can compare the WoW interface against screenshots of the real client.
#pragma once
#include "rendering/vk_context.hpp"
#include "wowcore/image.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct ImDrawData;

namespace wowcore {
// Texture store for WoWee's widget renderer (uploadImGuiTexture) and for ImGui 1.92's own textures (the font atlas,
// through ImDrawData::Textures). Handles are 1-based indices.
class SoftTextures : public wowee::rendering::VkContext {
public:
    VkDescriptorSet uploadImGuiTexture(const uint8_t* rgba, int width, int height) override;
    bool updateImGuiTexture(VkDescriptorSet texture, const uint8_t* rgba, int width, int height) override;
    void sync(ImDrawData* drawData);              // serve ImGui's create / update / destroy requests
    const std::vector<Image>* get(uint64_t id) const;   // mip chain, level 0 first; nullptr if none

private:
    std::vector<std::vector<Image>> images_;
};

enum class Filter { Bilinear, Trilinear };   // trilinear: mip level from the triangle's texel-to-pixel ratio

// Draws `drawData` over `target`: bilinear sampling with wrap addressing, vertex colour modulation, clip rects, and
// ImGui's blend (colour: src alpha / one minus src alpha; alpha: one / one minus src alpha). Each pixel is covered
// once along shared triangle edges (top-left rule), so a quad's diagonal is not blended twice.
void rasterize(const ImDrawData* drawData, const SoftTextures& textures, Image& target, Filter filter = Filter::Trilinear);

bool writePng(const std::string& path, const Image& image);
}  // namespace wowcore
