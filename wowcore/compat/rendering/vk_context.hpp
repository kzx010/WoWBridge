// WoWBridge compat: the texture-upload surface WoWee's widget renderer needs, implemented by the host backend.
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
namespace wowee::rendering {
class VkContext {
public:
    virtual ~VkContext() = default;
    // RGBA8, top row first; the returned handle is drawn as an ImTextureID. Null on failure.
    virtual VkDescriptorSet uploadImGuiTexture(const uint8_t* rgba, int width, int height) = 0;
    virtual bool updateImGuiTexture(VkDescriptorSet, const uint8_t*, int, int) { return false; }
    // Bumped when the host loses every texture (device reset) so the renderer drops its cache.
    virtual uint32_t uiTextureGeneration() const { return 0; }
    virtual void beginUploadBatch() {}
    virtual void endUploadBatchSync() {}
};
}  // namespace wowee::rendering
