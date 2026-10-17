#include "Carriers.h"
#include "CarrierPersistence.h"
#include <charconv>

#include "Character.h"
#include "Cast.h"
#include "Console.h"
#include "ControlTiming.h"
#include "Frame.h"
#include "Service.h"
#include "SkyrimAuras.h"

namespace Carriers
{
    using json=nlohmann::json;
	namespace
	{
		RE::SpellItem* g_damage = nullptr;
		RE::SpellItem* g_heal = nullptr;
		RE::SpellItem* g_root = nullptr;
		RE::SpellItem* g_sleep = nullptr;
		RE::SpellItem* g_calm = nullptr;
		RE::SpellItem* g_fear = nullptr;
		RE::SpellItem* g_stealth = nullptr;
		std::map<std::uint32_t, RE::SpellItem*> g_racials;

		RE::EffectSetting* MakeEffect(const char* a_name, bool a_hostile, RE::ActorValue a_value = RE::ActorValue::kHealth, bool a_lasting = false)
		{
			auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::EffectSetting>();
			auto* mgef = factory ? factory->Create() : nullptr;
			if (!mgef) {
				return nullptr;
			}
			mgef->fullName = a_name;
			auto& d = mgef->data;
			using F = RE::EffectSetting::EffectSettingData::Flag;
			d.flags.set(F::kNoArea, F::kPainless);
			d.flags.set(a_lasting ? F::kRecover : F::kNoDuration);
			// A harmful one is a hit, as any attack in Skyrim is: the victim and its friends fight back, and a person
			// struck is assaulted (the user's playtest: roots provoked nothing). A heal raises nothing.
			if (a_hostile) {
				d.flags.set(F::kHostile, F::kDetrimental);
			} else {
				d.flags.set(F::kNoHitEvent);
			}
			d.archetype = RE::EffectSetting::Archetype::kValueModifier;
			d.primaryAV = a_value;
			d.castingType = RE::MagicSystem::CastingType::kFireAndForget;
			d.delivery = RE::MagicSystem::Delivery::kTargetActor;
			d.associatedSkill = RE::ActorValue::kNone;
			// No vanilla school/element/healing keywords: the service owns those perk multipliers.
			// Necromage's target-only entry point can still match; it stays native, not a service modifier.
			d.resistVariable = RE::ActorValue::kNone;
			d.secondaryAV = RE::ActorValue::kNone;
			d.baseCost = 0.0f;
			return mgef;
		}

		RE::SpellItem* MakeSpell(const char* a_name, RE::EffectSetting* a_effect)
		{
			auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::SpellItem>();
			auto* spell = factory ? factory->Create() : nullptr;
			if (!spell || !a_effect) {
				return nullptr;
			}
			spell->fullName = a_name;
			auto& d = spell->data;
			d.spellType = RE::MagicSystem::SpellType::kSpell;
			d.castingType = RE::MagicSystem::CastingType::kFireAndForget;
			d.delivery = RE::MagicSystem::Delivery::kTargetActor;
			d.costOverride = 0;
			d.flags.set(RE::SpellItem::SpellFlag::kCostOverride, RE::SpellItem::SpellFlag::kIgnoreResistance,
				RE::SpellItem::SpellFlag::kNoAbsorb, RE::SpellItem::SpellFlag::kNoDualCastMods);
			auto* effect = new RE::Effect();
			effect->baseEffect = a_effect;
			effect->effectItem.magnitude = 1.0f;
			effect->effectItem.area = 0;
			effect->effectItem.duration = 0;
			effect->cost = 0.0f;
			spell->effects.push_back(effect);
			return spell;
		}

