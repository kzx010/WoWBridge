-- Full druid tree metadata from data/talents.json; non-reference rendering and native tests.
local trees = {
  {id=283, tab=1, name="Balance", icon="Interface\\Icons\\Spell_Nature_Lightning", background="DruidBalance", talents={
    {["id"]=762,["name"]="Improved Wrath",["icon"]="Interface\\Icons\\Spell_Nature_AbolishMagic",["tier"]=0,["column"]=0,["max_rank"]=5,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={16814,16815,16816,16817,16818}},
    {["id"]=761,["name"]="Nature's Grasp",["icon"]="Interface\\Icons\\Spell_Nature_NaturesWrath",["tier"]=0,["column"]=1,["max_rank"]=1,["exceptional"]=true,["prerequisites"]={},["rank_spells"]={16689}},
    {["id"]=921,["name"]="Improved Nature's Grasp",["icon"]="Interface\\Icons\\Spell_Nature_NaturesWrath",["tier"]=0,["column"]=2,["max_rank"]=4,["exceptional"]=false,["prerequisites"]={{["rank"]=1,["talent"]=761}},["rank_spells"]={17245,17247,17248,17249}},
    {["id"]=787,["name"]="Improved Entangling Roots",["icon"]="Interface\\Icons\\Spell_Nature_StrangleVines",["tier"]=1,["column"]=0,["max_rank"]=3,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={16918,16919,16920}},
    {["id"]=763,["name"]="Improved Moonfire",["icon"]="Interface\\Icons\\Spell_Nature_StarFall",["tier"]=1,["column"]=1,["max_rank"]=5,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={16821,16822,16823,16824,16825}},
    {["id"]=791,["name"]="Natural Weapons",["icon"]="Interface\\Icons\\INV_Staff_01",["tier"]=1,["column"]=2,["max_rank"]=5,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={16902,16903,16904,16905,16906}},
    {["id"]=781,["name"]="Natural Shapeshifter",["icon"]="Interface\\Icons\\Spell_Nature_WispSplode",["tier"]=1,["column"]=3,["max_rank"]=3,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={16833,16834,16835}},
    {["id"]=782,["name"]="Improved Thorns",["icon"]="Interface\\Icons\\Spell_Nature_Thorns",["tier"]=2,["column"]=0,["max_rank"]=3,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={16836,16839,16840}},
    {["id"]=788,["name"]="Omen of Clarity",["icon"]="Interface\\Icons\\Spell_Nature_CrystalBall",["tier"]=2,["column"]=2,["max_rank"]=1,["exceptional"]=true,["prerequisites"]={{["rank"]=5,["talent"]=791}},["rank_spells"]={16864}},
    {["id"]=764,["name"]="Nature's Reach",["icon"]="Interface\\Icons\\Spell_Nature_NatureTouchGrow",["tier"]=2,["column"]=3,["max_rank"]=2,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={16819,16820}},
    {["id"]=792,["name"]="Vengeance",["icon"]="Interface\\Icons\\Spell_Nature_Purge",["tier"]=3,["column"]=1,["max_rank"]=5,["exceptional"]=false,["prerequisites"]={{["rank"]=5,["talent"]=763}},["rank_spells"]={16909,16910,16911,16912,16913}},
    {["id"]=784,["name"]="Improved Starfire",["icon"]="Interface\\Icons\\Spell_Arcane_StarFire",["tier"]=3,["column"]=2,["max_rank"]=5,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={16850,16923,16924,16925,16926}},
    {["id"]=789,["name"]="Nature's Grace",["icon"]="Interface\\Icons\\Spell_Nature_NaturesBlessing",["tier"]=4,["column"]=1,["max_rank"]=1,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={16880}},
    {["id"]=783,["name"]="Moonglow",["icon"]="Interface\\Icons\\Spell_Nature_Sentinal",["tier"]=4,["column"]=2,["max_rank"]=3,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={16845,16846,16847}},
    {["id"]=790,["name"]="Moonfury",["icon"]="Interface\\Icons\\Spell_Nature_MoonGlow",["tier"]=5,["column"]=1,["max_rank"]=5,["exceptional"]=false,["prerequisites"]={{["rank"]=1,["talent"]=789}},["rank_spells"]={16896,16897,16899,16900,16901}},
    {["id"]=793,["name"]="Moonkin Form",["icon"]="Interface\\Icons\\Spell_Nature_ForceOfNature",["tier"]=6,["column"]=1,["max_rank"]=1,["exceptional"]=true,["prerequisites"]={},["rank_spells"]={24858}},
  }},
  {id=281, tab=2, name="Feral Combat", icon="Interface\\Icons\\Ability_Physical_Taunt", background="DruidFeralCombat", talents={
    {["id"]=796,["name"]="Ferocity",["icon"]="Interface\\Icons\\Ability_Hunter_Pet_Hyena",["tier"]=0,["column"]=1,["max_rank"]=5,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={16934,16935,16936,16937,16938}},
    {["id"]=795,["name"]="Feral Aggression",["icon"]="Interface\\Icons\\Ability_Druid_DemoralizingRoar",["tier"]=0,["column"]=2,["max_rank"]=5,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={16858,16859,16860,16861,16862}},
    {["id"]=799,["name"]="Feral Instinct",["icon"]="Interface\\Icons\\Ability_Ambush",["tier"]=1,["column"]=0,["max_rank"]=5,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={16947,16948,16949,16950,16951}},
    {["id"]=797,["name"]="Brutal Impact",["icon"]="Interface\\Icons\\Ability_Druid_Bash",["tier"]=1,["column"]=1,["max_rank"]=2,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={16940,16941}},
    {["id"]=794,["name"]="Thick Hide",["icon"]="Interface\\Icons\\INV_Misc_Pelt_Bear_03",["tier"]=1,["column"]=2,["max_rank"]=5,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={16929,16930,16931,16932,16933}},
    {["id"]=807,["name"]="Feline Swiftness",["icon"]="Interface\\Icons\\Spell_Nature_SpiritWolf",["tier"]=2,["column"]=0,["max_rank"]=2,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={17002,24866}},
    {["id"]=804,["name"]="Feral Charge",["icon"]="Interface\\Icons\\Ability_Hunter_Pet_Bear",["tier"]=2,["column"]=1,["max_rank"]=1,["exceptional"]=true,["prerequisites"]={},["rank_spells"]={16979}},
    {["id"]=798,["name"]="Sharpened Claws",["icon"]="Interface\\Icons\\INV_Misc_MonsterClaw_04",["tier"]=2,["column"]=2,["max_rank"]=3,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={16942,16943,16944}},
    {["id"]=802,["name"]="Improved Shred",["icon"]="Interface\\Icons\\Spell_Shadow_VampiricAura",["tier"]=3,["column"]=0,["max_rank"]=2,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={16966,16968}},
    {["id"]=803,["name"]="Predatory Strikes",["icon"]="Interface\\Icons\\Ability_Hunter_Pet_Cat",["tier"]=3,["column"]=1,["max_rank"]=3,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={16972,16974,16975}},
    {["id"]=800,["name"]="Blood Frenzy",["icon"]="Interface\\Icons\\Ability_GhoulFrenzy",["tier"]=3,["column"]=2,["max_rank"]=2,["exceptional"]=false,["prerequisites"]={{["rank"]=3,["talent"]=798}},["rank_spells"]={16952,16954}},
    {["id"]=801,["name"]="Primal Fury",["icon"]="Interface\\Icons\\Ability_Racial_Cannibalize",["tier"]=3,["column"]=3,["max_rank"]=2,["exceptional"]=false,["prerequisites"]={{["rank"]=3,["talent"]=798}},["rank_spells"]={16958,16961}},
    {["id"]=805,["name"]="Savage Fury",["icon"]="Interface\\Icons\\Ability_Druid_Ravage",["tier"]=4,["column"]=0,["max_rank"]=2,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={16998,16999}},
    {["id"]=1162,["name"]="Faerie Fire (Feral)",["icon"]="Interface\\Icons\\Spell_Nature_FaerieFire",["tier"]=4,["column"]=2,["max_rank"]=1,["exceptional"]=true,["prerequisites"]={},["rank_spells"]={16857}},
    {["id"]=808,["name"]="Heart of the Wild",["icon"]="Interface\\Icons\\Spell_Holy_BlessingOfAgility",["tier"]=5,["column"]=1,["max_rank"]=5,["exceptional"]=false,["prerequisites"]={{["rank"]=3,["talent"]=803}},["rank_spells"]={17003,17004,17005,17006,24894}},
    {["id"]=809,["name"]="Leader of the Pack",["icon"]="Interface\\Icons\\Spell_Nature_UnyeildingStamina",["tier"]=6,["column"]=1,["max_rank"]=1,["exceptional"]=true,["prerequisites"]={},["rank_spells"]={17007}},
  }},
  {id=282, tab=3, name="Restoration", icon="Interface\\Icons\\Spell_Nature_HealingTouch", background="DruidRestoration", talents={
    {["id"]=821,["name"]="Improved Mark of the Wild",["icon"]="Interface\\Icons\\Spell_Nature_Regeneration",["tier"]=0,["column"]=1,["max_rank"]=5,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={17050,17051,17053,17054,17055}},
    {["id"]=822,["name"]="Furor",["icon"]="Interface\\Icons\\Spell_Holy_BlessingOfStamina",["tier"]=0,["column"]=2,["max_rank"]=5,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={17056,17058,17059,17060,17061}},
    {["id"]=824,["name"]="Improved Healing Touch",["icon"]="Interface\\Icons\\Spell_Nature_HealingTouch",["tier"]=1,["column"]=0,["max_rank"]=5,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={17069,17070,17071,17072,17073}},
    {["id"]=823,["name"]="Nature's Focus",["icon"]="Interface\\Icons\\Spell_Nature_HealingWaveGreater",["tier"]=1,["column"]=1,["max_rank"]=5,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={17063,17065,17066,17067,17068}},
    {["id"]=826,["name"]="Improved Enrage",["icon"]="Interface\\Icons\\Ability_Druid_Enrage",["tier"]=1,["column"]=2,["max_rank"]=2,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={17079,17082}},
    {["id"]=829,["name"]="Reflection",["icon"]="Interface\\Icons\\Spell_Frost_WindWalkOn",["tier"]=2,["column"]=1,["max_rank"]=3,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={17106,17107,17108}},
    {["id"]=827,["name"]="Insect Swarm",["icon"]="Interface\\Icons\\Spell_Nature_InsectSwarm",["tier"]=2,["column"]=2,["max_rank"]=1,["exceptional"]=true,["prerequisites"]={},["rank_spells"]={5570}},
    {["id"]=841,["name"]="Subtlety",["icon"]="Interface\\Icons\\Ability_EyeOfTheOwl",["tier"]=2,["column"]=3,["max_rank"]=5,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={17118,17119,17120,17121,17122}},
    {["id"]=843,["name"]="Tranquil Spirit",["icon"]="Interface\\Icons\\Spell_Holy_ElunesGrace",["tier"]=3,["column"]=1,["max_rank"]=5,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={24968,24969,24970,24971,24972}},
    {["id"]=830,["name"]="Improved Rejuvenation",["icon"]="Interface\\Icons\\Spell_Nature_Rejuvenation",["tier"]=3,["column"]=3,["max_rank"]=3,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={17111,17112,17113}},
    {["id"]=831,["name"]="Nature's Swiftness",["icon"]="Interface\\Icons\\Spell_Nature_RavenForm",["tier"]=4,["column"]=0,["max_rank"]=1,["exceptional"]=true,["prerequisites"]={{["rank"]=5,["talent"]=824}},["rank_spells"]={17116}},
    {["id"]=828,["name"]="Gift of Nature",["icon"]="Interface\\Icons\\Spell_Nature_ProtectionformNature",["tier"]=4,["column"]=2,["max_rank"]=5,["exceptional"]=false,["prerequisites"]={{["rank"]=1,["talent"]=827}},["rank_spells"]={17104,24943,24944,24945,24946}},
    {["id"]=842,["name"]="Improved Tranquility",["icon"]="Interface\\Icons\\Spell_Nature_Tranquility",["tier"]=4,["column"]=3,["max_rank"]=2,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={17123,17124}},
    {["id"]=825,["name"]="Improved Regrowth",["icon"]="Interface\\Icons\\Spell_Nature_ResistNature",["tier"]=5,["column"]=2,["max_rank"]=5,["exceptional"]=false,["prerequisites"]={},["rank_spells"]={17074,17075,17076,17077,17078}},
    {["id"]=844,["name"]="Swiftmend",["icon"]="Interface\\Icons\\INV_Relics_IdolofRejuvenation",["tier"]=6,["column"]=1,["max_rank"]=1,["exceptional"]=true,["prerequisites"]={{["rank"]=5,["talent"]=843}},["rank_spells"]={18562}},
  }},
}

function __WBTalentFixture(spent)
  local allocation = spent and { [762]=5, [761]=1, [921]=4, [763]=5, [791]=5, [788]=1, [792]=3 } or {}
  local reply = { class="druid", class_id=11, level=60, points_total=51,
    points_spent=0, points_available=51, talents={}, trees={} }
  for i, source in ipairs(trees) do
    local tree = { id=source.id, tab=source.tab, name=source.name, icon=source.icon,
      background=source.background, talents={}, points_spent=0 }
    reply.trees[i] = tree
    for j, sourceTalent in ipairs(source.talents) do
      local t = {}
      for k,v in pairs(sourceTalent) do t[k] = v end
      t.rank = allocation[t.id] or 0
      t.meets_prerequisites = true
      for _,p in ipairs(t.prerequisites) do
        if (allocation[p.talent] or 0) < p.rank then t.meets_prerequisites = false end
      end
      tree.talents[j] = t
      tree.points_spent = tree.points_spent + t.rank
      if t.rank > 0 then reply.talents[tostring(t.id)] = t.rank end
    end
    reply.points_spent = reply.points_spent + tree.points_spent
  end
  reply.points_available = 51 - reply.points_spent
  __WBPlayerLevel(60)
  __WBTalents(reply)
end
