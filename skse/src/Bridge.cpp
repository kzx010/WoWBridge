#include "CompassLocations.h"
#include "Mounts.h"
#include "Bridge.h"
#include "Integration.h"
#include "Console.h"
#include "CompassMarkers.h"

#include "SkyrimAuras.h"
#include "SkyrimSpells.h"
#include "Forms.h"

#include "Avatar.h"
#include "../../wowcore/include/wowcore/combat_feedback.hpp"
#include "../../wowcore/include/wowcore/minimap.hpp"
#include "SpellVisuals.h"
#include "Cast.h"
#include "Character.h"
#include "Config.h"
#include "Frame.h"
#include "Input.h"
#include "LuaText.h"
#include "NativeText.h"
#include "Melee.h"
#include "Picker.h"
#include "Portrait.h"
#include "Progression.h"
#include "Quests.h"
#include "Reactions.h"
#include "Taxi.h"
#include "ui/WowUi.h"

namespace Bridge
{
    // Main thread, per service request. Keep raw AVs and actual applied bridge
    // modifiers separate; the service must not mirror its own carriers twice.
    nlohmann::json NativeActorValues(RE::Actor* actor)
    {
        auto data=nlohmann::json{{"live",nlohmann::json::object()},{"owned",nlohmann::json::object()},
            {"base",nlohmann::json::object()}};
        if (!actor) return data;
        auto* av=actor->AsActorValueOwner();
        for (const int id : {27,28,29,39,40,41,42,43,44,45,83,155,156,157}) {
            const auto value=static_cast<RE::ActorValue>(id);
            data["live"][std::to_string(id)]=av->GetActorValue(value);
            if (id>=27 && id<=29) data["base"][std::to_string(id)]=av->GetBaseActorValue(value);
        }
        if (auto* effects=actor->GetActiveEffectList()) for (auto* effect : *effects) {
            const auto* base=effect ? effect->GetBaseObject() : nullptr;
            if (!base || !effect->spell || effect->flags.any(RE::ActiveEffect::Flag::kInactive,RE::ActiveEffect::Flag::kDispelled)) continue;
            const auto* file=effect->spell->GetFile(0);
            const std::string_view source=file ? file->GetFilename() : "";
            const std::string_view name=base->fullName.c_str();
            // Generated equip/set abilities belong to these plugins. Runtime
            // aura/racial MGEFs have these names; unrelated runtime enchantments
            // using vanilla MGEFs remain external, even on a WoW item.
            const bool owned=source=="WoWBridge.esp" || source=="WoWBridgeArmory.esp" ||
                (base->GetFormID()>=0xFF000000 && (name=="WoWBridge aura" || name=="Quickness" || name=="Nature Resistance" || name=="Frost Resistance"));
            const auto* value=owned ? skyrim_cast<RE::ValueModifierEffect*>(effect) : nullptr;
            if (!value || !std::isfinite(value->value)) continue;
            const auto key=std::to_string(static_cast<int>(value->actorValue));
            if (data["live"].contains(key)) data["owned"][key]=data["owned"].value(key,0.f)+value->value;
        }
        return data;
    }

	namespace
	{
		RE::ActorHandle g_target;
		bool            g_targetSelf = false;   // F1: the player is the target
		std::string     g_last;   // the last snapshot published (only changes are sent)
		std::vector<RE::ActorHandle> g_party;
		std::map<RE::FormID, RE::ActorHandle> g_followers;
		std::map<RE::FormID, std::string> g_partyAuras;
		double g_partyNext = 0;
		std::map<RE::FormID, RE::ActorHandle> g_plateActors;
		std::uint32_t g_plateEpoch = 1;

		constexpr float kUnitsPerYard = 64.0f;

        // Main-thread diagnostic; no clock reads or actor counting when inactive.
        // Measured 1.28 ms mean with 31 Whiterun market actors and every tracking category on (run 20261014-142421);
        // 2 ms (about 12% of a 60 FPS frame) flags a regression of half again, not a target.
        constexpr double kScanBudgetMs=2.0;
        bool g_scanActive=false, g_scanMeasuring=false, g_scanValid=true;
        nlohmann::json g_scanSite, g_scanResult={{"active",false},{"done",false}};
        std::chrono::steady_clock::time_point g_scanIssued, g_scanStarted, g_scanPrevious;
        double g_scanSum=0, g_scanMax=0, g_scanActorSum=0;
        std::size_t g_scanFrames=0, g_scanActorMin=SIZE_MAX, g_scanActorMax=0;
        std::set<RE::FormID> g_scanActors;

        void FinishNPCScanCost(const char* error="")
        {
            const double mean=g_scanFrames ? g_scanSum/g_scanFrames : 0;
            g_scanActive=false;
            g_scanResult={{"active",false},{"done",true},{"mean_ms",mean},{"max_ms",g_scanMax},
                {"frames",g_scanFrames},{"duration_s",g_scanMeasuring ? std::chrono::duration<double>(std::chrono::steady_clock::now()-g_scanStarted).count() : 0},
                {"actor_count",g_scanActorMax},{"actor_count_min",g_scanFrames ? g_scanActorMin : 0},
                {"actor_count_mean",g_scanFrames ? g_scanActorSum/g_scanFrames : 0},
                {"budget_mean_ms",kScanBudgetMs},{"budget_reason","about 12% of a 60 FPS frame; 31 market actors measured 1.28 ms"},
                {"scope","NameplatePart + MinimapPart, including POIs/serialization; all tracking categories"},
                {"site",g_scanSite},{"error",error},
                {"pass",*error=='\0' && g_scanValid && g_scanFrames>=30 && g_scanActorMin>0 && mean<kScanBudgetMs}};
            Console::Print(std::format("NPC scan: mean {:.3f} ms, max {:.3f} ms, actors {}..{} (mean {:.1f}), {} frames / {:.2f} s; budget mean <{} ms: {} {}",
                mean,g_scanMax,g_scanFrames ? g_scanActorMin : 0,g_scanActorMax,g_scanFrames ? g_scanActorSum/g_scanFrames : 0,
                g_scanFrames,g_scanResult["duration_s"].get<double>(),kScanBudgetMs,g_scanResult["pass"].get<bool>() ? "PASS" : "FAIL",error));
        }

        void SampleNPCScanCost(RE::PlayerCharacter* player,double milliseconds)
        {
            const auto now=std::chrono::steady_clock::now();
            if (now-g_scanIssued<std::chrono::seconds(8)) return;
            const auto& p=g_scanSite["position"];
            const RE::NiPoint3 expected{p[0].get<float>(),p[1].get<float>(),p[2].get<float>()};
            auto* world=player->GetWorldspace();
            const bool atSite=world && world->GetFormID()==g_scanSite["world"].get<RE::FormID>() &&
                player->GetPosition().GetDistance(expected)<4096;
            auto* ui=RE::UI::GetSingleton();
            const bool playable=atSite && ui && !ui->GameIsPaused();
            if (!g_scanMeasuring) {
                if (!playable) {
                    if (now-g_scanIssued>std::chrono::seconds(30)) FinishNPCScanCost("market arrival/close console timed out");
                    return;
                }
                g_scanMeasuring=true; g_scanStarted=now; g_scanPrevious=now;
            }
            // A paused/loading interval or leaving the market invalidates this run.
            g_scanValid &= playable && now-g_scanPrevious<std::chrono::milliseconds(500);
            g_scanPrevious=now;
            if (playable) {
                ++g_scanFrames; g_scanSum+=milliseconds; g_scanMax=std::max(g_scanMax,milliseconds);
                g_scanActorSum+=g_scanActors.size(); g_scanActorMin=std::min(g_scanActorMin,g_scanActors.size());
                g_scanActorMax=std::max(g_scanActorMax,g_scanActors.size());
            }
            if (now-g_scanStarted>=std::chrono::seconds(10)) FinishNPCScanCost(g_scanValid ? "" : "paused/loading gap or left the market");
        }

		float Current(RE::Actor* a_actor, RE::ActorValue a_value) { return a_actor->AsActorValueOwner()->GetActorValue(a_value); }
		float Maximum(RE::Actor* a_actor, RE::ActorValue a_value)
		{
			return a_actor->AsActorValueOwner()->GetPermanentActorValue(a_value) +
			       a_actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, a_value);
		}

		bool Follower(RE::Actor* a_actor)
		{
			return a_actor && !a_actor->IsPlayerRef() && a_actor->IsPlayerTeammate() && !a_actor->IsDeleted() && !a_actor->IsDisabled();
		}