		void Cast(RE::SpellItem* a_spell, RE::Actor* a_target, float a_amount)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* caster = player ? player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant) : nullptr;
			if (!a_spell || !a_target || !caster || a_amount <= 0.0f) {
				return;
			}
			caster->CastSpellImmediate(a_spell, true, a_target, 1.0f, false, a_amount, player);
		}

		// Skyrim works out an actor's movement speed again only when its carry weight changes: a changed speed
		// multiplier is otherwise not felt.
		void RefreshSpeed(RE::Actor* a_actor)
		{
			auto* av = a_actor->AsActorValueOwner();
			av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kCarryWeight, 1.0f);
			av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kCarryWeight, -1.0f);
		}
	}

	bool Create()
	{
		g_damage = MakeSpell("WoWBridge damage", MakeEffect("WoWBridge damage", true));
		g_heal = MakeSpell("WoWBridge heal", MakeEffect("WoWBridge heal", false));
		g_root = MakeSpell("WoWBridge root", MakeEffect("WoWBridge root", true, RE::ActorValue::kSpeedMult, true));
		if (auto* sleep = MakeEffect("WoWBridge sleep", true, RE::ActorValue::kParalysis, true)) {
			sleep->data.archetype = RE::EffectSetting::Archetype::kParalysis;
			g_sleep = MakeSpell("WoWBridge sleep", sleep);
			if (g_sleep) g_sleep->effects[0]->effectItem.duration = ControlTiming::kParalysisBackstopSeconds;
		}
		if (auto* calm = MakeEffect("WoWBridge calm", false, RE::ActorValue::kAggression, true)) {
			calm->data.archetype = RE::EffectSetting::Archetype::kCalm;
			g_calm = MakeSpell("WoWBridge calm", calm);
			if (g_calm) g_calm->effects[0]->effectItem.duration = ControlTiming::kParalysisBackstopSeconds;
		}
		if (auto* fear = MakeEffect("WoWBridge fear", true, RE::ActorValue::kConfidence, true)) {
			fear->data.archetype = RE::EffectSetting::Archetype::kDemoralize;
			g_fear = MakeSpell("WoWBridge fear", fear);
			if (g_fear) g_fear->effects[0]->effectItem.duration = ControlTiming::kParalysisBackstopSeconds;
		}
		if (auto* stealth = MakeEffect("WoWBridge stealth", false, RE::ActorValue::kInvisibility, true)) {
			stealth->data.archetype = RE::EffectSetting::Archetype::kInvisibility;
			g_stealth = MakeSpell("Stealth", stealth);
		}
		// Skyrim's armor mitigates .12% per point: Quickness's 1% avoidance becomes 8 1/3 armor.
		// Nature follows the existing gear mapping, WoW resistance /3 -> Skyrim poison resistance.
		// Wisp Spirit is a named zero-magnitude marker; Skyrim has no released ghost to accelerate.
		// The Human's and Dwarf's (service/racials.py RACE_PASSIVES) the same ways: Frost Resistance as Nature
		// Resistance is, gear's rate (skyrim_effects RESIST_DIVISOR, WoW resistance /3) as Resist Frost; Sword and
		// Mace Specialization (+5 weapon skill on the WoW attack table melee.py rolls; Skyrim's One-Handed is not
		// raised), Diplomacy (reputation: Skyrim keeps none) and Gun Specialization (no Paladin has Guns; Skyrim has no
		// guns) as named zero-magnitude markers, as Cultivation is. The Human Spirit is a Buff carrier (HumanSpirit).
		for (const auto& [id, name, value, amount] : {
			std::tuple{ 20582u, "Quickness", RE::ActorValue::kDamageResist, 1.0f / 0.12f },
			std::tuple{ 20583u, "Nature Resistance", RE::ActorValue::kPoisonResist, 10.0f / 3.0f },
			std::tuple{ 20585u, "Wisp Spirit", RE::ActorValue::kSpeedMult, 0.0f },
			std::tuple{ 20550u, "Endurance", RE::ActorValue::kHealth, 0.0f },
			std::tuple{ 20551u, "Nature Resistance", RE::ActorValue::kPoisonResist, 10.0f / 3.0f },
			std::tuple{ 20552u, "Cultivation", RE::ActorValue::kAlchemy, 0.0f },
			std::tuple{ 20597u, "Sword Specialization", RE::ActorValue::kOneHanded, 0.0f },
			std::tuple{ 20599u, "Diplomacy", RE::ActorValue::kSpeech, 0.0f },
			std::tuple{ 20864u, "Mace Specialization", RE::ActorValue::kOneHanded, 0.0f },
			std::tuple{ 20595u, "Gun Specialization", RE::ActorValue::kArchery, 0.0f },
			std::tuple{ 20596u, "Frost Resistance", RE::ActorValue::kResistFrost, 10.0f / 3.0f } }) {
			auto* effect = MakeEffect(name, false, value, true);
			auto* spell = MakeSpell(name, effect);
			if (spell) {
				spell->data.spellType = RE::MagicSystem::SpellType::kAbility;
				spell->data.castingType = effect->data.castingType = RE::MagicSystem::CastingType::kConstantEffect;
				spell->data.delivery = effect->data.delivery = RE::MagicSystem::Delivery::kSelf;
				spell->effects[0]->effectItem.magnitude = amount;
				g_racials[id] = spell;
			}
		}
		logger::info("carriers: damage {} heal {} root {} sleep {}", g_damage != nullptr, g_heal != nullptr, g_root != nullptr, g_sleep != nullptr);
		return g_damage && g_heal && g_root;
	}

	void Damage(RE::Actor* a_target, float a_amount)
	{
		Cast(g_damage, a_target, a_amount);
		Provoke(a_target);
	}

	// The carriers land as direct effects, not hits, so alone they never raise the assault an attack does (the user's
	// report: Wrath, Faerie Fire and Roots on a townsperson provoked nobody). Skyrim's own alarm then does it all:
	// the victim fights or flees, witnesses and guards join, the hold's crime faction sets the bounty. Their
	// followers forgive the player, as WoW's party cannot be attacked; a periodic effect's ticks raise it once.
	void Provoke(RE::Actor* a_target)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!a_target || a_target == player || a_target->IsDead() || a_target->IsPlayerTeammate() || a_target->IsHostileToActor(player)) {
			return;
		}
		static std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> last;
		const auto now = std::chrono::steady_clock::now();
		if (auto it = last.find(a_target->GetFormID()); it != last.end() && now - it->second < std::chrono::seconds(3)) {
			return;
		}
		last[a_target->GetFormID()] = now;
		Console::Execute("sendassaultalarm", a_target);
	}
	void Heal(RE::Actor* a_target, float a_amount) { Cast(g_heal, a_target, a_amount); }

	namespace
	{
		constexpr std::uint32_t kHumanSpirit = 20598, kPerception = 20600, kStoneform = 20594;
		constexpr RE::FormID    kMagickaRate = 0x7A0FD;   // EnchFortifyMagickaRateConstantSelf (skyrim_effects STAT[6])
		constexpr RE::FormID    kDetectLifeScroll = 0x1019AE;   // TGDetectLifeScroll
		constexpr RE::FormID    kBleeding = 0x0C367A;   // PerkBleedingDamage: the axe perks' Bleeding Damage

		// Each race's General-tab passives (service/racials.py PASSIVES_BY_RACE, RACE_PASSIVES).
		bool Mine(std::uint32_t a_race, std::uint32_t a_spell)
		{
			static const std::map<std::uint32_t, std::set<std::uint32_t>> races{ { 4, { 20582, 20583, 20585 } },
				{ 6, { 20550, 20551, 20552 } }, { 1, { 20597, 20599, 20864 } }, { 3, { 20595, 20596 } } };
			const auto it = races.find(a_race);
			return it != races.end() && it->second.contains(a_spell);
		}

		bool Has(RE::Actor* a_actor, std::function<bool(const RE::ActiveEffect*)> a_which)
		{
			if (auto* effects = a_actor->GetActiveEffectList()) for (auto* e : *effects) {
				if (e && e->spell && !e->flags.any(RE::ActiveEffect::Flag::kDispelled) && a_which(e)) return true;
			}
			return false;
		}

		// The Human Spirit (Human): its share of their Spirit is bonus Spirit, natively Fortify Magicka Rate a point a
		// Spirit point - the service's stats racials[].skyrim (aura_effects' stat route), carried as Forms' talent
		// buff carries Spirit (a Buff, so the co-save ledger and Bridge's owned values know it). Asked again when what
		// the player is changes (Character::Caster, less its live actor values), at most once a second.
		float g_humanSpirit = 0.0f, g_humanSpiritOn = -1.0f;
		bool  g_spiritKnown = false;   // the service has said their Spirit share since the last load
		json  g_spiritAsked;
		bool  g_spiritPending = false;
		double g_spiritNext = 0;
		void HumanSpirit(RE::PlayerCharacter* a_player, bool a_human)
		{
			if (!a_human) {
				if (g_humanSpiritOn >= 0) Unbuff(a_player, kHumanSpirit);
				g_humanSpiritOn = -1.0f;
				return;
			}
			if (!g_spiritPending && Frame::Now() >= g_spiritNext) {
				g_spiritNext = Frame::Now() + 1.0;
				auto caster = Character::Caster();
				caster.erase("actor_values");   // they move as carriers (this one too) land; Spirit never reads them
				if (caster != g_spiritAsked) {
					g_spiritAsked = caster;
					g_spiritPending = true;
					Service::Request({ { "op", "stats" }, { "caster", caster } }, [](const json& a_reply) {
						g_spiritPending = false;
						if (!a_reply.value("ok", false)) { g_spiritAsked = json(); return; }
						g_spiritKnown = true;
						float share = 0.0f;
						for (const auto& p : a_reply.value("racials", json::array())) if (p.value("spell", 0u) == kHumanSpirit)
							for (const auto& e : p.value("skyrim", json::array())) if (e.value("mgef", 0u) == kMagickaRate) share += e.value("magnitude", 0.0f);
						g_humanSpirit = share;
					});
				}
			}
			const bool on = Has(a_player, [](const RE::ActiveEffect* e) { return IsBuff(e->spell->GetFormID(), kHumanSpirit); });
			if (!g_spiritKnown) return;   // a loaded buff stays until the service says their share (no flicker after a load)
			if (on && std::abs(g_humanSpiritOn - g_humanSpirit) < .001f) return;
			if (g_humanSpirit > 0) {
				Buff(a_player, kHumanSpirit, "The Human Spirit", { { kMagickaRate, g_humanSpirit } }, 1e7f);   // replaces the old
				g_humanSpiritOn = g_humanSpirit;
			} else if (on) {
				Unbuff(a_player, kHumanSpirit);
				g_humanSpiritOn = -1.0f;
			}
		}

		// Perception (Human): Skyrim has no stealth detection value; its sight of hidden creatures is Detect Life. While
		// their WoW aura lasts (Cast owns it), the Thieves Guild scroll's four fire-and-forget Detect Life effects - living
		// enemies and friends, 200 feet outdoors and 100 indoors, with its conditions - are on the player, ended with the aura.
		RE::SpellItem* g_perception = nullptr;
		bool           g_perceiving = false;
		RE::SpellItem* DetectLife()
		{
			static bool made = false;
			if (made) return g_perception;
			made = true;
			auto* scroll = RE::TESForm::LookupByID<RE::ScrollItem>(kDetectLifeScroll);
			auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::SpellItem>();
			auto* spell = scroll && factory ? factory->Create() : nullptr;
			if (!spell) { logger::warn("carriers: no Detect Life scroll for Perception"); return nullptr; }
			spell->fullName = "Perception";
			auto& d = spell->data;
			d.spellType = RE::MagicSystem::SpellType::kSpell;
			d.castingType = RE::MagicSystem::CastingType::kFireAndForget;
			d.delivery = RE::MagicSystem::Delivery::kSelf;
			d.costOverride = 0;
			d.flags.set(RE::SpellItem::SpellFlag::kCostOverride, RE::SpellItem::SpellFlag::kNoAbsorb, RE::SpellItem::SpellFlag::kNoDualCastMods);
			for (const auto* source : scroll->effects) {
				if (!source || !source->baseEffect) continue;
				auto* effect = new RE::Effect();
				effect->baseEffect = source->baseEffect;
				effect->effectItem = source->effectItem;
				auto** tail = &effect->conditions.head;
				for (auto* c = source->conditions.head; c; c = c->next) {
					auto* item = new RE::TESConditionItem();
					item->data = c->data;
					item->next = nullptr;
					*tail = item;
					tail = &item->next;
				}
				spell->effects.push_back(effect);
			}
			g_perception = spell->effects.empty() ? nullptr : spell;
			return g_perception;
		}
		// Cast once for the aura's time (its effects take the aura's remaining seconds): a fire-and-forget Detect Life
		// does not stay in their active effect list (it lights the living and is gone from the list), so the carrier keeps
		// its own clock instead of re-casting every half second as it once did.
		double g_perceptionUntil = 0;
		void Perception(RE::PlayerCharacter* a_player)
		{
			const double left = ::Cast::AuraRemaining(kPerception);
			auto* spell = left > 0 || g_perceiving ? DetectLife() : nullptr;
			if (!spell) return;
			const double until = Frame::Now() + left;
			if (left > 0 && (!g_perceiving || until > g_perceptionUntil + 1.0)) {   // a new aura, or a refreshed one
				for (auto* e : spell->effects) e->effectItem.duration = static_cast<std::uint32_t>(std::ceil(left));
				if (auto* caster = a_player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant))
					caster->CastSpellImmediate(spell, true, a_player, 1.0f, false, 0.0f, a_player);
				g_perceiving = true;
				g_perceptionUntil = until;
			} else if (left <= 0 && g_perceiving) {
				auto handle = a_player->CreateRefHandle();
				a_player->GetMagicTarget()->DispelEffect(spell, handle);   // cancelled early: its light goes too
				g_perceiving = false;
			}
		}

		// Stoneform (Dwarf): applying its immunities purges what they cover and they refuse new ones while it lasts
		// (Unit::ApplySpellDispelImmunity, SPELL_ATTR_EX_IMMUNITY_PURGES_EFFECT; Aura::HandleModMechanicImmunity's
		// RemoveAurasAtMechanicImmunity). Their Skyrim poisons and diseases, as a WoW dispel of those types reads them
		// (SkyrimAuras::DispelType; a contracted disease is a spell the player has), and Skyrim's bleeding end the frame they
		// are on the player. Its Resist Poison/Disease 100 (the aura's service-named Skyrim effects) refuse most of them first.
		void Stoneform(RE::PlayerCharacter* a_player)
		{
			if (::Cast::AuraRemaining(kStoneform) <= 0) return;
			std::vector<RE::ActiveEffect*> ending;
			if (auto* effects = a_player->GetActiveEffectList()) for (auto* e : *effects) {
				const auto* base = e ? e->GetBaseObject() : nullptr;
				if (!base || !e->spell || e->flags.any(RE::ActiveEffect::Flag::kDispelled)) continue;
				const auto type = SkyrimAuras::Visible(e) ? SkyrimAuras::DispelType(e) : std::string();
				if (type == "Poison" || type == "Disease" || base->GetFormID() == kBleeding) ending.push_back(e);
			}
			for (auto* e : ending) e->Dispel(true);
			std::vector<RE::SpellItem*> diseases;
			for (auto* spell : a_player->GetActorRuntimeData().addedSpells) {
				if (spell && spell->GetSpellType() == RE::MagicSystem::SpellType::kDisease) diseases.push_back(spell);
			}
			for (auto* disease : diseases) a_player->RemoveSpell(disease);
		}
	}

	bool Perceiving() { return g_perceiving; }

	void Racials()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !Character::Get().level) {
			return;
		}
		const auto race = Character::Looks().race;
		HumanSpirit(player, race == 1);
		Perception(player);
		Stoneform(player);
		for (const auto& [id, spell] : g_racials) {
			const bool mine = Mine(race, id);
			if (mine) {
				if (id == 20550) {
					float owned = 0.f;
					if (auto* effects = player->GetActiveEffectList()) for (auto* e : *effects) {
						if (e && e->spell == spell && !e->flags.any(RE::ActiveEffect::Flag::kInactive,RE::ActiveEffect::Flag::kDispelled)) {
							if (auto* value = skyrim_cast<RE::ValueModifierEffect*>(e)) owned += value->value;
						}
					}
					const auto av = RE::ActorValue::kHealth;
					const float maximum = player->AsActorValueOwner()->GetActorValue(av) - player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage,av);
					const float bonus = std::max(0.f,maximum - owned) * .05f;
					if (std::abs(bonus - owned) > .01f) {
						// Preserve the health percentage when recalculating; never heal by repeatedly installing the ability.
						const float fraction = maximum > 0 ? player->AsActorValueOwner()->GetActorValue(av) / maximum : 1.f;
						if (player->HasSpell(spell)) player->RemoveSpell(spell);
						spell->effects[0]->effectItem.magnitude = bonus;
						player->AddSpell(spell);
						const float after = player->AsActorValueOwner()->GetActorValue(av) - player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage,av);
						player->AsActorValueOwner()->RestoreActorValue(av, after * fraction - player->AsActorValueOwner()->GetActorValue(av));
					}
				}
				if (!player->HasSpell(spell)) {
					player->AddSpell(spell);
				}
			} else if (player->HasSpell(spell)) {
				const auto av = RE::ActorValue::kHealth;
				const float current = player->AsActorValueOwner()->GetActorValue(av);
				const float maximum = current - player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage,av);
				player->RemoveSpell(spell);
				if (id == 20550 && maximum > 0) {
					const float after = player->AsActorValueOwner()->GetActorValue(av) - player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage,av);
					player->AsActorValueOwner()->RestoreActorValue(av, after * current / maximum - player->AsActorValueOwner()->GetActorValue(av));
				}
			}
		}
	}

	bool HasRacial(std::uint32_t a_wowSpell)
	{
		const auto it = g_racials.find(a_wowSpell);
		auto* player = RE::PlayerCharacter::GetSingleton();
		return player && it != g_racials.end() && player->HasSpell(it->second);
	}

	void Root(RE::Actor* a_target, float a_seconds)
	{
		if (!g_root || !a_target || ::Cast::MovementImmune(a_target)) {
			return;
		}
		g_root->effects[0]->effectItem.duration = static_cast<std::uint32_t>(std::ceil(a_seconds)) + 1;
		Cast(g_root, a_target, 100.0f);   // the whole of a 100 speed multiplier
		RefreshSpeed(a_target);
	}

	namespace
	{
		// One lasting value-modifier effect per actor value and direction, made once.
		RE::EffectSetting* Lasting(RE::ActorValue a_value, bool a_lower)
		{
			static std::map<std::pair<RE::ActorValue, bool>, RE::EffectSetting*> made;
			auto& e = made[{ a_value, a_lower }];
			if (!e) {
				e = MakeEffect("WoWBridge aura", a_lower, a_value, true);
				if (e && a_lower) {
					e->data.flags.reset(RE::EffectSetting::EffectSettingData::Flag::kPainless);
				}
			}
			return e;
		}

		// A WoW spell's Skyrim spells, one per distinct set of changes and duration, never altered once cast: the
		// active effects it starts point into its effect list for as long as they last.
		std::map<std::uint32_t, std::vector<std::pair<std::string, RE::SpellItem*>>> g_buffs;
        std::optional<json> g_loadedModifiers;
        std::set<RE::FormID> CarrierIds() {
            std::set<RE::FormID> ids;
            for (const auto& [wow,versions] : g_buffs) for (const auto& [key,spell] : versions)
                if (spell) ids.insert(spell->GetFormID());
            for (const auto* spell : {g_root,g_sleep,g_calm,g_stealth}) if (spell) ids.insert(spell->GetFormID());
            return ids;
        }
	}

    void Save(SKSE::SerializationInterface* intfc) {
        auto* player=RE::PlayerCharacter::GetSingleton();
        const auto ids=CarrierIds();
        std::map<int,float> owned;
        if (auto* effects=player->GetActiveEffectList()) for (auto* effect : *effects) {
            if (!effect || !effect->spell || !ids.contains(effect->spell->GetFormID()) ||
                effect->flags.any(RE::ActiveEffect::Flag::kInactive,RE::ActiveEffect::Flag::kDispelled)) continue;
            const auto* value=skyrim_cast<RE::ValueModifierEffect*>(effect);
            if (value && value->actorValue!=RE::ActorValue::kNone && std::isfinite(value->value))
                owned[static_cast<int>(value->actorValue)]+=value->value;
        }
        json data={{"spells",ids},{"values",json::object()}};
        for (const auto& [id,amount] : owned)
            data["values"][std::to_string(id)]={{"owned",amount},{"temporary",player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary,static_cast<RE::ActorValue>(id))}};
        const auto bytes=data.dump();
        if (!intfc->OpenRecord(kRecord,kRecordVersion) || !intfc->WriteRecordData(bytes.data(),static_cast<std::uint32_t>(bytes.size())))
            logger::error("carriers: modifier ledger was not saved");
    }
    void PrepareLoad() { g_loadedModifiers.reset(); }
    void Load(SKSE::SerializationInterface* intfc,std::uint32_t length,std::uint32_t version) {
        if (version!=kRecordVersion || !length || length>1024*1024) { logger::warn("carriers: invalid modifier ledger"); return; }
        std::string bytes(length,'\0');
        if (intfc->ReadRecordData(bytes.data(),length)!=length) return;
        auto data=json::parse(bytes,nullptr,false);
        if (data.is_object() && data.contains("spells") && data["spells"].is_array() &&
            data.contains("values") && data["values"].is_object()) g_loadedModifiers=std::move(data);
        else logger::warn("carriers: invalid modifier ledger");
    }
    void OnGameLoaded() {
        g_perceiving = false; g_perceptionUntil = 0;   // a loaded aura casts its Detect Life again
        g_humanSpirit = 0; g_humanSpiritOn = -1.0f; g_spiritAsked = json(); g_spiritPending = false; g_spiritKnown = false;   // asked afresh for the player
        if (!g_loadedModifiers) return; // older co-saves have no ownership ledger
        auto data=std::move(*g_loadedModifiers); g_loadedModifiers.reset();
        std::set<RE::FormID> ids;
        for (const auto& id : data["spells"]) if (id.is_number_unsigned() && id.get<std::uint64_t>()<=0xFFFFFFFFu && id.get<std::uint32_t>()>=0xFF000000u)
            ids.insert(id.get<std::uint32_t>());
        auto* player=RE::PlayerCharacter::GetSingleton();
        std::set<int> surviving;
        if (auto* effects=player->GetActiveEffectList()) for (auto* effect : *effects) {
            if (!effect || !effect->spell || !ids.contains(effect->spell->GetFormID()) ||
                effect->flags.any(RE::ActiveEffect::Flag::kInactive,RE::ActiveEffect::Flag::kDispelled)) continue;
            if (const auto* value=skyrim_cast<RE::ValueModifierEffect*>(effect)) surviving.insert(static_cast<int>(value->actorValue));
        }
        for (const auto& [key,row] : data["values"].items()) {
            int id=-1;
            const auto [end,error]=std::from_chars(key.data(),key.data()+key.size(),id);
            if (error!=std::errc{} || end!=key.data()+key.size() || id<0 || id>=static_cast<int>(RE::ActorValue::kTotal) ||
                !row.is_object() || !row.contains("owned") || !row["owned"].is_number() || !row.contains("temporary") || !row["temporary"].is_number()) continue;
            const auto av=static_cast<RE::ActorValue>(id);
            const auto correction=CarrierPersistence::MissingModifier(row["temporary"].get<float>(),row["owned"].get<float>(),
                player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary,av),surviving.contains(id));
            if (!correction) { logger::warn("carriers: native modifier {} differs from the saved ledger; left unchanged",id); continue; }
            if (std::abs(*correction)<.001f) continue;
            player->AsActorValueOwner()->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary,av,*correction);
            logger::info("carriers: recovered orphaned modifier {} by {}",id,*correction);
        }
        RefreshSpeed(player);
    }

	void Buff(RE::Actor* a_target, std::uint32_t a_wowSpell, const std::string& a_name, const std::vector<Change>& a_changes, float a_seconds)
	{
		auto* caster = RE::PlayerCharacter::GetSingleton()->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
		if (!a_target || !caster || a_changes.empty()) {
			return;
		}
		const auto seconds = static_cast<std::uint32_t>(std::ceil(std::min(a_seconds, 1e7f))) + 1;   // a form lasts until left
		std::string key = std::to_string(seconds);
		for (const auto& c : a_changes) {
			key += std::format(" {:X}/{}:{:.3f}", c.vanilla, static_cast<int>(c.value), c.magnitude);
		}
		auto&          versions = g_buffs[a_wowSpell];
		RE::SpellItem* spell = nullptr;
		if (const auto it = std::ranges::find(versions, key, &std::pair<std::string, RE::SpellItem*>::first); it != versions.end()) {
			spell = it->second;
		} else {
			std::vector<RE::Effect*> effects;
			for (const auto& c : a_changes) {
				const auto* vanilla = c.vanilla ? RE::TESForm::LookupByID<RE::EffectSetting>(c.vanilla) : nullptr;
				// A detrimental vanilla effect lowers by its positive magnitude (Faerie Fire's armor), as a negative one does.
				auto* base = vanilla ? Lasting(vanilla->data.primaryAV, c.magnitude < 0.0f || vanilla->IsDetrimental()) :
				             c.value != RE::ActorValue::kNone ? Lasting(c.value, c.magnitude < 0.0f) : nullptr;
				if (!base) {
					logger::warn("carriers: {} names magic effect {:08X}, not a Skyrim.esm value modifier", a_name, c.vanilla);
					continue;
				}
				auto* effect = new RE::Effect();
				effect->baseEffect = base;
				effect->effectItem.magnitude = std::abs(c.magnitude);
				effect->effectItem.duration = seconds;
				effects.push_back(effect);
			}
			if (effects.empty() || !(spell = MakeSpell(a_name.c_str(), effects[0]->baseEffect))) {
				return;
			}
			delete spell->effects[0];   // MakeSpell's own, not yet used by anything
			spell->effects.clear();
			for (auto* e : effects) {
				spell->effects.push_back(e);
			}
			versions.emplace_back(key, spell);
		}
		Unbuff(a_target, a_wowSpell);   // a new cast replaces the old (the WoW aura did the same)
		caster->CastSpellImmediate(spell, true, a_target, 1.0f, false, 0.0f, RE::PlayerCharacter::GetSingleton());
		RefreshSpeed(a_target);   // Travel Form's, Dash's speed felt now
	}

	bool IsControlCarrier(const RE::MagicItem* a_spell)
	{
		return a_spell && (a_spell == g_root || a_spell == g_sleep || a_spell == g_fear);
	}

	bool IsBuff(RE::FormID a_nativeSpell, std::uint32_t a_wowSpell)
	{
		const auto it = g_buffs.find(a_wowSpell);
		return it != g_buffs.end() && std::ranges::any_of(it->second, [a_nativeSpell](const auto& version) {
			return version.second && version.second->GetFormID() == a_nativeSpell; });
	}

	void Unbuff(RE::Actor* a_target, std::uint32_t a_wowSpell)
	{
		const auto it = g_buffs.find(a_wowSpell);
		auto*      target = a_target ? a_target->GetMagicTarget() : nullptr;
		if (!target || it == g_buffs.end()) {
			return;
		}
		auto caster = RE::PlayerCharacter::GetSingleton()->CreateRefHandle();
		for (const auto& [key, spell] : it->second) {
			target->DispelEffect(spell, caster);
		}
		RefreshSpeed(a_target);
	}

	void Sleep(RE::Actor* a_target, float a_seconds)
	{
		if (!g_sleep || !a_target || a_seconds <= 0) {
			return;
		}
		// Cast owns the exact controlEnd and wakes the target on expiry/break/reset.
		// A second native deadline can expire early; mutating this shared Effect
		// also changes the form referenced by earlier active effects.
		Cast(g_sleep, a_target, 1.0f);
	}

	void Wake(RE::Actor* a_target)
	{
		if (auto* target = a_target ? a_target->GetMagicTarget() : nullptr; target && g_sleep) {
			auto caster = RE::PlayerCharacter::GetSingleton()->CreateRefHandle();
			target->DispelEffect(g_sleep, caster);
		}
	}

	bool IsSleep(RE::FormID a_spell) { return g_sleep && a_spell == g_sleep->GetFormID(); }
	bool IsDamage(RE::FormID a_spell) { return g_damage && a_spell == g_damage->GetFormID(); }

	void Calm(RE::Actor* a_target)
	{
		Cast(g_calm, a_target, 1000.0f);   // the magnitude is the highest level it calms
	}

	void Uncalm(RE::Actor* a_target)
	{
		if (auto* target = a_target ? a_target->GetMagicTarget() : nullptr; target && g_calm) {
			auto caster = RE::PlayerCharacter::GetSingleton()->CreateRefHandle();
			target->DispelEffect(g_calm, caster);
		}
	}

	void Fear(RE::Actor* a_target) { Cast(g_fear, a_target, 1000.f); }
	void Unfear(RE::Actor* a_target)
	{
		if (auto* target = a_target ? a_target->GetMagicTarget() : nullptr; target && g_fear) {
			auto caster = RE::PlayerCharacter::GetSingleton()->CreateRefHandle();
			target->DispelEffect(g_fear, caster);
		}
	}

	void Stealth(RE::Actor* a_target, bool a_on)
	{
		if (!g_stealth || !a_target) {
			return;
		}
		if (a_on) {
			g_stealth->effects[0]->effectItem.duration = 10000000;   // until taken off
			Cast(g_stealth, a_target, 1.0f);
		} else if (auto* target = a_target->GetMagicTarget()) {
			auto caster = RE::PlayerCharacter::GetSingleton()->CreateRefHandle();
			target->DispelEffect(g_stealth, caster);
		}
	}

	void Unroot(RE::Actor* a_target)
	{
		if (auto* target = a_target ? a_target->GetMagicTarget() : nullptr; target && g_root) {
			auto caster = RE::PlayerCharacter::GetSingleton()->CreateRefHandle();
			target->DispelEffect(g_root, caster);
			RefreshSpeed(a_target);
		}
	}

	// Skyrim has no mechanics: every timed effect lowering the speed multiplier is a root or snare (the root carrier is
	// one, at -100). Each slowing effect ends alone (WoW cancels the whole aura; a Skyrim spell's other effects - a frost
	// spell's damage, a poison's - are not WoW's snare). Daze and crowd control never touch SpeedMult here.
	int Unsnare(RE::Actor* a_target, bool a_all)
	{
		auto* effects = a_target ? a_target->GetActiveEffectList() : nullptr;
		if (!effects) {
			return 0;
		}
		std::set<RE::FormID> owned;   // a WoW aura's carriers: its aura ends them
		for (const auto& [wow, versions] : g_buffs) for (const auto& [key, spell] : versions) if (spell) owned.insert(spell->GetFormID());
		std::vector<RE::ActiveEffect*> snares;
		for (auto* e : *effects) {
			auto*       value = e ? skyrim_cast<RE::ValueModifierEffect*>(e) : nullptr;
			const auto* base = e ? e->GetBaseObject() : nullptr;
			if (!value || !base || !e->spell || value->actorValue != RE::ActorValue::kSpeedMult || e->flags.any(RE::ActiveEffect::Flag::kDispelled) ||
				(!a_all && (owned.contains(e->spell->GetFormID()) || e->spell->GetCastingType() == RE::MagicSystem::CastingType::kConstantEffect))) {
				continue;
			}
			if (e->magnitude < 0.0f) {   // the active effect's magnitude is signed: a detrimental one's is already negative
				snares.push_back(e);
			}
		}
		for (auto* e : snares) {
			e->Dispel(true);
		}
		if (!snares.empty()) {
			RefreshSpeed(a_target);
		}
		return static_cast<int>(snares.size());
	}

	namespace
	{
		std::map<std::string, RE::SpellItem*> g_tests;   // NativeForTesting's spells, by name
	}

	void NativeForTesting(RE::Actor* a_target, const std::string& a_name, RE::ActorValue a_value, float a_magnitude, float a_seconds)
	{
		auto* caster = RE::PlayerCharacter::GetSingleton()->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
		auto& spell = g_tests[a_name];
		if (!spell) {
			spell = MakeSpell(a_name.c_str(), Lasting(a_value, a_magnitude < 0.0f));
		}
		if (!a_target || !caster || !spell) {
			return;
		}
		spell->effects[0]->effectItem.magnitude = std::abs(a_magnitude);
		spell->effects[0]->effectItem.duration = static_cast<std::uint32_t>(std::ceil(a_seconds));
		caster->CastSpellImmediate(spell, true, a_target, 1.0f, false, 0.0f, RE::PlayerCharacter::GetSingleton());
		RefreshSpeed(a_target);
	}

	void MechanicForTesting(RE::Actor* a_target, RE::EffectSetting::Archetype a_archetype)
	{
		using A = RE::EffectSetting::Archetype;
		if (a_archetype != A::kDemoralize && a_archetype != A::kParalysis && a_archetype != A::kStagger) return;
		const auto name = std::format("Paladin mechanic fixture {}", static_cast<int>(a_archetype));
		auto& spell = g_tests[name];
		if (!spell) {
			auto* base = MakeEffect(name.c_str(), true, a_archetype == A::kDemoralize ? RE::ActorValue::kConfidence :
				a_archetype == A::kParalysis ? RE::ActorValue::kParalysis : RE::ActorValue::kNone, true);
			if (base) base->data.archetype = a_archetype;
			spell = MakeSpell(name.c_str(), base);
			if (spell) { spell->effects[0]->effectItem.duration = 10; spell->effects[0]->effectItem.magnitude = a_archetype == A::kStagger ? .1f : 100; }
		}
		// The ordinary engine application and Cast's Start hook, not a test-only resistance roll.
		Cast(spell, a_target, a_archetype == A::kStagger ? .1f : 100.f);
	}

	void ClearMechanicsForTesting(RE::Actor* a_target)
	{
		auto* target = a_target ? a_target->GetMagicTarget() : nullptr;
		if (!target) return;
		auto caster = RE::PlayerCharacter::GetSingleton()->CreateRefHandle();
		for (const auto& [name, spell] : g_tests) if (spell && name.starts_with("Paladin mechanic fixture")) target->DispelEffect(spell,caster);
	}

	json EffectsForTesting(RE::Actor* a_target)
	{
		json out = json::array();
		if (auto* effects = a_target ? a_target->GetActiveEffectList() : nullptr) {
			for (auto* e : *effects) {
				const auto* value = e ? skyrim_cast<RE::ValueModifierEffect*>(e) : nullptr;
				if (!value || !e->spell || e->flags.any(RE::ActiveEffect::Flag::kDispelled)) continue;
				out.push_back({ { "spell", e->spell->GetName() ? e->spell->GetName() : "" }, { "av", static_cast<int>(value->actorValue) },
					{ "magnitude", e->magnitude }, { "detrimental", e->GetBaseObject() && e->GetBaseObject()->IsDetrimental() },
					{ "remaining", e->duration - e->elapsedSeconds } });
			}
		}
		return out;
	}

	void ClearNativeForTesting(RE::Actor* a_target)
	{
		auto* target = a_target ? a_target->GetMagicTarget() : nullptr;
		if (!target) {
			return;
		}
		auto caster = RE::PlayerCharacter::GetSingleton()->CreateRefHandle();
		for (const auto& [name, spell] : g_tests) {
			if (spell) target->DispelEffect(spell, caster);
		}
		RefreshSpeed(a_target);
	}

	void NativeHitForTesting(RE::Actor* a_caster, RE::Actor* a_target, float a_amount)
	{
		static RE::SpellItem* fire = [] {   // a fire damage spell of Skyrim's (a value modifier, as Firebolt's), not theirs
			auto* effect = MakeEffect("WB test fire", true);
			if (effect) effect->data.resistVariable = RE::ActorValue::kResistFire;
			return MakeSpell("WB test fire", effect);
		}();
		auto* caster = a_caster ? a_caster->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant) : nullptr;
		if (fire && caster && a_target && a_amount > 0.0f) {
			caster->CastSpellImmediate(fire, true, a_target, 1.0f, false, a_amount, a_caster);
		}
	}
}
