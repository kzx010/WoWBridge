#pragma once
// A 1.12 player character as the client dresses it: the body model, the body texture composited from the
// character's skin, face, markings, scalp and underwear and the worn items' texture components (CharSections,
// ItemDisplayInfo), the geosets its hair and gear show (CharHairGeosets, ItemDisplayInfo.GeosetGroup,
// HelmetGeosetVisData), and the models attached to it (helm, shoulders, weapons).
#include "wowcore/image.hpp"
#include "wowcore/model.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace wowcore {
class Mpq;

struct Appearance {
    uint32_t race = 4, sex = 1;    // ChrRaces id; 0 male, 1 female
    uint32_t skin = 0, face = 0, hairStyle = 0, hairColor = 0, facialHair = 0;
};

// A worn item: the equipment slot (0 head, 2 shoulders, 4 chest, 5 waist, 6 legs, 7 feet, 8 wrists, 9 hands,
// 14 back, 15 main hand, 16 off hand, 18 tabard) and its ItemDisplayInfo id; sheath is item_template.sheath.
struct Worn {
    uint32_t slot = 0;
    uint32_t displayId = 0;
    uint32_t sheath = 0;
};

// A model hung on an attachment point of the body (M2 attachment ids: 1 right hand, 5/6 shoulders, 11 helm,
// 26/27 the back), with its texture. sheathed: a weapon, on the back until drawn (placeAttached).
struct Attached {
    uint32_t point = 0;
    std::string model;
    std::string texture;
    uint32_t slot = 0;
    bool sheathed = false;
};

struct Look {
    std::string body;               // the race/sex model
    Image skin;                     // texture type 1: the composited body
    std::string hair;               // texture type 6
    std::string cape;               // texture type 2
    std::set<uint16_t> geosets;     // shown
    std::vector<Attached> attached;
    // Creature skins: M2 texture type -> full MPQ path (11-13 from TextureVariation[0-2]).
    // Character texture types 1/6/2 remain in skin/hair/cape above.
    std::map<uint32_t, std::string> replaceableTextures;
    float scale = 1.0f;             // uniform model scale, applied by the host
};

Look dressCharacter(const Mpq& mpq, const Appearance& who, const std::vector<Worn>& worn);
// A creature display, including druid forms. No character equipment is attached. Returns no look for missing
// display/model data or character-based NPC displays (CreatureDisplayInfoExtra), which need character dressing.
std::optional<Look> dressCreature(const Mpq& mpq, uint32_t displayId);

// Where an attached model goes on the posed body: its point, or for a weapon, the hand while drawn and the back
// (turned as the client hangs it) while sheathed.
std::optional<glm::mat4> placeAttached(const Model& body, const std::vector<glm::mat4>& bones, const Attached& item,
                                       bool weaponsDrawn);

// The melee animations the 1.12 client plays for what is in the main hand (AnimationData ids, from item_template's
// class, subclass and InventoryType): the stance while fighting and the swing. Staves and polearms are held loose
// (Ready2HL 28, Attack2HL 19: matched to the client's staff, build/wowref melee_*); other two-handers 27/18,
// one-handers 26/17, nothing or not a weapon 25/16 (WoWee's mapping, not yet checked against the client; 1.12 has no
// fist or dagger stances of its own). The swing's whoosh: WeaponSwingSounds2's heavy (2) for a two-hander,
// medium (1) for the rest - a two-hander's checked, the rest WoWee's.
struct MeleeAnims {
    uint32_t ready = 25, attack = 16, swingType = 1;
};
MeleeAnims meleeAnims(uint32_t itemClass, uint32_t subclass, uint32_t inventoryType);

// A BLP from the client's data, decoded to RGBA.
Image loadImage(const Mpq& mpq, const std::string& path);
}  // namespace wowcore
