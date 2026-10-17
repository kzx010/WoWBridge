#include "SkyrimAuras.h"

#include <fstream>
#include <nlohmann/json.hpp>

#include "Frame.h"
#include "LuaText.h"
#include "NativeText.h"
#include "ui/WowUi.h"

namespace SkyrimAuras
{
	namespace
	{
		struct Shown
		{
			std::string name, icon, text, dispel;
			bool        harmful = false;
			double      until = 0;   // when it ends (Frame::Now() time); 0: until cured or cancelled
		};
		std::map<RE::FormID, Shown> g_shown;   // by Skyrim spell
		double                      g_next = 0;

		// Its own WoW icon (tools/skyrim_icons.py's manifest: "Plugin.esm:local id" -> its WoW path), if it has one.
		std::optional<std::string> Drawn(const RE::TESForm* a_form)
		{
			static const auto icons = [] {
				std::unordered_map<std::string, std::string> map;
				std::ifstream in("Data/SKSE/Plugins/WoWBridge/Icons/manifest.json");
				if (!in) {
					logger::info("skyrim auras: no Skyrim icons; WoW's by kind");
					return map;
				}
				try {
					for (const auto& [key, form] : nlohmann::json::parse(in).at("forms").items()) {
						map.emplace(key, form.value("wow_path", ""));
					}
				} catch (const std::exception& e) {
					logger::error("skyrim auras: icon manifest: {}", e.what());
				}
				logger::info("skyrim auras: {} Skyrim icons", map.size());
				return map;
			}();
			const auto* file = a_form ? a_form->GetFile(0) : nullptr;
			if (!file) {
				return std::nullopt;
			}
			const auto it = icons.find(std::format("{}:{:06X}", file->GetFilename(), a_form->GetLocalFormID()));
			return it != icons.end() && !it->second.empty() ? std::optional(it->second) : std::nullopt;
		}

		// A WoW icon for a Skyrim effect: its own, its effect's, else one of WoW's by what it is (potion, poison,
		// disease, blessing) and what it changes.
		std::string IconOf(const RE::MagicItem* a_spell, const RE::EffectSetting* a_base)
		{
			if (auto own = Drawn(a_spell)) {
				return *own;
			}
			if (auto effect = Drawn(a_base)) {
				return *effect;
			}
			const auto type = a_spell->GetSpellType();
			const auto av = a_base->data.primaryAV;
			std::string_view icon = "INV_Misc_QuestionMark";
			if (type == RE::MagicSystem::SpellType::kDisease) {
				icon = "Spell_Shadow_CallofBone";
			} else if (type == RE::MagicSystem::SpellType::kPoison || a_base->data.resistVariable == RE::ActorValue::kPoisonResist) {
				icon = "Spell_Nature_CorrosiveBreath";
			} else if (type == RE::MagicSystem::SpellType::kPotion || type == RE::MagicSystem::SpellType::kIngredient) {
				icon = av == RE::ActorValue::kHealth ? "INV_Potion_51" : av == RE::ActorValue::kMagicka ? "INV_Potion_72" :
				       av == RE::ActorValue::kStamina ? "INV_Potion_64" : "INV_Potion_93";
			} else if (a_base->data.resistVariable == RE::ActorValue::kResistFire || av == RE::ActorValue::kResistFire) {
				icon = "Spell_Fire_Fire";
			} else if (a_base->data.resistVariable == RE::ActorValue::kResistFrost || av == RE::ActorValue::kResistFrost) {
				icon = "Spell_Frost_FrostArmor";
			} else if (a_base->data.resistVariable == RE::ActorValue::kResistShock || av == RE::ActorValue::kResistShock) {
				icon = "Spell_Nature_Lightning";
			} else if (av == RE::ActorValue::kDamageResist) {
				icon = "Spell_Holy_HolyProtection";
			} else if (av == RE::ActorValue::kHealth || av == RE::ActorValue::kHealRate || av == RE::ActorValue::kHealRateMult) {
				icon = "Spell_Nature_Regenerate";
			} else if (a_base->data.archetype == RE::EffectSetting::Archetype::kParalysis) {
				icon = "Ability_Rogue_FeignDeath";
			} else if (type == RE::MagicSystem::SpellType::kAbility || type == RE::MagicSystem::SpellType::kPower ||
					   type == RE::MagicSystem::SpellType::kLesserPower) {
				icon = "Spell_Holy_GreaterBlessingofKings";   // a shrine's blessing, a standing stone's power
			} else if (!a_base->IsDetrimental()) {
				icon = "Spell_Holy_SealOfSacrifice";
			}
			return std::format("Interface\\Icons\\{}", icon);
		}

