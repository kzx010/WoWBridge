#include "Mounts.h"
#include "SkyrimSpells.h"

#include "Avatar.h"
#include "Bridge.h"
#include "Cast.h"
#include "Frame.h"
#include "Forms.h"
#include "LuaText.h"
#include "SkyrimAuras.h"
#include "ui/WowUi.h"

#include <nlohmann/json.hpp>

namespace SkyrimSpells
{
	namespace
	{
		constexpr double kGcd = 1.5, kChannel = 10.0;   // each concentration use is bounded, even without Escape

		struct Pending
		{
			RE::SpellItem*  spell = nullptr;
			RE::ActorHandle target;    // empty: herself for Self delivery, otherwise native aim
			double          end = 0;   // the cast's end, or the channel's
			double          nextTick = 0;   // a channel: its next second
			bool            channel = false;
			bool            started = false;
		};
		std::optional<Pending>  g_cast;
		std::vector<RE::FormID> g_known;   // what the book was told
		double                  g_nextScan = 0, g_gcdUntil = 0;
		std::string             g_shoutsShown, g_powersShown;
		bool                    g_refresh = true;
		std::map<RE::FormID, double> g_powerEnds, g_loadedPowerEnds;   // Skyrim hours, not the interface's clock
		std::map<RE::FormID, double> g_powerUiEnds;
		double                  g_nextCooldown = 0, g_shoutUiEnd = -1;
		std::vector<RE::FormID> g_usedShouts, g_usedPowers;

		struct Voice
		{
			RE::TESForm* form;
			RE::TESForm* previous;
			int          word;   // -1: a power; otherwise the shout's selected variation
			double       start;
			bool         released = false, fired = false;
		};
		std::optional<Voice> g_voice;

		RE::PlayerCharacter* Player() { return RE::PlayerCharacter::GetSingleton(); }
		std::int64_t         Id(const RE::TESForm* a_form) { return -static_cast<std::int64_t>(a_form->GetFormID()); }
		double Hours() { return RE::Calendar::GetSingleton()->GetHoursPassed(); }
		double Scale() { return std::max(0.0f, RE::Calendar::GetSingleton()->GetTimescale()); }
		double DaySeconds() { return Scale() > 0 ? 86400.0 / Scale() : 86400.0; }

		// WOOP uses bit 16 for a word unlocked by a soul (the generic TESForm kUnlocked is a different flag).
		bool Unlocked(const RE::TESWordOfPower* a_word)
		{
			return a_word && a_word->GetKnown() && (a_word->GetFormFlags() & (1u << 16));
		}

		int Word(RE::TESShout* a_shout)
		{
			if (!a_shout || !Player()->HasShout(a_shout)) {
				return -1;
			}
			int word = -1;
			for (int i = 0; i < 3; ++i) {
				if (!Unlocked(a_shout->variations[i].word) || !a_shout->variations[i].spell) {
					break;
				}
				word = i;
			}
			return word;
		}

		std::string Icon(RE::TESForm* a_form, RE::SpellItem* a_spell = nullptr)
		{
			static const auto icons = [] {
				std::unordered_map<std::string, std::string> out;
				std::ifstream in("Data/SKSE/Plugins/WoWBridge/Icons/manifest.json");
				if (in) {
					try {
						for (const auto& [key, entry] : nlohmann::json::parse(in).at("forms").items()) {
							out.emplace(key, entry.value("wow_path", ""));
						}
					} catch (const std::exception& e) {
						logger::error("skyrim spells: icon manifest: {}", e.what());
					}
				}
				return out;
			}();
			if (const auto* file = a_form->GetFile(0)) {
				const auto it = icons.find(std::format("{}:{:06X}", file->GetFilename(), a_form->GetLocalFormID()));
				if (it != icons.end() && !it->second.empty()) {
					return it->second;
				}
			}
			const auto* base = a_spell ? a_spell->GetAVEffect() : nullptr;
			return base ? SkyrimAuras::Icon(a_spell, base) : "Interface\\Icons\\INV_Misc_QuestionMark";
		}