		void RefreshParty()
		{
			if (Frame::Now() < g_partyNext) return;
			g_partyNext = Frame::Now() + 0.5;
			std::erase_if(g_followers, [](const auto& entry) { return !Follower(entry.second.get().get()); });
			if (auto* processes = RE::ProcessLists::GetSingleton()) {
				processes->ForAllActors([](RE::Actor* actor) {
					if (Follower(actor)) g_followers[actor->GetFormID()] = actor->CreateRefHandle();
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}
			// Keep survivors in order. Newcomers use form order, never process-list or distance order.
			std::vector<RE::ActorHandle> next;
			std::set<RE::FormID> seen;
			for (const auto& handle : g_party) {
				const auto actor = handle.get();
				if (actor && g_followers.contains(actor->GetFormID())) { next.push_back(handle); seen.insert(actor->GetFormID()); }
			}
			for (const auto& [id, handle] : g_followers) {
				if (next.size() == 4) break;
				if (seen.insert(id).second) next.push_back(handle);
			}
			for (std::size_t i = 0; i < 4; ++i) {
				const auto old = i < g_party.size() ? g_party[i] : RE::ActorHandle{};
				const auto now = i < next.size() ? next[i] : RE::ActorHandle{};
				if (old != now) Portrait::Capture(now.get().get(), i + 1);
			}
			g_party = std::move(next);
			g_partyAuras.clear();
			for (const auto& handle : g_party) {
				if (const auto actor = handle.get()) g_partyAuras[actor->GetFormID()] = SkyrimAuras::Snapshot(actor.get());
			}
		}

		bool UsesMagicka(RE::Actor* a_actor)
		{
			if (Maximum(a_actor, RE::ActorValue::kMagicka) <= 0) return false;
			auto* base = a_actor->GetActorBase();
			if (base && base->npcClass) {
				const auto& w = base->npcClass->data.skillWeights;
				if (std::max({ w.alteration, w.conjuration, w.destruction, w.illusion, w.restoration }) >
					std::max({ w.oneHanded, w.twoHanded, w.archery })) return true;
			}
			const auto* style = base ? base->GetCombatStyle() : nullptr;
			return style && style->generalData.magicScoreMult > std::max(style->generalData.meleeScoreMult, style->generalData.rangedScoreMult);
		}

		// The WoW creature type the target frame and tooltip show, from Skyrim's actor type keywords.
		const char* CreatureType(RE::Actor* a_actor)
		{
			static const std::pair<const char*, const char*> kTypes[] = {
				{ "ActorTypeNPC", "Humanoid" }, { "ActorTypeUndead", "Undead" }, { "ActorTypeDragon", "Dragonkin" },
				{ "ActorTypeDaedra", "Demon" }, { "ActorTypeDwarven", "Mechanical" }, { "ActorTypeAnimal", "Beast" },
				{ "ActorTypeCreature", "Beast" }
			};
			for (const auto& [keyword, type] : kTypes) {
				if (a_actor->HasKeywordString(keyword)) {
					return type;
				}
			}
			return "";
		}


		// The target's selection circle, coloured by its reaction. The client makes its radius the unit's collision
		// width - twice its half-width (a night elf's 0.778 yards, measured off the client, evidence/charref/wow_circle_*);
		// a Skyrim actor's half-width is its character controller's radius (Havok units).
		void SelectionCircle(RE::PlayerCharacter* a_player)
		{
			auto*       target = g_targetSelf ? nullptr : g_target.get().get();
			const auto* controller = target && target->Is3DLoaded() ? target->GetCharController() : nullptr;
			if (!target || !target->Is3DLoaded()) {
				Avatar::SetCircle({}, 0.0f, {});
				return;
			}
			constexpr float kHavokToUnits = 69.99124f;
			const int       reaction = Reaction(target);
			// Ragdolls have no character controller; tiny creatures can report radius zero.
			// OBND horizontal width is a better fallback than the root's height-inflated sphere.
			const auto* base = target->GetActorBase();
			const float width = base ? std::max(base->boundData.boundMax.x - base->boundData.boundMin.x,
				base->boundData.boundMax.y - base->boundData.boundMin.y) * target->GetScale() : 0.0f;
			const float radius = wowcore::selectionCircleRadius(controller ? 2.0f * controller->radius * kHavokToUnits : 0.0f, width);
			Avatar::SetCircle(target->GetPosition(), radius,
				reaction <= 2 ? std::array{ 1.0f, 0.0f, 0.0f } : reaction == 4 ? std::array{ 1.0f, 1.0f, 0.0f } : std::array{ 0.0f, 1.0f, 0.0f });
		}

		// Their level is Skyrim's. A rise is WoW's level up (__WBLevelUp: its chat lines and sound; the light at the player's feet by
		// the levelUp command), told half a second after it (the attribute the player chose may land a frame later than the
		// level: its gain is measured then). Keep the notification in the snapshot: the render thread may coalesce
		// several main-thread ticks. Publish the actual level immediately; only the celebration waits.
		int g_levelShown = 0;
		std::string g_levelUp = "nil";
		int Level(RE::PlayerCharacter* a_player)
		{
			static std::array<float, 3> base{};
			static int                  rising = 0;
			static double               risingSince = 0;
			auto*                       av = a_player->AsActorValueOwner();
			const std::array<float, 3>  now{ av->GetBaseActorValue(RE::ActorValue::kHealth), av->GetBaseActorValue(RE::ActorValue::kMagicka),
				 av->GetBaseActorValue(RE::ActorValue::kStamina) };
			const int level = a_player->GetLevel();
			if (!g_levelShown || level < g_levelShown) {   // first seen, or a load: no level up
				g_levelShown = level, rising = 0, base = now;
				g_levelUp = "nil";
				return level;
			}
			if (level > g_levelShown) {
				rising = level, risingSince = Frame::Now();
				g_levelShown = level;
				g_levelUp = "nil";
			}
			if (!rising) {
				base = now;   // what the player has before the next rise
				return level;
			}
			if (Frame::Now() - risingSince < 0.5) {
				return level;
			}
			g_levelUp = std::format("{{ level = {}, gains = {{ health = {}, magicka = {}, stamina = {} }} }}", rising,
				std::max(0, static_cast<int>(now[0] - base[0])), std::max(0, static_cast<int>(now[1] - base[1])),
				std::max(0, static_cast<int>(now[2] - base[2])));
			g_levelShown = rising;
			rising = 0, base = now;
			return level;
		}

		std::string PlayerPart(RE::PlayerCharacter* a_player)
		{
			const auto hp = static_cast<int>(std::ceil(Current(a_player, RE::ActorValue::kHealth)));
			const auto hpMax = static_cast<int>(std::ceil(Maximum(a_player, RE::ActorValue::kHealth)));
			// skyrim: their mana is magicka, and the spell tooltips quote what the service answers in (service/skyrim.py).
			const auto skill = [&](RE::ActorValue a_value) { return static_cast<int>(Current(a_player, a_value)); };
			std::string skills;
			for (const auto& [name, value] : {
				std::pair{ "One-Handed", RE::ActorValue::kOneHanded }, { "Two-Handed", RE::ActorValue::kTwoHanded },
				{ "Archery", RE::ActorValue::kArchery }, { "Block", RE::ActorValue::kBlock },
				{ "Smithing", RE::ActorValue::kSmithing }, { "Heavy Armor", RE::ActorValue::kHeavyArmor },
				{ "Light Armor", RE::ActorValue::kLightArmor }, { "Pickpocket", RE::ActorValue::kPickpocket },
				{ "Lockpicking", RE::ActorValue::kLockpicking }, { "Sneak", RE::ActorValue::kSneak },
				{ "Alchemy", RE::ActorValue::kAlchemy }, { "Speech", RE::ActorValue::kSpeech },
				{ "Alteration", RE::ActorValue::kAlteration }, { "Conjuration", RE::ActorValue::kConjuration },
				{ "Destruction", RE::ActorValue::kDestruction }, { "Illusion", RE::ActorValue::kIllusion },
				{ "Restoration", RE::ActorValue::kRestoration }, { "Enchanting", RE::ActorValue::kEnchanting } }) {
				skills += std::format("[{}]={},", LuaQuote(name), skill(value));
			}
			const int level = Level(a_player);
			const auto* skillData = a_player->GetInfoRuntimeData().skills;
			const auto* data = skillData ? skillData->data : nullptr;
			const float xpMax = data && std::isfinite(data->levelThreshold) && data->levelThreshold > 0 ? data->levelThreshold : 1.0f;
			const float xp = data && std::isfinite(data->xp) ? std::max(data->xp, 0.0f) : 0.0f;
			const auto damage=[&](bool left) {
				auto* entry=a_player->GetEquippedEntryData(left);
				return entry && entry->object && entry->object->As<RE::TESObjectWEAP>() ? a_player->GetDamage(entry) : 0.f;
			};
			return std::format("player = {{ name = {}, level = {}, levelUp = {}, xp = {}, xpMax = {}, sex = {}, health = {}, healthMax = {}, power = {}, powerMax = {}, combat = {}, skyrim = {{ {} }}, armor = {{ {}, {}, 0, 0, 0 }}, nativeSheet = {{ health={}, healthMax={}, magicka={}, magickaMax={}, stamina={}, staminaMax={}, weight={}, carry={}, armor={}, damage={}, offDamage={}, fire={}, frost={}, shock={}, magic={}, poison={}, disease={} }} }}",
				LuaQuote(Integration::PlayerName()), level, g_levelUp, xp, xpMax, Character::Get().looks.sex + 2, std::max(hp, 0), std::max(hpMax, 1), Forms::PowerType() ? Forms::Power() : Cast::Mana(), Forms::PowerType() ? 100 : Cast::MaxMana(),
				a_player->IsInCombat() ? "true" : "false", skills,
				Current(a_player, RE::ActorValue::kDamageResist), Current(a_player, RE::ActorValue::kDamageResist),
				std::max(hp,0), std::max(hpMax,1), Current(a_player,RE::ActorValue::kMagicka), Maximum(a_player,RE::ActorValue::kMagicka),
				Current(a_player,RE::ActorValue::kStamina), Maximum(a_player,RE::ActorValue::kStamina),
				Current(a_player,RE::ActorValue::kInventoryWeight), Current(a_player,RE::ActorValue::kCarryWeight),
				Current(a_player,RE::ActorValue::kDamageResist), damage(false), damage(true),
				Current(a_player,RE::ActorValue::kResistFire), Current(a_player,RE::ActorValue::kResistFrost),
				Current(a_player,RE::ActorValue::kResistShock), Current(a_player,RE::ActorValue::kResistMagic),
				Current(a_player,RE::ActorValue::kPoisonResist), Current(a_player,RE::ActorValue::kResistDisease));
		}

		// A unit as the interface knows it (its WB.units entry).
		std::string UnitTable(RE::PlayerCharacter* a_player, RE::Actor* a_actor)
		{
			const bool  dead = a_actor->IsDead();
			const float hpMax = std::max(Maximum(a_actor, RE::ActorValue::kHealth), 1.0f);
			const float hp = dead ? 0.0f : std::max(Current(a_actor, RE::ActorValue::kHealth), 0.0f);
			const int   reaction = Reaction(a_actor);
			auto*       base = a_actor->GetActorBase();
			const bool  female = base && base->IsFemale();
			std::string resource;
			int powerType = 1;
			{
				const bool mage = UsesMagicka(a_actor);
				powerType = mage ? 0 : 3;
				const auto value = mage ? RE::ActorValue::kMagicka : RE::ActorValue::kStamina;
				resource = std::format(", power = {}, powerMax = {}, connected = {}",
					dead ? 0 : static_cast<int>(std::ceil(std::max(Current(a_actor, value), 0.0f))),
					static_cast<int>(std::ceil(std::max(Maximum(a_actor, value), 1.0f))), a_actor->Is3DLoaded() ? "true" : "false");
			}
			return std::format(
				"{{ guid = {}, name = {}, level = {}, health = {}, healthMax = {}, reaction = {}, attackable = {}, "
				"isPlayer = false, classification = \"normal\", creatureType = {}, distance = {:.1f}, dead = {}, sex = {}, "
				"class = \"Warrior\", classToken = \"WARRIOR\", powerType = {}{} }}",
				a_actor->GetFormID(), LuaQuote(a_actor->GetDisplayFullName()), a_actor->GetLevel(),
				static_cast<int>(std::ceil(hp)), static_cast<int>(std::ceil(hpMax)), reaction,
				reaction <= 4 ? "true" : "false", LuaQuote(CreatureType(a_actor)),
				a_player->GetPosition().GetDistance(a_actor->GetPosition()) / kUnitsPerYard, dead ? "true" : "false",
				female ? 3 : 2, powerType, resource);
		}

		// Where their head is on screen (fractions), for their chat bubble; false when it is not (first person, off screen).
		std::string HeadPart(RE::PlayerCharacter* a_player)
		{
			auto* camera = RE::PlayerCamera::GetSingleton();
			const auto [width, height] = RE::BSGraphics::Renderer::GetScreenSize();
			const bool first = camera && camera->currentState && camera->currentState->id == RE::CameraState::kFirstPerson;
			const auto head = Portrait::HeadPosition(a_player);
			float x = 0, y = 0;
			if (first || !head || width <= 0 || height <= 0 || !Picker::WorldToScreen(*head + RE::NiPoint3{ 0, 0, 24 }, x, y) ||
				!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x >= width || y >= height) return "head=false";
			return std::format("head={{x={:.6f},y={:.6f}}}", x / width, y / height);
		}

		std::string NameplatePart(RE::PlayerCharacter* a_player)
		{
			// The vanilla client's short nameplate range is 20 yards. Read nodes and
			// cast against the rendered camera every frame, never on the UI/loader thread.
			g_plateActors.clear();
			std::string rows;
			const auto [width, height] = RE::BSGraphics::Renderer::GetScreenSize();
			auto* processes = RE::ProcessLists::GetSingleton();
			if (processes && width > 0 && height > 0) for (const auto& handle : processes->highActorHandles) {
				const auto actor = handle.get();
				if (!actor || actor->IsPlayerRef() || actor->IsDead() || actor->IsDeleted() || actor->IsDisabled() || !actor->Is3DLoaded() ||
					Current(actor.get(), RE::ActorValue::kInvisibility) > 0 ||
					a_player->GetPosition().GetDistance(actor->GetPosition()) > 20.0f * kUnitsPerYard) continue;
				const auto head = Portrait::HeadPosition(actor.get());
				const auto at = head.value_or(actor->GetPosition() + RE::NiPoint3{ 0, 0, actor->GetHeight() });
				float x = 0, y = 0;
				if (!Picker::WorldToScreen(at, x, y) || !std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x >= width || y >= height) continue;
				RE::NiPoint3 origin, direction;
				if (!Picker::ScreenRay(x, y, origin, direction) || Picker::Cast(origin, at, false, actor.get()).hit) continue;
				auto row = UnitTable(a_player, actor.get());
				row.pop_back();
				rows += row + std::format(", x={:.8f}, y={:.8f}", x / width, y / height);
				// Its chat bubble's point (wowapi_bubbles.lua), as ChatBubbleFrame places it: 0.7 yards over the top of its
				// model (bounds x scale, as GetHeight). Off the top of the screen the bubble is, too.
				float bx = 0, by = 0;
				if (Picker::WorldToScreen(actor->GetPosition() + RE::NiPoint3{ 0, 0, actor->GetHeight() + 0.7f * kUnitsPerYard }, bx, by) &&
					std::isfinite(bx) && std::isfinite(by)) rows += std::format(", bx={:.8f}, by={:.8f}", bx / width, by / height);
				rows += " },";
				g_plateActors[actor->GetFormID()] = actor->CreateRefHandle();
			}
			return std::format("nameplates={{epoch={},units={{{}}}}}", g_plateEpoch, rows);
		}

		std::string PartyPart(RE::PlayerCharacter* a_player)
		{
			std::string out = "party = {";
			for (const auto& handle : g_party) {
				if (const auto actor = handle.get()) {
					auto row = UnitTable(a_player, actor.get());
					row.pop_back();
					out += row + ", auras = " + g_partyAuras[actor->GetFormID()] + " },";
				}
			}
			return out + "}";
		}

		std::string TargetPart(RE::PlayerCharacter* a_player)
		{
			if (g_targetSelf) {
				return "target = \"player\"";
			}
			auto target = g_target.get();
			auto* actor = target.get();
			if (!actor || actor->IsDisabled() || actor->IsDeleted()) {
				g_target.reset();
				return "target = false";
			}
			return "target = " + UnitTable(a_player, actor);
		}

		// What the pointer is over in the world, for the client's mouseover tooltip: a unit, or a thing by its name -
		// locked, or Skyrim's theft to take or open (the activation prompt that would say so is hidden). Nothing over
		// the interface, or over what has no name (the ground, walls).
		RE::ObjectRefHandle g_hover;
		const char* Offer(RE::TESObjectREFR* a_ref, bool a_alt);
		RE::TESBoundObject* Produce(RE::TESObjectREFR* a_ref)
		{
			auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
			if (auto* flora = base ? base->As<RE::TESFlora>() : nullptr) return flora->produceItem;
			if (auto* tree = base ? base->As<RE::TESObjectTREE>() : nullptr) return tree->produceItem;
			return nullptr;
		}
		std::string ActivationHint(RE::TESObjectREFR* a_ref)
		{
			const std::string offer = Offer(a_ref, false);
			if (offer == "HERB") {
				auto* item = Produce(a_ref);
				return std::string("Right-click: Harvest ") + (item && item->GetName() ? item->GetName() : "ingredient");
			}
			if (offer == "PICKUP") return "Right-click: Pick up";
			if (offer == "INTERACT") {
				RE::BSString text;
				if (a_ref->GetBaseObject()->GetActivateText(a_ref, text) && text.length()) return std::string("Right-click: ") + NativeText::Activation(text.c_str());
				return "Right-click: Activate";
			}
			return "";
		}
		RE::NiPoint3        g_holdTarget;   // where what the player holds is being pulled (tests read it)
		std::string MouseoverPart(RE::PlayerCharacter* a_player)
		{
			const auto ref = g_hover.get();
			if (!ref || ref->IsDisabled() || ref->IsDeleted()) {
				return "mouseover = false";
			}
			if (auto* actor = ref->As<RE::Actor>()) {
				return "mouseover = " + UnitTable(a_player, actor);
			}
			// Its name as plain text: Scaleform markup (a quest diamond) is stripped as its activation prompt's is.
			const std::string name = NativeText::Activation(ref->GetDisplayFullName());
			if (name.empty()) {
				return "mouseover = false";
			}
			return std::format("mouseover = {{ object = {}, action = {}, locked = {}, steal = {} }}", LuaQuote(name), LuaQuote(ActivationHint(ref.get())), ref->IsLocked() ? "true" : "false",
				ref->IsCrimeToActivate() ? "true" : "false");
		}

		// Subzone: the location the player is in; zone: the hold it belongs to (else the worldspace).
		std::string WorldPart(RE::PlayerCharacter* a_player)
		{
			std::string subzone, zone;
			if (auto* loc = a_player->GetCurrentLocation()) {
				subzone = loc->GetName();
				for (auto* l = loc; l; l = l->parentLoc) {
					if (l->HasKeywordString("LocTypeHold")) {
						zone = l->GetName();
						break;
					}
				}
			}
			if (zone.empty()) {
				if (auto* ws = a_player->GetWorldspace()) {
					zone = ws->GetName();
				}
			}
			if (zone.empty()) {
				zone = subzone;
			}
			const float hours = RE::Calendar::GetSingleton() ? RE::Calendar::GetSingleton()->GetHour() : 12.0f;
			const int   h = static_cast<int>(hours) % 24, m = static_cast<int>((hours - std::floor(hours)) * 60.0f);
			// Their septims as WoW money, 1 septim = 1 gold (the user's choice), in copper as GetMoney answers.
			const auto money = std::min<std::int64_t>(std::int64_t{ a_player->GetGoldAmount() } * 10000, INT32_MAX);
			return std::format("world = {{ zone = {}, subzone = {}, hour = {}, minute = {}, money = {} }}", LuaQuote(zone),
				LuaQuote(subzone == zone ? "" : subzone), h, m, std::max<std::int64_t>(money, 0));
		}

		int LocationIcon(RE::MARKER_TYPE type)
		{
			return type == RE::MARKER_TYPE::kCave || type == RE::MARKER_TYPE::kMine ? 16 :
				type == RE::MARKER_TYPE::kFarm || type == RE::MARKER_TYPE::kTown || type == RE::MARKER_TYPE::kSettlement ||
				type == RE::MARKER_TYPE::kShack || type == RE::MARKER_TYPE::kWoodMill ? 4 :
				type == RE::MARKER_TYPE::kSmelter ? 26 : type == RE::MARKER_TYPE::kStable ? 36 : 5;
		}

		// The full map uses Skyrim's own exterior cache even after an interior
		// save/load. Never project interior coordinates onto exterior terrain.
		std::string WorldMapPart(RE::PlayerCharacter* player)
		{
			const auto* cell = player->GetParentCell();
			if (!cell) return "worldmap = false";
			const bool interior = cell->IsInteriorCell();
			auto* world = interior ? player->GetPlayerRuntimeData().cachedWorldSpace : player->GetWorldspace();
			if (!world) return "worldmap = false";
			const auto p = interior ? player->GetPlayerRuntimeData().exteriorPosition : player->GetPosition();
			for (int depth = 0; world->parentWorld && depth < 16 && world->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseLODData); ++depth)
				world = world->parentWorld;
			std::string parents;
			auto* parent = world->parentWorld;
			for (int depth = 0; parent && parent != world && depth < 16; ++depth) {
				parents += LuaQuote(parent->GetFormEditorID()) + ",";
				parent = parent->parentWorld;
			}
			std::string pois;
			const auto add = [&](RE::FormID id, const char* kind, const std::string& name, const RE::NiPoint3& at, int icon, RE::FormID quest = 0) {
				if (name.empty() || !std::isfinite(at.x) || !std::isfinite(at.y)) return;
				pois += std::format("{{guid={},kind={},name={},x={:.6f},y={:.6f},icon={},discovered=true,quest={}}},",
					id,LuaQuote(kind),LuaQuote(name),at.x/kUnitsPerYard,at.y/kUnitsPerYard,icon,quest);
			};
			// All discovered markers in Skyrim's current-world cache, without the
			// compass distance cap. Fast travel remains the native map's operation.
			std::set<RE::FormID> seen;
			for (const auto& handle : player->GetPlayerRuntimeData().currentMapMarkers) {
				const auto ref = handle.get();
				if (!ref || !seen.insert(ref->GetFormID()).second) continue;
				const auto* extra = ref->extraList.GetByType<RE::ExtraMapMarker>();
				const auto* data = extra ? extra->mapData : nullptr;
				if (!data || data->type.get() == RE::MARKER_TYPE::kNone || !data->flags.any(RE::MapMarkerData::Flag::kCanTravelTo)) continue;
				if (const auto at = Quests::CompassPosition(player,ref.get(),nullptr,true))
					add(ref->GetFormID(),"location",data->locationName.fullName.c_str(),*at,LocationIcon(data->type.get()));
			}
			for (const auto& target : Quests::CompassTargets(player,true))
				add(target.reference,"quest",target.name,target.position,6,target.quest);
			const auto& info = player->GetInfoRuntimeData();
			if (const auto ref = info.playerMapMarker.get())
				if (const auto at = Quests::CompassPosition(player,ref.get(),info.playerMarkerPath,true))
					add(ref->GetFormID(),"custom","Custom map marker",*at,0);
			const auto* name = world->GetName();
			return std::format("worldmap={{worldspace={},name={},parents={{{}}},x={:.6f},y={:.6f},heading={:.6f},interior={},pois={{{}}}}}",
				LuaQuote(world->GetFormEditorID()),LuaQuote(name && *name ? name : world->GetFormEditorID()),parents,
				p.x/kUnitsPerYard,p.y/kUnitsPerYard,player->GetHeading(false),interior?"true":"false",pois);
		}

		// Indoors the client shows its building's floor plan; a Skyrim interior's walkable floor (its navmesh) stands in
		// for it. Sent when their cell or its loaded navmesh changes; an empty plan outdoors.
		void PublishFloor(const RE::TESObjectCELL* a_cell)
		{
			static RE::FormID  sentCell = 0;
			static std::size_t sentTriangles = 0;
			auto* meshes = a_cell && a_cell->IsInteriorCell() ? a_cell->GetRuntimeData().navMeshes : nullptr;
			std::size_t triangles = 0;
			if (meshes) for (const auto& mesh : meshes->navMeshes) triangles += mesh ? mesh->triangles.size() : 0;
			const RE::FormID cell = meshes ? a_cell->GetFormID() : 0;
			if (cell == sentCell && triangles == sentTriangles) return;
			sentCell = cell, sentTriangles = triangles;
			std::vector<wowcore::MinimapFloorTriangle> floor;
			floor.reserve(triangles);
			if (meshes) for (const auto& mesh : meshes->navMeshes) {
				if (!mesh) continue;
				const auto& v = mesh->vertices;
				for (const auto& t : mesh->triangles) {
					using Flag = RE::BSNavmeshTriangle::TriangleFlag;
					if (t.triangleFlags.any(Flag::kDeleted) || t.vertices[0] >= v.size() || t.vertices[1] >= v.size() || t.vertices[2] >= v.size()) continue;
					const auto& a = v[t.vertices[0]].location; const auto& b = v[t.vertices[1]].location; const auto& c = v[t.vertices[2]].location;
					const float low = std::min({ a.z, b.z, c.z }), high = std::max({ a.z, b.z, c.z });
					wowcore::MinimapFloorTriangle f{ { a.x / kUnitsPerYard, a.y / kUnitsPerYard }, { b.x / kUnitsPerYard, b.y / kUnitsPerYard },
						{ c.x / kUnitsPerYard, c.y / kUnitsPerYard }, (low + high) / 2 / kUnitsPerYard };
					f.rise = (high - low) / 2 / kUnitsPerYard;
					// A wall: no floor across the edge, in this navmesh or a linked one.
					for (int e = 0; e < 3; ++e) {
						if (t.triangles[e] == 0xFFFF && !t.triangleFlags.any(static_cast<Flag>(1 << e))) f.walls |= 1u << e;
					}
					floor.push_back(f);
				}
			}
			WowUi::SetMinimapFloor(std::move(floor));
		}

		// A text emote (DoEmote: /dance, /wave, /spit): their own line in chat, as the server tells the client, their race and
		// sex's voice, and its animation - once, or a state the player keeps until the player moves (dancing, sitting, kneeling).
		bool g_emoteHeld = false;
		void Emote(RE::PlayerCharacter* a_player, const std::string& a_token)
		{
			const auto looks = Character::Looks();
			const auto e = WowUi::EmoteText(a_token, looks.race, looks.sex);
			if (!e) return;
			auto* target = Target();
			std::string text = e->alone;
			if (target && target != a_player && !e->atTarget.empty()) {   // at their target, as the client's /wave does
				text = e->atTarget;
				if (const auto at = text.find("%s"); at != std::string::npos) text.replace(at, 2, target->GetDisplayFullName());
			}
			WowUi::Run(std::format("__WBFire('CHAT_MSG_TEXT_EMOTE',{},{})", LuaQuote(text), LuaQuote(Integration::PlayerName())));
			if (e->sound) WowUi::PlaySound(e->sound);
			Reactions::OnEmote(a_player, target, a_token);
			// The client's stand states: down, then held (SitGroundDown/SitGround, SleepDown/Sleep, KneelStart/KneelLoop).
			if (e->specProc == 1 && e->standState) {
				const std::array<std::pair<int, int>, 3> states = { { { 96, 97 }, { 99, 100 }, { 114, 115 } } };
				const int index = e->standState == 1 ? 0 : e->standState == 3 ? 1 : e->standState == 8 ? 2 : -1;
				if (index >= 0) Avatar::ActThenLoop(states[index].first, states[index].second);
				g_emoteHeld = index >= 0;
			} else if (e->specProc == 1) {   // /stand
				Avatar::StopActing(); g_emoteHeld = false;
			} else if (e->anim) {
				Avatar::Act(static_cast<std::int32_t>(e->anim), e->specProc == 2);
				g_emoteHeld = e->specProc == 2;
			}
		}

		// Moving, jumping or casting ends an emote the player holds, as in the client.
		void EmoteEnds(RE::PlayerCharacter* a_player)
		{
			if (!g_emoteHeld) return;
			const auto* controls = RE::PlayerControls::GetSingleton();
			const bool moving = (controls && (controls->data.moveInputVec.Length() > 0.01f || controls->data.autoMove)) ||
				a_player->IsInMidair() || Cast::Casting();
			if (moving || Avatar::Acting() < 0) {
				if (moving) Avatar::StopActing();
				g_emoteHeld = false;
			}
		}

		std::string TreasureBlips(RE::PlayerCharacter* player, std::uint32_t mask)
		{
			static std::vector<RE::FormID> containers, factions;
			static double next = 0;
			if (!mask) { containers.clear(); factions.clear(); next = 0; return {}; }
			static std::set<RE::FormID> merchants;
			if (Frame::Now() >= next) {
				// Every 5 s (a hitch on a modded load: the whole form table): containers and merchants' chests kept
				// from inside the one read lock; nothing looked up again per frame (review, October 16).
				next = Frame::Now()+5; containers.clear(); factions.clear(); merchants.clear();
				// As CompassLocations: never walk attached-cell arrays while the loader changes them.
				const auto [forms, lock] = RE::TESForm::GetAllForms();
				RE::BSReadLockGuard guard(lock.get());
				if (forms) for (const auto& [id, form] : *forms) if (form) {
					if (form->GetFormType() == RE::FormType::Reference) {
						const auto* base = static_cast<RE::TESObjectREFR*>(form)->GetBaseObject();
						if (base && base->As<RE::TESObjectCONT>()) containers.push_back(id);
					} else if (form->GetFormType() == RE::FormType::Faction) {
						if (const auto* ref = static_cast<RE::TESFaction*>(form)->vendorData.merchantContainer) merchants.insert(ref->GetFormID());
					}
				}
			}
			std::string blips;
			const auto* here = player->GetParentCell(); const auto p = player->GetPosition();
			for (const auto id : containers) {
				auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				auto* cell = ref ? ref->GetParentCell() : nullptr;
				if (!ref || !cell || !cell->IsAttached() || !ref->Is3DLoaded() || ref->IsDeleted() || ref->IsDisabled() ||
					ref->IsActivationBlocked() || merchants.contains(id) || ref->GetOwner() || cell->GetOwner()) continue;
				if (here->IsInteriorCell() ? cell != here : cell->IsInteriorCell() || ref->GetWorldspace() != player->GetWorldspace()) continue;
				const auto at = ref->GetPosition();
				if (!std::isfinite(at.x) || !std::isfinite(at.y) || std::hypot(at.x-p.x,at.y-p.y) > 200.f*kUnitsPerYard) continue;
				// Include lootable CONTs, including locked chests, urns, strongboxes and other treasure vessels. Reading
				// without initialization preserves unopened leveled loot; emptied containers no longer have a blip.
				const auto items = ref->GetInventory([](RE::TESBoundObject&) { return true; }, true);
				if (std::ranges::none_of(items, [](const auto& item) { return item.first && item.second.first > 0; })) continue;
				const auto* name = ref->GetDisplayFullName(); if (!name || !*name) continue;
				blips += std::format("{{ guid = {}, name = {}, x = {:.6f}, y = {:.6f}, reaction = 4, resource = 6 }},",
					id,LuaQuote(name),at.x/kUnitsPerYard,at.y/kUnitsPerYard);
			}
			return blips;
		}

		// Copy terrain identity, pose and compass targets on the main thread.
		// Interiors retain directional POIs, but have no terrain or world-map coordinates.
		std::string MinimapPart(RE::PlayerCharacter* a_player)
		{
			const auto* cell = a_player->GetParentCell();
			auto* world = a_player->GetWorldspace();
			auto*       ui = RE::UI::GetSingleton();
			// While a loading screen is up the game's loader threads change the cells and references walked below.
			if (!cell || (!cell->IsInteriorCell() && !world) || !ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
				return "minimap = false";
			}
			const bool interior = cell->IsInteriorCell();
			PublishFloor(cell);
			for (int depth = 0; !interior && world->parentWorld && depth < 16 && world->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseLODData); ++depth) {
				world = world->parentWorld;
			}
			const auto p = a_player->GetPosition();
			std::string parents;
			// Only parents whose LOD the worldspace draws (PNAM use-LOD-data): the walled cities show Tamriel's
			// terrain, Markarth, Blackreach and the cave worldspaces their own; the atlas builder applies the same rule.
			const auto lodParent = [](RE::TESWorldSpace* w) {
				return w && w->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseLODData) ? w->parentWorld : nullptr; };
			auto* parent = interior ? nullptr : lodParent(world);
			for (int depth = 0; parent && parent != world && depth < 16; ++depth) {
				parents += LuaQuote(parent->GetFormEditorID()) + ",";
				parent = lodParent(parent);
			}
			struct Candidate { RE::FormID id, quest; std::string kind, name; double bearing; int markerType=0; RE::NiPoint3 at{}; int icon=0; bool discovered=true; };
			std::vector<Candidate> candidates;
			std::string pois;
			auto add = [&](RE::FormID id, const char* kind, const std::string& name, const RE::NiPoint3& at, int icon, bool discovered, RE::FormID quest = 0) {
				if (name.empty() || !std::isfinite(at.x) || !std::isfinite(at.y)) return;
				candidates.push_back({id,quest,kind,name,std::atan2(at.x-p.x,at.y-p.y)*180/std::numbers::pi,0,at,icon,discovered});
				pois += std::format("{{ guid = {}, kind = {}, name = {}, x = {:.6f}, y = {:.6f}, icon = {}, discovered = {}, quest = {} }},",
					id, LuaQuote(kind), LuaQuote(name), at.x / kUnitsPerYard, at.y / kUnitsPerYard, icon, discovered ? "true" : "false", quest);
			};
			// The player's map-marker cache includes undiscovered locations. Use
			// Skyrim's live compass distance, not the minimap's current zoom radius.
			auto* settings = RE::GameSettingCollection::GetSingleton();
			const auto* distanceSetting = settings ? settings->GetSetting("fHUDCompassLocationMaxDist") : nullptr;
			const float distance = distanceSetting ? std::max(0.0f, distanceSetting->GetFloat()) : 20000.0f;
			std::set<RE::FormID> seen;
			for (const auto& handle : a_player->GetPlayerRuntimeData().currentMapMarkers) {
				if (interior) break;
				const auto ref = handle.get();
				if (!ref || !seen.insert(ref->GetFormID()).second) continue;
				const auto* extra = ref->extraList.GetByType<RE::ExtraMapMarker>();
				const auto* data = extra ? extra->mapData : nullptr;
				const auto at = Quests::CompassPosition(a_player, ref.get(), nullptr);
				if (!data || data->type.get() == RE::MARKER_TYPE::kNone || !at ||
					std::hypot(at->x - p.x, at->y - p.y) > distance) continue;
				// Neutral tower, house, mountain/mine, anvil/forge, or stable from POIIcons.
				const int icon = LocationIcon(data->type.get());
				add(ref->GetFormID(), "location", data->locationName.fullName.c_str(), *at, icon, data->flags.any(RE::MapMarkerData::Flag::kCanTravelTo));
			}
			for (const auto& target : Quests::CompassTargets(a_player))
				add(target.reference, "quest", target.name, target.position, 6, true, target.quest);
			const auto& info = a_player->GetInfoRuntimeData();
			const auto custom = info.playerMapMarker.get();
			if (custom) {
				if (const auto at = Quests::CompassPosition(a_player, custom.get(), info.playerMarkerPath))
					add(custom->GetFormID(), "custom", "Custom map marker", *at, 0, true);
			}
			const auto compass=CompassMarkers::Snapshot();
			const bool nativeCompass=compass.value("ready",false);
			if (nativeCompass) {
				std::erase_if(candidates,[](const auto& c) { return c.kind=="location"; });
				for (const auto& location:CompassLocations::Snapshot(a_player))
					candidates.push_back({location.id,0,"location",location.name,
						std::atan2(location.position.x-p.x,location.position.y-p.y)*180/std::numbers::pi,location.type,location.position,LocationIcon(static_cast<RE::MARKER_TYPE>(location.type))});
				// Names only: native HUD markers still determine which enemies appear.
				if (std::ranges::any_of(compass["markers"],[](const auto& marker) { return marker.at("frame").template get<int>()==4; })) {
					if (auto* lists=RE::ProcessLists::GetSingleton()) {
						std::set<RE::FormID> enemies;
						for (auto* handles : {&lists->highActorHandles,&lists->middleHighActorHandles,&lists->middleLowActorHandles,&lists->lowActorHandles})
							for (const auto& handle:*handles) {
								const auto actor=handle.get();
								if (!actor || actor.get()==a_player || actor->IsDead() || !actor->IsHostileToActor(a_player) ||
									!enemies.insert(actor->GetFormID()).second) continue;
								const auto at=Quests::CompassPosition(a_player,actor.get(),nullptr);
								const std::string name=actor->GetDisplayFullName();
								if (at && !name.empty()) candidates.push_back({actor->GetFormID(),0,"enemy",name,
									std::atan2(at->x-p.x,at->y-p.y)*180/std::numbers::pi,0,*at});
							}
					}
				}
				pois.clear();
				for (const auto& marker : compass["markers"]) {
					const int frame=marker["frame"]; const double bearing=marker["bearing"];
					const std::string kind=frame<=2 ? "quest" : frame==3 ? "custom" : frame==4 ? "enemy" : "location";
					std::string name=frame==1 ? "Quest objective" : frame==2 ? "Quest destination (door)" : frame==3 ? "Custom map marker" : frame==4 ? "Enemy" : "Location";
					RE::FormID id=0,quest=0; double nearest=3; const Candidate* matched=nullptr;
					for (const auto& candidate : candidates) if (candidate.kind==kind &&
						(kind!="location" || frame==4+candidate.markerType || frame==71+candidate.markerType)) {
						const double delta=std::abs(std::remainder(candidate.bearing-bearing,360.0));
						if (delta<nearest) { nearest=delta; name=candidate.name; id=candidate.id; quest=candidate.quest; matched=&candidate; }
					}
					const double radians=bearing*std::numbers::pi/180;
					// A native compass marker is a bearing, independent of map zoom; a matched candidate also carries its
					// real position and WoW icon so the renderer can place it as 1.12 would (undiscovered: frames 71+).
					const auto at=matched ? RE::NiPoint3{matched->at.x/kUnitsPerYard,matched->at.y/kUnitsPerYard,0} :
						RE::NiPoint3{float(p.x/kUnitsPerYard+std::sin(radians)*1e6),float(p.y/kUnitsPerYard+std::cos(radians)*1e6),0};
					pois+=std::format("{{ guid={}, quest={}, kind={}, name={}, x={:.6f}, y={:.6f}, bearing={:.6f}, placed={}, icon={}, discovered={}, native={}, opacity={:.6f}, scale={:.6f} }},",
						id,quest,LuaQuote(kind),LuaQuote(name),at.x,at.y,bearing,matched ? "true" : "false",matched ? matched->icon : 0,
						matched && (kind!="location" || frame<71) ? "true" : "false",frame,marker["opacity"].get<double>(),marker["scale"].get<double>());
				}
			}
			std::string blips;
			const auto mask = interior ? 0u : g_scanActive ? 0x1FFu : Cast::TrackingMask();
			const auto resources = Cast::ResourceTrackingMask();
			blips += TreasureBlips(a_player, resources);
			if (mask && RE::ProcessLists::GetSingleton()) {
				// Every loaded actor, whatever its process level (walking all attached references each frame raced the
				// loader threads' cell changes and crashed a cow). Not restricted to their target, hostility, camera or sight.
				auto* lists = RE::ProcessLists::GetSingleton();
				auto  each = [&](auto&& a_visit) {
					for (auto* handles : { &lists->highActorHandles, &lists->middleHighActorHandles, &lists->middleLowActorHandles, &lists->lowActorHandles }) {
						for (auto& h : *handles) {
							if (auto a = h.get()) {
								a_visit(a.get());
							}
						}
					}
				};
				each([&](RE::Actor* actor) {
					if (!actor || actor == a_player || actor->IsDisabled() || actor->IsDead() ||
						actor->GetWorldspace() != a_player->GetWorldspace()) return;
					static constexpr std::string_view types[] = {
						"Beast", "Dragonkin", "Demon", "Elemental", "Giant", "Undead", "Humanoid", "Critter", "Mechanical"
					};
					const auto type = std::ranges::find(types, std::string_view(CreatureTypeName(actor)));
					if (type == std::end(types) || !(mask & (1u << (type - std::begin(types))))) return;
					const auto at = actor->GetPosition();
					if (std::hypot(at.x - p.x, at.y - p.y) <= 200.0f * kUnitsPerYard) {
						if (g_scanActive) g_scanActors.insert(actor->GetFormID());
						blips += std::format("{{ guid = {}, name = {}, x = {:.6f}, y = {:.6f}, reaction = {} }},",
							actor->GetFormID(), LuaQuote(actor->GetDisplayFullName()), at.x / kUnitsPerYard, at.y / kUnitsPerYard, Reaction(actor));
					}
				});
			}
			return std::format("minimap = {{ worldspace = {}, parents = {{ {} }}, x = {:.6f}, y = {:.6f}, z = {:.6f}, heading = {:.6f}, trackingMask = {}, resourceTrackingMask = {}, blips = {{ {} }}, pois = {{ {} }}, compass = {} }}",
				LuaQuote(interior ? std::format("interior:{:08X}", cell->GetFormID()) : std::string(world->GetFormEditorID())),
				parents, p.x / kUnitsPerYard, p.y / kUnitsPerYard, p.z / kUnitsPerYard, a_player->GetHeading(false), resources ? 0x10000u | resources : mask, resources, blips, pois, nativeCompass ? "true" : "false");
		}

		// Tab targeting: the units in front of you, nearest first, in sight and within 40 yards; each press takes the
		// next after the current target (enemies: what you can attack; friends: your followers), Shift backwards.
		void TargetNearest(bool a_enemy, bool a_backwards)
		{
			auto*                   player = RE::PlayerCharacter::GetSingleton();
			const auto              [w, h] = RE::BSGraphics::Renderer::GetScreenSize();
			std::vector<std::pair<float, RE::Actor*>> found;
			for (auto& handle : RE::ProcessLists::GetSingleton()->highActorHandles) {
				auto  a = handle.get();
				auto* actor = a.get();
				if (!actor || actor == player || actor->IsDead() || !actor->Is3DLoaded() || (Reaction(actor) <= 4) != a_enemy) {
					continue;
				}
				const float d = player->GetPosition().GetDistance(actor->GetPosition());
				float       x = 0, y = 0;
				bool        unused = false;
				if (d > 40.0f * kUnitsPerYard || !Picker::WorldToScreen(actor->GetPosition() + RE::NiPoint3{ 0, 0, actor->GetHeight() * 0.5f }, x, y) ||
					x < 0 || y < 0 || x >= w || y >= h || !player->HasLineOfSight(actor, unused)) {
					continue;
				}
				found.emplace_back(d, actor);
			}
			if (found.empty()) {
				return;
			}
			std::ranges::sort(found, {}, &std::pair<float, RE::Actor*>::first);
			const auto current = std::ranges::find(found, Target(), &std::pair<float, RE::Actor*>::second);
			std::size_t i = 0;
			if (current != found.end()) {
				const auto at = static_cast<std::size_t>(current - found.begin());
				i = a_backwards ? (at + found.size() - 1) % found.size() : (at + 1) % found.size();
			}
			SetTarget(found[i].second);
		}

		// The commands the interface sends the game ("command", name, args...).
		void RunCommand(const std::string& a_name, const std::vector<std::string>& a_args)
		{
			auto arg = [&](std::size_t i) { return i < a_args.size() ? a_args[i] : std::string(); };
			if (a_name == "TargetNearestEnemy" || a_name == "TargetNearestFriend") {
				TargetNearest(a_name == "TargetNearestEnemy", arg(0) == "1");
			} else if (a_name == "ClearTarget") {
				SetTarget(nullptr);
			} else if (a_name == "AttackTarget") {
				Melee::Start(Target());
			} else if (a_name == "ToggleSheath") {   // their weapon, not Skyrim's: the player draws and sheathes as the client does
				Avatar::SetWeaponsDrawn(!Avatar::WeaponsDrawn(), true);
			} else if (a_name == "TargetUnit") {
				if (arg(0) == "player") {
					SetTarget(RE::PlayerCharacter::GetSingleton());
				} else if (arg(0).empty()) {
					SetTarget(nullptr);
				} else if (const auto token = arg(0); token.size() == 6 && token.starts_with("party") && token[5] >= '1' && token[5] <= '4') {
					auto* actor = PartyMember(token[5] - '1');
					const auto expected = static_cast<RE::FormID>(std::strtoul(arg(1).c_str(), nullptr, 10));
					if (actor && (!expected || actor->GetFormID() == expected)) SetTarget(actor);
				}
			}
		}

		// Skyrim's menus the interface stands in for (__WBSkyrimMenus).
		// Skyrim's own menu controls, pressed: Journal opens the journal on its quests, Pause on its system page (what
		// Esc opens) - a menu shown by name opens on whatever page it was last left at.
		void PressMenuControl(const RE::BSFixedString& a_userEvent, bool a_release = false)
		{
			auto* controls = RE::MenuControls::GetSingleton();
			auto* handler = controls ? controls->menuOpenHandler : nullptr;
			const auto key = RE::ControlMap::GetSingleton()->GetMappedKey(a_userEvent, RE::INPUT_DEVICE::kKeyboard);
			if (auto* press = handler ? RE::ButtonEvent::Create(RE::INPUT_DEVICE::kKeyboard, a_userEvent, key, 1.0f, 0.0f) : nullptr) {
				handler->ProcessButton(press);
				RE::free(press);
			}
			// Some open as the key comes up (the Tab menu: a held Tab is the wait menu's on a gamepad): let go of it too.
			if (a_release) {
				if (auto* up = handler ? RE::ButtonEvent::Create(RE::INPUT_DEVICE::kKeyboard, a_userEvent, key, 0.0f, 0.1f) : nullptr) {
					handler->ProcessButton(up);
					RE::free(up);
				}
			}
		}

		void OpenMenu(const std::string& a_name)
		{
			auto* ue = RE::UserEvents::GetSingleton();
			if (a_name == "Quests") {
				PressMenuControl(ue->journal);
			} else if (a_name == "Tween") {
				PressMenuControl(ue->tweenMenu, true);
			} else if (a_name == "System") {
				PressMenuControl(ue->pause);
			} else if (a_name == RE::StatsMenu::MENU_NAME) {
				// MenuOpenHandler accepts Quick Stats, not the Tween menu's Stats event.
				// Keep Skyrim's own eligibility checks and LevelUp Menu flow.
				PressMenuControl(ue->quickStats);
			} else if (a_name == RE::MagicMenu::MENU_NAME) {
				PressMenuControl(ue->quickMagic);
			} else if (a_name == RE::InventoryMenu::MENU_NAME || a_name == RE::MapMenu::MENU_NAME) {
				RE::UIMessageQueue::GetSingleton()->AddMessage(a_name, RE::UI_MESSAGE_TYPE::kShow, nullptr);
			}
		}

		// What people say - Skyrim's subtitles, dialogue and chatter alike - into chat as WoW shows a creature's speech
		// (the user: the chat should log the fights and what people say). Each line once, as it appears.
		void Subtitles()
		{
			auto* subtitles = RE::SubtitleManager::GetSingleton();
			if (!subtitles) {
				return;
			}
			static std::set<std::pair<std::uint32_t, std::string>> shown;
			std::set<std::pair<std::uint32_t, std::string>>        now;
			std::vector<std::pair<RE::NiPointer<RE::TESObjectREFR>, std::string>> said;
			{
				RE::BSSpinLockGuard guard(subtitles->lock);
				for (const auto& s : subtitles->subtitles) {
					const std::string text = s.subtitle.c_str() ? s.subtitle.c_str() : "";
					const auto        speaker = s.speaker.get();
					if (text.empty() || !speaker) {
						continue;
					}
					const auto key = std::make_pair(s.speaker.native_handle(), text);
					now.insert(key);
					if (!shown.contains(key)) {
						said.emplace_back(speaker, text);
					}
				}
			}
			shown = std::move(now);
			for (const auto& [who, text] : said) {
				Say(who.get(), text);
			}
		}

		// A fall as the client's server takes it (Player::HandleFall): from where the player left the ground to where the player lands,
		// in yards; 14.57 and more costs 0.018 x (drop - safe fall) - 0.2426 of their health (20 yards 12%, 69 all of it),
		// water none. Safe fall (SPELL_AURA_SAFE_FALL) is Feline Grace's 17 yards, in Cat Form only (its ShapeshiftMask).
		// Skyrim's own fall damage for the player is off (fJumpFallHeightMin): a drop from a roof had killed the player.
		RE::Setting* g_fallSetting = nullptr;
		std::optional<float> g_nativeFallHeight;
		bool g_fallAirborne = false;
		float g_fallFromZ = 0;
		void FallDamage(RE::PlayerCharacter* a_player)
		{
			if (!g_nativeFallHeight) {
				g_fallSetting = RE::GameSettingCollection::GetSingleton()->GetSetting("fJumpFallHeightMin");
				if (g_fallSetting) { g_nativeFallHeight = g_fallSetting->data.f; g_fallSetting->data.f = 1.0e9f; }
			}
			// Off the ground by the character controller's own state (a jump or a fall; IsInMidair misses a drop that
			// does not start with a jump).
			const auto* controller = a_player->GetCharController();
			const auto  state = controller ? controller->context.currentState : RE::hkpCharacterStateTypes::kOnGround;
			const bool  inAir = state == RE::hkpCharacterStateTypes::kJumping || state == RE::hkpCharacterStateTypes::kInAir;
			if (inAir && !g_fallAirborne) {
				g_fallFromZ = a_player->GetPositionZ();
			}
			if (!inAir && g_fallAirborne && !a_player->IsDead() && !a_player->AsActorState()->IsSwimming()) {
				const float drop = (g_fallFromZ - a_player->GetPositionZ()) / kUnitsPerYard;
				const float safeFall = Forms::Current() == "cat" && Progression::Knows(20719) ? 17.0f : 0.0f;
				const float share = drop >= 14.57f ? 0.018f * (drop - safeFall) - 0.2426f : 0.0f;
				if (share > 0.0f) {
					const float hpMax = Maximum(a_player, RE::ActorValue::kHealth);
					const float damage = std::min(share * hpMax, hpMax);
					a_player->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kHealth, damage);
					WowUi::Run(std::format("__WBLog('CHAT_MSG_COMBAT_SELF_HITS', 'VSENVIRONMENTALDAMAGE_FALLING_SELF', {})",
						static_cast<int>(std::lround(damage))));
					if (Current(a_player, RE::ActorValue::kHealth) <= 0.0f) {
						a_player->KillImmediate();
					}
				}
			}
			g_fallAirborne = inAir;
		}

		// Keyboard turning at the client's rate, half a turn a second.
		void Turn(RE::PlayerCharacter* a_player)
		{
			static double last = Frame::Now();
			const double  now = Frame::Now();
			const float   dt = static_cast<float>(std::min(now - last, 0.1));
			last = now;
			if (const int dir = Input::Turning()) {
				a_player->SetHeading(a_player->GetHeading(false) + static_cast<float>(dir) * RE::NI_PI * dt);
			}
		}



		// Mouselook: while the right button steers, the character turns to where the camera looks, as in WoW (Skyrim's
		// third person orbits the camera around a character standing still): the camera's free rotation becomes heading.
		void Mouselook(RE::PlayerCharacter* a_player)
		{
			auto* camera = RE::PlayerCamera::GetSingleton();
			auto* state = camera ? camera->currentState.get() : nullptr;
			if (!Input::SteeringHeld() || !state || state->id != RE::CameraState::kThirdPerson) {
				return;
			}
			auto* third = static_cast<RE::ThirdPersonState*>(state);
			if (third->freeRotation.x != 0.0f) {
				a_player->SetHeading(a_player->GetHeading(false) + third->freeRotation.x);
				third->freeRotation.x = 0.0f;
			}
		}

		// What a reference under the pointer offers, as the client's cursor says it: attack a hostile unit, speak to a
		// person, loot a corpse, pick up an item, gather a plant, use anything else that can be used; with Alt held, attack
		// anything the player may strike (the user's scheme: right click is Skyrim's E, Alt+right click attacks). "" for nothing.
		const char* Offer(RE::TESObjectREFR* a_ref, bool a_alt)
		{
			if (!a_ref || a_ref->IsDeleted() || a_ref->IsDisabled() || a_ref->IsPlayerRef()) return "";
			if (auto* actor = a_ref->As<RE::Actor>()) {
				if (actor->IsDead()) {
					if (a_ref->IsActivationBlocked()) return "";
					return "LOOT";
				}
				if (Reaction(actor) == 2 || (a_alt && Attackable(actor))) {
					return "ATTACK";
				}
				return !a_ref->IsActivationBlocked() && Reaction(actor) == 5 && actor->CanTalkToPlayer() ? "SPEAK" : "";   // a creature or a foe has nothing to say
			}
			auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
			if (!base || a_ref->IsActivationBlocked()) return "";
			switch (base->GetFormType()) {
			case RE::FormType::Door:
			case RE::FormType::Container:
			case RE::FormType::Activator:
			case RE::FormType::TalkingActivator:
			case RE::FormType::Furniture:
			{
				RE::BSString text;
				return base->GetActivateText(a_ref, text) && text.length() ? "INTERACT" : "";
			}
			case RE::FormType::Flora:
			case RE::FormType::Tree:
				return Produce(a_ref) && !(a_ref->formFlags & RE::TESObjectREFR::RecordFlags::kHarvested) ? "HERB" : "";
			case RE::FormType::Weapon:
			case RE::FormType::Armor:
			case RE::FormType::Misc:
			case RE::FormType::Book:
			case RE::FormType::Ingredient:
			case RE::FormType::AlchemyItem:
			case RE::FormType::Ammo:
			case RE::FormType::KeyMaster:
			case RE::FormType::Scroll:
			case RE::FormType::SoulGem:
				return base->GetPlayable() ? "PICKUP" : "";
			case RE::FormType::Light:
				return base->As<RE::TESObjectLIGH>()->data.flags.any(RE::TES_LIGHT_FLAGS::kCanCarry) ? "PICKUP" : "";
			default:
				return "";
			}
		}

		// Within reach of the point under the cursor: CMaNGOS's INTERACTION_DISTANCE, 5 yards.
		bool InReach(const Picker::Hit& a_hit)
		{
			return RE::PlayerCharacter::GetSingleton()->GetPosition().GetDistance(a_hit.pos) <= 5.0f * kUnitsPerYard;
		}

		// The unit under the pointer brightened, as the client brightens a model under the mouse (its hovered NPC is about
		// 1.7 times as bright, evidence/charref/wow_hover.png): Skyrim's own-emit light added to what lights its
		// geometry, put back as it was when the pointer leaves.
		RE::ActorHandle g_highlighted;
		constexpr float kHighlightEmit = 2.0f;

		struct Emit
		{
			RE::NiPointer<RE::BSLightingShaderProperty> property;
			bool                                        ownEmit;
			RE::NiColor                                 color;
			float                                       mult;
		};
		std::vector<Emit> g_emits;

		void Brighten(RE::Actor* a_actor, bool a_on)
		{
			if (!a_on) {
				for (auto& e : g_emits) {
					auto* p = e.property.get();
					if (e.ownEmit) {
						p->flags.set(RE::BSShaderProperty::EShaderPropertyFlag::kOwnEmit);
					} else {
						p->flags.reset(RE::BSShaderProperty::EShaderPropertyFlag::kOwnEmit);
					}
					*p->emissiveColor = e.color;
					p->emissiveMult = e.mult;
					p->lastRenderPassState = (std::numeric_limits<std::int32_t>::max)();
				}
				g_emits.clear();
				return;
			}
			auto* root = a_actor ? a_actor->Get3D(false) : nullptr;
			if (!root) {
				return;
			}
			RE::BSVisit::TraverseScenegraphGeometries(root, [](RE::BSGeometry* a_geo) {
				auto* p = netimmerse_cast<RE::BSLightingShaderProperty*>(a_geo->GetGeometryRuntimeData().shaderProperty.get());
				if (!p || !p->emissiveColor || std::ranges::any_of(g_emits, [&](const Emit& e) { return e.property.get() == p; })) {
					return RE::BSVisit::BSVisitControl::kContinue;
				}
				g_emits.push_back({ RE::NiPointer(p), p->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kOwnEmit), *p->emissiveColor, p->emissiveMult });
				p->flags.set(RE::BSShaderProperty::EShaderPropertyFlag::kOwnEmit);
				*p->emissiveColor = RE::NiColor(kHighlightEmit, kHighlightEmit, kHighlightEmit);
				p->emissiveMult = 1.0f;
				p->lastRenderPassState = (std::numeric_limits<std::int32_t>::max)();
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}

		void Highlight(RE::Actor* a_actor)
		{
			auto current = g_highlighted.get();
			if (current.get() != a_actor) {
				Brighten(nullptr, false);
				g_highlighted = a_actor ? a_actor->CreateRefHandle() : RE::ActorHandle{};
			}
			Brighten(a_actor, true);   // again each frame: its 3D may have been rebuilt (new geometry is taken in)
		}

		// What the pointer is over in the world sets the cursor, as the client's does (greyed out of reach; attack is not).
		void HoverCursor()
		{
			if (!Input::Active()) {
				Highlight(nullptr);
				g_hover = {};
				WowUi::SetWorldCursor("");
				return;
			}
			const auto [x, y] = Input::Cursor();
			const auto  hit = Picker::PickScreen(x, y);
			auto*       unit = hit.ref ? hit.ref->As<RE::Actor>() : nullptr;
			Highlight(unit && !unit->IsPlayerRef() ? unit : nullptr);
			g_hover = hit.ref && !hit.ref->IsPlayerRef() && !WowUi::PointerOverInterface() ? hit.ref->CreateRefHandle() : RE::ObjectRefHandle{};
			if (Cast::Placing()) {
				WowUi::SetWorldCursor("CAST_CURSOR");
				return;
			}
			const std::string offer = Offer(g_hover.get().get(), Input::AltHeld());
			std::string name;
			if (!offer.empty()) {
				name = offer + (offer == "ATTACK" || InReach(hit) ? "_CURSOR" : "_ERROR_CURSOR");
			}
			WowUi::SetWorldCursor(name);
		}

		// A right click in the world, as the client's: target what it is on, and use it when in reach - talk, loot, open,
		// take. Out of reach: "You are too far away."
		void Interact(const Picker::Hit& a_hit, bool a_alt)
		{
			auto*             player = RE::PlayerCharacter::GetSingleton();
			const std::string offer = Offer(a_hit.ref, a_alt);
			if (offer.empty()) {
				return;
			}
			auto* actor = a_hit.ref->As<RE::Actor>();
			if (actor) {
				SetTarget(actor);
			}
			if (offer == "ATTACK") {
				Melee::Start(actor);   // a right click on a hostile unit, or Alt+right click on anything, attacks it
				return;
			}
			if (!InReach(a_hit)) {
				WowUi::Run(std::format("__WBUiError({})", LuaQuote(offer == "LOOT" ? "ERR_LOOT_TOO_FAR" : "ERR_USE_TOO_FAR")));
				return;
			}
			if (offer == "SPEAK" && Taxi::Open(actor)) return;
			// As the Activate key does it: the crosshair's target is what the cursor is on, then the player activates it
			// (talking to an NPC starts there; a plain ActivateRef does not open dialogue).
			auto*      pick = RE::CrosshairPickData::GetSingleton();
			const auto handle = a_hit.ref->CreateRefHandle();
			pick->target = handle;
			pick->targetActor = actor ? handle : RE::ObjectRefHandle{};
			player->ActivatePickRef();
		}

		// Alt+left click (the user's scheme): take hold of a loose thing; it follows the pointer (HoldUnderPointer) until
		// any left click lets it fall. False: not a grab.
		RE::ObjectRefHandle             g_held;
		RE::NiPointer<RE::bhkRigidBody> g_heldBody;
        bool g_holdPlaneReady=false, g_holdVertical=false;
        RE::NiPoint3 g_holdAnchor, g_holdPlaneNormal, g_holdRayAnchor;
		bool Grab(const Picker::Hit& a_hit, bool a_alt)
		{
			if (g_held) {
				g_held = {}, g_heldBody.reset();
				return true;
			}
			if (!a_alt) {
				return false;
			}
			// Clutter (4), weapons (5), props (10) and large clutter (29): what Skyrim lets you pick up and move.
			constexpr int kLoose[] = { 4, 5, 10, 29 };
			if (!a_hit.ref || a_hit.ref->As<RE::Actor>() || !a_hit.body || std::ranges::find(kLoose, a_hit.layer) == std::end(kLoose)) {
				return true;
			}
			if (!InReach(a_hit)) {
				WowUi::Run(std::format("__WBUiError({})", LuaQuote("ERR_USE_TOO_FAR")));
				return true;
			}
			g_held = a_hit.ref->CreateRefHandle();
			g_heldBody.reset(a_hit.body);
            g_holdPlaneReady=false;
			return true;
		}

		// Held objects move horizontally at a fixed height; Alt switches to vertical movement at fixed XY.
        // Re-anchor on each mode switch so pressing Alt with a still pointer never moves the object. Its body is driven
		// there each frame (Skyrim's own grab aims at the camera's line and rewrites its springs every frame), so physics
		// still stops it at walls and shelves; it keeps its turn while held.
		constexpr float kHoldReach = 5.0f * kUnitsPerYard;
		void HoldUnderPointer(RE::PlayerCharacter* a_player)
		{
			const auto ref = g_held.get();
			if (!ref) {
				return;
			}
			if (ref->IsDisabled() || !ref->Is3DLoaded() || !Input::Active() || !g_heldBody) {
				g_held = {}, g_heldBody.reset();
				return;
			}
			const auto [x, y] = Input::Cursor();
			RE::NiPoint3 origin, dir;
			if (!Picker::ScreenRay(x, y, origin, dir)) {
				return;
			}
            const float scale=RE::bhkWorld::GetWorldScale();
            RE::hkVector4 com;
            g_heldBody->GetCenterOfMassWorld(com);
            float c[4]; _mm_storeu_ps(c,com.quad);
            const RE::NiPoint3 centre{c[0]/scale,c[1]/scale,c[2]/scale};
            const bool vertical=Input::AltHeld();
            const bool reset=!g_holdPlaneReady || vertical!=g_holdVertical;
            if (!g_holdPlaneReady) g_holdTarget=centre;
            if (reset) {
                g_holdAnchor=g_holdTarget;
                g_holdPlaneNormal=vertical ? RE::NiPoint3{dir.x,dir.y,0} : RE::NiPoint3{0,0,1};
                g_holdPlaneNormal.Unitize();
            }
            const auto dot=[](const RE::NiPoint3& a,const RE::NiPoint3& b) { return a.x*b.x+a.y*b.y+a.z*b.z; };
            const float denominator=dot(dir,g_holdPlaneNormal);
            if (std::abs(denominator)<.02f) return; // near-parallel rays have no stable plane intersection
            const float distance=dot(g_holdAnchor-origin,g_holdPlaneNormal)/denominator;
            if (distance<0) return;
            const auto point=origin+dir*distance;
            if (reset) { g_holdRayAnchor=point; g_holdVertical=vertical; g_holdPlaneReady=true; }
            auto to=g_holdAnchor+(point-g_holdRayAnchor);
            const auto chest=a_player->GetPosition()+RE::NiPoint3{0,0,100};
            if (vertical) {
                to.x=g_holdAnchor.x; to.y=g_holdAnchor.y;
                const float dx=to.x-chest.x,dy=to.y-chest.y;
                const float extent=std::sqrt(std::max(0.f,kHoldReach*kHoldReach-dx*dx-dy*dy));
                to.z=std::clamp(to.z,chest.z-extent,chest.z+extent);
            } else {
                to.z=g_holdAnchor.z;
                const float dz=to.z-chest.z;
                const float extent=std::sqrt(std::max(0.f,kHoldReach*kHoldReach-dz*dz));
                auto off=to-chest; off.z=0;
                if (off.Length()>extent) { off*=extent/off.Length(); to.x=chest.x+off.x; to.y=chest.y+off.y; }
            }
			auto vel = (to - RE::NiPoint3{ c[0] / scale, c[1] / scale, c[2] / scale }) * 10.0f;   // most of the way in a tenth of a second
			if (const float v = vel.Length(); v > 1500.0f) {
				vel *= 1500.0f / v;
			}
			if (auto* body = static_cast<RE::hkpEntity*>(g_heldBody->referencedObject.get())) {
				static REL::Relocation<void (*)(RE::hkpEntity*)> activate{ RELOCATION_ID(60096, 60849) };   // hkpEntity::activate
				activate(body);   // a resting body sleeps and would not move
			}
			g_heldBody->SetLinearVelocity(RE::hkVector4(vel.x * scale, vel.y * scale, vel.z * scale, 0.0f));
			g_heldBody->SetAngularVelocity(RE::hkVector4(0.0f, 0.0f, 0.0f, 0.0f));
			g_holdTarget = to;
		}

		void HandleClicks()
		{
			for (const auto& click : WowUi::TakeWorldClicks()) {
				if (WowUi::Eval(std::format("__WBBagWorldClick({})",click.button))=="1") continue;
				const auto hit = Picker::PickScreen(click.x, click.y);
				if (Cast::Placing()) {   // a ground spell: left places it on the ground under the pointer, right cancels
					if (click.button == 1) {
						Cast::CancelPlacing();
					} else if (const auto ground = Picker::PickScreen(click.x, click.y, 8000.0f, true); ground.hit) {
						Cast::Place(ground.pos);
					}
					continue;
				}
				if (click.button == 1) {
					logger::info("bridge: right click at {:.0f},{:.0f} -> {} ({}) {:.0f} units", click.x, click.y,
						hit.ref ? hit.ref->GetDisplayFullName() : "nothing", Offer(hit.ref, click.alt),
						hit.hit ? RE::PlayerCharacter::GetSingleton()->GetPosition().GetDistance(hit.pos) : -1.0f);
					Interact(hit, click.alt);
					continue;
				}
				if (Grab(hit, click.alt)) {
					continue;
				}
				auto* actor = hit.ref ? hit.ref->As<RE::Actor>() : nullptr;
				SetTarget(actor);   // empty ground clears the target, as in WoW
			}
		}

		// Skyrim HUD elements the WoW HUD replaces (docs: plan "Skyrim's native health/magicka/stamina bars, crosshair,
		// activation prompt and enemy bar are hidden"), by their instance names in HUDMovieBaseInstance. The minimap
		// now replaces compass POIs. SkyrimSpells::Cooldowns publishes GetVoiceRecoveryTime to their WoW bars;
		// messages, subtitles, quest updates and the sneak eye stay (they are separate root children).
		// Re-applied every frame: the HUD's own scripts show some of them again (the crosshair on every activation).
		constexpr const char* kHidden[] = {
			"Compass", "CompassShoutMeter",
			"Health", "Magica", "Stamina", "HealthMeterAnim", "MagickaMeterAnim", "StaminaMeterAnim",
			"Crosshair", "CrosshairInstance", "CrosshairAlert",
			"RolloverNameInstance", "RolloverInfoInstance", "RolloverText", "RolloverInfoText", "RolloverGrayBar_mc",
			"RolloverButton_tf", "ActivateButton_tf", "GrayBarInstance",
			"EnemyHealth_mc", "LeftChargeMeterAnim", "RightChargeMeterAnim", "TemperatureMeter_mc"
		};

		RE::GFxMovieView* g_hiddenHud = nullptr;
		std::map<std::string, bool> g_nativeHud; // UI-thread only
		void HideSkyrimHud()
		{
			SKSE::GetTaskInterface()->AddUITask([] {
				if (!Integration::Active()) return;
				auto* ui = RE::UI::GetSingleton();
				auto  hud = ui ? ui->GetMenu<RE::HUDMenu>() : nullptr;
				RE::GFxValue root;
				if (!hud || !hud->uiMovie || !hud->uiMovie->GetVariable(&root, "_root.HUDMovieBaseInstance") || !root.IsObject()) {
					return;
				}
				if (g_hiddenHud != hud->uiMovie.get()) { g_nativeHud.clear(); g_hiddenHud = hud->uiMovie.get(); }
				for (const char* name : kHidden) {
					RE::GFxValue mc;
					if (root.GetMember(name, &mc) && mc.IsDisplayObject()) {
						RE::GFxValue was;
						if (!g_nativeHud.contains(name) && mc.GetMember("_visible", &was) && was.IsBool()) g_nativeHud[name] = was.GetBool();
						mc.SetMember("_visible", RE::GFxValue(false));
					}
				}
			});
		}
	}

	void IntegrationChanged(bool active)
	{
		if (active) {
			// Their Show Helm / Show Cloak again: their interface may have started, and sent them, while it was off.
			WowUi::Run("if __WBShowGear then __WBShowGear() end if __WBAreaFriendly then __WBAreaFriendly() end");
			return;
		}
		CompassMarkers::Update(false);
		if (g_fallSetting && g_nativeFallHeight) g_fallSetting->data.f = *g_nativeFallHeight;
		g_nativeFallHeight.reset();
		g_fallAirborne = false;
		// Guard queued UI tasks: a native save can replace a WoW save in the same process.
		SKSE::GetTaskInterface()->AddUITask([] {
			if (Integration::Active()) return;
			auto* ui = RE::UI::GetSingleton();
			auto hud = ui ? ui->GetMenu<RE::HUDMenu>() : nullptr;
			RE::GFxValue root;
			if (hud && hud->uiMovie.get() == g_hiddenHud && hud->uiMovie->GetVariable(&root, "_root.HUDMovieBaseInstance")) {
				for (const auto& [name, visible] : g_nativeHud) {
					RE::GFxValue mc;
					if (root.GetMember(name.c_str(), &mc) && mc.IsDisplayObject()) mc.SetMember("_visible", RE::GFxValue(visible));
				}
			}
			g_nativeHud.clear(); g_hiddenHud = nullptr;
		});
	}

	const char* WorldOffer(RE::TESObjectREFR* a_ref, bool a_alt) { return Offer(a_ref, a_alt); }
	std::string WorldActivationHint(RE::TESObjectREFR* a_ref) { return ActivationHint(a_ref); }
	RE::Actor* Highlighted() { return g_highlighted.get().get(); }

	int Reaction(RE::Actor* a_actor)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (a_actor->IsHostileToActor(player)) {
			return 2;
		}
		return a_actor->IsPlayerTeammate() || a_actor->HasKeywordString("ActorTypeNPC") ? 5 : 4;
	}

	bool Attackable(RE::Actor* a_actor) { return a_actor && a_actor != RE::PlayerCharacter::GetSingleton() && !Cast::Unattackable(a_actor); }
	void OpenSkyrimMenu(const std::string& a_name) { OpenMenu(a_name); }
	RE::NiPoint3 HoldTarget() { return g_holdTarget; }
	bool         Holding() { return static_cast<bool>(g_held); }
	const char* CreatureTypeName(RE::Actor* a_actor)
	{
		// Vampires and Dremora can also be Skyrim NPCs; the Paladin's creature gates need their Undead/Demon type.
		if (a_actor && Character::Get().classId == 2) {
			if (a_actor->HasKeywordString("ActorTypeUndead")) return "Undead";
			if (a_actor->HasKeywordString("ActorTypeDaedra")) return "Demon";
		}
		return CreatureType(a_actor);
	}

	bool Healable(RE::Actor* a_actor) { return a_actor && Reaction(a_actor) != 2; }

	const char* ReactionName(RE::Actor* a_actor)
	{
		const int r = Reaction(a_actor);
		return r == 2 ? "hostile" : r == 5 ? "friendly" : "neutral";
	}

	void ForgetLevel()
	{
		g_levelShown = 0;
		g_party.clear(); g_followers.clear(); g_partyAuras.clear(); g_partyNext = 0;
		g_last.clear();
		for (std::size_t i = 1; i <= 4; ++i) Portrait::Capture(nullptr, i);
	}

	void ForgetNameplates()
	{
		++g_plateEpoch;
		g_plateActors.clear();
		g_last.clear();
		// Run tasks follow the latest snapshot on the render thread. A delayed
		// invalidation must not clear a newer world's already published actors.
		WowUi::Run(std::format("if not __WB.nameplates or (__WB.nameplates.epoch or 0)<{0} then __WBNameplates({{epoch={0},units={{}}}}) end "
			"if __WBClearChatBubbles then __WBClearChatBubbles() end", g_plateEpoch));
	}

	void Say(RE::TESObjectREFR* a_speaker, std::string_view a_text)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!a_speaker || !player || a_text.empty()) return;
		WowUi::Run(std::format("__WBSay({}, {}, {}, {:.2f})", LuaQuote(a_speaker->GetDisplayFullName()), LuaQuote(a_text),
			a_speaker->GetFormID(), player->GetPosition().GetDistance(a_speaker->GetPosition()) / kUnitsPerYard));
	}

	RE::Actor* PartyMember(std::size_t a_index)
	{
		auto* actor = a_index < g_party.size() ? g_party[a_index].get().get() : nullptr;
		return Follower(actor) ? actor : nullptr;
	}

	RE::Actor* Target()
	{
		return g_targetSelf ? RE::PlayerCharacter::GetSingleton() : g_target.get().get();
	}

	void SetTarget(RE::Actor* a_actor)
	{
		const auto before = Target();
		g_targetSelf = a_actor && a_actor->IsPlayerRef();
		g_target = a_actor && !g_targetSelf ? a_actor->CreateRefHandle() : RE::ActorHandle{};
		if (Target() != before) {
			Portrait::Capture(g_targetSelf ? nullptr : a_actor);   // the face for the target frame (theirs is drawn already)
			// Auto-attack follows a new hostile target, as the client's does; anything else stops it.
			if (Melee::Attacking()) {
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (a_actor && !g_targetSelf && !a_actor->IsDead() && a_actor->IsHostileToActor(player)) {
					Melee::Start(a_actor);
				} else {
					Melee::Stop();
				}
			}
		}
	}

    void StartNPCScanCost(const nlohmann::json& site)
    {
        g_scanSite=site;
        if (!g_scanSite.contains("position")) {
            // The original Whiterun cow grid remains the explicit missing-reference fallback.
            g_scanSite["world"]=0x1A26F;
            g_scanSite["position"]={26624.f,-6144.f,-3392.f};
        }
        g_scanActive=true; g_scanMeasuring=false; g_scanValid=true;
        g_scanFrames=0; g_scanSum=0; g_scanMax=0; g_scanActorSum=0;
        g_scanActorMin=SIZE_MAX; g_scanActorMax=0;
        g_scanIssued=std::chrono::steady_clock::now();
        g_scanResult={{"active",true},{"done",false},{"site",g_scanSite},{"budget_mean_ms",kScanBudgetMs}};
    }

    nlohmann::json NPCScanCost() { return g_scanResult; }
    void CancelNPCScanCost() { if (g_scanActive) FinishNPCScanCost("cancelled (load/new game or explicit stop)"); }

	void Tick()
	{
		const bool wowReady=Integration::Active() && Config::Get().wowUi && WowUi::Ready();
		CompassMarkers::Update(wowReady && WowUi::Eval("__WB.nativeCompassReady and 1 or 0")=="1");
		if (!wowReady) {
			return;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->Is3DLoaded()) {
			return;
		}
		if (auto* ui = RE::UI::GetSingleton(); !ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) return;
		const bool scanFrame=g_scanActive;
        const auto scanStart=scanFrame ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (scanFrame) g_scanActors.clear();
        const auto nameplates = NameplatePart(player); // refresh click eligibility before draining UI commands
        const double plateMs=scanFrame ? std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-scanStart).count() : 0;
		RefreshParty();
		// WoW cursor control in play; a self-test run drives cursor mode itself.
		if (!Input::CursorMode() && Config::Get().cursor && !Config::Get().autorun) {
			Input::SetCursorMode(true);
		}
		HandleClicks();
		HoverCursor();
		for (auto& c : WowUi::TakeCommands()) {   // what the interface asked of the game
			auto arg = [&](std::size_t i) { return i < c.args.size() ? c.args[i] : std::string(); };
			if (c.name == "useAction" || c.name == "castSpell") {
				// useAction(slot, spell, onSelf) / castSpell(spell)
				const auto id = std::strtoll(arg(c.name == "useAction" ? 1 : 0).c_str(), nullptr, 10);
				const auto spell = static_cast<std::uint32_t>(std::max<long long>(id, 0));
				if (SkyrimSpells::Use(id)) {   // their Skyrim spells, shouts and powers (below 0)
				} else if (spell == 6603) {
					Melee::Toggle(Target());   // the Attack action
				} else if (spell) {
					Cast::Use(spell);
				}
			} else if (c.name == "castSpellByName") {
				SkyrimSpells::UseByName(arg(0));
			} else if (c.name == "nameplateTarget") {
				const auto id = static_cast<RE::FormID>(std::strtoul(arg(0).c_str(), nullptr, 10));
				const auto epoch = static_cast<std::uint32_t>(std::strtoul(arg(1).c_str(), nullptr, 10));
				if (epoch == g_plateEpoch) if (const auto found = g_plateActors.find(id); found != g_plateActors.end()) {
					if (const auto actor = found->second.get(); actor && !actor->IsDead() && !actor->IsDisabled() && !actor->IsDeleted()) SetTarget(actor.get());
				}
			} else if (c.name == "cancelAura") {   // a right click on one of their buffs: a helpful one comes off
				const auto id = static_cast<std::uint32_t>(std::strtoul(arg(0).c_str(), nullptr, 10));
				if (arg(1) == "1") {
					SkyrimAuras::Cancel(id);
				} else {
					Cast::CancelAura(id);
				}
			} else if (c.name == "shapeshift") {   // the shapeshift bar: a form (its spell, which also leaves it), 0 back to the player
				const int  index = std::atoi(arg(0).c_str());
				const auto& current = Forms::Current();
				const std::uint32_t now = current == "dire_bear" ? 9634 : current == "bear" ? 5487 : current == "aquatic" ? 1066 :
				                          current == "cat" ? 768 : current == "travel" ? 783 : current == "moonkin" ? 24858 : 0;
				const auto selected = static_cast<std::uint32_t>(std::strtoul(arg(1).c_str(), nullptr, 10));
				if (const auto spell = index > 0 ? selected : now; spell && Progression::Knows(spell)) {
					Cast::Use(spell);
				}
			} else if (c.name == "learnTalent") {
				Progression::LearnTalent(std::atoi(arg(0).c_str()), std::atoi(arg(1).c_str()));
			} else if (c.name == "requestTalents") {
				Progression::RefreshTalents();
			} else if (c.name == "talentWipe") {   // CONFIRM_TALENT_WIPE accepted: its offer's session and the price shown
				Progression::ConfirmTalentWipe(static_cast<std::uint32_t>(std::strtoul(arg(0).c_str(), nullptr, 10)), std::atoi(arg(1).c_str()));
			} else if (c.name == "levelUp") {
				SpellVisuals::Play(SpellVisuals::kLevelUp, SpellVisuals::Kit::Impact, {}, false);
			} else if (c.name == "stopCasting") {
				if (Mounts::Pending()) Mounts::Dismount();
				Cast::Stop();
			} else if (c.name == "stopTargeting") {
				Cast::CancelPlacing();
			} else if (c.name == "emote") {
				Emote(player, arg(0));
			} else if (c.name == "areaFriendly") {   // Interface Options' Area Spells Hit Friendly
				Cast::SetAreaFriendly(arg(0) == "1");
			} else if (c.name == "showGear") {   // Show Helm, Show Cloak (Interface Options): 1 shown, 0 hidden
				Character::SetShown(arg(0) != "0", arg(1) != "0");
			} else if (c.name == "appearance") {   // the customization window's Accept (race, sex, skin, face, hair style and colour,
				const auto n = [&](std::size_t i) { return static_cast<std::uint32_t>(std::strtoul(arg(i).c_str(), nullptr, 10)); };
				if (c.args.empty()) {          // facial hair), or its Cancel (nothing)
					Character::KeepLooks();
				} else if (c.args.size() != 7 || !Character::SetLooks({ n(0), n(1), n(2), n(3), n(4), n(5), n(6) })) {
					logger::warn("bridge: appearance {} {} {} {} {} {} {} refused", n(0), n(1), n(2), n(3), n(4), n(5), n(6));
				}
			} else if (c.name == "chat") {   // SendChatMessage: what the player says, yells or emotes, as the server echoes it back
				std::string type = arg(1).empty() ? "SAY" : arg(1);
				std::ranges::transform(type, type.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
				if ((type == "SAY" || type == "YELL" || type == "EMOTE") && !arg(0).empty()) {
					WowUi::Run(std::format("__WBFire({},{},{},GetDefaultLanguage('player'))", LuaQuote("CHAT_MSG_" + type), LuaQuote(arg(0)),
						LuaQuote(Integration::PlayerName())));
					if (type != "EMOTE") Reactions::OnChat(player, Target(), arg(0));   // a spoken greeting is answered
				}
			} else if (c.name == "testCommand") {
				Console::Command(arg(0),arg(1),arg(2));
			} else if (c.name == "menu") {
				OpenMenu(arg(0));
			} else if (c.name == "command" && !c.args.empty()) {
				RunCommand(c.args[0], { c.args.begin() + 1, c.args.end() });
			}
		}
		Turn(player);
		Mouselook(player);
		Input::UpdateRunByButtons();
		HoldUnderPointer(player);
		FallDamage(player);
		EmoteEnds(player);
		Reactions::Tick();
		Subtitles();
		if (static bool held = false; static_cast<bool>(g_held) != held) {
			held = !held;
			logger::info("bridge: {} hold of a loose thing", held ? "took" : "let go of");
		}
		Mounts::Tick();
		Cast::Tick();
		Melee::Tick();
		SelectionCircle(player);
		Portrait::Update();
		Avatar::Tick();
		HideSkyrimHud();
        const auto mapStart=scanFrame ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const auto minimap=MinimapPart(player);
        if (scanFrame && g_scanActive) SampleNPCScanCost(player,plateMs+std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-mapStart).count());
		std::string snapshot = "{ " + PlayerPart(player) + ", " + PartyPart(player) + ", " + TargetPart(player) + ", " + MouseoverPart(player) + ", " + WorldPart(player) + ", " + minimap + ", " + WorldMapPart(player) + ", " + nameplates + ", " + HeadPart(player) + ", comboPoints=" + std::to_string(Forms::ComboPoints()) + " }";
		if (snapshot != g_last) {
			g_last = snapshot;
			WowUi::Publish(std::move(snapshot));
		}
	}
}
