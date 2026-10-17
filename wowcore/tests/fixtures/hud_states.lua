-- The HUD reference states (tools/hudref.py -> evidence/hudref) as wowcore reproduces them once the login has been
-- replayed: the fixture state, the events the client raised on the way there (as evidence/events/login_actions.lua
-- recorded them), what the player did (Lua the binding would run, or the pointer), and how many seconds the client
-- ran before the screenshot.
-- Host-driven abilities, shared by native tests and a non-reference HUD state. hud_render loads archive art only;
-- useManifestIcons tests the real manifest paths, while the preview uses available WoW icons as stand-ins.
-- Populated catalogs for testing-panel renders and the portable layout walk.
function __WBTestingFixture()
  __WBProgressionFixture(false)
  local skills={}
  for _,key in ipairs({"onehanded","twohanded","marksman","block","smithing","heavyarmor","lightarmor","pickpocket","lockpicking","sneak","alchemy","speechcraft","alteration","conjuration","destruction","illusion","restoration","enchanting"}) do tinsert(skills,{key=key,value=30}) end
  local ranks={}
  for _,s in ipairs({
    {5176,5176,1,"Wrath","destruction",1,"Balance",15},
    {5177,5176,2,"Wrath","destruction",1,"Balance",23},
    {5178,5176,3,"Wrath","destruction",1,"Balance",34},
    {8921,8921,1,"Moonfire","destruction",1,"Balance",19},
    {339,339,1,"Entangling Roots","destruction",1,"Balance",26},
    {16914,16914,1,"Hurricane","destruction",1,"Balance",72},
    {1082,1082,1,"Claw","one_handed",2,"Feral Combat",43},
    {5185,5185,1,"Healing Touch","restoration",3,"Restoration",15},
    {5186,5185,2,"Healing Touch","restoration",3,"Restoration",26},
    {1126,1126,1,"Mark of the Wild","restoration",3,"Restoration",15},
    {16814,16814,1,"Improved Wrath","destruction",1,"Balance",15},
    {25298,2912,7,"Starfire","destruction",1,"Balance",100},
  }) do tinsert(ranks,{spell=s[1],chain=s[2],rank=s[3],rank_name="Rank "..s[3],name=s[4],skill=s[5],
    tab=s[6],tab_name=s[7],learn_skill_value=s[8],source=s[1]==16814 and "talent" or s[1]==25298 and "item" or "trainer"}) end
  __WBTestCatalog({skills=skills,activeClass="druid",classes={
    {key="druid",name="Druid",id=11,spells=ranks},
    {key="mage",name="Mage",id=8,spells={{spell=133,chain=133,rank=1,rank_name="Rank 1",name="Fireball",skill="destruction",tab=2,tab_name="Fire",learn_skill_value=15,source="trainer"}}},
  },
    sets={{key="cenarion",name="Cenarion",count=8},{key="arcanist",name="Arcanist",count=8}},gear={
      {entry=873,name="Staff of Jordan"},
      {entry=1168,name="Skullflame Shield"},
      {entry=5201,name="Emberstone Staff"},
      {entry=16795,name="Arcanist Crown"},
      {entry=16796,name="Arcanist Leggings"},
      {entry=16797,name="Arcanist Mantle"},
      {entry=16798,name="Arcanist Robes"},
      {entry=16799,name="Arcanist Bindings"},
      {entry=16800,name="Arcanist Boots"},
      {entry=16801,name="Arcanist Gloves"},
      {entry=16802,name="Arcanist Belt"},
      {entry=16803,name="Felheart Slippers"},
      {entry=16804,name="Felheart Bracers"},
      {entry=16805,name="Felheart Gloves"},
      {entry=16806,name="Felheart Belt"},
      {entry=16807,name="Felheart Shoulder Pads"},
    },places={
      {key="riverwood",name="Riverwood",hint="Town and nearby wildlife"},
      {key="whiterun",name="Whiterun",hint="City services"},
      {key="solitude",name="Solitude",hint="City services"},
      {key="windhelm",name="Windhelm",hint="City services"},
      {key="riften",name="Riften",hint="City services"},
      {key="markarth",name="Markarth",hint="City services"},
      {key="dawnstar",name="Dawnstar",hint="Coast and town"},
      {key="blackreach",name="Blackreach",hint="Underground worldspace"},
      {key="qasmoke",name="QASmoke",hint="Indoor item testing"},
      {key="barrow",name="Bleak Falls Barrow",hint="Rooms, line of sight and dungeon combat"},
      {key="lake",name="Lake Ilinalta",hint="Open water by Ilinalta's Deep; Aquatic Form"},
      {key="flight",name="High Hrothgar exterior",hint="Mountain flight and outdoor mounts"},
      {key="shrine",name="Temple of Kynareth",hint="Activate the shrine to refund talents"},
      {key="merchant",name="Riverwood Trader",hint="Lucan; merchant and inventory checks"},
      {key="crafting",name="Warmaidens",hint="Native crafting; forge outside"},
      {key="stables",name="Whiterun stables",hint="Exit to Bjorlam and the carriage"},
      {key="trainer",name="Faendal's house",hint="Archery trainer; may be outside during the day"},
      {key="bandits",name="Valtheim Towers",hint="Bandits and outdoor combat"},
      {key="dragon",name="Dragon lair",hint="Dragon presence depends on quest and respawn state"},
    }})
  __WBMountCatalog({
    {key="swift_mistsaber",spell=23219,name="Swift Mistsaber",display=14332,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Mount_BlackPanther"},
    {key="swift_dawnsaber",spell=23220,name="Swift Dawnsaber",display=14329,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Mount_JungleTiger"},
    {key="swift_frostsaber",spell=23221,name="Swift Frostsaber",display=14331,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Mount_WhiteTiger"},
    {key="swift_yellow_mechanostrider",spell=23222,name="Swift Yellow Mechanostrider",display=14377,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Mount_MechaStrider"},
    {key="swift_white_mechanostrider",spell=23223,name="Swift White Mechanostrider",display=14376,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Mount_MechaStrider"},
    {key="swift_green_mechanostrider",spell=23225,name="Swift Green Mechanostrider",display=14374,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Mount_MechaStrider"},
    {key="swift_palomino",spell=23227,name="Swift Palomino",display=14582,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Mount_RidingHorse"},
    {key="swift_white_steed",spell=23228,name="Swift White Steed",display=14338,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Mount_RidingHorse"},
    {key="swift_brown_steed",spell=23229,name="Swift Brown Steed",display=14583,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Mount_RidingHorse"},
    {key="swift_brown_ram",spell=23238,name="Swift Brown Ram",display=14347,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Mount_MountainRam"},
    {key="swift_gray_ram",spell=23239,name="Swift Gray Ram",display=14576,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Mount_MountainRam"},
    {key="swift_white_ram",spell=23240,name="Swift White Ram",display=14346,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Mount_MountainRam"},
    {key="swift_blue_raptor",spell=23241,name="Swift Blue Raptor",display=14339,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Mount_Raptor"},
    {key="swift_olive_raptor",spell=23242,name="Swift Olive Raptor",display=14344,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Mount_Raptor"},
    {key="swift_orange_raptor",spell=23243,name="Swift Orange Raptor",display=14342,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Mount_Raptor"},
    {key="purple_skeletal_warhorse",spell=23246,name="Purple Skeletal Warhorse",display=10721,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Mount_Undeadhorse"},
    {key="great_white_kodo",spell=23247,name="Great White Kodo",display=14349,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Mount_Kodo_01"},
    {key="great_gray_kodo",spell=23248,name="Great Gray Kodo",display=14579,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Mount_Kodo_01"},
    {key="great_brown_kodo",spell=23249,name="Great Brown Kodo",display=14578,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Mount_Kodo_03"},
    {key="swift_brown_wolf",spell=23250,name="Swift Brown Wolf",display=14573,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Mount_BlackDireWolf"},
    {key="swift_timber_wolf",spell=23251,name="Swift Timber Wolf",display=14575,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Mount_WhiteDireWolf"},
    {key="swift_golden_gryphon",spell=32235,name="Swift Golden Gryphon",display=6852,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Hunter_Pet_Owl",flying=true,flight_speed_percent=280,description="Summons and dismisses a rideable Gryphon.  This is a very fast mount that can fly outdoors."},
    {key="swift_green_wind_rider",spell=32295,name="Swift Green Wind Rider",display=11011,speed_percent=100,cast_ms=3000,icon="Interface\\Icons\\Ability_Hunter_Pet_Bat",flying=true,flight_speed_percent=280,description="Summons and dismisses a rideable Wind Rider.  This is a very fast mount that can fly outdoors."},
  })
  __WBSkyrimSpells({{id=-77772,name='Healing',cost=12},{id=-77773,name='Flames',cost=14},
    {id=-4261416960,name='A deliberately long spell name to exercise single-line catalog truncation and readable tooltips',cost=20}})
  __WBTestResult({ok=true,message='Testing tools ready'})
end

function __WBShoutFixture(useManifestIcons)
  local function icon(id, fallback)
    return "Interface\\Icons\\" .. (useManifestIcons and ("WB_Skyrim_" .. id) or fallback)
  end
  __WBSkyrimShouts({
    { id = -0x013E07, name = "Unrelenting Force", icon = icon("013E07", "Spell_Nature_Cyclone"),
      description = "Your Voice is raw power, pushing aside anything that stands in your path.",
      words = {
        { name = "Fus", spell = -0x013E09, learned = true, unlocked = true, recovery = 15 },
        { name = "Ro", spell = -0x013F39, learned = true, unlocked = true, recovery = 20 },
        { name = "D4", spell = -0x013F3A, recovery = 45 },
      } },
    { id = -0x02F7BA, name = "Whirlwind Sprint", icon = icon("02F7BA", "Spell_Nature_AstralRecal"),
      description = "The Thu'um rushes forward, carrying you in its wake with the speed of a tempest.",
      words = {
        { name = "Wuld", spell = -0x02F7BE, learned = true, unlocked = true, recovery = 20 },
        { name = "N4", spell = -0x02F7BF, recovery = 25 },
        { name = "Kest", spell = -0x02F7C0, recovery = 35 },
      } },
  })
  __WBSkyrimPowers({
    { id = -0x0E40C3, name = "Battle Cry", kind = "greater", icon = icon("0E40C3", "Spell_Shadow_Possession"),
      description = "Targets flee for 30 seconds." },
  })
  for i, id in ipairs({ -0x013E07, -0x02F7BA, -0x0E40C3 }) do __WB.actions[i] = { spell = id } end
  __WBFire("ACTIONBAR_SLOT_CHANGED", 0)
end

function __WBSpellFeedbackFixture()
  __WBSkyrimSpells({
    {id=-77772,name='Healing',icon='Interface\\Icons\\WB_Skyrim_012FCC',cost=12,channelled=true},
    {id=-77773,name='Flames',icon='Interface\\Icons\\WB_Skyrim_012FCD',cost=14,channelled=true,harmful=true},
  })
  for slot,id in ipairs({6603,-77772,-77773,5185,2912}) do __WB.actions[slot]={spell=id} end
  __WBSkyrimAura(77772,'Healing','Interface\\Icons\\WB_Skyrim_012FCC',false,10,'Restores health while channeling.')
  __WBSkyrimAura(77773,'Flames','Interface\\Icons\\WB_Skyrim_012FCD',true,10,'Fire damage while channeling.','Magic')
  __WBChannelStart(-77772,10000)
  __WBFire('ACTIONBAR_SLOT_CHANGED',0)
end

-- A selected-rank service snapshot shared by the progression API test and the non-reference render.
function __WBProgressionFixture(upgraded)
  local function row(id, line, tab, skill, value)
    return { spell = id, skill_line = line, tab = tab, learn_skill = skill, learn_skill_value = value }
  end
  local reply = {
    lines = { ["574"] = { name = "Balance", tab = 1 }, ["134"] = { name = "Feral Combat", tab = 2 },
              ["573"] = { name = "Restoration", tab = 3 } },
    known_spells = { 5176, 8921, 5185, 1126 },
    spells = { row(5176, 574, 1), row(8921, 574, 1), row(5185, 573, 3), row(1126, 573, 3) },
    unlearned_spells = { row(16914, 574, 1, "Destruction", 15 + 39*85/59),
                        row(5177, 574, 1, "Destruction", 15 + 5*85/59),
                        row(1082, 134, 2, "One-handed", 15 + 19*85/59),
                        row(5186, 573, 3, "Restoration", 15 + 7*85/59) },
  }
  if upgraded then
    table.remove(reply.unlearned_spells, 2)
    tinsert(reply.known_spells, 5177)
    tinsert(reply.spells, row(5177, 574, 1))
    -- A physical class ability lets the test check ERR_LEARN_ABILITY_S as well.
    table.remove(reply.unlearned_spells, 2)
    tinsert(reply.known_spells, 1082)
    tinsert(reply.spells, row(1082, 134, 2))
  end
  __WBClassSpells(reply)
end

-- Loot includes Skyrim/native art, an actual WoW entry, theft, counts and septims as gold.
-- Selected druid rows from data/progression.json / known_spells at Destruction
-- 22: Wrath II unlocks at 23, Roots I at 26, Moonfire II at 28, Hurricane at 72.
function __WBTrainerFixture(upgraded)
  local function spell(id,chain,rank,name,skill,value,requirements)
    return {spell=id,chain=chain,rank=rank,rank_name="Rank "..rank,name=name,skill=skill,
      skill_line=skill=="restoration" and 573 or 574,tab=skill=="restoration" and 3 or 1,
      learn_skill=skill=="restoration" and "Restoration" or "Destruction",learn_skill_value=value,
      learn_requirements=requirements or {}}
  end
  local reply={lines={["574"]={name="Balance",tab=1},["573"]={name="Restoration",tab=3}},
    known_spells={5176,8921,5185},spells={spell(5176,5176,1,"Wrath","destruction"),
      spell(8921,8921,1,"Moonfire","destruction"),spell(5185,5185,1,"Healing Touch","restoration")},
    unlearned_spells={spell(5177,5176,2,"Wrath","destruction",23),
      spell(5178,5176,3,"Wrath","destruction",34,{"Requires Wrath (Rank 2)"}),
      spell(339,339,1,"Entangling Roots","destruction",26),spell(8924,8921,2,"Moonfire","destruction",28),
      spell(16914,16914,1,"Hurricane","destruction",72),spell(5186,5185,2,"Healing Touch","restoration",26)},
    highest_ranks={["5176"]=5176,["8921"]=8921,["5185"]=5185}}
  if upgraded then
    tinsert(reply.known_spells,5177) tinsert(reply.spells,table.remove(reply.unlearned_spells,1))
    reply.highest_ranks["5176"]=5177
  end
  __WBClassSpells(reply)
  __WBTrainer({session=201,name="Wuunferth the Unliving",skill="destruction",skillName="Destruction",
    current=upgraded and 23 or 22,maximum=50,trained=upgraded and 1 or 0,limit=5,cost=upgraded and 280 or 270,
    gold=upgraded and 1730 or 2000},upgraded and 2 or 1)
end

function __WBLootFixture(shift)
  __WBLoot(101,{
    {slot=1,key="coin",form=15,name="Gold",count=42,kind="Miscellaneous"},
    {slot=2,key="embers",form=0x12eb8,name="Iron Sword of Embers",count=3,kind="Weapon",weapon=1,
      enchanted=true,steal=true,slotName="One-Hand",typeName="Sword",damage=7,weight=9,value=150,
      effects={"Burns the target for 10 points."}},
    {slot=3,key="staff",form=0xfe800,entry=873,name="Staff of Jordan",count=1,kind="Weapon",weapon=8},
    {slot=4,key="potion",form=0x3eadd,name="Potion of Minor Healing",count=5,kind="Potion",potion="health"},
    {slot=5,key="lockpick",form=10,name="Lockpick",count=12,kind="Miscellaneous",steal=true},
    {slot=6,key="apple",form=0x64b2e,name="Red Apple",count=2,kind="Food",keywords={"VendorItemFood"}}
  },true,shift)
end

-- A deliberately sparse three-page Skyrim inventory; all five client bags are real 16-slot frames.
-- Skyrim.esm's CarriageSystemScript arrival properties, in yards (64 units).
function __WBTaxiFixture()
  __WB.world.money=1854*10000
  __WBTaxi({session=72,name="Bjorlam",worldspace="Tamriel",nodes={
    {id=1,name="Whiterun",x=272.069672,y=-177.839493,cost=0,current=true},
    {id=2,name="Solitude",x=-1151.557495,y=1558.714111,cost=20},
    {id=3,name="Markarth",x=-2662.828369,y=88.744812,cost=20},
    {id=4,name="Riften",x=2698.798096,y=-1422.154419,cost=20},
    {id=5,name="Windhelm",x=2110.410400,y=395.048279,cost=20},
    {id=6,name="Morthal",x=-603.745972,y=1042.723389,cost=50},
    {id=7,name="Dawnstar",x=481.377411,y=1658.410645,cost=50},
    {id=8,name="Falkreath",x=-473.380188,y=-1348.185913,cost=50},
    {id=9,name="Winterhold",x=1706.367676,y=1607.250610,cost=50},
  }})
end

function __WBBagsFixture()
  local list={
    {key="staff",pos=1,form=0xfe800,entry=873,name="Staff of Jordan",count=1,kind="Weapon",weapon=8},
    {key="sword",pos=2,form=0x12eb7,name="Iron Sword",count=1,kind="Weapon",weapon=1,slotName="One-Hand",typeName="Sword",damage=7,weight=9,value=25},
    {key="enchanted",pos=3,form=0x12eb8,name="Iron Sword of Embers",count=1,kind="Weapon",weapon=1,slotName="One-Hand",typeName="Sword",damage=7,weight=9,value=150,enchanted=true,effects={"Burns the target for 10 points."}},
    {key="potion",pos=4,form=0x3eadd,name="Potion of Minor Healing",count=12,kind="Potion",potion="health",weight=0.5,value=17,effects={"Restore 25 points of Health."}},
    {key="gold",pos=5,form=15,name="Gold",count=1854,kind="Miscellaneous",weight=0,value=1},
    {key="lockpicks",pos=6,form=10,name="Lockpick",count=37,kind="Miscellaneous",weight=0,value=2},
  }
  local examples={
    {name="Iron Helmet",kind="Armor",armorSlot="head",armorType=1,slotName="Head",typeName="Heavy Armor",armor=15,weight=5,value=60},
    {name="Leather Boots",kind="Armor",armorSlot="feet",armorType=0,slotName="Feet",typeName="Light Armor",armor=7,weight=2,value=25},
    {name="Red Apple",kind="Food",keywords={"VendorItemFood"},weight=0.1,value=3},
    {name="Sapphire",kind="Miscellaneous",keywords={"VendorItemGem"},weight=0.1,value=400},
    {name="Blue Mountain Flower",kind="Ingredient",weight=0.1,value=2},
    {name="Iron Ingot",kind="Miscellaneous",keywords={"VendorItemOreIngot"},weight=1,value=7},
    {name="Petty Soul Gem",kind="SoulGem",weight=0.1,value=10},
    {name="Potion of Minor Magicka",kind="Potion",potion="magicka",weight=0.5,value=20},
    {name="Potion of Minor Stamina",kind="Potion",potion="stamina",weight=0.5,value=20},
    {name="Hunting Bow",kind="Weapon",weapon=7,damage=7,weight=7,value=50},
    {name="The Book of the Dragonborn",kind="Book",weight=1,value=12},
    {name="Wolf Pelt",kind="Miscellaneous",keywords={"VendorItemAnimalHide"},weight=1,value=10},
  }
  for pos=7,179 do
    if pos~=64 and pos~=100 and pos~=173 then
      local example=examples[math.mod(pos-7,table.getn(examples))+1]
      local item={} for k,v in pairs(example) do item[k]=v end
      item.pos=pos item.form=0x10000+pos item.key="sample"..pos item.count=math.mod(pos,9)+1
      tinsert(list,item)
    end
  end
  __WBInstallBags() __WBInventory(list,1)
end

-- Non-reference merchant: native prices in gold, finite stock and Skyrim-style repurchase.
function __WBMerchantFixture(revision)
  local sword={key="vendorSword",form=0x12eb7,name="Iron Sword",count=4,kind="Weapon",weapon=1,
    price=75,damage=7,weight=9,value=25}
  local items={sword}
  for n=2,13 do tinsert(items,{key="vendor"..n,form=0x3eadd,name="Potion of Minor Healing",count=n,
    kind="Potion",potion="health",price=43,weight=0.5,value=17,effects={"Restore 25 points of Health."}}) end
  __WBMerchant({session=91,name="Lucan Valerius",gold=750,money=1854*10000,items=items,
    sales={sword={price=8}},buyback=revision and revision>=2 and {
      {key="vendorSword",form=0x12eb7,name="Iron Sword",count=2,kind="Weapon",weapon=1,price=75,weight=9,value=25}} or {}},revision or 1)
end

-- Non-reference Character sheet: WoW staff/chest and Skyrim helmet/shield, live values and the dressed body.
function __WBCharacterFixture()
  __WBSkyrimMenus()
  __WBApplyLive({player={nativeSheet={health=1234,healthMax=1800,magicka=1200,magickaMax=1600,stamina=90,staminaMax=150,
    weight=142,carry=300,armor=176,damage=123,offDamage=0,fire=45,frost=20,shock=15,magic=25,poison=5,disease=100},
    skyrim={['One-Handed']=45,['Two-Handed']=50,Archery=37,Block=22,Smithing=67,
    ['Heavy Armor']=35,['Light Armor']=41,Pickpocket=20,Lockpicking=30,Sneak=55,Alchemy=45,Speech=40,
    Alteration=60,Conjuration=35,Destruction=70,Illusion=30,Restoration=65,Enchanting=50},
    armor={176,176,0,0,0},damage={123,145,0,0,0,0,1},attackPower={350,0,0},attackSpeed=3,
    attackBothHands={300,0,0,0},stats={{100,100,0,0},{90,90,0,0},{80,80,0,0},{120,120,0,0},{110,110,0,0}},
    resistances={[2]={45,45,0,0},[3]={15,15,0,0},[4]={20,20,0,0},[5]={0,0,0,0},[6]={0,0,0,0}}}})
  __WBInventory({{key='helm',form=1,name='Iron Helmet',kind='Armor',armorSlot='head',armorType=1,
      armor=12,weight=5,value=60,worn=true,inventorySlots={1},pos=1,count=1},
    {key='staff',form=2,name='Staff of Jordan',kind='Weapon',weapon=8,entry=873,worn=true,
      inventorySlots={16},pos=2,count=1},
    {key='chest',form=3,name='Stormrage Chestguard',kind='Armor',entry=16897,display=31797,worn=true,
      inventorySlots={5},pos=3,count=1,
      wow={class=4,subclass=2,InventoryType=20,armor=176,stat_type1=5,stat_value1=20,RequiredLevel=60}},
    {key='shield',form=4,name='Iron Shield',kind='Armor',armorSlot='shield',armorType=1,
      armor=15,weight=12,value=60,worn=true,inventorySlots={17},pos=4,count=1}},1)
  __WBCharacterLook({race=4,sex=1,skin=5,face=8,hairStyle=5,hairColor=3},
    {{slot=4,displayId=31797},{slot=15,displayId=20298,sheath=2}})
end

HUD_STATES = {
  trainer = { fixture = "idle", settle = 1, run = "__WBSkyrimMenus() __WBTrainerFixture()" },
  trainer_spells = { fixture = "idle", settle = 1, run = "__WBSkyrimMenus() __WBTrainerFixture() ClassTrainerFrame.showSkillDetails=1 ClassTrainer_SetSelection(7) ClassTrainerFrame_Update()" },
  world_feedback = {fixture="idle",settle=2,run=[[
    __WBSkyrimMenus()
    __WBApplyLive({player={level=12,xp=1100,xpMax=1000},
      mouseover={object='Mountain Flower',action='Right-click: Harvest Blue Mountain Flower'}})
  ]]},
  level_choice = {fixture="idle",settle=2,run=[[
    __WBSkyrimMenus()
    __WBApplyLive({player={level=12,xp=1100,xpMax=1000}})
    this=MainMenuExpBar MainMenuExpBar:GetScript('OnEnter')() this=nil
  ]]},
  spells_feedback = {fixture='idle',settle=1,mouse={900,300},after_live='__WBSpellFeedbackFixture()'},
  -- Non-reference: the client's plate border/level box, three reactions and an elite.
  nameplates = {fixture="idle",settle=1,run=[[
    __WBApplyLive({nameplates={epoch=1,units={
      {guid=101,name='Bandit Marauder',level=60,health=140,healthMax=200,reaction=2,attackable=true,
        classification='normal',distance=8,x=0.34,y=0.35},
      {guid=102,name='Elk',level=12,health=70,healthMax=100,reaction=4,attackable=true,
        classification='normal',distance=12,x=0.65,y=0.46},
      {guid=103,name='Lydia',level=60,health=245,healthMax=310,reaction=5,attackable=false,
        classification='normal',distance=5,x=0.48,y=0.62},
      {guid=104,name='Elite sample',level=62,health=800,healthMax=1000,reaction=2,attackable=true,
        classification='elite',distance=15,x=0.72,y=0.25}}}})
    RunBinding('ALLNAMEPLATES','down')
  ]],print="__WBNameplate(101).name:GetText() .. ': ' .. __WBNameplate(104).level:GetText()"},
  -- Non-reference: the client's chat bubbles over people who speak (wowapi_bubbles.lua), one wrapped, and theirs.
  chat_bubbles = {fixture="idle",settle=1,run=[[
    __WBSkyrimMenus()
    __WBApplyLive({nameplates={epoch=1,units={
      {guid=201,name='Whiterun Guard',level=20,health=100,healthMax=100,reaction=5,attackable=false,
        classification='normal',distance=6,x=0.3,y=0.42,bx=0.3,by=0.36},
      {guid=202,name='Heimskr',level=10,health=100,healthMax=100,reaction=5,attackable=false,
        classification='normal',distance=12,x=0.66,y=0.36,bx=0.66,by=0.3}}},head={x=0.5,y=0.62}})
    RunBinding('ALLNAMEPLATES','down')
    __WBSay('Whiterun Guard','Watch the skies, traveler.',201,6)
    __WBSay('Heimskr','Talos! Mightiest Nord of all time! He who was born in Atmora as Hjalti Early-Beard, rose to unite us all!',202,12)
    __WBFire('CHAT_MSG_SAY','Well met.',UnitName('player'),GetDefaultLanguage('player'),'','','',0,0,'')
  ]],print="__WBChatBubbleFrame(201).text:GetText() .. ' | ' .. __WBChatBubbleFrame(202):GetAlpha()"},
  world_map = {fixture="idle",settle=3,run=[[
    __WBInstallWorldMap()
    __WBApplyLive({worldmap={worldspace='Tamriel',name='Skyrim',x=175,y=-1030,heading=0.65,pois={
      {guid=1,kind='quest',name='Talk to Alvor',x=190,y=-1015,icon=6},
      {guid=2,kind='quest',name='Retrieve the Dragonstone',x=-500,y=-500,icon=6},
      {guid=3,kind='location',name='Riverwood',x=130,y=-1080,icon=4,discovered=true},
      {guid=4,kind='location',name='Whiterun',x=150,y=100,icon=5,discovered=true}}}})
    ShowUIPanel(WorldMapFrame)
  ]],after_live=[[
    SetMapZoom(1)
    this=WorldMapFramePOI1 this:GetScript('OnEnter')() this=nil
  ]],print="GetMapContinents() .. ': ' .. GetNumMapLandmarks() .. ' POIs; ' .. WBWorldMapNotice:GetText()"},
  world_map_local = {fixture="idle",settle=3,run=[[
    __WBInstallWorldMap()
    __WBApplyLive({worldmap={worldspace='Tamriel',name='Skyrim',x=175,y=-1030,heading=0.65,interior=true,pois={
      {guid=1,kind='quest',name='Talk to Alvor',x=190,y=-1015,icon=6},
      {guid=2,kind='location',name='Riverwood',x=130,y=-1080,icon=4,discovered=true}}}})
    ShowUIPanel(WorldMapFrame)
  ]],print="GetCurrentMapZone() .. ': ' .. WBWorldMapNotice:GetText()"},
  -- Non-reference state: four Skyrim teammates, including a dog and a magicka user.
  party = { fixture = "idle", settle = 2, run = [[
    local function member(id,name,powerType,health,power,creatureType)
      return {guid=id,name=name,level=24,health=health,healthMax=310,power=power,powerMax=150,
        powerType=powerType,reaction=5,isPlayer=false,connected=true,creatureType=creatureType or 'Humanoid',auras={}}
    end
    local lydia=member(10,'Lydia',3,245,72)
    lydia.auras={{skyrim=123,name='Poison',texture='Interface\\Icons\\Spell_Nature_CorrosiveBreath',
      text='Lose health.',harmful=true,dispel='Poison',count=1}}
    __WBApplyLive({party={lydia,member(11,'Marcurio',0,310,94),member(12,'Meeko',3,270,128,'Beast'),member(13,'Jenassa',3,198,61)},target=lydia})
    __WBAuraAdd(11,9885,false,1800)
  ]], print = "GetNumPartyMembers() .. ' followers: ' .. UnitName('party1') .. ', ' .. UnitName('party2') .. ', ' .. UnitName('party3') .. ', ' .. UnitName('party4')" },
  -- Non-reference state: their Skyrim overall XP, with the client's hover text and newbie explanation.
  xp_bar = { fixture = "idle", settle = 2, mouse = { { 0, 360 }, { 640, 676 } }, run = [[
    __WBApplyLive({player={level=60,xp=375,xpMax=1000}})
  ]], print = "MainMenuBarExpText:GetText() .. ' tooltip=' .. tostring(GameTooltipTextLeft1:GetText())" },
  character = {fixture="idle",settle=1,run=[[
    __WBCharacterFixture() ToggleCharacter('PaperDollFrame')
    GameTooltip:SetOwner(CharacterHeadSlot,'ANCHOR_RIGHT') GameTooltip:SetInventoryItem('player',1)
  ]]},
  character_skills = {fixture="idle",settle=1,run=[[
    __WBCharacterFixture() ToggleCharacter('SkillFrame')
  ]]},
  dressing_room = {fixture="idle",settle=1,run=[[
    __WBCharacterFixture() ShowUIPanel(DressUpFrame) DressUpModel:SetUnit('player')
  ]]},
  dressing_room_male = {fixture="idle",settle=1,run=[[
    __WBCharacterFixture()
    __WBCharacterLook({race=4,sex=0,skin=5,face=0,hairStyle=3,hairColor=3,facialHair=1},
      {{slot=4,displayId=31797},{slot=15,displayId=20298,sheath=2}})
    ShowUIPanel(DressUpFrame) DressUpModel:SetUnit('player')
  ]]},
  -- Not reference states: the customization window (GlueXML CharacterCreate's parts) on their packaged looks.
  character_customize = {fixture="idle",settle=1,run=[[
    __WBCharacterFixture()
    __WBAppearance({race=4,sex=1,skin=5,face=8,hairStyle=5,hairColor=3,facialHair=0},
      {[4]={[0]={race=4,sex=0,skin=5,face=0,hairStyle=3,hairColor=3,facialHair=1},[1]={race=4,sex=1,skin=5,face=8,hairStyle=5,hairColor=3,facialHair=0}},[6]={[0]={race=6,sex=0,skin=0,face=0,hairStyle=0,hairColor=0,facialHair=1},[1]={race=6,sex=1,skin=0,face=0,hairStyle=0,hairColor=0,facialHair=1}}})
    __WBCustomizeAppearance()
  ]]},
  character_customize_male = {fixture="idle",settle=1,run=[[
    __WBCharacterFixture()
    __WBAppearance({race=4,sex=1,skin=5,face=8,hairStyle=5,hairColor=3,facialHair=0},
      {[4]={[0]={race=4,sex=0,skin=5,face=0,hairStyle=3,hairColor=3,facialHair=1},[1]={race=4,sex=1,skin=5,face=8,hairStyle=5,hairColor=3,facialHair=0}},[6]={[0]={race=6,sex=0,skin=0,face=0,hairStyle=0,hairColor=0,facialHair=1},[1]={race=6,sex=1,skin=0,face=0,hairStyle=0,hairColor=0,facialHair=1}}})
    __WBCustomizeAppearance() WBCustomizeGenderMale:Click()
  ]]},
  character_customize_tauren_male = {fixture="idle",settle=1,run=[[
    __WBCharacterFixture()
    local look={race=6,sex=0,skin=0,face=0,hairStyle=0,hairColor=0,facialHair=1}
    __WBCharacterLook(look,{{slot=4,displayId=12684},{slot=6,displayId=9987},{slot=15,displayId=472,sheath=2}})
    __WBAppearance(look,{[4]={[0]={race=4,sex=0,skin=5,face=0,hairStyle=3,hairColor=3,facialHair=1},[1]={race=4,sex=1,skin=5,face=8,hairStyle=5,hairColor=3,facialHair=0}},[6]={[0]={race=6,sex=0,skin=0,face=0,hairStyle=0,hairColor=0,facialHair=1},[1]={race=6,sex=1,skin=0,face=0,hairStyle=0,hairColor=0,facialHair=1}}})
    __WBCustomizeAppearance()
  ]]},
  character_customize_tauren_female = {fixture="idle",settle=1,run=[[
    __WBCharacterFixture()
    local look={race=6,sex=1,skin=0,face=0,hairStyle=0,hairColor=0,facialHair=1}
    __WBCharacterLook(look,{{slot=4,displayId=12684},{slot=6,displayId=9987},{slot=15,displayId=472,sheath=2}})
    __WBAppearance(look,{[4]={[0]={race=4,sex=0,skin=5,face=0,hairStyle=3,hairColor=3,facialHair=1},[1]={race=4,sex=1,skin=5,face=8,hairStyle=5,hairColor=3,facialHair=0}},[6]={[0]={race=6,sex=0,skin=0,face=0,hairStyle=0,hairColor=0,facialHair=1},[1]={race=6,sex=1,skin=0,face=0,hairStyle=0,hairColor=0,facialHair=1}}})
    __WBCustomizeAppearance()
  ]]},
  minimap_poi = { fixture = "idle", settle = 2, run = [[
    __WBApplyLive({minimap={worldspace='Tamriel',x=0,y=0,heading=0,pois={
      {guid=1,kind='quest',name='Talk to Alvor',x=75,y=50},
      {guid=2,kind='quest',name='Retrieve the Dragonstone',x=-900,y=1200},
      {guid=3,kind='location',name='Riverwood',icon=4,discovered=true,x=-80,y=-30},
      {guid=4,kind='location',name='Undiscovered cave',icon=16,x=800,y=-400},
      {guid=5,kind='custom',name='Custom map marker',x=0,y=-100}}}})
  ]] },
  tracking_humanoids = { fixture = "idle", settle = 2, run = [[
    __WBDefaultFormActions(); __WBForm(3, 3)
    __WBAuraAdd('player', 768, false, 0)
    __WBAuraAdd('player', 5225, false, 0)
    __WBApplyLive({minimap = {worldspace='Tamriel', x=0, y=0, heading=0, trackingMask=64,
      blips={{guid=1,x=0,y=80,reaction=5}, {guid=2,x=100,y=20,reaction=2},
             {guid=3,x=-70,y=-70,reaction=4}, {guid=4,x=198,y=0,reaction=2}}}})
    GameTooltip:SetOwner(MiniMapTrackingFrame, 'ANCHOR_BOTTOMLEFT')
    GameTooltip:SetTrackingSpell()
  ]] },
  -- Non-reference states: the complete host snapshot over Blizzard's actual addon.
  talents_balance = { fixture = "idle", settle = 2, run = [[
    __WBTalentFixture(false)
    ToggleTalentFrame()
  ]] },
  talents_balance_spent = { fixture = "idle", settle = 2, run = [[
    __WBTalentFixture(true)
    ToggleTalentFrame()
  ]] },
  talents_mark_tooltip = { fixture = "idle", settle = 2, run = [[
    __WBTalentFeedbackFixture()
    GameTooltip:SetOwner(ActionButton2, 'ANCHOR_RIGHT')
    GameTooltip:SetAction(2)
  ]] },
  talents_mark_buff = { fixture = "idle", settle = 2, run = [[
    __WBTalentFeedbackFixture()
    __WBAuraAdd('player',9885,false,1800,__WBMarkTooltip(true))
    GameTooltip:SetOwner(BuffButton0,'ANCHOR_BOTTOMLEFT')
    GameTooltip:SetPlayerBuff(0)
  ]] },
  talents_passive_book = { fixture = "idle", settle = 2, run = [[
    __WBTalentFeedbackFixture()
    ToggleSpellBook(BOOKTYPE_SPELL)
    this = SpellBookSkillLineTab2 SpellBookSkillLineTab_OnClick() this = nil
    local _,_,offset = GetSpellTabInfo(2)
    GameTooltip_SetDefaultAnchor(GameTooltip,UIParent)
    GameTooltip:SetSpell(offset+1,BOOKTYPE_SPELL)
  ]] },
  talents_feral = { fixture = "idle", settle = 2, run = [[
    __WBFeralFeedbackFixture()
    ToggleTalentFrame()
    PanelTemplates_SetTab(TalentFrame,2) TalentFrame_Update()
  ]] },
  talents_feral_claw = { fixture = "idle", settle = 2, run = [[
    __WBFeralFeedbackFixture()
    GameTooltip:SetOwner(ActionButton2,'ANCHOR_RIGHT') GameTooltip:SetAction(2)
  ]] },
  talents_feral_leader = { fixture = "idle", settle = 2, run = [[
    __WBFeralFeedbackFixture()
    __WBAuraAdd('player',24932,false,0,{effects={{3,3}}})
    GameTooltip:SetOwner(BuffButton0,'ANCHOR_BOTTOMLEFT') GameTooltip:SetPlayerBuff(0)
  ]] },
  quest_log = { fixture = "idle", settle = 2, run = [[
    __WBSkyrimMenus()
    __WBQuests({
      {id=0xFE000001, title="Before the Storm", header="Main Quest",
        text="I should find someone in Riverwood who can help.",
        objectives={{text="Go to Riverwood",done=true},{text="Talk to Alvor"}}},
      {id=0xFE000002, title="A Lovely Letter", header="Miscellaneous",
        text="Faendal has asked me to give Camilla a letter.",
        objectives={{text="Deliver the letter to Camilla Valerius"}}},
    })
    ToggleQuestLog()
    QuestLog_SetSelection(2)
    AddQuestWatch(2)
    QuestWatch_Update()
  ]] },
  taxi = {fixture="idle",settle=1,run=[[
    __WBTaxiFixture()
    this=TaxiButton2 TaxiNodeOnButtonEnter(this) this=nil
  ]]},
  merchant = {fixture="idle",settle=1,run=[[
    __WBSkyrimMenus() __WBBagsFixture() __WBMerchantFixture()
    GameTooltip_SetDefaultAnchor(GameTooltip,UIParent) GameTooltip:SetMerchantItem(1)
  ]]},
  merchant_buyback = {fixture="idle",settle=1,run=[[
    __WBSkyrimMenus() __WBBagsFixture() __WBMerchantFixture(2) MerchantFrameTab2:Click('LeftButton')
  ]]},
  loot = { fixture="idle",settle=1,run=[[
    __WBLootFixture(false)
    GameTooltip:SetOwner(LootButton2,'ANCHOR_RIGHT') GameTooltip:SetLootItem(2)
  ]] },
  loot_page2 = { fixture="idle",settle=1,run=[[
    __WBLootFixture(false) LootFrame_PageDown()
  ]] },
  bags = { fixture="idle",settle=1,run=[[
    __WBBagsFixture() OpenAllBags(1)
    GameTooltip_SetDefaultAnchor(GameTooltip,UIParent) GameTooltip:SetBagItem(0,3)
  ]] },
  bags_page2 = { fixture="idle",settle=1,run=[[
    __WBBagsFixture() OpenAllBags(1) __WBBagPage(2)
    GameTooltip_SetDefaultAnchor(GameTooltip,UIParent) GameTooltip:SetBagItem(0,2)
  ]] },
  bags_split = { fixture="idle",settle=1,run=[[
    __WBBagsFixture() OpenAllBags(1)
    local button
    for n=1,16 do local b=getglobal('ContainerFrame1Item'..n) if b:GetID()==4 then button=b end end
    __WB.modifiers={shift=1} button:Click('LeftButton') __WB.modifiers={}
  ]] },
  class_progression = { fixture = "idle", settle = 1, run = [[
    __WBProgressionFixture()
    __WBPlayerLevel(7)
    ToggleSpellBook(BOOKTYPE_SPELL)
    this = SpellBookSkillLineTab2
    SpellBookSkillLineTab_OnClick()
    this = nil
    GameTooltip_SetDefaultAnchor(GameTooltip, UIParent)
    local name, icon, offset = GetSpellTabInfo(2)
    GameTooltip:SetSpell(offset + 3, BOOKTYPE_SPELL)
  ]] },
  -- Non-reference state: two shouts and a greater power, in the Skyrim tab and on the bar. Using one shout
  -- puts both into Skyrim's shared recovery. Archive icons stand in for the host's generated manifest art.
  skyrim_shouts = { fixture = "idle", settle = 10, mouse = { 184, 698 },
    run = [[
      __WBShoutFixture(false)
      ToggleSpellBook(BOOKTYPE_SPELL)
      this = getglobal("SpellBookSkillLineTab"..GetNumSpellTabs())
      SpellBookSkillLineTab_OnClick()
      this = nil
      __WBSkyrimShoutUsed(-0x013E07)
      __WBSkyrimPowerUsed(-0x0E40C3)
    ]] },
  -- Non-reference state: evidence/buffref/motw_buff_tooltip.png, their Mark of the Wild buff hovered.
  buff_motw = { fixture = "idle", settle = 1, mouse = { { 0, 360 }, { 1100, 25 } },
    run = "__WBAuraAdd('player', 9885, false, 1800)" },
  -- Host-driven forms, outside the seven HUD states. A new layout uses the shared form defaults;
  -- fill the tooltip explicitly because hud_render dispatches its pointer before the new buttons' first layout.
  -- Combat feedback outside the seven reference states: the original error
  -- font/position and live target frame combo points at native 2560x1440.
  druid_combo_error = { fixture = "target", settle = 1,
    run = [[
      __WBForm(3,3)
      __WBApplyLive({player={power=100,powerMax=100,powerType=3},comboPoints=3})
      __WBUiError("SPELL_FAILED_NO_COMBO_POINTS")
    ]], events = { { "PLAYER_TARGET_CHANGED" } } },
  druid_cat = { fixture = "target", settle = 2,
    run = [[
      __WBDefaultFormActions()
      __WBForm(3, 3)
      __WBApplyLive({player = {power = 65, powerMax = 100, powerType = 3}})
      __WBComboPoints(3)
      GameTooltip_SetDefaultAnchor(GameTooltip, ShapeshiftButton3)
      GameTooltip:SetShapeshift(3)
    ]], events = { { "PLAYER_TARGET_CHANGED" } } },
  druid_dire_bear = { fixture = "target", settle = 2,
    run = [[
      __WBDefaultFormActions()
      __WBForm(1, 1)
      __WBApplyLive({player = {power = 32.5, powerMax = 100, powerType = 1}})
      GameTooltip_SetDefaultAnchor(GameTooltip, ShapeshiftButton1)
      GameTooltip:SetShapeshift(1)
    ]], events = { { "PLAYER_TARGET_CHANGED" } } },
  regrowth_tooltip = { fixture = "idle", settle = 2, mouse = { 184, 698 },
    run = "__WB.actions[1] = {spell = 9858} __WBFire('ACTIONBAR_SLOT_CHANGED', 0)" },
  regrowth_skyrim_tooltip = { fixture = "idle", settle = 2, mouse = { 184, 698 },
    run = [[
      __WB.actions[1] = {spell = 9858}
      __WBApplyLive({player = {skyrim = {Restoration = 60}}})
      __WBFire('ACTIONBAR_SLOT_CHANGED', 0)
    ]] },
  -- Non-reference states: Hurricane's one-minute cooldown at 10%, 50% and 90%.
  cooldown_10 = { fixture = "idle", settle = 6, run = "__WBCooldown(17402, 60)" },
  cooldown_50 = { fixture = "idle", settle = 30, run = "__WBCooldown(17402, 60)" },
  cooldown_90 = { fixture = "idle", settle = 54, run = "__WBCooldown(17402, 60)" },
  hud_idle = { fixture = "idle", settle = 2 },
  -- By the screenshot only Entangling Roots was left on the tester (frames.lua: TargetFrameDebuff1 alone).
  hud_target_debuffs = { fixture = "target", settle = 2.2,
    run = "table.remove(__WB.units[2].debuffs, 1)",
    events = { { "PLAYER_TARGET_CHANGED" }, { "UNIT_AURA", "target" }, { "PLAYER_REGEN_DISABLED" } } },
  -- Healing Touch begun as the host begins a cast (__WBCastStart: SPELLCAST_START, and its button checked).
  hud_casting = { fixture = "self", settle = 1.5, run = "__WBCastStart(25297, 3500)",
    events = { { "PLAYER_TARGET_CHANGED" }, { "UNIT_AURA", "player" }, { "UNIT_AURA", "target" },
               { "PLAYER_AURAS_CHANGED" } } },
  -- Over their portrait first (its newbie tip wraps a line of the tooltip), then the Wrath button: the reused lines must
  -- come out as a fresh tooltip's do.
  hud_tooltip = { fixture = "idle", settle = 1.5, mouse = { { 40, 40 }, { 184, 698 } } },
  -- Not reference states: the testing panel's pages, for layout review.
  testing_panel_1 = { fixture = "idle", settle = 1, run = "__WBTestingFixture() __WBTestPanelToggle() __WBTestResult({ok=true,message='Testing tools ready'}) WBTestPanelTab1:Click('LeftButton')" },   -- Character
  testing_panel_2 = { fixture = "idle", settle = 1, run = "__WBTestingFixture() __WBTestPanelToggle() __WBTestResult({ok=true,message='Testing tools ready'}) WBTestPanelTab2:Click('LeftButton')" },   -- Skills
  testing_panel_3 = { fixture = "idle", settle = 1, run = "__WBTestingFixture() __WBTestPanelToggle() __WBTestResult({ok=true,message='Testing tools ready'}) WBTestPanelTab3:Click('LeftButton')" },   -- Gear
  testing_panel_4 = { fixture = "idle", settle = 1, run = "__WBTestingFixture() __WBTestPanelToggle() __WBTestResult({ok=true,message='Testing tools ready'}) WBTestPanelTab4:Click('LeftButton')" },   -- Items
  testing_panel_5 = { fixture = "idle", settle = 1, run = "__WBTestingFixture() __WBTestPanelToggle() __WBTestResult({ok=true,message='Testing tools ready'}) WBTestPanelTab5:Click('LeftButton')" },   -- Spells
  testing_panel_6 = { fixture = "idle", settle = 1, run = "__WBTestingFixture() __WBTestPanelToggle() __WBTestResult({ok=true,message='Testing tools ready'}) WBTestPanelTab6:Click('LeftButton')" },   -- Travel
  testing_panel_7 = { fixture = "idle", settle = 1, run = "__WBTestingFixture() __WBTestPanelToggle() __WBTestResult({ok=true,message='Testing tools ready'}) WBTestPanelTab7:Click('LeftButton')" },   -- Spawns
  testing_panel_8 = { fixture = "idle", settle = 1, run = "__WBTestingFixture() __WBTestPanelToggle() __WBTestResult({ok=true,message='Testing tools ready'}) WBTestPanelTab8:Click('LeftButton')" },   -- Mounts
  testing_panel_9 = { fixture = "idle", settle = 1, run = "__WBTestingFixture() __WBTestPanelToggle() __WBTestResult({ok=true,message='Testing tools ready'}) WBTestPanelTab9:Click('LeftButton')" },   -- World / Target
  testing_panel_10 = { fixture = "idle", settle = 1, run = "__WBTestingFixture() __WBTestPanelToggle() __WBTestResult({ok=true,message='Testing tools ready'}) WBTestPanelTab10:Click('LeftButton')" }, -- Class Spells
  testing_panel_11 = { fixture = "idle", settle = 1, run = "__WBTestingFixture() __WBTestPanelToggle() __WBTestResult({ok=true,message='Testing tools ready'}) WBTestPanelTab11:Click('LeftButton')" }, -- Features
  testing_panel_12 = { fixture = "idle", settle = 1, run = "__WBTestingFixture() __WBTestPanelToggle() __WBTestResult({ok=true,message='Testing tools ready'}) WBTestPanelTab12:Click('LeftButton')" }, -- Emotes
  testing_panel_13 = { fixture = "idle", settle = 1, run = "__WBTestingFixture() __WBTestPanelToggle() __WBTestResult({ok=true,message='Testing tools ready'}) WBTestPanelTab13:Click('LeftButton')" }, -- Level Up
  spellbook = { fixture = "idle", settle = 1, run = "ToggleSpellBook(BOOKTYPE_SPELL)" },
  game_menu = { fixture = "idle", settle = 1, run = "ShowUIPanel(GameMenuFrame)" },
  -- Not a reference state: the mod's trimmed Interface Options.
  interface_options = { fixture = "idle", settle = 1, run = "__WBSkyrimMenus() ShowUIPanel(UIOptionsFrame)" },
  skyrim_game_menu = { fixture = "idle", settle = 1, run = "__WBSkyrimMenus() ShowUIPanel(GameMenuFrame)" },
  skyrim_key_bindings = { fixture = "idle", settle = 1, run = "__WBSkyrimMenus() KeyBindingFrame_LoadUI() ShowUIPanel(KeyBindingFrame)" },
  key_bindings = { fixture = "idle", settle = 1, run = "KeyBindingFrame_LoadUI() ShowUIPanel(KeyBindingFrame)" },
  -- Not a reference state: hover fades in both docked tabs, then their actual OnClick handlers switch windows.
  combat_log = { fixture = "idle", settle = 2, mouse = { 200, 560 },
    run = "__WBLog('CHAT_MSG_SPELL_SELF_DAMAGE', 'SPELLLOGSCHOOLSELFOTHER', 'Wrath', 'Wolf', 100, 'Nature')",
    print = "(function() local function tab(f) return tostring(f:IsVisible()) .. '/' .. tostring(f:GetAlpha()) end local hover = 'over=' .. tostring(MouseIsOver(ChatFrame1)) .. ' tabs(visible/alpha)=' .. tab(ChatFrame1Tab) .. ',' .. tab(ChatFrame2Tab) local function click(f) local ok, err = pcall(function() f:Click('LeftButton') end) return ok and (tostring((ChatFrame1:IsShown())) .. ',' .. tostring((ChatFrame2:IsShown()))) or tostring(err) end return hover .. ' CombatLog(frames1,2)=' .. click(ChatFrame2Tab) .. ' General(frames1,2)=' .. click(ChatFrame1Tab) end)()" },
}
