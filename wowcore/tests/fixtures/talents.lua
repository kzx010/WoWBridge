-- Representative service talent snapshot, from data/talents.json. Client indices follow these arrays.
__wbTalentReply = {
  ["class"]="druid",
  ["class_id"]=11,
  ["level"]=40,
  ["points_total"]=31,
  ["points_spent"]=0,
  ["points_available"]=31,
  ["talents"]={
    },
  ["learned_spells"]={
    },
  ["trees"]={
    {
      ["background"]="DruidBalance",
      ["background_textures"]={
        ["BottomLeft"]="Interface\\TalentFrame\\DruidBalance-BottomLeft",
        ["BottomRight"]="Interface\\TalentFrame\\DruidBalance-BottomRight",
        ["TopLeft"]="Interface\\TalentFrame\\DruidBalance-TopLeft",
        ["TopRight"]="Interface\\TalentFrame\\DruidBalance-TopRight"},
      ["icon"]="Interface\\Icons\\Spell_Nature_Lightning",
      ["id"]=283,
      ["name"]="Balance",
      ["tab"]=1,
      ["talents"]={
        {
          ["column"]=1,
          ["descriptions"]={
            "While active, any time an enemy strikes the caster they have a $h% chance to become afflicted by Entangling Roots (Rank 1).  Only useable outdoors.  $n charge.  Lasts $d."},
          ["exceptional"]=true,
          ["granted_spells"]={
            ["16689"]={
              }},
          ["icon"]="Interface\\Icons\\Spell_Nature_NaturesWrath",
          ["id"]=761,
          ["max_rank"]=1,
          ["name"]="Nature's Grasp",
          ["prerequisites"]={
            },
          ["rank_spells"]={
            16689},
          ["required_spell"]=339,
          ["tier"]=0,
          ["rank"]=0,
          ["next_rank"]=1,
          ["learnable"]=true,
          ["reasons"]={
            },
          ["meets_prerequisites"]=true},
        {
          ["column"]=2,
          ["descriptions"]={
            "Increases the chance for your Nature's Grasp to entangle an enemy by $s1%.",
            "Increases the chance for your Nature's Grasp to entangle an enemy by $s1%.",
            "Increases the chance for your Nature's Grasp to entangle an enemy by $s1%.",
            "Increases the chance for your Nature's Grasp to entangle an enemy by $s1%."},
          ["exceptional"]=false,
          ["granted_spells"]={
            ["17245"]={
              },
            ["17247"]={
              },
            ["17248"]={
              },
            ["17249"]={
              }},
          ["icon"]="Interface\\Icons\\Spell_Nature_NaturesWrath",
          ["id"]=921,
          ["max_rank"]=4,
          ["name"]="Improved Nature's Grasp",
          ["prerequisites"]={
            {
              ["rank"]=1,
              ["talent"]=761}},
          ["rank_spells"]={
            17245,
            17247,
            17248,
            17249},
          ["required_spell"]=0,
          ["tier"]=0,
          ["rank"]=0,
          ["next_rank"]=1,
          ["learnable"]=false,
          ["reasons"]={
            "prerequisite"},
          ["meets_prerequisites"]=false}},
      ["points_spent"]=0},
    {
      ["background"]="DruidFeralCombat",
      ["background_textures"]={
        ["BottomLeft"]="Interface\\TalentFrame\\DruidFeralCombat-BottomLeft",
        ["BottomRight"]="Interface\\TalentFrame\\DruidFeralCombat-BottomRight",
        ["TopLeft"]="Interface\\TalentFrame\\DruidFeralCombat-TopLeft",
        ["TopRight"]="Interface\\TalentFrame\\DruidFeralCombat-TopRight"},
      ["icon"]="Interface\\Icons\\Ability_Physical_Taunt",
      ["id"]=281,
      ["name"]="Feral Combat",
      ["tab"]=2,
      ["talents"]={
        {
          ["column"]=2,
          ["descriptions"]={
            "Increases the Attack Power reduction of your Demoralizing Roar by $s1% and the damage caused by your Ferocious Bite by $s2%.",
            "Increases the Attack Power reduction of your Demoralizing Roar by $s1% and the damage caused by your Ferocious Bite by $s2%.",
            "Increases the Attack Power reduction of your Demoralizing Roar by $s1% and the damage caused by your Ferocious Bite by $s2%.",
            "Increases the Attack Power reduction of your Demoralizing Roar by $s1% and the damage caused by your Ferocious Bite by $s2%.",
            "Increases the Attack Power reduction of your Demoralizing Roar by $s1% and the damage caused by your Ferocious Bite by $s2%."},
          ["exceptional"]=false,
          ["granted_spells"]={
            ["16858"]={
              },
            ["16859"]={
              },
            ["16860"]={
              },
            ["16861"]={
              },
            ["16862"]={
              }},
          ["icon"]="Interface\\Icons\\Ability_Druid_DemoralizingRoar",
          ["id"]=795,
          ["max_rank"]=5,
          ["name"]="Feral Aggression",
          ["prerequisites"]={
            },
          ["rank_spells"]={
            16858,
            16859,
            16860,
            16861,
            16862},
          ["required_spell"]=0,
          ["tier"]=0,
          ["rank"]=0,
          ["next_rank"]=1,
          ["learnable"]=true,
          ["reasons"]={
            },
          ["meets_prerequisites"]=true},
        {
          ["column"]=0,
          ["descriptions"]={
            "Increases threat caused in Bear and Dire Bear Form by $s2% and reduces the chance enemies have to detect you while Prowling.",
            "Increases threat caused in Bear and Dire Bear Form by $s2% and reduces the chance enemies have to detect you while Prowling.",
            "Increases threat caused in Bear and Dire Bear Form by $s2% and reduces the chance enemies have to detect you while Prowling.",
            "Increases threat caused in Bear and Dire Bear Form by $s2% and reduces the chance enemies have to detect you while Prowling.",
            "Increases threat caused in Bear and Dire Bear Form by $s2% and reduces the chance enemies have to detect you while Prowling."},
          ["exceptional"]=false,
          ["granted_spells"]={
            ["16947"]={
              },
            ["16948"]={
              },
            ["16949"]={
              },
            ["16950"]={
              },
            ["16951"]={
              }},
          ["icon"]="Interface\\Icons\\Ability_Ambush",
          ["id"]=799,
          ["max_rank"]=5,
          ["name"]="Feral Instinct",
          ["prerequisites"]={
            },
          ["rank_spells"]={
            16947,
            16948,
            16949,
            16950,
            16951},
          ["required_spell"]=0,
          ["tier"]=1,
          ["rank"]=0,
          ["next_rank"]=1,
          ["learnable"]=false,
          ["reasons"]={
            "tier_points"},
          ["meets_prerequisites"]=true},
        {
          ["column"]=0,
          ["descriptions"]={
            "Increases your movement speed by $s1% while outdoors in Cat Form and increases your chance to dodge while in Cat Form by $24867s1%.",
            "Increases your movement speed by $s1% while outdoors in Cat Form and increases your chance to dodge while in Cat Form by $24864s1%."},
          ["exceptional"]=false,
          ["granted_spells"]={
            ["17002"]={
              },
            ["24866"]={
              }},
          ["icon"]="Interface\\Icons\\Spell_Nature_SpiritWolf",
          ["id"]=807,
          ["max_rank"]=2,
          ["name"]="Feline Swiftness",
          ["prerequisites"]={
            },
          ["rank_spells"]={
            17002,
            24866},
          ["required_spell"]=0,
          ["tier"]=2,
          ["rank"]=0,
          ["next_rank"]=1,
          ["learnable"]=false,
          ["reasons"]={
            "tier_points"},
          ["meets_prerequisites"]=true}},
      ["points_spent"]=0},
    {
      ["background"]="DruidRestoration",
      ["background_textures"]={
        ["BottomLeft"]="Interface\\TalentFrame\\DruidRestoration-BottomLeft",
        ["BottomRight"]="Interface\\TalentFrame\\DruidRestoration-BottomRight",
        ["TopLeft"]="Interface\\TalentFrame\\DruidRestoration-TopLeft",
        ["TopRight"]="Interface\\TalentFrame\\DruidRestoration-TopRight"},
      ["icon"]="Interface\\Icons\\Spell_Nature_HealingTouch",
      ["id"]=282,
      ["name"]="Restoration",
      ["tab"]=3,
      ["talents"]={
        {
          ["column"]=1,
          ["descriptions"]={
            "Increases the effects of your Mark of the Wild and Gift of the Wild spells by $s1%.",
            "Increases the effects of your Mark of the Wild and Gift of the Wild spells by $s1%.",
            "Increases the effects of your Mark of the Wild and Gift of the Wild spells by $s1%.",
            "Increases the effects of your Mark of the Wild and Gift of the Wild spells by $s1%.",
            "Increases the effects of your Mark of the Wild and Gift of the Wild spells by $s1%."},
          ["exceptional"]=false,
          ["granted_spells"]={
            ["17050"]={
              },
            ["17051"]={
              },
            ["17053"]={
              },
            ["17054"]={
              },
            ["17055"]={
              }},
          ["icon"]="Interface\\Icons\\Spell_Nature_Regeneration",
          ["id"]=821,
          ["max_rank"]=5,
          ["name"]="Improved Mark of the Wild",
          ["prerequisites"]={
            },
          ["rank_spells"]={
            17050,
            17051,
            17053,
            17054,
            17055},
          ["required_spell"]=0,
          ["tier"]=0,
          ["rank"]=0,
          ["next_rank"]=1,
          ["learnable"]=true,
          ["reasons"]={
            },
          ["meets_prerequisites"]=true},
        {
          ["column"]=1,
          ["descriptions"]={
            "Gives you a $s1% chance to avoid interruption caused by damage while casting the Healing Touch, Regrowth and Tranquility spells.",
            "Gives you a $s1% chance to avoid interruption caused by damage while casting the Healing Touch, Regrowth and Tranquility spells.",
            "Gives you a $s1% chance to avoid interruption caused by damage while casting the Healing Touch, Regrowth and Tranquility spells.",
            "Gives you a $s1% chance to avoid interruption caused by damage while casting the Healing Touch, Regrowth and Tranquility spells.",
            "Gives you a $s1% chance to avoid interruption caused by damage while casting the Healing Touch, Regrowth and Tranquility spells."},
          ["exceptional"]=false,
          ["granted_spells"]={
            ["17063"]={
              },
            ["17065"]={
              },
            ["17066"]={
              },
            ["17067"]={
              },
            ["17068"]={
              }},
          ["icon"]="Interface\\Icons\\Spell_Nature_HealingWaveGreater",
          ["id"]=823,
          ["max_rank"]=5,
          ["name"]="Nature's Focus",
          ["prerequisites"]={
            },
          ["rank_spells"]={
            17063,
            17065,
            17066,
            17067,
            17068},
          ["required_spell"]=0,
          ["tier"]=1,
          ["rank"]=0,
          ["next_rank"]=1,
          ["learnable"]=false,
          ["reasons"]={
            "tier_points"},
          ["meets_prerequisites"]=true},
        {
          ["column"]=2,
          ["descriptions"]={
            "The enemy target is swarmed by insects, decreasing their chance to hit by $s2% and causing $o1 Nature damage over $d."},
          ["exceptional"]=true,
          ["granted_spells"]={
            ["5570"]={
              }},
          ["icon"]="Interface\\Icons\\Spell_Nature_InsectSwarm",
          ["id"]=827,
          ["max_rank"]=1,
          ["name"]="Insect Swarm",
          ["prerequisites"]={
            },
          ["rank_spells"]={
            5570},
          ["required_spell"]=0,
          ["tier"]=2,
          ["rank"]=0,
          ["next_rank"]=1,
          ["learnable"]=false,
          ["reasons"]={
            "tier_points"},
          ["meets_prerequisites"]=true}},
      ["points_spent"]=0}}}
