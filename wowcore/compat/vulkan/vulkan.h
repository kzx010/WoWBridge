// WoWBridge compat: WoWee's renderer only passes VkDescriptorSet through as an opaque ImTextureID. Here it is the
// host's texture handle (ID3D11ShaderResourceView* in the plugin, a CPU texture in native tests).
#pragma once
typedef struct VkDescriptorSet_T* VkDescriptorSet;
#define VK_NULL_HANDLE nullptr