		bool Power(const RE::SpellItem* a_spell)
		{
			return a_spell && (a_spell->GetSpellType() == RE::MagicSystem::SpellType::kPower ||
				a_spell->GetSpellType() == RE::MagicSystem::SpellType::kLesserPower);
		}

		// Her learned spells, base spells and racial spells; hand spells and powers have separate book lists.
		std::vector<RE::SpellItem*> Known(bool a_powers = false)
		{
			std::vector<RE::SpellItem*> out;
			auto add = [&](RE::SpellItem* a_spell) {
				if (a_spell && (a_powers ? Power(a_spell) : a_spell->GetFormID() < 0xFF000000 && a_spell->GetSpellType() == RE::MagicSystem::SpellType::kSpell) &&
					(a_powers || a_spell->GetCastingType() == RE::MagicSystem::CastingType::kFireAndForget ||
						a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) &&
					std::ranges::find(out, a_spell) == out.end()) {
					out.push_back(a_spell);
				}
			};
			for (auto* s : Player()->GetActorRuntimeData().addedSpells) {
				add(s);
			}
			if (const auto* base = Player()->GetActorBase(); base && base->actorEffects) {
				for (std::uint32_t i = 0; i < base->actorEffects->numSpells; ++i) {
					add(base->actorEffects->spells[i]);
				}
			}
			if (const auto* race = Player()->GetRace(); race && race->actorEffects) {
				for (std::uint32_t i = 0; i < race->actorEffects->numSpells; ++i) {
					add(race->actorEffects->spells[i]);
				}
			}
			return out;
		}

		// Her WoW body casts it as she casts her own: Wrath's precast and release, a channel's stance (Hurricane's at a
		// foe, Tranquility's otherwise). Skyrim's own casting animations are her hidden body's.
		WowUi::SpellSounds Kit(bool a_hostile, bool a_channel) { return WowUi::Sounds(a_channel ? (a_hostile ? 17402 : 9863) : 9912); }

		std::string Entry(RE::SpellItem* a_spell)
		{
			auto*       player = Player();
			std::string text;
			for (const auto* e : a_spell->effects) {
				if (e && e->baseEffect) {
					const auto words = SkyrimAuras::Words(e->effectItem.magnitude, static_cast<float>(e->effectItem.duration), e->baseEffect);
					if (!words.empty() && text.find(words) == std::string::npos) {
						text += (text.empty() ? "" : "\n") + words;
					}
				}
			}
			const bool concentration = a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
			const bool self = a_spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf;
			return std::format("{{ id = {}, name = {}, icon = {}, cost = {}, castTime = {}, rangeMin = 0, rangeMax = {}, targetsUnit = {}, "
							   "harmful = {}, channelled = {}, description = {} }}",
				Id(a_spell), LuaQuote(a_spell->GetFullName()), LuaQuote(Icon(a_spell, a_spell)),
				static_cast<int>(std::lround(a_spell->CalculateMagickaCost(player))), concentration ? 0 : static_cast<int>(a_spell->GetChargeTime() * 1000.0f),
				self ? 0 : 40, a_spell->GetDelivery() == RE::MagicSystem::Delivery::kTargetActor ? "true" : "false", a_spell->IsHostile() ? "true" : "false", concentration ? "true" : "false", LuaQuote(text));
		}

		void Tell(const std::vector<RE::SpellItem*>& a_spells)
		{
			std::string lua = "__WBSkyrimSpells({";
			for (auto* s : a_spells) {
				lua += Entry(s) + ",";
			}
			WowUi::Run(lua + "})");
		}

		std::vector<RE::TESShout*> Shouts()
		{
			std::vector<RE::TESShout*> out;
			for (auto* shout : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESShout>()) {
				if (shout && Player()->HasShout(shout) && std::ranges::any_of(shout->variations, [](const auto& v) { return v.word && v.word->GetKnown(); })) {
					out.push_back(shout);
				}
			}
			return out;
		}

