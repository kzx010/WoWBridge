#include "Carriers.h"
#include "CarrierPersistence.h"
#include <charconv>

#include "Character.h"
#include "ControlTiming.h"

namespace Carriers
{
    using json=nlohmann::json;
	namespace
	{
		RE::SpellItem* g_damage = nullptr;
		RE::SpellItem* g_heal = nullptr;
		RE::SpellItem* g_root = nullptr;
		RE::SpellItem* g_sleep = nullptr;
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
		if (auto* stealth = MakeEffect("WoWBridge stealth", false, RE::ActorValue::kInvisibility, true)) {
			stealth->data.archetype = RE::EffectSetting::Archetype::kInvisibility;
			g_stealth = MakeSpell("Stealth", stealth);
		}
		// Skyrim's armor mitigates .12% per point: Quickness's 1% avoidance becomes 8 1/3 armor.
		// Nature follows the existing gear mapping, WoW resistance /3 -> Skyrim poison resistance.
		// Wisp Spirit is a named zero-magnitude marker; Skyrim has no released ghost to accelerate.
		for (const auto& [id, name, value, amount] : {
			std::tuple{ 20582u, "Quickness", RE::ActorValue::kDamageResist, 1.0f / 0.12f },
			std::tuple{ 20583u, "Nature Resistance", RE::ActorValue::kPoisonResist, 10.0f / 3.0f },
			std::tuple{ 20585u, "Wisp Spirit", RE::ActorValue::kSpeedMult, 0.0f } }) {
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

	void Damage(RE::Actor* a_target, float a_amount) { Cast(g_damage, a_target, a_amount); }
	void Heal(RE::Actor* a_target, float a_amount) { Cast(g_heal, a_target, a_amount); }

	void Racials()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !Character::Get().level) {
			return;
		}
		for (const auto& [id, spell] : g_racials) {
			if (Character::Get().looks.race == 4) {
				if (!player->HasSpell(spell)) {
					player->AddSpell(spell);
				}
			} else if (player->HasSpell(spell)) {
				player->RemoveSpell(spell);
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
		if (!g_root || !a_target) {
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
            for (const auto* spell : {g_root,g_sleep,g_stealth}) if (spell) ids.insert(spell->GetFormID());
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
}
