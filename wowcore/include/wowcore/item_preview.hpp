#pragma once

#include "wowcore/character.hpp"

#include <array>

namespace wowcore {
// From the realm's item_template (gear.json), keyed by item entry. ItemDisplayInfo is keyed by display ID,
// not entry; the client archives alone cannot supply this mapping.
struct ItemDisplay {
    uint32_t displayId = 0;
    uint32_t inventoryType = 0;
};
using ItemDisplayCatalog = std::map<uint32_t, ItemDisplay>;

enum class ItemPreviewStatus {
    Ready, MissingItem, MissingDisplay, NoStandaloneModel, UnsupportedInventoryType,
    InvalidAppearance, MissingModel, MissingTexture
};
const char* itemPreviewStatusName(ItemPreviewStatus status);

// Owned, movable CPU assets; batchTextures is parallel to model.batches and indexes images by path.
// Shoulders return left then right as separate parts in their own model spaces, without invented placement.
struct ItemPreviewPart {
    Model model;
    std::map<std::string, Image> images;
    std::vector<std::string> batchTextures;
    glm::mat4 modelToPreview{1.0f};
};
struct ItemPreview {
    ItemPreviewStatus status = ItemPreviewStatus::MissingItem;
    uint32_t entry = 0, displayId = 0;
    std::string detail;   // missing path or unsupported type
    std::vector<ItemPreviewPart> parts;  // published only when all requested assets are ready
    bool armorOnly = false;  // a piece cut from the character model, never a dressed figure
};

// Standalone helm (race/sex variant), shoulders, weapons, ranged weapons, shields and held offhands.
// Body armour/capes use loadArmorItemPreview instead. Only the first texture stage per batch is resolved,
// matching Avatar::Resolve; this is not a complete dressing-room or item-enchantment renderer.
ItemPreview loadItemPreview(const Mpq& mpq, uint32_t entry, const ItemDisplayCatalog& catalog,
                            const Appearance& who = {});

// Body armour, shirts, robes, capes and tabards: their equipment slot, or no character preview policy.
std::optional<uint32_t> characterPreviewSlot(uint32_t inventoryType);
// The selected item's body-atlas UV rectangles (min x/y, max x/y), and its visible geosets.
// Rectangles may be smaller than their atlas region when a component has transparent margins.
struct ItemBodySelection {
    std::vector<glm::vec4> regions;
    std::set<uint16_t> geosets;
};
std::set<uint16_t> itemBodyGeosets(uint32_t inventoryType, const std::array<uint32_t, 3>& groups);
// Clip to the selected UV rectangles/geosets, baking Stand t=0 before creating boundary vertices.
// Only referenced geometry survives. Invalid geometry or an empty selection returns false, without mutation.
bool isolateItemBody(Model& model, const ItemBodySelection& selection);
// Only this item's textures and geometry on their race/sex body: no skin, hair, outfit or attachments.
// Owned batch bindings; missing assets and empty geometry return failure without partial parts.
ItemPreview loadArmorItemPreview(const Mpq& mpq, uint32_t entry, const ItemDisplayCatalog& catalog,
                                 const Appearance& who);
}  // namespace wowcore