		// Skyrim's words for an effect: its description with the magnitude and duration in it.
		std::string WordsOf(float a_magnitude, float a_duration, const RE::EffectSetting* a_base)
		{
			std::string text = a_base->magicItemDescription.c_str() ? a_base->magicItemDescription.c_str() : "";
			auto        put = [&](std::string_view a_tag, const std::string& a_value) {
                for (std::size_t at; (at = text.find(a_tag)) != std::string::npos;) {
                    text.replace(at, a_tag.size(), a_value);
                }
			};
			put("<mag>", std::to_string(static_cast<int>(std::lround(a_magnitude))));
			put("<dur>", std::to_string(static_cast<int>(std::lround(a_duration))));
			std::erase(text, '\r');
			// Scaleform markup and Survival clauses, as Bags.cpp cleans an item's effects (a plain-text tooltip).
			const auto* survival = RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESGlobal>(0x314A, "Update.esm");   // Survival_ModeEnabledShared
			return NativeText::ItemDescription(text, survival && survival->value > 0);
		}

		bool Shows(const RE::ActiveEffect* a_effect)
		{
			const auto* spell = a_effect ? a_effect->spell : nullptr;
			const auto* base = a_effect ? a_effect->GetBaseObject() : nullptr;
			if (!spell || !base || a_effect->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled)) {
				return false;
			}
			if (spell->GetFormID() >= 0xFF000000) {   // made at runtime: the plugin's carriers (their WoW auras show)
				return false;
			}
			const auto type = spell->GetSpellType();
			if (type == RE::MagicSystem::SpellType::kDisease) {
				return true;
			}
			// What lasts a while: not their gear's enchantments, not their race's or a perk's constant abilities.
			return a_effect->duration > 0.0f && type != RE::MagicSystem::SpellType::kEnchantment && !base->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kHideInUI);
		}
	}

	bool Visible(const RE::ActiveEffect* a_effect)
	{
		return Shows(a_effect);
	}

	std::string DispelType(const RE::ActiveEffect* a_effect)
	{
		using Type = RE::MagicSystem::SpellType;
		const auto* spell = a_effect ? a_effect->spell : nullptr;
		const auto* base = a_effect ? a_effect->GetBaseObject() : nullptr;
		if (!spell || !base) {
			return {};
		}
		switch (spell->GetSpellType()) {
		case Type::kDisease:
			return "Disease";
		case Type::kPoison:
			return "Poison";
		case Type::kSpell:
		case Type::kScroll:
		case Type::kStaffEnchantment:
		case Type::kLeveledSpell:
		case Type::kPower:
		case Type::kLesserPower:
		case Type::kVoicePower:
			return base->data.resistVariable == RE::ActorValue::kPoisonResist ? "Poison" : "Magic";
		default:
			return base->data.resistVariable == RE::ActorValue::kPoisonResist ? "Poison" : "";
		}
	}

	// The same visibility rules and Skyrim words apply to their followers.
	static std::map<RE::FormID, Shown> Gather(RE::Actor* a_actor)
	{
		auto* effects = a_actor ? a_actor->GetActiveEffectList() : nullptr;
		if (!effects) return {};
		std::map<RE::FormID, Shown> current;
		for (auto* e : *effects) {
			if (!Shows(e)) {
				continue;
			}
			const auto* base = e->GetBaseObject();
			auto&       s = current[e->spell->GetFormID()];
			if (s.name.empty()) {
				s.name = e->spell->GetFullName() && *e->spell->GetFullName() ? e->spell->GetFullName() : base->GetFullName();
				s.icon = IconOf(e->spell, base);
			}
			const auto words = WordsOf(e->magnitude, e->duration, base);
			if (!words.empty() && s.text.find(words) == std::string::npos) {
				s.text += (s.text.empty() ? "" : "\n") + words;
			}
			s.harmful = s.harmful || base->IsDetrimental() || e->spell->GetSpellType() == RE::MagicSystem::SpellType::kDisease;
			if (const auto type = DispelType(e); !type.empty() && s.dispel != "Disease" && s.dispel != "Poison") {
				s.dispel = type;   // a disease's or a poison's type wins over a magic effect of the same spell
			}
			if (e->spell->GetSpellType() != RE::MagicSystem::SpellType::kDisease) {
				s.until = std::max(s.until, Frame::Now() + std::max(0.0f, e->duration - e->elapsedSeconds));
			}
		}
		return current;
	}

	std::string Snapshot(RE::Actor* a_actor)
	{
		if (!a_actor) return "nil";
		if (!a_actor->GetActiveEffectList()) return a_actor->Is3DLoaded() ? "{}" : "nil";
		std::string out = "{";
		for (const auto& [id, s] : Gather(a_actor)) {
			out += std::format("{{ skyrim = {}, name = {}, texture = {}, text = {}, harmful = {}, count = 1, dispel = {} }},",
				id, LuaQuote(s.name), LuaQuote(s.icon), LuaQuote(s.text), s.harmful ? "true" : "false", s.dispel.empty() ? "nil" : LuaQuote(s.dispel));
		}
		return out + "}";
	}

	void Tick()
	{
		const double now = Frame::Now();
		if (now < g_next) return;
		g_next = now + 0.5;
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->GetActiveEffectList()) return;
		auto current = Gather(player);
		std::string lua;
		for (const auto& [id, s] : g_shown) {
			if (!current.contains(id)) {
				lua += std::format("__WBSkyrimAuraRemove({}) ", id);
			}
		}
		for (auto& [id, s] : current) {
			// Told again only when it changed: other words, or an end moved (a refresh), not its countdown.
			if (const auto it = g_shown.find(id); it != g_shown.end() && it->second.text == s.text && std::abs(it->second.until - s.until) <= 2.0) {
				s.until = it->second.until;
				continue;
			}
			lua += std::format("__WBSkyrimAura({}, {}, {}, {}, {:.1f}, {}, {}) ", id, LuaQuote(s.name), LuaQuote(s.icon),
				s.harmful ? "true" : "false", s.until > 0 ? s.until - now : 0.0, LuaQuote(s.text), s.dispel.empty() ? "nil" : LuaQuote(s.dispel));
		}
		if (!lua.empty()) {
			WowUi::Run(lua);
		}
		g_shown = std::move(current);
	}

	void Reset() { g_shown.clear(); }

	std::string Icon(const RE::MagicItem* a_spell, const RE::EffectSetting* a_base) { return IconOf(a_spell, a_base); }
	std::string Words(float a_magnitude, float a_duration, const RE::EffectSetting* a_base) { return WordsOf(a_magnitude, a_duration, a_base); }

	void Cancel(RE::FormID a_spell)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* spell = RE::TESForm::LookupByID<RE::MagicItem>(a_spell);
		const auto it = g_shown.find(a_spell);
		if (!player || !spell || it == g_shown.end() || it->second.harmful) {
			return;   // only a helpful one comes off at a click, as in WoW
		}
		auto caster = player->CreateRefHandle();
		player->GetMagicTarget()->DispelEffect(spell, caster);
	}
}
