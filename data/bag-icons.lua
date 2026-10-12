-- Reviewed Skyrim -> vanilla 1.12 icon table. Names are client MPQ assets, verified by [bags] tests.
-- Selection uses record/weapon/biped types, exact vendor keywords and alchemy actor values, never item names.
-- Each row's comment explains the comparable WoW object. The default is deliberately ordinary clutter.
__WBBagIcons = {
  default = "INV_Misc_Bag_10", -- a pouch for uncategorised carried objects
  weapon = { [0]="INV_Weapon_ShortBlade_05", [1]="INV_Sword_04", [2]="INV_Weapon_ShortBlade_05",
    [3]="INV_Axe_04", [4]="INV_Mace_01", [5]="INV_Sword_09", [6]="INV_Axe_09",
    [7]="INV_Weapon_Bow_05", [8]="INV_Staff_13", [9]="INV_Weapon_Crossbow_01" },
  -- Light armour resembles leather, heavy armour mail/plate, clothing cloth.
  armor = {
    head={"INV_Helmet_09","INV_Helmet_03","INV_Helmet_29"},
    body={"INV_Chest_Leather_04","INV_Chest_Chain_04","INV_Chest_Cloth_18"},
    hands={"INV_Gauntlets_05","INV_Gauntlets_04","INV_Gauntlets_06"},
    feet={"INV_Boots_05","INV_Boots_Plate_01","INV_Boots_03"},
    wrists={"INV_Bracer_05","INV_Bracer_03","INV_Bracer_02"},
    waist={"INV_Belt_04","INV_Belt_03","INV_Belt_06"},
    legs={"INV_Pants_05","INV_Pants_03","INV_Pants_01"},
    back={"INV_Misc_Cape_02","INV_Misc_Cape_02","INV_Misc_Cape_02"},
    shoulders={"INV_Shoulder_05","INV_Shoulder_07","INV_Shoulder_02"},
    shield={"INV_Shield_04","INV_Shield_04","INV_Shield_04"},
    ring={"INV_Jewelry_Ring_02","INV_Jewelry_Ring_02","INV_Jewelry_Ring_02"},
    amulet={"INV_Jewelry_Necklace_01","INV_Jewelry_Necklace_01","INV_Jewelry_Necklace_01"},
  },
  potion = { health="INV_Potion_51", magicka="INV_Potion_76", stamina="INV_Potion_43",
    poison="INV_Potion_19", default="INV_Potion_54" }, -- red healing, blue mana, yellow stamina, green poison
  keywords = {
    VendorItemFood="INV_Misc_Food_14", -- cooked food rather than a magic potion
    VendorItemGem="INV_Misc_Gem_Sapphire_02", -- cut precious stone
    VendorItemOreIngot="INV_Ingot_02", -- metal bar
    VendorItemAnimalHide="INV_Misc_Pelt_Wolf_01", -- leather crafting material
    VendorItemAnimalPart="INV_Misc_Bone_01", -- teeth/bones
    VendorItemIngredient="INV_Misc_Herb_01", -- alchemy reagent
    VendorItemBook="INV_Misc_Book_09", -- bound book
    VendorItemScroll="INV_Scroll_03", -- spell scroll
    VendorItemJewelry="INV_Jewelry_Ring_02", -- jewellery without a biped slot
    VendorItemPotion="INV_Potion_54", -- fallback for modded potion records
  },
  form = { Food="INV_Misc_Food_14", Potion="INV_Potion_54", Ammo="INV_Ammo_Arrow_02", Book="INV_Misc_Book_09", Scroll="INV_Scroll_03",
    Ingredient="INV_Misc_Herb_01", SoulGem="INV_Misc_Gem_Amethyst_02", KeyMaster="INV_Misc_Key_03" },
  gold="INV_Misc_Coin_01", lockpick="INV_Misc_Key_04",
  bag="INV_Misc_Bag_08", backpack="INV_Misc_Bag_07",
}