		void TellVoice()
		{
			std::string lua = "__WBSkyrimShouts({";
			const float mult = std::max(0.0f, Player()->AsActorValueOwner()->GetActorValue(RE::ActorValue::kShoutRecoveryMult));
			for (auto* shout : Shouts()) {
				RE::BSString description;
				shout->GetDescription(description, shout);
				lua += std::format("{{ id = {}, name = {}, icon = {}, description = {}, words = {{", Id(shout),
					LuaQuote(shout->GetFullName()), LuaQuote(Icon(shout)), LuaQuote(description.c_str()));
				for (const auto& v : shout->variations) {
					if (!v.word) {
						break;
					}
					lua += std::format("{{ name = {}, learned = {}, unlocked = {}, recovery = {:.3f}", LuaQuote(v.word->GetFullName()),
						v.word->GetKnown() ? "true" : "false", Unlocked(v.word) ? "true" : "false", std::max(0.0f, v.recoveryTime * mult));
					if (v.spell) {
						lua += std::format(", spell = {}", Id(v.spell));
					}
					lua += "},";
				}
				lua += "}},";
			}
			lua += "})";
			if (g_refresh || lua != g_shoutsShown) {
				g_shoutsShown = lua;
				WowUi::Run(lua);
			}
			lua = "__WBSkyrimPowers({";
			for (auto* spell : Known(true)) {
				auto entry = Entry(spell);
				entry.pop_back();
				lua += entry + std::format(", kind = {}, cooldownSeconds = {:.3f} }},", LuaQuote(
					spell->GetSpellType() == RE::MagicSystem::SpellType::kPower ? "greater" : "lesser"),
					spell->GetSpellType() == RE::MagicSystem::SpellType::kPower ? DaySeconds() : 0.0);
			}
			lua += "})";
			if (g_refresh || lua != g_powersShown) {
				g_powersShown = lua;
				WowUi::Run(lua);
			}
		}

		// Her bar uses Skyrim's voice handler, just as the native shout control does: charge, then release. Skyrim
		// chooses the variation and casts it; its fire event, not the button press, starts the interface's recovery.
		bool VoiceButton(float a_value, float a_held)
		{
			auto* controls = RE::PlayerControls::GetSingleton();
			auto* handler = controls ? controls->shoutHandler : nullptr;
			const auto& event = RE::UserEvents::GetSingleton()->shout;
			const auto key = RE::ControlMap::GetSingleton()->GetMappedKey(event, RE::INPUT_DEVICE::kKeyboard);
			auto* button = handler ? RE::ButtonEvent::Create(RE::INPUT_DEVICE::kKeyboard, event, key, a_value, a_held) : nullptr;
			if (!button) {
				return false;
			}
			const bool can = handler->CanProcess(button);
			if (can) {
				handler->ProcessButton(button, &controls->data);
			}
			RE::free(button);
			return can;
		}

		void FinishVoice(bool a_interrupt)
		{
			if (!g_voice) {
				return;
			}
			if (a_interrupt) {
				RE::SourceActionMap::DoAction(Player(), RE::DEFAULT_OBJECT::kActionVoiceInterrupt);
			}
			if (Player()->GetActorRuntimeData().selectedPower == g_voice->form) {
				Player()->GetActorRuntimeData().selectedPower = g_voice->previous;
			}
			g_voice.reset();
		}

		struct VoiceEvents : RE::BSTEventSink<SKSE::ActionEvent>, RE::BSTEventSink<RE::TESSpellCastEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const SKSE::ActionEvent* a_event, RE::BSTEventSource<SKSE::ActionEvent>*) override
			{
				if (a_event && a_event->actor == Player() && a_event->type == SKSE::ActionEvent::Type::kVoiceFire && a_event->sourceForm) {
					if (auto* shout = a_event->sourceForm->As<RE::TESShout>(); shout && Word(shout) >= 0) {
						g_usedShouts.push_back(shout->GetFormID());
					}
					if (g_voice && a_event->sourceForm == g_voice->form) {
						g_voice->fired = true;
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event, RE::BSTEventSource<RE::TESSpellCastEvent>*) override
			{
				if (a_event && a_event->object.get() == Player()) {
					if (auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(a_event->spell); Power(spell)) {
						if (spell->GetSpellType() == RE::MagicSystem::SpellType::kPower) {
							g_powerEnds[spell->GetFormID()] = Hours() + 24.0;
						}
						g_usedPowers.push_back(spell->GetFormID());
						if (g_voice && spell == g_voice->form) {
							g_voice->fired = true;
						}
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		void Listen()
		{
			static VoiceEvents sink;
			static bool added = [] {
				SKSE::GetActionEventSource()->AddEventSink(&sink);
				RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESSpellCastEvent>(&sink);
				return true;
			}();
			(void)added;
		}

		void TickVoice(double a_now)
		{
			if (!g_voice) {
				return;
			}
			auto& v = *g_voice;
			const float held = static_cast<float>(a_now - v.start);
			if (v.fired) {
				FinishVoice(false);
				return;
			}
			if (held > 8.0f || Player()->IsDead() || Player()->GetActorRuntimeData().selectedPower != v.form) {
				FinishVoice(true);
				WowUi::Run("__WBUiError(\"SPELL_FAILED_INTERRUPTED\")");
				return;
			}
			if (!v.released && held >= 0.1f) {
				if (auto* shout = v.form->As<RE::TESShout>(); shout && Word(shout) != v.word) {
					FinishVoice(true);   // what she knows changed while charging; resolve it on the next use
					return;
				}
				VoiceButton(1.0f, held);
				const auto* process = Player()->GetActorRuntimeData().currentProcess;
				if (v.word < 0 || (process && process->high && static_cast<int>(process->high->currentShoutVariation) == v.word)) {
					v.released = VoiceButton(0.0f, held);
				}
			}
		}

		void Cooldowns(double a_now)
		{
			const double recovery = std::max(0.0f, Player()->GetVoiceRecoveryTime());
			const bool used = !g_usedShouts.empty();
			for (auto id : g_usedShouts) {
				if (auto* shout = RE::TESForm::LookupByID<RE::TESShout>(id); Word(shout) >= 0) {
					WowUi::Run(std::format("__WBSkyrimShoutUsed({}, {:.3f})", Id(shout), recovery));
				}
			}
			g_usedShouts.clear();
			if (used || g_refresh || std::abs(std::max(0.0, g_shoutUiEnd - a_now) - recovery) > 0.75) {
				WowUi::Run(std::format("__WBSkyrimShoutCooldown({:.3f})", recovery));
				g_shoutUiEnd = a_now + recovery;
			}
			const auto powers = Known(true);
			std::map<RE::FormID, double> ends;
			for (auto* spell : powers) {
				const auto id = spell->GetFormID();
				double seconds = 0;
				const bool unavailable = spell->GetSpellType() == RE::MagicSystem::SpellType::kPower && Player()->IsInCastPowerList(spell);
				if (unavailable) {
					if (const auto it = g_powerEnds.find(id); it != g_powerEnds.end() && Scale() > 0) {
						seconds = std::max(0.0, it->second - Hours()) * 3600.0 / Scale();
					} else if (!g_powerUiEnds.contains(id)) {
						logger::warn("skyrim powers: {:08X} unavailable; saved use time is missing", id);
					}
				} else {
					g_powerEnds.erase(id);
				}
				const bool fired = std::ranges::find(g_usedPowers, id) != g_usedPowers.end();
				if (fired) {
					WowUi::Run(std::format("__WBSkyrimPowerUsed({}, {:.3f})", Id(spell), seconds));
				}
				const auto old = g_powerUiEnds.find(id);
				if (fired || g_refresh || old == g_powerUiEnds.end() || std::abs(std::max(0.0, old->second - a_now) - seconds) > 0.75) {
					WowUi::Run(std::format("__WBSkyrimPowerCooldown({}, {:.3f})", Id(spell), seconds));
					ends[id] = a_now + seconds;
				} else {
					ends[id] = old->second;
				}
			}
			g_powerUiEnds = std::move(ends);
			g_usedPowers.clear();
		}

		bool UseVoice(RE::TESForm* a_form)
		{
			auto* shout = a_form->As<RE::TESShout>();
			auto* spell = a_form->As<RE::SpellItem>();
			const int word = shout ? Word(shout) : -1;
			if (shout ? word < 0 : !Power(spell) || !Player()->HasSpell(spell)) {
				WowUi::Run("__WBUiError(\"SPELL_FAILED_NOT_KNOWN\")");
				return true;
			}
			if ((shout && Player()->GetVoiceRecoveryTime() > 0) || (spell && spell->GetSpellType() == RE::MagicSystem::SpellType::kPower && Player()->IsInCastPowerList(spell))) {
				WowUi::Run("__WBUiError(\"SPELL_FAILED_NOT_READY\")");
				return true;
			}
			if (g_voice || g_cast || Cast::Casting() || Player()->IsDead()) {
				WowUi::Run("__WBUiError(\"SPELL_FAILED_SPELL_IN_PROGRESS\")");
				return true;
			}
			Listen();
			g_voice = Voice{ a_form, Player()->GetActorRuntimeData().selectedPower, word, Frame::Now() };
			if (shout) {
				RE::ActorEquipManager::GetSingleton()->EquipShout(Player(), shout);
			} else {
				RE::ActorEquipManager::GetSingleton()->EquipSpell(Player(), spell, spell->GetEquipSlot());
			}
			if (!VoiceButton(1.0f, 0.0f)) {
				FinishVoice(false);
				WowUi::Run("__WBUiError(\"SPELL_FAILED_NOT_READY\")");
			}
			return true;
		}

		// It goes off: its magicka, Skyrim's spell on its target, its skill trained.
		bool Release(RE::SpellItem* a_spell, RE::Actor* a_target, bool a_start = true)
		{
			auto*       player = Player();
			auto*       av = player->AsActorValueOwner();
			const float cost = a_spell->CalculateMagickaCost(player);
			if (av->GetActorValue(RE::ActorValue::kMagicka) < cost) {
				WowUi::Run("__WBCastFailed(\"ERR_OUT_OF_MANA\")");
				return false;
			}
			av->DamageActorValue(RE::ActorValue::kMagicka, cost);
			// Concentration stays active in the native caster. Start it once, rather than
			// repeatedly starting more casts while charging the bridge's per-second cost.
			if (a_start) {
				player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)->CastSpellImmediate(a_spell, false, a_spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf ? player : a_target, 1.0f, false, 0.0f, player);
			}
			if (a_spell->GetCastingType() != RE::MagicSystem::CastingType::kConcentration) {
				Avatar::Act(Kit(a_spell->IsHostile(), false).castAnim, false);
			}
			if (const auto skill = a_spell->GetAssociatedSkill(); skill != RE::ActorValue::kNone) {
				player->AddSkillExperience(skill, cost);
			}
			return true;
		}
	}

	bool Use(std::int64_t a_id)
	{
		if (a_id >= 0) {
			return false;
		}
		if (a_id < -static_cast<std::int64_t>(0xFFFFFFFFu) || !Player() || !Player()->Is3DLoaded()) {
			return true;
		}
		auto* form = RE::TESForm::LookupByID(static_cast<RE::FormID>(-a_id));
		if (!form) {
			return true;
		}
		auto* spell = form->As<RE::SpellItem>();
		if (Forms::Current() != "none") {
			WowUi::Run("__WBUiError(\"SPELL_FAILED_NOT_SHAPESHIFT\")");
			return true;
		}
		Mounts::Dismount();
		if (form->As<RE::TESShout>() || Power(spell)) {
			return UseVoice(form);
		}
		const auto known = Known();
		if (!spell || std::ranges::find(known, spell) == known.end()) {
			WowUi::Run("__WBUiError(\"SPELL_FAILED_NOT_KNOWN\")");
			return true;
		}
		const double now = Frame::Now();
		if (g_voice || g_cast || Cast::Casting()) {
			WowUi::Run("__WBUiError(\"SPELL_FAILED_SPELL_IN_PROGRESS\")");
			return true;
		}
		if (now < g_gcdUntil) {
			WowUi::Run("__WBUiError(\"SPELL_FAILED_NOT_READY\")");
			return true;
		}
		RE::Actor* target = nullptr;
		if (spell->GetDelivery() != RE::MagicSystem::Delivery::kSelf) {
			target = Bridge::Target();
			if (target == Player()) {
				target = nullptr;
			}
			if (spell->GetDelivery() == RE::MagicSystem::Delivery::kTargetActor && spell->IsHostile() && !target) {
				WowUi::Run("__WBUiError(\"ERR_GENERIC_NO_TARGET\")");
				return true;
			}
		}
		if (Player()->AsActorValueOwner()->GetActorValue(RE::ActorValue::kMagicka) < spell->CalculateMagickaCost(Player())) {
			WowUi::Run("__WBCastFailed(\"ERR_OUT_OF_MANA\")");
			return true;
		}
		g_gcdUntil = now + kGcd;
		WowUi::Run(std::format("__WBGcd({:.3f})", kGcd));
		Pending c{ spell, target ? target->CreateRefHandle() : RE::ActorHandle{} };
		if (spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {   // held: a channel, once a second
			c.channel = true;
			c.end = now + kChannel;
			c.nextTick = now;
			WowUi::Run(std::format("__WBChannelStart({}, {})", -static_cast<std::int64_t>(spell->GetFormID()), static_cast<int>(kChannel * 1000)));
			Avatar::Act(Kit(spell->IsHostile(), true).channelAnim, true);
		} else if (const float charge = spell->GetChargeTime(); charge > 0.0f) {
			c.end = now + charge;
			WowUi::Run(std::format("__WBCastStart({}, {})", -static_cast<std::int64_t>(spell->GetFormID()), static_cast<int>(charge * 1000.0f)));
			Avatar::Act(Kit(spell->IsHostile(), false).precastAnim, true);
		} else {
			Release(spell, target);
			return true;
		}
		g_cast = c;
		return true;
	}

	void Tick()
	{
		const double now = Frame::Now();
		auto*        player = Player();
		if (!player || !player->Is3DLoaded() || !WowUi::Ready() || !RE::Calendar::GetSingleton()) {
			return;
		}
        // A rebuilt interface has empty native lists even if the actor has
        // exactly the same spells. Publish lists before their cooldowns, while
        // retaining the actor's native timers and cast/power state.
        static std::uint64_t uiGeneration = 0;
        if (const auto generation = WowUi::Generation(); generation != uiGeneration) {
            uiGeneration = generation;
            g_refresh = true; g_nextScan = g_nextCooldown = 0;
        }
		Listen();
		TickVoice(now);
		if (now >= g_nextScan || !g_usedShouts.empty() || !g_usedPowers.empty()) {   // changes, or a native use before the next scan
			g_nextScan = now + 2.0;
			auto known = Known();
			std::vector<RE::FormID> ids;
			for (auto* s : known) {
				ids.push_back(s->GetFormID());
			}
			std::ranges::sort(ids);
			if (g_refresh || ids != g_known) {
				g_known = std::move(ids);
				Tell(known);
			}
			TellVoice();
		}
		if (now > 0 && (g_refresh || now >= g_nextCooldown || !g_usedShouts.empty() || !g_usedPowers.empty())) {
			g_nextCooldown = now + 0.5;
			// The interface advances during Skyrim's paused menus too. Compare against its clock so returning from a
			// menu restores a sweep that expired there, even when Skyrim's voice timer did not advance at all.
			const auto clock = WowUi::Eval("GetTime()");
			const double uiNow = std::strtod(clock.c_str(), nullptr);
			if (std::isfinite(uiNow) && uiNow > 0) {
				Cooldowns(uiNow);
				g_refresh = false;
			}
		}
		if (!g_cast) {
			return;
		}
		auto& c = *g_cast;
		auto  target = c.target.get();
		// Skyrim's one-hand concentration allows movement. BothHands ritual spells
		// (e.g. Lightning Storm) immobilize the native caster; keep that restriction.
		const auto* controls = RE::PlayerControls::GetSingleton();
		const bool  moving = (controls && (controls->data.moveInputVec.Length() > 0.01f || controls->data.autoMove)) || player->IsInMidair();
		const bool allowsMovement = c.channel && c.spell->GetEquipSlot() != RE::TESForm::LookupByID<RE::BGSEquipSlot>(0x00013F45);
		if ((!allowsMovement && moving) || player->IsDead() || Forms::Current() != "none" ||
			(c.target && (!target || target->IsDead()))) {
			Stop();
			return;
		}
		if (c.channel) {
			while (now >= c.nextTick && now < c.end) {
				const bool start = !c.started;
				c.nextTick += 1.0;
				if (!Release(c.spell, target.get(), start)) {
					Stop();
					return;
				}
				c.started = true;
			}
			if (now >= c.end) {
				Stop();
			}
			return;
		}
		if (now >= c.end) {
			WowUi::Run("__WBCastStop()");
			Release(c.spell, target.get());
			g_cast.reset();
		}
	}

	void Stop()
	{
		if (g_cast) {
			if (g_cast->channel) {
				if (auto* player = Player()) {
					if (auto* caster = player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)) {
						caster->InterruptCast(false);
						caster->SetCurrentSpell(nullptr);
					}
				}
			}
			WowUi::Run(g_cast->channel ? "__WBChannelStop()" : "__WBCastInterrupted()");
			Avatar::StopActing();
			g_cast.reset();
		}
		FinishVoice(true);
	}

	void Reset()
	{
		g_voice.reset();   // a different save's selected power must not be interrupted
		Stop();
		g_known.clear();
		g_shoutsShown.clear();
		g_powersShown.clear();
		g_powerEnds = std::move(g_loadedPowerEnds);
		g_loadedPowerEnds.clear();
		g_powerUiEnds.clear();
		g_usedShouts.clear();
		g_usedPowers.clear();
		g_shoutUiEnd = -1;
		g_nextCooldown = 0;
		g_gcdUntil = 0;
		g_refresh = true;
		g_nextScan = 0;
	}

	bool UseByName(std::string_view a_name)
	{
		if (!Player() || !Player()->Is3DLoaded()) {
			return false;
		}
		for (auto* shout : Shouts()) {
			if (_stricmp(shout->GetFullName(), std::string(a_name).c_str()) == 0) {
				return Use(Id(shout));
			}
		}
		for (bool powers : { false, true }) {
			for (auto* spell : Known(powers)) {
				if (_stricmp(spell->GetFullName(), std::string(a_name).c_str()) == 0) {
					return Use(Id(spell));
				}
			}
		}
		return false;
	}

	void Save(SKSE::SerializationInterface* a_intfc)
	{
		const auto count = static_cast<std::uint32_t>(g_powerEnds.size());
		if (!a_intfc->OpenRecord(kRecord, kRecordVersion) || !a_intfc->WriteRecordData(count)) {
			logger::error("skyrim powers: save record");
			return;
		}
		for (const auto& [id, end] : g_powerEnds) {
			if (!a_intfc->WriteRecordData(id) || !a_intfc->WriteRecordData(end)) {
				logger::error("skyrim powers: save use time");
				return;
			}
		}
	}

	void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length)
	{
		g_loadedPowerEnds.clear();
		std::uint32_t count = 0;
		if (a_length < sizeof(count) || a_intfc->ReadRecordData(count) != sizeof(count) ||
			(a_length - sizeof(count)) % (sizeof(RE::FormID) + sizeof(double)) != 0 ||
			count != (a_length - sizeof(count)) / (sizeof(RE::FormID) + sizeof(double))) {
			logger::error("skyrim powers: invalid save record");
			return;
		}
		for (std::uint32_t i = 0; i < count; ++i) {
			RE::FormID saved = 0, resolved = 0;
			double end = 0;
			if (a_intfc->ReadRecordData(saved) != sizeof(saved) || a_intfc->ReadRecordData(end) != sizeof(end)) {
				g_loadedPowerEnds.clear();
				logger::error("skyrim powers: truncated save record");
				return;
			}
			if (std::isfinite(end) && a_intfc->ResolveFormID(saved, resolved)) {
				g_loadedPowerEnds[resolved] = end;
			}
		}
	}

	bool Casting() { return g_cast.has_value() || g_voice.has_value(); }
}
