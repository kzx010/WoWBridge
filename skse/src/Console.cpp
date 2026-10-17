#include "Mounts.h"
#include "Reactions.h"
#include "Console.h"
#include "SelfTest.h"
#include "Integration.h"

#include "Armory.h"
#include "Distribution.h"
#include "Gear.h"
#include "render/NpcShoulders.h"
#include "render/NpcWeapons.h"
#include "render/NpcHelms.h"
#include "render/PlayerWeapons.h"
#include "render/Shadows.h"

#include "Cast.h"
#include "Melee.h"
#include "Carriers.h"
#include "Bags.h"
#include "Bridge.h"
#include "LuaText.h"
#include "ui/WowUi.h"
#include "Character.h"
#include "Forms.h"
#include "Learning.h"
#include "Progression.h"
#include "Service.h"
#include "SkyrimAuras.h"
#include "Surrogate.h"
#include "Loot.h"
#include "Merchant.h"
#include "TalentWipe.h"

#include <charconv>

// Diagnostic API kept in the allowed implementation files. Main/game thread only.
namespace Bridge { void StartNPCScanCost(const nlohmann::json& site); nlohmann::json NPCScanCost(); void CancelNPCScanCost(); }

namespace Console
{
	namespace
	{
		using json = nlohmann::json;
		json g_result = json::object();
		std::uint64_t g_calls = 0, g_formRequest = 0;
        struct LevelRequest {
            std::uint64_t call;
            std::string attribute;
            int count, completed=0, stage=0, level=0, perks=0;
            float base=0, carry=0, excess=0;
            std::chrono::steady_clock::time_point began=std::chrono::steady_clock::now(), changed=began;
            json evidence=json::array();
            bool hasIteration=false;
            RE::PlayerCharacter::PlayerSkills::Data iterationSkills{};
            std::array<float,4> bases{}, permanent{}, damage{};
        };
        std::optional<LevelRequest> g_levelRequest;
        std::atomic<bool> g_levelActive=false;
        std::mutex g_levelLock;
		std::vector<RE::ActorHandle> g_spawned;
        std::vector<RE::ObjectRefHandle> g_objects;
        RE::ActorHandle g_vendor;
        RE::ObjectRefHandle g_vendorChest, g_lootChest;
        RE::TESFaction* g_vendorFaction = nullptr;
        RE::TESObjectREFR* g_lentChest = nullptr;   // Lucan's merchant chest, back in his faction when the test merchant goes
        // Verified native items: weapon, armor, potion, food, ingredient, book, note, key,
        // empty/filled soul gems, and an intentionally unnamed inventory record.
        constexpr RE::FormID kStock[]{0x12EB7,0x12E49,0x3EADD,0x3133B,0x77E1C,0x1AFCA,
            0xF798C,0x10BEFF,0x2E4E2,0x2E4E3,0x104B40};
        json Classes()
        {
            std::ifstream in("Data/SKSE/Plugins/WoWBridge/progression.json");
            if (!in) throw std::runtime_error("packaged progression.json unavailable");
            const auto data=json::parse(in);
            const auto& curve=data.at("curve");
            const double lo=curve.at("skill_min"), hi=curve.at("skill_max"),
                first=curve.at("level_min"), last=curve.at("level_max");
            if (last<=first || hi<=lo) throw std::runtime_error("invalid progression curve");
            json classes=json::array();
            for (const auto& [key,definition] : data.at("classes").items()) {
                json rows=json::array(); std::set<int> seen;
                for (const auto* group : {"spells","excluded"}) for (auto spell : definition.at(group)) {
                    const int id=spell.at("spell"); if (!seen.insert(id).second) continue;
                    const int tab=spell.value("tab",0), line=spell.value("skill_line",0);
                    const auto it=definition.at("lines").find(std::to_string(line));
                    spell["tab_name"]=it==definition.at("lines").end() ? "General" : it->value("name","General");
                    spell["tab"]=tab;
                    spell["known"]=definition.at("id")==Character::Get().classId && Progression::Knows(id);
                    const double level=spell.value("required_level",1);
                    spell["learn_skill_value"]=static_cast<int>(std::ceil(std::clamp(lo+(level-first)*(hi-lo)/(last-first),lo,hi)));
                    if (spell.value("required_spell",0)) spell["learn_requirements"]=std::format("Requires spell {}.",spell.at("required_spell").get<int>());
                    // Trainer alternatives are large and unnecessary for this browser.
                    spell.erase("trainer_options"); rows.push_back(std::move(spell));
                }
                classes.push_back({{"key",key},{"name",definition.at("name")},{"id",definition.at("id")},{"spells",std::move(rows)}});
            }
            return classes;
        }
        RE::TESObjectREFR* PlaceNearby(RE::TESBoundObject* base, float distance=150.f)
        {
            auto* player=RE::PlayerCharacter::GetSingleton();
            const auto ref=base ? player->PlaceObjectAtMe(base,false) : RE::NiPointer<RE::TESObjectREFR>{};
            if (!ref) return nullptr;
            const float h=player->GetHeading(false);
            ref->SetPosition(player->GetPosition()+RE::NiPoint3{std::sin(h)*distance,std::cos(h)*distance,0});
            return ref.get();
        }
        bool Stock(RE::TESObjectREFR* chest, bool merchant)
        {
            std::vector<RE::TESBoundObject*> items;
            for (auto id : kStock) {
                auto* item=RE::TESForm::LookupByID<RE::TESBoundObject>(id);
                if (!item) return false;
                items.push_back(item);
            }
            for (auto* item : items) chest->AddObjectToContainer(item,nullptr,merchant ? 5 : 3,nullptr);
            if (merchant) {
                chest->AddObjectToContainer(RE::TESForm::LookupByID<RE::TESBoundObject>(0xF),nullptr,10000,nullptr);
                // Spell books to buy (Moonfire I, Wrath II and VIII, the book-only Healing Touch XI, the made-up Cat
                // Form) and a ground and a flying mount's item.
                for (const auto spell : {8921u,5177u,9912u,25297u,768u}) if (auto* book=Learning::Book(spell)) chest->AddObjectToContainer(book,nullptr,2,nullptr);
                for (const auto* key : {"swift_frostsaber","swift_golden_gryphon"}) if (auto* item=Learning::MountItem(key)) chest->AddObjectToContainer(item,nullptr,1,nullptr);
            }
            return true;
        }
        void RemoveVendor()
        {
            Merchant::Reset();
            RE::UIMessageQueue::GetSingleton()->AddMessage(RE::BarterMenu::MENU_NAME,RE::UI_MESSAGE_TYPE::kHide,nullptr);
            // Disabled, never deleted: deleting a freshly spawned actor near a cell load crashed (handoff known limits).
            if (auto actor=g_vendor.get()) { actor->StopCombat(); actor->Disable(); }
            if (auto chest=g_vendorChest.get()) { chest->Disable(); chest->SetDelete(true); }
            if (g_vendorFaction && g_lentChest) g_vendorFaction->vendorData.merchantContainer=g_lentChest;
            g_lentChest=nullptr;
            g_vendor.reset(); g_vendorChest.reset();
        }
        void RemoveLootChest()
        {
            Loot::Reset();
            if (auto chest=g_lootChest.get()) { chest->Disable(); chest->SetDelete(true); }
            g_lootChest.reset();
        }
		struct Place { const char* command; const char* name; const char* hint; RE::FormID reference; const char* plugin = "Skyrim.esm"; };
		// Live REFRs verified in the shipped masters: city activity markers move with Open Cities.
		// Interior door references stay in their interiors. Commands are logged fallbacks only.
		// Keep console input on this allowlist; resolve plugin-local IDs without load-order prefixes.
		const std::map<std::string,Place> kPlaces{
			{"riverwood",{"coc Riverwood","Riverwood","Town and nearby wildlife",0x162A4}},
			{"whiterun",{"cow WhiterunWorld 6 -2","Whiterun","City services",0x2BE1B}},
			{"solitude",{"cow SolitudeWorld -13 25","Solitude","City services",0x4DEB7}},
			{"windhelm",{"cow WindhelmWorld 33 9","Windhelm","City services",0x2A975}},
			{"riften",{"cow RiftenWorld 42 -24","Riften","City services",0x44346}},
			{"markarth",{"cow MarkarthWorld -43 1","Markarth","City services",0x1B38D}},
			{"dawnstar",{"cow Tamriel 6 24","Dawnstar","Coast and town",0x1773A}},
			{"blackreach",{"cow Blackreach 0 0","Blackreach","Underground worldspace",0xEF7CD}},
			{"qasmoke",{"coc QASmoke","QASmoke","Indoor item testing",0x32AFB}},
			{"barrow",{"coc BleakFallsBarrow01","Bleak Falls Barrow","Rooms, line of sight and dungeon combat",0x37603}},
			{"lake",{"cow Tamriel -8 -12","Lake Ilinalta","Open water by Ilinalta's Deep; Aquatic Form",0x1626A}},
			{"flight",{"coc HighHrothgarExterior01","High Hrothgar exterior","Mountain flight and outdoor mounts",0x16352}},
			{"shrine",{"coc WhiterunTempleofKynareth","Temple of Kynareth","Activate the shrine to refund talents",0x166C7}},
			{"merchant",{"coc RiverwoodRiverwoodTrader","Riverwood Trader","Lucan; merchant and inventory checks",0x133E9}},
			{"crafting",{"coc WhiterunWarmaidens","Warmaidens","Native smithing/enchanting checks; forge outside",0x1DB63}},
            {"stables",{"coc WhiterunStables","Whiterun stables","Exit to Bjorlam and the carriage/taxi map",0x16729}},
			{"trainer",{"coc RiverwoodFaendalsHouse","Faendal's house","Archery trainer; may be outside during the day",0x133DA}},
			{"bandits",{"coc ValtheimKeepExterior01","Valtheim Towers","Bandits and outdoor combat",0x162C9}},
			{"dragon",{"coc DragonLair04Exterior01","Dragon lair","Dragon presence depends on quest and respawn state",0x16233}},
            {"ravenrock",{"cow DLC2SolstheimWorld 7 8","Raven Rock","Solstheim town",0x143DC,"Dragonborn.esm"}}
		};
        json Travel(const Place& place)
        {
            auto* data=RE::TESDataHandler::GetSingleton();
            auto* ref=data ? data->LookupForm<RE::TESObjectREFR>(place.reference,place.plugin) : nullptr;
            // A persistent reference nothing has loaded yet has no parent cell: its cell is the save parent.
            auto* cell=ref ? ref->GetSaveParentCell() : nullptr;
            auto* world=cell && !cell->IsInteriorCell() ? cell->GetRuntimeData().worldSpace : nullptr;
            const auto pos=ref ? ref->GetPosition() : RE::NiPoint3{};
            if (ref && !ref->IsDeleted() && !ref->IsDisabled() && cell && (cell->IsInteriorCell() || world) &&
                std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z)) {
                auto* terrain=world;
                for (int depth=0; terrain && terrain->parentWorld && depth<16 &&
                    terrain->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseLODData); ++depth) terrain=terrain->parentWorld;
                json result={{"source","live_reference"},{"reference",ref->GetFormID()},{"plugin",place.plugin},
                    {"cell",cell->GetFormID()},{"world",world ? world->GetFormID() : 0},
                    {"terrain_world",terrain ? terrain->GetFormEditorID() : ""},{"position",{pos.x,pos.y,pos.z}}};
                Print(std::format("Travel {}: live reference {:08X}, cell {:08X}, world {:08X}, position {:.1f} {:.1f} {:.1f}",
                    place.name,ref->GetFormID(),cell->GetFormID(),world ? world->GetFormID() : 0,pos.x,pos.y,pos.z));
                Execute(std::format("player.moveto {:X}",ref->GetFormID()));   // the engine attaches the reference's cell itself
                return result;
            }
            Print(std::format("Travel {}: fallback '{}' ({}:{:06X} {})",place.name,place.command,place.plugin,place.reference,
                !ref ? "not found" : ref->IsDeleted() ? "deleted" : ref->IsDisabled() ? "disabled" : !cell ? "without a parent cell" :
                !world ? "exterior without a worldspace" : "without a finite position"));
            Execute(place.command);
            return {{"source","fallback"},{"command",place.command},{"reference",place.reference},{"plugin",place.plugin}};
        }
		// Skyrim.esm NPC_ bases: EncWolf, EncChicken, EncBandit01Melee1HNordM, EncBear,
		// EncSkeever, EncMudcrabGiant and WEFarmerMale (a generic, unaggressive farmer).
		const std::map<std::string,RE::FormID> kSpawns{{"wolf",0x23ABE},{"dummy",0x23ABE},{"chicken",0xA91A0},
			{"bandit",0x1BCD8},{"bear",0x23A8A},{"skeever",0x23AB7},{"mudcrab",0x21875},{"townsperson",0x1034E4}};
		constexpr RE::FormID kCrimeFactions[]{0x267EA,0x29DB0,0x267E3,0x2816B,0x2816C,0x2816D,0x2816E,0x2816F,0x28170};

		struct Skill { const char* name; RE::ActorValue value; };
		constexpr Skill kSkills[] = {
			{ "onehanded", RE::ActorValue::kOneHanded }, { "twohanded", RE::ActorValue::kTwoHanded },
			{ "marksman", RE::ActorValue::kArchery }, { "block", RE::ActorValue::kBlock },
			{ "smithing", RE::ActorValue::kSmithing }, { "heavyarmor", RE::ActorValue::kHeavyArmor },
			{ "lightarmor", RE::ActorValue::kLightArmor }, { "pickpocket", RE::ActorValue::kPickpocket },
			{ "lockpicking", RE::ActorValue::kLockpicking }, { "sneak", RE::ActorValue::kSneak },
			{ "alchemy", RE::ActorValue::kAlchemy }, { "speechcraft", RE::ActorValue::kSpeech },
			{ "alteration", RE::ActorValue::kAlteration }, { "conjuration", RE::ActorValue::kConjuration },
			{ "destruction", RE::ActorValue::kDestruction }, { "illusion", RE::ActorValue::kIllusion },
			{ "restoration", RE::ActorValue::kRestoration }, { "enchanting", RE::ActorValue::kEnchanting }
		};

		std::string Lower(std::string a_text)
		{
			for (auto& c : a_text) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
			return a_text;
		}

		bool Number(std::string_view a_text, std::uint32_t& a_value, std::uint32_t a_min, std::uint32_t a_max)
		{
			const auto [end, error] = std::from_chars(a_text.data(), a_text.data() + a_text.size(), a_value);
			return error == std::errc{} && end == a_text.data() + a_text.size() && a_value >= a_min && a_value <= a_max;
		}

		// A WoW item by its entry or its name (any case; else the one name holding the text), WoWBridge.esp's or the
		// Armory's: its entry, or 0 with why not.
		std::uint32_t ItemNamed(std::string_view a_text, std::string& a_why)
		{
			std::uint32_t entry = 0;
			if (Number(a_text, entry, 1, UINT32_MAX)) {
				if (Gear::FormOf(entry)) return entry;
				a_why = std::format("no WoW item {} in WoWBridge.esp or WoWBridgeArmory.esp", entry);
				return 0;
			}
			std::vector<std::pair<std::uint32_t, std::string>> named;
			for (const auto& [id, item] : Character::Gear()) {
				if (auto* form = Gear::FormOf(id); form && form->GetName()) named.emplace_back(id, form->GetName());
			}
			for (const auto& item : Armory::Items()) named.emplace_back(item.entry, item.name);
			std::vector<std::pair<std::uint32_t, std::string>> partial;
			for (const auto& [id, name] : named) {
				const auto lower = Lower(name);
				if (lower == a_text) return id;
				if (lower.find(a_text) != std::string::npos) partial.emplace_back(id, name);
			}
			if (partial.size() == 1) return partial.front().first;
			a_why = partial.empty() ? std::format("no WoW item named {}", a_text) :
				std::format("{} items match {} (first: {} [{}]); wb find TEXT lists them", partial.size(), a_text, partial.front().second, partial.front().first);
			return 0;
		}

		// Give a WoW item, or equip it (given if the player has none); the message names it.
		std::string GiveItem(std::uint32_t a_entry, bool a_equip)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* form = Gear::FormOf(a_entry);
			if (!a_equip || player->GetItemCount(form) == 0) player->AddObjectToContainer(form, nullptr, 1, nullptr);
			if (a_equip) RE::ActorEquipManager::GetSingleton()->EquipObject(player, form, nullptr, 1, nullptr, false, true, false, true);
			const auto* armory = Armory::Find(a_entry);
			return std::format("{} WoW entry {} {} ({:08X}{})", a_equip ? "equipped" : "gave", a_entry, form->GetName(), form->GetFormID(),
				armory ? std::format(", Armory: Skyrim level {}, quality {}", armory->level, armory->quality) : "");
		}

        void RefreshSkills()
        {
            auto* player=RE::PlayerCharacter::GetSingleton();
            if (!player) return;
            json skills=json::array();
            for (const auto& skill : kSkills) skills.push_back({{"key",skill.name},{"value",player->AsActorValueOwner()->GetActorValue(skill.value)}});
            WowUi::Run("__WBTestSkills("+Bags::ItemLua(skills)+")");
        }
		void Result(std::uint64_t a_call, bool a_ok, std::string a_message, bool a_pending = false)
		{
			Print(a_message);
			if (a_call == g_calls) {
				g_result.update({ { "ok", a_ok }, { "pending", a_pending }, { "message", std::move(a_message) } });
				WowUi::Run("__WBTestResult("+Bags::ItemLua(g_result)+")");
			}
		}

        bool RestoreLevelIteration(const LevelRequest& request,RE::PlayerCharacter* player)
        {
            if (!request.hasIteration || !player) return false;
            auto* owner=player->AsActorValueOwner();
            constexpr std::array values{RE::ActorValue::kHealth,RE::ActorValue::kMagicka,RE::ActorValue::kStamina,RE::ActorValue::kCarryWeight};
            Execute(std::format("player.setlevel {}",request.level));
            player->GetGameStatsData().perkCount=static_cast<std::uint8_t>(request.perks);
            *player->GetInfoRuntimeData().skills->data=request.iterationSkills;
            for (std::size_t i=0;i<values.size();++i) {
                owner->SetBaseActorValue(values[i],request.bases[i]);
                owner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent,values[i],request.permanent[i]-player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kPermanent,values[i]));
                owner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,values[i],request.damage[i]-player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage,values[i]));
            }
            return true;
        }
        void LevelMenuFrame()
        {
            std::lock_guard lock(g_levelLock);
            if (!g_levelRequest) return;
            auto& request=*g_levelRequest;
            auto* player=RE::PlayerCharacter::GetSingleton(); auto* ui=RE::UI::GetSingleton();
            auto* skills=player ? player->GetInfoRuntimeData().skills : nullptr;
            const auto now=std::chrono::steady_clock::now();
            if (request.stage==0 && ui->IsMenuOpen(RE::Console::MENU_NAME)) { request.changed=now; return; }
            const auto attribute=request.attribute=="health" ? RE::ActorValue::kHealth : request.attribute=="magicka" ? RE::ActorValue::kMagicka : RE::ActorValue::kStamina;
            const auto close=[] {
                auto* queue=RE::UIMessageQueue::GetSingleton();
                queue->AddMessage(RE::LevelUpMenu::MENU_NAME,RE::UI_MESSAGE_TYPE::kHide,nullptr);
                queue->AddMessage(RE::StatsMenu::MENU_NAME,RE::UI_MESSAGE_TYPE::kHide,nullptr);
            };
            const auto finish=[&](bool ok,const std::string& message) {
                const auto call=request.call;
                if (!ok && skills && skills->data && RestoreLevelIteration(request,player)) request.excess=0;
                if (skills && skills->data) skills->data->xp+=request.excess;
                if (call==g_calls) g_result["levels"]=request.evidence;
                close(); g_levelRequest.reset(); g_levelActive=false;
                SelfTest::ClaimMenu("StatsMenu",false); SelfTest::ClaimMenu("LevelUp Menu",false);
                Result(call,ok,message);
            };
            if (!skills || !skills->data || now-request.changed>std::chrono::seconds(12)) {
                finish(false,"Native scripted level choice timed out or player XP data disappeared"); return;
            }
            auto* av=player->AsActorValueOwner();
            if (request.stage==0) {
                if (ui->IsMenuOpen(RE::StatsMenu::MENU_NAME) || ui->IsMenuOpen(RE::LevelUpMenu::MENU_NAME) ||
                    ui->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME) || now-request.changed<std::chrono::milliseconds(900)) return;
                request.level=player->GetLevel(); request.perks=player->GetGameStatsData().perkCount;
                request.base=av->GetBaseActorValue(attribute); request.carry=av->GetPermanentActorValue(RE::ActorValue::kCarryWeight);
                constexpr std::array values{RE::ActorValue::kHealth,RE::ActorValue::kMagicka,RE::ActorValue::kStamina,RE::ActorValue::kCarryWeight};
                for (std::size_t i=0;i<values.size();++i) {
                    request.bases[i]=av->GetBaseActorValue(values[i]);
                    request.permanent[i]=player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kPermanent,values[i]);
                    request.damage[i]=player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage,values[i]);
                }
                // Exactly one eligible level per native menu visit. Preserve any pre-existing
                // excess XP separately, so native multi-level prompts cannot steal a choice.
                const float available=skills->data->xp+request.excess;
                request.iterationSkills=*skills->data; request.iterationSkills.xp=available; request.hasIteration=true;
                request.excess=std::max(0.f,available-skills->data->levelThreshold);
                skills->data->xp=skills->data->levelThreshold;
                request.stage=1; request.changed=now;
                WowUi::Run("__WB.host('menu','StatsMenu')"); return;
            }
            if (request.stage==1) {
                const auto menu=ui->GetMenu<RE::LevelUpMenu>();
                if (!menu || !menu->uiMovie || !menu->uiMovie->GetVisible() || !ui->IsMenuOpen(RE::LevelUpMenu::MENU_NAME)) return;
                // Use the movie's registered native callback, then accept only its own
                // ConfirmLevelUpAttribute message box. No input or unrelated prompts.
                struct Callbacks : RE::FxDelegateHandler::CallbackProcessor {
                    std::string wanted; RE::FxDelegateHandler::CallbackFn* choose=nullptr;
                    void Process(const RE::GString& name,RE::FxDelegateHandler::CallbackFn* fn) override {
                        if (std::string_view(name.c_str())==wanted) choose=fn;
                    }
                } callbacks;
                callbacks.wanted=request.attribute=="health" ? "addHealth" : request.attribute=="magicka" ? "addMagicka" : "addStamina";
                menu->Accept(&callbacks);
                if (!callbacks.choose) { finish(false,"Native level-up attribute callback unavailable"); return; }
                request.stage=2; request.changed=now;
                callbacks.choose(RE::FxDelegateArgs(RE::GFxValue(),menu.get(),menu->uiMovie.get(),nullptr,0)); return;
            }
            if (request.stage==2) {
                auto* message=RE::MessageBoxMenu::GetCurrentMessageBoxData();
                const auto callback=message ? message->callback : RE::BSTSmartPointer<RE::IMessageBoxCallback>{};
                const REL::Relocation<std::uintptr_t> expected{RE::VTABLE___ConfirmLevelUpAttributeCallback[0]};
                if (!callback || *reinterpret_cast<const std::uintptr_t*>(callback.get())!=expected.address()) return;
                auto* choice=static_cast<RE::LevelUpMenu::ConfirmLevelUpAttributeCallback*>(callback.get());
                if (choice->actorValue!=attribute) { finish(false,"Native confirmation chose a different attribute"); return; }
                request.stage=3; request.changed=now; RE::MessageBoxMenu::SelectOption(0); return;
            }
            if (request.stage==3) {
                if (ui->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME) || now-request.changed<std::chrono::milliseconds(150)) return;
                const bool ok=player->GetLevel()==request.level+1 && av->GetBaseActorValue(attribute)==request.base+10.f &&
                    av->GetPermanentActorValue(RE::ActorValue::kCarryWeight)==request.carry+(request.attribute=="stamina" ? 5.f : 0.f) &&
                    player->GetGameStatsData().perkCount==request.perks+1;
                request.evidence.push_back({{"before",request.level},{"after",player->GetLevel()},{"attribute",request.attribute},
                    {"base_before",request.base},{"base_after",av->GetBaseActorValue(attribute)},{"carry_before",request.carry},
                    {"carry_after",av->GetPermanentActorValue(RE::ActorValue::kCarryWeight)},{"perks_before",request.perks},
                    {"perks_after",player->GetGameStatsData().perkCount},{"xp",skills->data->xp},{"threshold",skills->data->levelThreshold},{"pass",ok}});
                if (!ok) { finish(false,"Native level/attribute/carry/perk delta differed from the requested choice"); return; }
                ++request.completed; request.hasIteration=false; close();
                Progression::Acquire(std::span<const int>{}); // request a fresh class book without discarding earned ranks
                Progression::RefreshTalents(); RefreshSkills();
                if (request.completed==request.count) {
                    const auto message=std::format("Advanced {} native levels; {} +{}, perks +{}",request.completed,request.attribute,request.completed*10,request.completed);
                    finish(true,message); return;
                }
                request.stage=0; request.changed=now;
            }
        }
        struct LevelDisplay {
            template<int Index> static void thunk(RE::IMenu* menu) {
                original[Index](menu);
                if (g_levelActive) SKSE::GetTaskInterface()->AddUITask(LevelMenuFrame);
            }
            static inline REL::Relocation<void(RE::IMenu*)> original[3];
        };
        bool InstallLevelObserver()
        {
            static bool installed=false;
            if (installed) return true;
            // PostDisplay slot audited alongside SelfTest's paused-menu observer.
            if (REL::Module::get().version()!=REL::Version{1,7,104,0}) return false;
            REL::Relocation<std::uintptr_t> hud{RE::VTABLE_HUDMenu[0]}, stats{RE::VTABLE_StatsMenu[0]}, choice{RE::VTABLE_LevelUpMenu[0]};
            LevelDisplay::original[0]=hud.write_vfunc(6,LevelDisplay::thunk<0>);
            LevelDisplay::original[1]=stats.write_vfunc(6,LevelDisplay::thunk<1>);
            LevelDisplay::original[2]=choice.write_vfunc(6,LevelDisplay::thunk<2>);
            installed=true; return true;
        }

		bool Dispatch(std::string a_command, std::string a_arg, std::string a_value)
		{
			a_command = Lower(a_command), a_arg = Lower(a_arg), a_value = Lower(a_value);
			const auto call = ++g_calls;
			g_result = { { "calls", call }, { "command", a_command }, { "arg", a_arg }, { "value", a_value } };
			auto done = [call,&a_command](bool ok, std::string message) {
                if (ok && (a_command=="skill" || a_command=="skills" || a_command=="xp")) RefreshSkills();
                Result(call, ok, std::move(message)); return ok;
            };
			if (a_command == "start" && a_arg.empty() && a_value.empty()) {
				const bool ok = Integration::Start();
				return done(ok, ok ? "WoWBridge setup requested; close the console to continue" : "setup unavailable: load a game, enable WoW UI, or finish the current setup");
			}
			if (a_command.empty() && a_arg.empty() && a_value.empty()) {
				if (!Integration::Active()) return done(false, "use wb start to choose WoWBridge before opening testing tools");
				WowUi::Run("__WBTestPanelToggle()"); return done(true,"testing panel opened; close the console to use it");
			}
			if (a_command == "help" && a_arg.empty() && a_value.empty()) {
				Print("wb start opens WoWBridge setup; wb opens testing tools; help | sex male|female | level N | skill NAME N | skills N | learnall | unlearn");
				Print("mount KEY|none | form cat|bear|travel|aquatic|none | give ENTRY|NAME | give set KEY|ID | sets | magicka N | reset | talents reset | talentpoints N");
				Print("xp SKILL N | heal | equip ENTRY|NAME | nativeitem HEX COUNT | learnwow ID | learnskyrim HEX");
				Print("find TEXT | random LEVEL QUALITY | randomequip LEVEL QUALITY | randomset LEVEL | lootroll [COUNT] | usetrinkets");
				Print("shadows status|receive on|off|blob on|off|cascades on|off|mask");
				Print("npcshoulders on|off|status (nearby NPCs wearing Armory shoulders)");
				Print("npchelms on|off|status; describe FORMID; lift|out|scale|pitch N[f] (nearby NPC head models)");
				Print("npcweapons on|off|status; type NAME; lift|out|scale|roll N[f] (nearby NPC weapon/shield models)");
				Print("playerweapons on|off|status; glow|stance|frontview on|off; type NAME; lift|out|scale|roll N (Skyrim weapons on the player's WoW body)");
				Print("health N | stamina N | damage PERCENT | speed PERCENT | multipliers | cooldowns | travel PLACE | npcscan [status|cancel] | clearspawn");
				Print("spawn dummy|wolf|chicken|bandit|bear|skeever|mudcrab|townsperson COUNT (1..5)");
				Print("book CLASS TAB (0=all) | learnwow ID [CLASS] | learnchain ROOT [CLASS] | learntab TAB [CLASS]");
                Print("levelup health|magicka|stamina COUNT (1..20) | levelup cancel | merchant spawn|open|remove | gold N (1..1000000)");
                Print("lootchest spawn|open|remove | scenario beast|observers|shrine|inventory|reagents|corpse|crafting|threat|snare|cc");
                Print("emote TOKEN | react EMOTE|status | say [long] | showhelm on|off|toggle | showcloak on|off|toggle | talents price | talentpoints 0..51 | appearance");
                Print("givebook SPELL | givebook chain ROOT | givebook all | givereins KEY|all | mounts forget|restore");
                Print("identity CLASS RACE,SEX,SKIN,FACE,HAIR,COLOR,FACIAL (11 Druid: 4/6; 2 Paladin: 1/3)");
                Print("target kill|revive|heal | time HOUR (0..23) | weather clear | bounty clear | god");
                Print("paladin holy_light|flash|lay_hands|righteousness|command|light|wisdom|justice|crusader|judgement|divine_shield|divine_protection|protection");
                Print("paladin stun|wrath|exorcism|holy_wrath|turn|consecration|fury|holy_shield|holy_shock|attack|hit [SEED]|status");
                Print("paladin intervention|might|greater_might|salvation|greater_salvation|divine_favor|spell_hit|follower_hit|follower_spell|devotion|retribution|concentration|fire_aura|frost_aura|shadow_aura|sanctity|sanctuary|sacrifice|freedom|party_hit|party_block|snare");
                Print("paladin perception (Human)|stoneform|find_treasure (Dwarf)|bleed (native Bleeding Damage on you; afflict: disease and poison)");
                Print("defenses: live player/target actor values, converted resistance percentages and regeneration");
				std::string places="Places:"; for (const auto& [key,place] : kPlaces) places+=" "+key; Print(places);
				Print("Skills: onehanded twohanded marksman block smithing heavyarmor lightarmor pickpocket lockpicking sneak alchemy speechcraft alteration conjuration destruction illusion restoration enchanting");
				return done(true, "help: docs/console-commands.md");
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || !player->Is3DLoaded() || Character::Get().name.empty()) {
				return done(false, "load a game with character.json before using wb");
			}
			if (!Integration::Active()) return done(false, "WoWBridge is waiting; use wb start to begin setup");
			auto* av = player->AsActorValueOwner();
			std::uint32_t n = 0;
			if (a_command == "paladin") {
				if (Character::Get().classId != 2) return done(false, "Choose a Paladin (class 2) first");
				if (a_arg == "status" && a_value.empty()) {
					const auto state = Cast::TalentDiagnostics(); Print(state.dump()); g_result["paladin"] = state;
					return done(true, "Paladin aura, proc, area and defense state printed");
				}
				if (a_arg == "attack" && a_value.empty()) {
					Melee::Toggle(Bridge::Target()); return done(true, "Paladin auto attack toggled");
				}
				if ((a_arg == "fear_probe" || a_arg == "paralysis_probe" || a_arg == "stagger_probe" || a_arg == "interrupt_probe") && a_value.empty()) {
					if (a_arg == "interrupt_probe") Cast::Interrupt();
					else Carriers::MechanicForTesting(player, a_arg == "fear_probe" ? RE::EffectSetting::Archetype::kDemoralize :
						a_arg == "paralysis_probe" ? RE::EffectSetting::Archetype::kParalysis : RE::EffectSetting::Archetype::kStagger);
					return done(true, "Native mechanic probe applied; paladin status prints the chance and rolls");
				}
				if (a_arg == "hit" && (a_value.empty() || Number(a_value, n, 0, INT32_MAX))) {
					auto* attacker = Bridge::Target();
					if (!attacker || attacker == player || attacker->IsDead()) return done(false, "Select a living fixture attacker");
					Cast::DefenseForTesting(attacker, 5.f, n);
					return done(true, "Five-health incoming roll queued; paladin status prints the reply");
				}
				if ((a_arg == "spell_hit" || a_arg == "follower_hit" || a_arg == "follower_spell") && a_value.empty()) {
					auto* target = Bridge::Target();
					if (!target || target == player || target->IsDead()) return done(false, "Select a living fixture target");
					if (a_arg == "spell_hit") Carriers::NativeHitForTesting(target, player, 10.f);
					else {
						auto* follower = Bridge::PartyMember(0);
						if (!follower || follower == target) return done(false, "A follower and a different target are required");
						if (a_arg == "follower_spell") Carriers::NativeHitForTesting(follower, target, 10.f);
						else Cast::WeaponHitForTesting(follower, target, 10.f);
					}
					return done(true, "Native ten-health hit requested; paladin status prints proc evidence");
				}
				if ((a_arg == "party_hit" || a_arg == "party_block") && a_value.empty()) {
					auto* member = Bridge::PartyMember(0); auto* attacker = Bridge::Target();
					if (!member || !attacker || attacker == member || attacker->IsDead()) return done(false, "Select an attacker; a blessed party member is required");
					if (!Cast::PartyHitForTesting(member, attacker, 10.f, a_arg == "party_block")) return done(false, "The first follower needs Sanctuary or Sacrifice");
					return done(true, "Ten-health party hit queued; paladin status prints the reply");
				}
				if (a_arg == "snare" && a_value.empty()) {
					auto* target = Bridge::Target(); if (!target) target = player;
					Carriers::NativeForTesting(target, "Paladin purge fixture", RE::ActorValue::kSpeedMult, -20, 5);
					return done(true, "Five-second native snare requested on the target");
				}
				if (a_arg == "bleed" && a_value.empty()) {   // Stoneform's purge: Skyrim's own bleeding (an axe perk's)
					auto* bleed = RE::TESForm::LookupByID<RE::MagicItem>(0x0C5BEC);   // PerkBleedingSwordDaedric75
					if (!bleed) return done(false, "Skyrim's Bleeding Damage spell unavailable");
					player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)->CastSpellImmediate(bleed, false, player, 1.0f, false, 0.0f, nullptr);
					return done(true, "Six seconds of native Bleeding Damage on you");
				}
				static const std::map<std::string, std::uint32_t> spells = {
					{"intervention", 19752}, {"might", 25291}, {"greater_might", 25916}, {"salvation", 1038}, {"greater_salvation", 25895}, {"divine_favor", 20216},
					{"holy_light", 25292}, {"flash", 19943}, {"lay_hands", 10310}, {"righteousness", 20293},
					{"command", 20920}, {"light", 20349}, {"wisdom", 20357}, {"justice", 20164}, {"crusader", 20308},
					{"judgement", 20271}, {"divine_shield", 1020}, {"divine_protection", 5573}, {"protection", 10278},
					{"stun", 10308}, {"wrath", 24239}, {"exorcism", 10314}, {"holy_wrath", 10318}, {"turn", 10326},
					{"consecration", 20924}, {"fury", 25780}, {"holy_shield", 20928},
					{"devotion", 10293}, {"retribution", 10301}, {"concentration", 19746}, {"fire_aura", 19900},
					{"frost_aura", 19898}, {"shadow_aura", 19896}, {"sanctity", 20218},
					{"sanctuary", 20914}, {"sacrifice", 20729}, {"freedom", 1044}, {"holy_shock", 20930},
					{"perception", 20600}, {"stoneform", 20594}, {"find_treasure", 2481}};   // their race's General-tab actives
				if (const auto it = spells.find(a_arg); it != spells.end() && a_value.empty()) {
					Cast::Use(it->second); return done(true, "Cast requested; normal knowledge, target, mana and cooldown rules apply");
				}
				return done(false, "Unknown Paladin action; wb help");
			}
            if (a_command=="defenses" && a_arg.empty() && a_value.empty()) {
                auto* actor=Bridge::Target();
                if (!actor) return done(false,"Select a WoW target first (yourself is allowed)");
                auto caster=Character::Caster(); Forms::Augment(caster);
                json target={{"level",actor->GetLevel()}}; Cast::AugmentTarget(target,actor);
                Result(call,true,"Reading live defenses; close the console for the service reply",true);
                Service::Request({{"op","native_defenses"},{"caster",caster},{"target",target}},
                    [call,name=std::string(actor->GetDisplayFullName())](const json& r) {
                        if (!r.value("ok",false)) { Result(call,false,"defenses: "+r.value("error",std::string("no answer"))); return; }
                        if (call==g_calls) g_result["defenses"]=r;
                        for (const char* unit : {"player","target"}) {
                            const auto& report=r.at(unit);
                            const auto& live=report.at("actor_values").at("live");
                            const auto& external=report.at("external");
                            Print(std::format("{} {}: raw AV (external after bridge subtraction)",unit,std::string_view(unit)=="target" ? name : Character::Get().name));
                            for (const auto& [id,label] : std::initializer_list<std::pair<const char*,const char*>>{
                                {"44","ResistMagic"},{"41","ResistFire"},{"43","ResistFrost"},{"42","ResistShock"},
                                {"40","PoisonResist"},{"45","ResistDisease"},{"83","AbsorbChance"},{"39","DamageResist"},
                                {"28","MagickaRate"},{"156","MagickaRateMult"},{"27","HealRate"},{"155","HealRateMult"},
                                {"29","StaminaRate"},{"157","StaminaRateMult"}})
                                Print(std::format("  {}={:.3f} (external {:.3f})",label,live.value(id,0.f),external.value(id,0.f)));
                            Print(std::format("  WoW R={} effective %={} binary %={}",report.at("resist").dump(),
                                report.at("resist_percent").dump(),report.at("binary_resist_percent").dump()));
                            Print(std::format("  absorption {}%, armor {}, regen multipliers {}",report.at("absorb_chance").dump(),
                                report.at("armor").dump(),report.at("regen_multipliers").dump()));
                        }
                        Result(call,true,"Live defenses printed to the Skyrim console/log; target effective %: "+r.at("target").at("resist_percent").dump());
                    });
                return true;
            }
            if (a_command=="appearance" && a_arg.empty() && a_value.empty()) {   // the WoW character creation window, for the player
                if (!Integration::Active()) return done(false,"Customization needs WoWBridge active (wb start)");
                WowUi::Run("if WBTestPanel then WBTestPanel:Hide() end __WBCustomizeAppearance()");
                return done(true,"Customization opened; close the console to choose their look");
            }
            if (a_command=="identity" || a_command=="appearance") {
                std::uint32_t classId = 0;
                if (!Number(a_arg,classId,2,11)) return done(false,"identity CLASS RACE,SEX,SKIN,FACE,HAIR,COLOR,FACIAL");
                try {
                    const auto v = json::parse("["+a_value+"]");
                    if (v.size()!=7 || !std::ranges::all_of(v,[](const auto& x) { return x.is_number_unsigned() && x.template get<std::uint64_t>()<=0xffffffffu; }))
                        return done(false,"identity needs seven unsigned appearance fields");
                    const wowcore::Appearance looks{v[0],v[1],v[2],v[3],v[4],v[5],v[6]};
                    const bool ok = Character::SetIdentity(classId,looks,a_command=="identity");
                    if (ok) Distribution::SetClass(Character::Get().classId);   // their class's books roll
                    return done(ok,ok ? "Identity accepted" : "Unavailable class, race, sex or appearance");
                } catch (const std::exception&) { return done(false,"Invalid identity fields"); }
            }
            if (a_command=="gold" && a_value.empty() && Number(a_arg,n,1,1000000)) {
                auto* gold=RE::TESForm::LookupByID<RE::TESBoundObject>(0xF);
                if (!gold) return done(false,"Skyrim gold unavailable");
                const int before=player->GetGoldAmount(); player->AddObjectToContainer(gold,nullptr,n,nullptr);
                g_result["gold_before"]=before; g_result["gold_after"]=player->GetGoldAmount();
                return done(player->GetGoldAmount()==before+static_cast<int>(n),std::format("Gave {} gold; total {}",n,player->GetGoldAmount()));
            }
            if (a_command=="levelup" && a_arg=="cancel" && a_value.empty()) {
                std::lock_guard lock(g_levelLock);
                if (g_levelRequest) {
                    auto* skills=player->GetInfoRuntimeData().skills;
                    if (skills && skills->data) { if (RestoreLevelIteration(*g_levelRequest,player)) g_levelRequest->excess=0; skills->data->xp+=g_levelRequest->excess; }
                    g_result["levels"]=g_levelRequest->evidence;
                    g_levelRequest.reset(); g_levelActive=false;
                    auto* message=RE::MessageBoxMenu::GetCurrentMessageBoxData();
                    const REL::Relocation<std::uintptr_t> expected{RE::VTABLE___ConfirmLevelUpAttributeCallback[0]};
                    if (message && message->callback && *reinterpret_cast<const std::uintptr_t*>(message->callback.get())==expected.address()) RE::MessageBoxMenu::SelectOption(1);
                    auto* queue=RE::UIMessageQueue::GetSingleton();
                    queue->AddMessage(RE::LevelUpMenu::MENU_NAME,RE::UI_MESSAGE_TYPE::kHide,nullptr);
                    queue->AddMessage(RE::StatsMenu::MENU_NAME,RE::UI_MESSAGE_TYPE::kHide,nullptr);
                }
                return done(true,"Scripted leveling cancelled; completed levels retained");
            }
            if (a_command=="levelup" && Number(a_value,n,1,20) && (a_arg=="health" || a_arg=="magicka" || a_arg=="stamina")) {
                auto* skills=player->GetInfoRuntimeData().skills; auto* ui=RE::UI::GetSingleton();
                if (!skills || !skills->data || !std::isfinite(skills->data->xp) || !std::isfinite(skills->data->levelThreshold) || skills->data->levelThreshold<=0 ||
                    player->GetLevel()+n>65535 || player->GetGameStatsData().perkCount+n>255) return done(false,"native level/XP data unavailable or level/perk limit exceeded");
                if (ui->IsMenuOpen(RE::StatsMenu::MENU_NAME) || ui->IsMenuOpen(RE::LevelUpMenu::MENU_NAME) || ui->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME))
                    return done(false,"Close Skills, LevelUp and message boxes before scripted leveling");
                if (!InstallLevelObserver()) return done(false,"Scripted native level choice requires the audited Skyrim 1.7.104 menu layout");
                { std::lock_guard lock(g_levelLock);
                  if (g_levelRequest) return done(false,"a scripted level choice is already pending");
                  g_levelRequest=LevelRequest{call,a_arg,static_cast<int>(n)}; g_levelActive=true;
                  SelfTest::ClaimMenu("StatsMenu",true); SelfTest::ClaimMenu("LevelUp Menu",true);   // its own paused menus
                }
                WowUi::Run("if WBTestPanel then WBTestPanel:Hide() end");
                Result(call,true,"Native scripted level choice queued; close the console. Skills opens and accepts the selected attribute automatically",true);
                return true;
            }
            if ((a_command=="merchant" || a_command=="lootchest") && a_value.empty() && (a_arg=="spawn" || a_arg=="open" || a_arg=="remove")) {
                const bool merchant=a_command=="merchant";
                if (a_arg=="remove") { if (merchant) RemoveVendor(); else RemoveLootChest(); return done(true,a_command+" helper removed"); }
                if (a_arg=="spawn") {
                    auto* base=RE::TESForm::LookupByID<RE::TESObjectCONT>(0xF8478); // empty, non-respawning native chest
                    if (!base) return done(false,"native empty chest unavailable");
                    if (merchant) RemoveVendor(); else RemoveLootChest();
                    auto* chest=PlaceNearby(base,merchant ? 220.f : 120.f);
                    if (!chest || !Stock(chest,merchant)) { if (chest) { chest->Disable(); chest->SetDelete(true); } return done(false,"could not place or stock native chest"); }
                    if (merchant) {
                        g_vendorChest=chest->CreateRefHandle();
                        auto* source=RE::TESForm::LookupByID<RE::TESFaction>(0x5A665); // Lucan's general vendor
                        auto* actor=PlaceNearby(RE::TESForm::LookupByID<RE::TESNPC>(0x1034E4));
                        if (!source || !actor || !actor->As<RE::Actor>()) { RemoveVendor(); return done(false,"could not place merchant or find vendor faction"); }
                        auto* vendor=actor->As<RE::Actor>(); g_vendor=vendor->CreateRefHandle();
                        // Lucan's own vendor faction, its merchant chest lent to this one's stock while it stands (restored on
                        // remove): a runtime duplicate of the faction, with its conditions and location cleared, crashed the
                        // barter menu (SkyrimSE+4A355F reading 8).
                        g_vendorFaction=source;
                        if (!g_lentChest) g_lentChest=source->vendorData.merchantContainer;
                        source->vendorData.merchantContainer=chest;
                        vendor->AddToFaction(g_vendorFaction,0); vendor->EnableAI(false);
                        // Its cached vendor faction, set outright: CommonLib's GetVendorFaction recalculates but returns the
                        // stale empty value, and Skyrim only recalculates it on its own schedule.
                        vendor->GetActorRuntimeData().vendorFaction=g_vendorFaction;
                        vendor->SetDisplayName("WoWBridge test merchant",true);
                        vendor->AddObjectToContainer(RE::TESForm::LookupByID<RE::TESBoundObject>(0xF),nullptr,10000,nullptr);
                    } else { g_lootChest=chest->CreateRefHandle(); chest->SetDisplayName("WoWBridge varied loot",true); }
                }
                WowUi::Run("if WBTestPanel then WBTestPanel:Hide() end");
                if (merchant) {
                    auto vendor=g_vendor.get(); auto chest=g_vendorChest.get();
                    if (!vendor || !chest || vendor->IsDisabled() || vendor->IsDead() || vendor->GetActorRuntimeData().vendorFaction!=g_vendorFaction ||
                        player->GetPosition().GetDistance(vendor->GetPosition())>300.f) return done(false,"spawn a merchant nearby first (vendor faction must be available)");
                    g_result["actor"]=vendor->GetFormID(); g_result["chest"]=chest->GetFormID(); g_result["stock_types"]=std::size(kStock);
                    RE::BarterMenu::OpenMenu(vendor.get());
                    return done(true,"Native barter requested: test vendor with 11 item types, spell books, mount items and 10000 gold; buy/sell, then merchant remove");
                }
                auto chest=g_lootChest.get();
                if (!chest || chest->IsDisabled() || player->GetPosition().GetDistance(chest->GetPosition())>300.f) return done(false,"spawn a loot chest nearby first");
                g_result["chest"]=chest->GetFormID(); g_result["stock_types"]=std::size(kStock);
                return done(chest->ActivateRef(player,0,nullptr,1,true),"Varied chest activated; WoW loot window opens through the native container backend");
            }
            if (a_command=="talents" && a_arg=="price" && a_value.empty()) {
                auto saved=Progression::SnapshotForTesting(); TalentWipe::Write(saved,{}); Progression::RestoreForTesting(saved);
                return done(true,"Kynareth talent reset price ladder reset; next wipe costs 1 gold (allocation preserved)");
            }
            if ((a_command=="showhelm" || a_command=="showcloak") && a_value.empty() && (a_arg=="on" || a_arg=="off" || a_arg=="toggle")) {
                const auto getter=a_command=="showhelm" ? "ShowingHelm" : "ShowingCloak", setter=a_command=="showhelm" ? "ShowHelm" : "ShowCloak";
                const bool show=a_arg=="on" || (a_arg=="toggle" && WowUi::Eval(std::format("{}() and 1 or 0",getter))!="1");
                WowUi::Run(std::format("{}({})",setter,show ? "true" : "false"));
                return done(true,std::format("{} {}",a_command,show ? "shown" : "hidden"));
            }
            if (a_command=="emote" && a_value.empty()) {
                auto token=a_arg; std::ranges::transform(token,token.begin(),[](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                const auto looks=Character::Looks();
                if (!WowUi::EmoteText(token,looks.race,looks.sex)) return done(false,"unknown client emote token; browse the Emotes testing page");
                WowUi::Run("DoEmote("+LuaQuote(token)+")"); return done(true,"Playing emote "+token);
            }
            if (a_command=="react" && a_value.empty()) {   // nearby NPCs' answer to an emote, or the last one's details
                if (a_arg=="status" || a_arg.empty()) { g_result["react"]=Reactions::Status(); Print(Reactions::Status().dump()); return done(true,"Last NPC reaction printed"); }
                auto token=a_arg; std::ranges::transform(token,token.begin(),[](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                WowUi::Run("DoEmote("+LuaQuote(token)+")");
                return done(true,"Emote "+token+"; wb react status shows who answered");
            }
            if (a_command=="say" && a_value.empty() && (a_arg.empty() || a_arg=="long")) {   // a line and its chat bubble
                RE::Actor* who=Bridge::Target(); float nearest=20.f*64.f;
                if (!who || who==player) {
                    who=nullptr;
                    for (const auto& handle : RE::ProcessLists::GetSingleton()->highActorHandles) {
                        const auto actor=handle.get();
                        if (!actor || actor.get()==player || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) continue;
                        if (const float d=player->GetPosition().GetDistance(actor->GetPosition()); d<nearest) { nearest=d; who=actor.get(); }
                    }
                }
                if (!who) return done(false,"nobody within 20 yards to speak; target someone or spawn a townsperson");
                Bridge::Say(who,a_arg=="long" ? "Talos! Mightiest Nord of all time! He who was born in Atmora as Hjalti Early-Beard, rose to unite us all!" :
                    "Watch the skies, traveler.");
                return done(true,std::format("{} speaks: chat, and its bubble within 20 yards with Show Chat Bubbles",who->GetDisplayFullName()));
            }
            if (a_command=="scenario" && a_value.empty()) {
                if (a_arg=="inventory") {
                    for (auto id : kStock) if (!RE::TESForm::LookupByID<RE::TESBoundObject>(id)) return done(false,"native fixture item missing");
                    for (auto id : kStock) player->AddObjectToContainer(RE::TESForm::LookupByID<RE::TESBoundObject>(id),nullptr,3,nullptr);
                    return done(true,"Gave varied inventory items for bag swaps, deposits, tooltips, books and nameless-item checks");
                }
                if (a_arg=="crafting") {
                    constexpr RE::FormID materials[]{0xDB5D2,0x800E4,0x5ACE4,0x5ACE5,0x2E4FF};
                    std::vector<RE::TESBoundObject*> items;
                    for (auto id : materials) { auto* form=RE::TESForm::LookupByID<RE::TESBoundObject>(id); if (!form) return done(false,"native crafting material missing"); items.push_back(form); }
                    for (auto entry : {873u,16897u}) {
                        const auto it=Character::Gear().find(entry);
                        auto* form=it==Character::Gear().end() ? nullptr : RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESBoundObject>(it->second.form,"WoWBridge.esp");
                        if (!form) return done(false,"packaged crafting test equipment missing"); items.push_back(form);
                    }
                    for (std::size_t i=0;i<items.size();++i) player->AddObjectToContainer(items[i],nullptr,i<std::size(materials) ? 10 : 1,nullptr);
                    return done(true,"Gave staff/chestguard, leather, strips, iron/steel and filled grand gems; improve/enchant at native crafting stations");
                }
                if (a_arg=="reagents") {
                    std::vector<RE::TESBoundObject*> reagents;
                    for (const auto& [entry,id] : Character::Reagents()) {
                        auto* form=RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESBoundObject>(id,"WoWBridge.esp");
                        if (!form) return done(false,"packaged reagent form missing"); reagents.push_back(form);
                    }
                    if (reagents.empty()) return done(false,"packaged reagent catalog missing");
                    for (auto* form : reagents) player->AddObjectToContainer(form,nullptr,20,nullptr);
                    return done(true,"Gave 20 of every packaged reagent; spawn a corpse for Rebirth");
                }
                if (a_arg=="shrine") {
                    auto* shrine=PlaceNearby(RE::TESForm::LookupByID<RE::TESBoundObject>(0xD987F));
                    if (!shrine) return done(false,"Kynareth shrine could not be placed");
                    g_objects.push_back(shrine->CreateRefHandle()); g_result["shrine"]=shrine->GetFormID();
                    return done(true,"Kynareth shrine nearby: allocate talents, activate it, then accept the price offer");
                }
                if (a_arg=="threat") {   // Growl, Challenging Roar, Cower: two wolves fighting a guard made their follower, AI on
                    auto* guardRef=PlaceNearby(RE::TESForm::LookupByID<RE::TESNPC>(0x267EE),200.f);
                    auto* guard=guardRef ? guardRef->As<RE::Actor>() : nullptr;
                    if (!guard || !RE::TESForm::LookupByID<RE::TESNPC>(0x23ABE)) return done(false,"scenario actor placement failed");
                    Execute("setplayerteammate 1",guard);
                    g_spawned.push_back(guard->CreateRefHandle()); g_result["spawned"]=json::array({guard->GetFormID()});
                    for (int i=0;i<2;++i) {
                        auto* ref=PlaceNearby(RE::TESForm::LookupByID<RE::TESNPC>(0x23ABE),450.f+i*120.f);
                        auto* wolf=ref ? ref->As<RE::Actor>() : nullptr;
                        if (!wolf) return done(false,"scenario actor placement failed");
                        wolf->AsActorValueOwner()->SetBaseActorValue(RE::ActorValue::kHealth,1000);
                        g_spawned.push_back(wolf->CreateRefHandle()); g_result["spawned"].push_back(wolf->GetFormID());
                        Execute(std::format("startcombat {:08X}",guard->GetFormID()),wolf);
                    }
                    return done(true,"Two wolves fight a guard made your follower: Growl / Challenging Roar hold them on you 3 / 6 s; Cower sends one attacking you to the guard; clearspawn removes them");
                }
                if (a_arg=="cc") {   // their crowd control against Skyrim's damage: two still wolves side by side
                    const float h=player->GetHeading(false);
                    g_result["spawned"]=json::array();
                    for (const float side : {-150.f,150.f}) {
                        auto* ref=PlaceNearby(RE::TESForm::LookupByID<RE::TESNPC>(0x23ABE),500.f);
                        auto* wolf=ref ? ref->As<RE::Actor>() : nullptr;
                        if (!wolf) return done(false,"scenario actor placement failed");
                        ref->SetPosition(ref->GetPosition()+RE::NiPoint3{std::cos(h)*side,-std::sin(h)*side,0});
                        wolf->EnableAI(false); wolf->AsActorValueOwner()->SetBaseActorValue(RE::ActorValue::kHealth,1000);
                        g_spawned.push_back(wolf->CreateRefHandle()); g_result["spawned"].push_back(wolf->GetFormID());
                    }
                    return done(true,"Two still wolves (1000 health): Hibernate, Roots or Soothe one, then hurt it with a Skyrim spell or let a follower; "
                        "any damage wakes or soothes no more, roots may break. Hibernate or Root the other: the first is freed. clearspawn removes them");
                }
                if (a_arg=="snare") {   // shapeshift freeing: a native slow and WoWBridge's root carrier on the player
                    Carriers::NativeForTesting(player,"WB test snare",RE::ActorValue::kSpeedMult,-50.f,30.f);
                    Carriers::Root(player,8.f);
                    return done(true,"A native -50% slow (30 s) and WoWBridge's root (8 s) on you: entering or leaving any form ends both");
                }
                std::vector<RE::FormID> bases;
                if (a_arg=="beast") bases={0x4359C}; // passive domestic goat, AI remains enabled for detection
                else if (a_arg=="observers") bases={0x267EE,0x1034E4};
                else if (a_arg=="corpse") bases={0x23ABE};
                else return done(false,"scenario must be beast|observers|shrine|inventory|reagents|corpse|crafting|threat|snare|cc");
                for (auto id : bases) if (!RE::TESForm::LookupByID<RE::TESNPC>(id)) return done(false,"scenario actor base missing");
                g_result["spawned"]=json::array();
                for (auto id : bases) {
                    auto* ref=PlaceNearby(RE::TESForm::LookupByID<RE::TESNPC>(id),a_arg=="beast" ? 250.f : 350.f+g_result["spawned"].size()*120.f);
                    auto* actor=ref ? ref->As<RE::Actor>() : nullptr;
                    if (!actor) return done(false,"scenario actor placement failed");
                    g_spawned.push_back(actor->CreateRefHandle()); g_result["spawned"].push_back(actor->GetFormID());
                    if (a_arg=="corpse") { actor->EnableAI(false); Execute("kill",actor); }
                }
                return done(true,"Scenario "+a_arg+" ready; AI-enabled observers use native detection and aggression; clearspawn removes scenario fixtures");
            }

			if (a_command == "sex" && a_value.empty() && (a_arg == "male" || a_arg == "female")) {
				if (!Character::SetSex(a_arg == "female" ? 1 : 0)) { return done(false, "male appearance data unavailable"); }
				const bool matched = Surrogate::SetSex(a_arg == "female");
				return done(matched, std::format("sex {}: redressing body, portrait and previews; UnitSex {}; surrogate {}", a_arg,
					a_arg == "male" ? 2 : 3, matched ? "matched" : "voice unavailable"));
			}
			if (a_command == "level" && a_value.empty() && Number(a_arg, n, 1, 65535)) {
				Execute(std::format("player.setlevel {}", n));
				return done(player->GetLevel() == n, std::format("level {}: setlevel skips attribute choices. Earn skill XP (player.advskill SKILL XP), then click the XP bar or use Shift+Tab > Skills for Skyrim's Health/Magicka/Stamina choice", player->GetLevel()));
			}
			if ((a_command == "skill" || a_command == "xp") && Number(a_value, n, 0, a_command=="xp" ? 1000000 : 100)) {
				if (a_arg == "archery") { a_arg = "marksman"; }
				if (a_arg == "speech") { a_arg = "speechcraft"; }
				if (a_arg == "one_handed") { a_arg = "onehanded"; }
				if (a_arg == "two_handed") { a_arg = "twohanded"; }
				const auto it = std::ranges::find(kSkills, a_arg, &Skill::name);
				if (it == std::end(kSkills)) { return done(false, "unknown skill; wb help lists names"); }
				if (a_command=="xp") Execute(std::format("player.advskill {} {}",a_arg,n));
				else av->SetActorValue(it->value, static_cast<float>(n));
				return done(true, std::format("{} {} {}: progression refreshes shortly", a_command,a_arg,n));
			}
			if (a_command == "skills" && a_value.empty() && Number(a_arg, n, 0, 100)) {
				for (const auto& skill : kSkills) { av->SetActorValue(skill.value, static_cast<float>(n)); }
				return done(true, std::format("all 18 Skyrim skills {}; spell progression refreshes after closing the console", n));
			}
			if ((a_command == "learnall" || a_command == "unlearn") && a_arg.empty() && a_value.empty()) {
				Result(call, true, a_command + ": requesting progression book; close the console to receive the answer", true);
				Progression::TestBook(a_command == "learnall", [call, command = a_command](bool ok, std::size_t count, std::string error) {
					Result(call, ok, ok ? std::format("{}: {} ranks known", command, count) : command + ": " + error);
				});
				return true;
			}
			if (a_command=="givebook" && ((a_arg=="all" && a_value.empty()) || (a_arg=="chain" && Number(a_value,n,1,INT32_MAX)) ||
                    (a_value.empty() && Number(a_arg,n,1,INT32_MAX)))) {
                // A rank's spell book, every book of a chain, or all of them (the class spell browser's routes).
                std::set<int> chain;
                if (a_arg=="chain") for (const auto& c : Classes()) if (c.at("key")=="druid") for (const auto& rank : c.at("spells"))
                    if (rank.at("chain")==n) chain.insert(rank.at("spell").get<int>());
                std::vector<RE::TESBoundObject*> books;
                for (const auto spell : Learning::Books()) {
                    if (a_arg=="all" || (a_arg=="chain" ? chain.contains(static_cast<int>(spell)) : spell==n)) books.push_back(Learning::Book(spell));
                }
                if (books.empty()) return done(false,"No book teaches that: druid ranks a trainer, quest or item teaches (wb book druid 0)");
                for (auto* book : books) player->AddObjectToContainer(book,nullptr,1,nullptr);
                g_result["books"]=books.size();
                return done(true,std::format("Gave {} spell book{}: use it from the bags (B) or read it in the inventory",books.size(),books.size()==1 ? "" : "s"));
            }
            if (a_command=="givereins" && a_value.empty()) {
                std::vector<RE::TESBoundObject*> items;
                for (const auto& key : Learning::MountKeys()) if (a_arg=="all" || key==a_arg) items.push_back(Learning::MountItem(key));
                if (items.empty()) return done(false,"givereins needs a mount key from the Mounts testing page, or all");
                for (auto* item : items) player->AddObjectToContainer(item,nullptr,1,nullptr);
                g_result["items"]=items.size();
                return done(true,std::format("Gave {} mount item{}: use it from the bags (B) or read it in the inventory",items.size(),items.size()==1 ? "" : "s"));
            }
            if (a_command=="mounts" && a_value.empty() && (a_arg=="forget" || a_arg=="restore")) {
                Mounts::SetLearned(a_arg=="restore" ? Mounts::Keys() : std::vector<std::string>{});
                return done(true,a_arg=="restore" ? std::format("All {} mounts learned (an existing save's mounts)",Mounts::Keys().size()) :
                    "Every mount forgotten, as a new game: learn them from their reins");
            }
			if (a_command == "mount" && a_value.empty()) {
                std::string message; const bool ok=Mounts::Select(a_arg,message);
                return done(ok,std::move(message));
            }
            if (a_command == "form" && a_value.empty()) {
				const std::map<std::string, std::uint32_t> spells{ { "cat", 768 }, { "bear", 5487 }, { "travel", 783 }, { "aquatic", 1066 } };
				const auto it = spells.find(a_arg);
				if (a_arg != "none" && it == spells.end()) { return done(false, "form must be cat|bear|travel|aquatic|none"); }
				Mounts::Dismount();
				const auto request = ++g_formRequest;
				if (a_arg == "none") { Cast::FormForTesting(0, json::object()); return done(true, "form none"); }
				auto caster = Character::Caster();
				caster["form"] = "none";   // a testing helper enters the requested form, never toggles it off
				caster["swimming"] = true;  // Aquatic preview can be tested on land
				caster["race"] = Character::Get().looks.race;
				Result(call, true, "form " + a_arg + ": requesting form effects; close the console", true);
				Service::Request({ { "op", "resolve" }, { "spell", it->second }, { "caster", std::move(caster) },
					{ "target", { { "kind", "self" } } }, { "ctx", { { "indoors", false } } } },
					[call, request, form = a_arg, spell = it->second](const json& r) {
						if (request != g_formRequest) { Result(call, false, "form request superseded"); return; }
						if (!r.value("ok", false)) { Result(call, false, "form: " + r.value("error", std::string("no answer"))); return; }
						Cast::FormForTesting(spell, r);
						Result(call, Forms::Current() == form, "form " + Forms::Current());
					});
				return true;
			}
			if (a_command == "playerweapons") {
				std::string message;
				const bool ok = PlayerWeapons::Command(a_arg, a_value, message);
				g_result["her_weapons"] = PlayerWeapons::Status();
				return done(ok, std::move(message));
			}
			if (a_command == "npchelms") {
				if (a_arg == "lift" || a_arg == "out" || a_arg == "scale" || a_arg == "pitch") {
					const bool female = !a_value.empty() && a_value.back() == 'f';
					std::istringstream input(female ? a_value.substr(0, a_value.size() - 1) : a_value);
					float v = 0;
					if (!(input >> v) || !(input >> std::ws).eof() || !std::isfinite(v))
						return done(false, "npchelms lift|out|scale|pitch N[f] (finite additive trim)");
					const auto fit = NpcHelms::SetFit(female, a_arg.c_str(), v);
					return done(true, std::format("NPC helms {} additive trim: lift {} out {} scale {} pitch {}", female ? "female" : "male", fit[0], fit[1], fit[2], fit[3]));
				}
				if (a_arg == "describe") {   // why an actor's head does or does not carry a WoW helm
					RE::FormID id = 0; std::istringstream input(a_value); input >> std::hex >> id;
					g_result["npc_helms_describe"] = NpcHelms::Describe(id);
					return done(id != 0, g_result["npc_helms_describe"].dump());
				}
				if (!a_value.empty() || (a_arg != "on" && a_arg != "off" && a_arg != "status"))
					return done(false, "npchelms on|off|status, describe FORMID, or lift|out|scale|pitch N[f]");
				if (a_arg != "status") NpcHelms::SetEnabled(a_arg == "on");
				const auto instances = NpcHelms::Current();
				const auto draws = NpcHelms::Draws();
				json items = json::array();
				for (const auto& i : instances) items.push_back({ { "actor", i.ref }, { "entry", i.entry }, { "display", i.display },
					{ "female", i.female }, { "hidden", i.hidden }, { "hair_hidden", i.hairHidden }, { "beard_hidden", i.beardHidden } });
				g_result["npc_helms"] = { { "enabled", NpcHelms::Enabled() }, { "gathered", instances.size() },
					{ "drawn_items", draws.size() }, { "items", items } };
				return done(true, std::format("NPC helms {}: {} gathered, {} items drawn in the last world frame (32 yd)",
					NpcHelms::Enabled() ? "on" : "off", instances.size(), draws.size()));
			}
			if (a_command == "npcweapons") {
				if (a_arg == "type") {
					return done(NpcWeapons::SelectType(a_value), std::format("NPC weapons fit type: {} (dagger/sword/mace/axe/greatsword/battleaxe/hammer/staff/bow/crossbow/shield)",
						NpcWeapons::TypeName(NpcWeapons::SelectedType())));
				}
				if (a_arg == "lift" || a_arg == "out" || a_arg == "scale" || a_arg == "roll") {
					const bool female = !a_value.empty() && a_value.back() == 'f';
					std::istringstream input(female ? a_value.substr(0, a_value.size() - 1) : a_value);
					float v = 0;
					if (!(input >> v) || !(input >> std::ws).eof() || !std::isfinite(v) || (a_arg == "scale" && v <= 0))
						return done(false, "npcweapons lift|out|scale|roll N[f] (finite value, positive scale)");
					const auto fit = NpcWeapons::SetFit(female, a_arg.c_str(), v);
					return done(true, std::format("NPC weapons {} {} fit: lift {} out {} scale {} roll {}", NpcWeapons::TypeName(NpcWeapons::SelectedType()),
						female ? "female" : "male", fit[0], fit[1], fit[2], fit[3]));
				}
				if (a_arg == "describe") {   // why an actor's hands do or do not carry a WoW model
					RE::FormID id = 0; std::istringstream input(a_value); input >> std::hex >> id;
					g_result["npc_weapons_describe"] = NpcWeapons::Describe(id);
					return done(id != 0, g_result["npc_weapons_describe"].dump());
				}
				if (!a_value.empty() || (a_arg != "on" && a_arg != "off" && a_arg != "status"))
					return done(false, "npcweapons on|off|status, type NAME, describe FORMID, or lift|out|scale|roll N[f]");
				if (a_arg != "status") NpcWeapons::SetEnabled(a_arg == "on");
				const auto instances = NpcWeapons::Current();
				const auto draws = NpcWeapons::Draws();
				json items = json::array();
				for (const auto& i : instances) items.push_back({ { "actor", i.ref }, { "entry", i.entry }, { "display", i.display },
					{ "type", NpcWeapons::TypeName(i.type) }, { "node", i.node }, { "drawn", i.drawn }, { "hidden", i.hidden } });
				g_result["npc_weapons"] = { { "enabled", NpcWeapons::Enabled() }, { "gathered", instances.size() },
					{ "drawn_items", draws.size() }, { "fit_type", NpcWeapons::TypeName(NpcWeapons::SelectedType()) }, { "items", items } };
				return done(true, std::format("NPC weapons {}: {} gathered, {} items drawn in the last world frame (32 yd); fit {}",
					NpcWeapons::Enabled() ? "on" : "off", instances.size(), draws.size(), NpcWeapons::TypeName(NpcWeapons::SelectedType())));
			}
			if (a_command == "shadows") {
				std::string message;
				const bool ok = Shadows::Command(a_arg, a_value, message);
				g_result["shadows"] = Shadows::Status();
				return done(ok, std::move(message));
			}
			if (a_command == "npcshoulders") {
				if (a_arg == "lift" || a_arg == "out" || a_arg == "scale") {   // tuning by eye: male NPCs' fit (female: add f)
					const bool female = !a_value.empty() && a_value.back() == 'f';
					const auto number = female ? a_value.substr(0, a_value.size() - 1) : a_value;
					float v = 0; if (number.empty() || !(std::istringstream(number) >> v)) return done(false, "npcshoulders lift|out|scale N[f]");
					const auto fit = NpcShoulders::SetFit(female, a_arg.c_str(), v);
					return done(true, std::format("NPC shoulders {} fit: lift {} out {} scale {}", female ? "female" : "male", fit[0], fit[1], fit[2]));
				}
				if (!a_value.empty() || (a_arg != "on" && a_arg != "off" && a_arg != "status")) return done(false, "npcshoulders needs on, off, status, or lift|out|scale N[f]");
				if (a_arg != "status") NpcShoulders::SetEnabled(a_arg == "on");
				const auto instances = NpcShoulders::Current();
				const auto draws = NpcShoulders::Draws();
				std::size_t models = 0;
				for (const auto& draw : draws) for (const auto batches : draw.batches) { models += batches > 0; }
				g_result["npc_shoulders"] = { { "enabled", NpcShoulders::Enabled() }, { "gathered", instances.size() },
					{ "drawn_actors", draws.size() }, { "drawn_models", models } };
				return done(true, std::format("NPC shoulders {}: {} gathered, {} actors / {} models drawn in the last world frame (32 yd)",
					NpcShoulders::Enabled() ? "on" : "off", instances.size(), draws.size(), models));
			}
			if (a_command == "sets" && a_arg.empty() && a_value.empty()) {
				for (const auto& [key, set] : Character::Sets()) {
					std::string entries;
					for (const auto entry : set.entries) { entries += std::format(" {}", entry); }
					Print(std::format("{}: {} —{}", key, set.name, entries));
				}
				for (const auto& [id, set] : Armory::Sets()) {   // the Armory's, by their ItemSet.dbc ID
					Print(std::format("{}: {} (Skyrim level {}, {} pieces)", id, set.name, set.level, set.entries.size()));
				}
				return done(!Character::Sets().empty() || !Armory::Sets().empty(),
					std::format("{} equipment sets in gear.json, {} in armory.json", Character::Sets().size(), Armory::Sets().size()));
			}
			if ((a_command == "give" && a_arg == "set" && !a_value.empty()) || (a_command == "randomset" && a_value.empty() && Number(a_arg, n, 1, 100))) {
				std::vector<std::uint32_t> entries;
				std::string name = a_value;
				auto armory = a_command == "give" && Number(a_value, n, 1, UINT32_MAX) ? Armory::Sets().find(n) : Armory::Sets().end();
				if (a_command == "randomset") {   // a complete Armory set of the highest Skyrim level up to this one, at random
					std::vector<std::uint32_t> ids;
					std::uint32_t best = 0;
					for (const auto& [id, set] : Armory::Sets()) if (set.level <= n) best = std::max<std::uint32_t>(best, set.level);
					for (const auto& [id, set] : Armory::Sets()) if (best && set.level == best) ids.push_back(id);
					if (ids.empty()) return done(false, "no Armory set at or below that level");
					static std::mt19937 rng{ std::random_device{}() };
					armory = Armory::Sets().find(ids[std::uniform_int_distribution<std::size_t>(0, ids.size() - 1)(rng)]);
				}
				if (const auto it = Character::Sets().find(a_value); a_command == "give" && it != Character::Sets().end()) {
					entries = it->second.entries;
				} else if (armory != Armory::Sets().end()) {
					for (const auto entry : armory->second.entries) if (Gear::FormOf(entry)) entries.push_back(entry);   // what Skyrim has of it
					name = armory->second.name;
					g_result["set"] = armory->first;
				} else {
					return done(false, "unknown equipment set; wb sets lists keys and Armory set IDs");
				}
				// Resolve the whole set before giving anything, so a mismatched ESP cannot give half a set.
				for (const auto entry : entries) {
					if (!Gear::FormOf(entry)) { return done(false, std::format("give {}: no gear.json item or installed ESP form", entry)); }
				}
				for (const auto entry : entries) { player->AddObjectToContainer(Gear::FormOf(entry), nullptr, 1, nullptr); }
				g_result["entries"] = entries;
				return done(!entries.empty(), std::format("gave set {} ({} items); equip them in inventory", name, entries.size()));
			}
			if (a_command == "usetrinkets" && a_arg.empty() && a_value.empty()) {
				// Resolve both first; a missing Armory must not partially change their outfit.
				for (const auto entry : {20130u, 23558u})
					if (!Gear::FormOf(entry)) return done(false, "WoWBridgeArmory.esp's test trinkets are not installed");
				for (const auto entry : {20130u, 23558u}) Print(GiveItem(entry, true));
				g_result["entries"] = {20130, 23558};
				return done(true, "equipped Diamond Flask and The Burrower's Shell; use from character slots or drag to the action bar");
			}
			if ((a_command == "give" || a_command == "equip") && !a_arg.empty() && a_arg != "set") {   // by entry or name
				std::string why;
				const auto entry = ItemNamed(a_value.empty() ? a_arg : a_arg + " " + a_value, why);   // two words need no quotes
				if (!entry) return done(false, why);
				g_result["entry"] = entry;
				return done(true, GiveItem(entry, a_command == "equip"));
			}
			if ((a_command == "random" || a_command == "randomequip") && Number(a_arg, n, 1, 100)) {
				std::uint32_t quality = 0;
				if (!Number(a_value, quality, 0, 5)) return done(false, "random LEVEL (1-100) QUALITY (0 poor .. 5 legendary)");
				const auto* item = Armory::Random(n, quality);
				if (!item) return done(false, Armory::Ready() ? "no Armory item of that quality" : "WoWBridgeArmory.esp is not loaded");
				g_result["entry"] = item->entry; g_result["level"] = item->level;
				return done(true, GiveItem(item->entry, a_command == "randomequip"));
			}
			if (a_command == "find" && !a_arg.empty()) {
				const auto text = a_value.empty() ? a_arg : a_arg + " " + a_value;
				std::size_t shown = 0;
				for (const auto& [id, item] : Character::Gear()) {
					auto* form = Gear::FormOf(id);
					if (form && form->GetName() && (Lower(form->GetName()).find(text) != std::string::npos || std::to_string(id) == text)) {
						Print(std::format("{} {} (WoWBridge.esp)", id, form->GetName()));
						++shown;
					}
				}
				const auto found = Armory::Search(text, 20);
				for (const auto* item : found) Print(std::format("{} {} (Armory: Skyrim level {}, quality {})", item->entry, item->name, item->level, item->quality));
				return done(shown + found.size() > 0, std::format("{} WoW items match {}{}", shown + found.size(), text, found.size() == 20 ? " (first 20 of the Armory's)" : ""));
			}
			if (a_command == "lootroll" && a_value.empty() && (a_arg.empty() || Number(a_arg, n, 1, 1000))) {
				// What their level rolls from each list the Armory joined, as a chest, corpse or vendor would: WoW items found.
				const auto rolls = a_arg.empty() ? 100u : n;
				const auto level = player->GetLevel();
				json rows = json::array();
				std::set<RE::FormID> seen;
				const auto roll = [&](RE::TESLevItem* a_list, const std::string& a_name) {
					int wow = 0;
					std::string example;
					for (std::uint32_t i = 0; i < rolls; ++i) {
						for (auto* form : Distribution::Roll(a_list, level)) {
							const auto* bound = form->As<RE::TESBoundObject>();
							if (const auto entry = Gear::EntryOf(bound); entry) { ++wow; if (example.empty()) example = std::format("{} [{}]", bound->GetName(), entry); }
						}
					}
					rows.push_back({ { "list", a_name }, { "rolls", rolls }, { "wow", wow }, { "example", example } });
					Print(std::format("{} x{} at level {}: {} WoW items{}", a_name, rolls, level, wow, example.empty() ? "" : ", e.g. " + example));
				};
				for (const auto& row : Armory::Distribution()) {
					for (const auto& target : row.value("lists", json::array())) {
						auto* into = RE::TESForm::LookupByID<RE::TESLevItem>(target.at("form").get<RE::FormID>());
						if (into && seen.insert(into->GetFormID()).second) roll(into, into->GetFormEditorID() && *into->GetFormEditorID() ? into->GetFormEditorID() : std::format("{:08X}", into->GetFormID()));
					}
					if (!row.value("vendors", json::array()).empty()) {
						if (auto* list = Distribution::List(row.at("name").get<std::string>())) roll(list, row.at("name").get<std::string>());
					}
				}
				g_result["rolls"] = rows;
				return done(!rows.empty(), rows.empty() ? "WoWBridgeArmory.esp's lists are not loaded" : std::format("rolled {} lists {} times at level {}", rows.size(), rolls, level));
			}
			if ((a_command=="catalog" && a_arg.empty() && a_value.empty()) || a_command=="book") {
                const auto classes=Classes();
                std::string active;
                for (const auto& c : classes) if (c.at("id")==Character::Get().classId) active=c.at("key");
                if (a_command=="book") {
                    if (!Number(a_value,n,0,255)) return done(false,"book needs class key and tab number (0=all)");
                    const auto c=std::ranges::find_if(classes,[&](const json& c) { return c.at("key")==a_arg; });
                    if (c==classes.end()) return done(false,"class not in packaged progression.json");
                    bool found=false;
                    for (const auto& rank : c->at("spells")) if (!n || rank.at("tab")==n) {
                        found=true; Print(std::format("{} / {} / {} {} [{}] {}: {} {}; source {}",a_arg,rank.at("tab_name").get<std::string>(),
                            rank.at("name").get<std::string>(),rank.value("rank_name",""),rank.at("spell").get<int>(),rank.at("known").get<bool>() ? "known" : "unknown",
                            rank.at("skill").get<std::string>(),rank.at("learn_skill_value").get<int>(),rank.value("source","")));
                    }
                    if (!found) return done(false,"no ranks in that class tab");
                }
				json catalog={{"sets",json::array()},{"gear",json::array()},{"skills",json::array()},{"places",json::array()}};
				for (const auto& [key,set] : Character::Sets()) catalog["sets"].push_back({{"key",key},{"name",set.name},{"count",set.entries.size()}});
				// The Armory's sets, and its 8,500 items for the Gear page's search: once a session (the interface keeps them).
				for (const auto& [id,set] : Armory::Sets()) catalog["sets"].push_back({{"key",std::to_string(id)},{"name",set.name},{"count",set.entries.size()},{"level",set.level}});
				if (Armory::Ready() && WowUi::Eval("__WB.testPanelArmory and 1 or 0")!="1") {
					auto& items=catalog["armory"]=json::array();
					for (const auto& item : Armory::Items()) items.push_back({item.entry,item.name,item.level,item.quality});
				}
				std::ifstream in("Data/SKSE/Plugins/WoWBridge/gear.json");
				if (in) {
                    const auto gear=json::parse(in);
                    for (const auto& [key,item] : gear.at("items").items()) catalog["gear"].push_back({{"entry",std::stoul(key)},{"name",item.at("name")}});
                    // The Mounts page's items to give, the Class Spells page's book names
                    catalog["reins"]=json::array(); catalog["books"]=json::object();
                    for (const auto& [key,item] : gear.value("mounts",json::object()).items())
                        catalog["reins"].push_back({{"key",item.at("teaches").at("mount")},{"name",item.at("name")}});
                    for (const auto& [key,item] : gear.value("books",json::object()).items())
                        catalog["books"][std::to_string(item.at("teaches").at("spell").get<int>())]=item.at("name");
                }
				for (const auto& skill : kSkills) catalog["skills"].push_back({{"key",skill.name},{"value",av->GetActorValue(skill.value)}});
				for (const auto& [key,place] : kPlaces) catalog["places"].push_back({{"key",key},{"name",place.name},{"hint",place.hint}});
				catalog["classes"]=classes; catalog["activeClass"]=active;
                WowUi::Run("__WBTestCatalog("+Bags::ItemLua(catalog)+")"); return done(true,a_command=="book" ? "Class ranks listed; known state is for your playable class" : "Testing tools ready");
			}
            if (a_command=="afflict" && a_arg.empty() && a_value.empty()) {   // a disease, a poison and a harmful spell to dispel
                auto* disease=RE::TESForm::LookupByID<RE::SpellItem>(0xB8781);   // Rattles
                auto* poison=RE::TESForm::LookupByID<RE::MagicItem>(0x65A6F);    // Lingering Damage Health
                auto* frost=RE::TESForm::LookupByID<RE::MagicItem>(0x3E07D);    // an ice hazard's frost
                if (!disease || !poison || !frost) return done(false,"Skyrim debuff spells unavailable");
                player->AddSpell(disease);
                auto* caster=player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
                caster->CastSpellImmediate(poison,false,player,1.0f,false,0.0f,nullptr);
                caster->CastSpellImmediate(frost,false,player,1.0f,false,0.0f,nullptr);
                return done(true,"Rattles, a lingering poison and frost applied");
            }
            if (a_command=="cooldowns" && a_arg.empty() && a_value.empty()) {
                Cast::RefreshCooldownsForTesting(); return done(true,"WoW spell/item cooldowns and global cooldown refreshed");
            }
			if (a_command=="heal" && a_arg.empty() && a_value.empty()) {
				for (auto value : {RE::ActorValue::kHealth,RE::ActorValue::kMagicka,RE::ActorValue::kStamina}) av->RestoreActorValue(value,1000000.f);
				return done(true,"Health, magicka and stamina refilled");
			}
			if ((a_command=="health" || a_command=="stamina") && a_value.empty() && Number(a_arg,n,1,1000000)) {
				const auto value=a_command=="health" ? RE::ActorValue::kHealth : RE::ActorValue::kStamina;
				av->SetActorValue(value,static_cast<float>(n)); av->RestoreActorValue(value,1000000.f);
				return done(true,std::format("{} base {} and refilled",a_command,n));
			}
			if (a_command=="multipliers" && a_arg.empty() && a_value.empty()) {
				av->SetActorValue(RE::ActorValue::kAttackDamageMult,1); av->SetActorValue(RE::ActorValue::kSpeedMult,100);
				return done(true,"Native weapon damage and movement speed multipliers reset to 100%");
			}
			if ((a_command=="damage" || a_command=="speed") && a_value.empty() && Number(a_arg,n,10,300)) {
				av->SetActorValue(a_command=="damage" ? RE::ActorValue::kAttackDamageMult : RE::ActorValue::kSpeedMult,static_cast<float>(n)/(a_command=="damage" ? 100.f : 1.f));
				return done(true,std::format("Native {} multiplier base {}%",a_command,n));
			}
			if ((a_command=="learnwow" || a_command=="learnchain" || a_command=="learntab") && Number(a_arg,n,a_command=="learntab" ? 0 : 1,INT32_MAX)) {
                std::vector<int> ranks;
                for (const auto& c : Classes()) if (c.at("id")==Character::Get().classId && (a_value.empty() || c.at("key")==a_value)) {
                    for (const auto& rank : c.at("spells")) {
                        if (rank.value("source","")=="talent") continue;
                        if ((a_command=="learnwow" && rank.at("spell")==n) || (a_command=="learnchain" && rank.at("chain")==n) ||
                            (a_command=="learntab" && (!n || rank.at("tab")==n))) ranks.push_back(rank.at("spell"));
                    }
                }
                if (ranks.empty()) return done(false,"Choose a non-talent rank/chain/tab of your playable class from wb book CLASS TAB");
                Progression::Acquire(ranks);
                g_result["learned"]=ranks;
                return done(true,std::format("{}: acquired {} ranks; book refreshes shortly",a_command,ranks.size()));
            }
			if (a_command=="nativeitem" || a_command=="learnskyrim") {
				std::string_view text=a_arg; if (text.starts_with("0x")) text.remove_prefix(2);
				const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),n,16);
				if (error!=std::errc{} || end!=text.data()+text.size() || !n) return done(false,"Skyrim form ID must be hexadecimal");
				auto* form=RE::TESForm::LookupByID(n);
				if (a_command=="learnskyrim" && a_value.empty()) {
					auto* spell=form ? form->As<RE::SpellItem>() : nullptr;
					if (!spell) return done(false,"That Skyrim form is not a spell");
					player->AddSpell(spell); return done(true,std::format("Learned Skyrim spell {:08X}",n));
				}
				std::uint32_t count=0;
				if (!Number(a_value,count,1,100)) return done(false,"Item quantity must be 1..100");
				const bool inventory=form && (form->As<RE::TESObjectARMO>() || form->As<RE::TESObjectWEAP>() || form->As<RE::TESObjectMISC>() ||
					form->As<RE::AlchemyItem>() || form->As<RE::IngredientItem>() || form->As<RE::TESAmmo>() || form->As<RE::TESObjectBOOK>() || form->As<RE::TESSoulGem>());
				if (!inventory) return done(false,"Choose an inventory item, not an actor/world object");
				player->AddObjectToContainer(form->As<RE::TESBoundObject>(),nullptr,count,nullptr);
				return done(true,std::format("Gave {} of Skyrim item {:08X}",count,n));
			}
			if (a_command=="travel" && a_value.empty()) {
				const auto place=kPlaces.find(a_arg); if (place==kPlaces.end()) return done(false,"Unknown testing destination");
				Cast::Stop(); WowUi::Run("if WBTestPanel then WBTestPanel:Hide() end");
                g_result["travel"]=Travel(place->second); return done(true,"Travel to "+std::string(place->second.name));
			}
            if (a_command=="npcscan" && a_value.empty() && (a_arg.empty() || a_arg=="status" || a_arg=="cancel")) {
                if (a_arg.empty()) {
                    if (Bridge::NPCScanCost().value("active",false)) return done(false,"NPC scan measurement already running");
                    Cast::Stop(); WowUi::Run("if WBTestPanel then WBTestPanel:Hide() end");
                    g_result["travel"]=Travel(kPlaces.at("whiterun"));
                    Bridge::StartNPCScanCost(g_result["travel"]);
                }
                if (a_arg=="cancel") Bridge::CancelNPCScanCost();
                g_result["scan_cost"]=Bridge::NPCScanCost();
                return done(true,a_arg.empty() ? "Whiterun market NPC scan: settle 8 s, then sample 10 s; mean budget <1.0 ms; close console" : g_result["scan_cost"].dump());
            }
			if (a_command=="spawn" && Number(a_value,n,1,5)) {
				const auto it=kSpawns.find(a_arg); auto* base=it==kSpawns.end() ? nullptr : RE::TESForm::LookupByID<RE::TESNPC>(it->second);
				if (!base) return done(false,"Spawn type must be dummy, wolf, chicken, bandit, bear, skeever, mudcrab or townsperson");
				g_result["spawned"]=json::array();
				int made=0; const float h=player->GetHeading(false);
				for (std::uint32_t i=0;i<n;++i) if (auto ref=player->PlaceObjectAtMe(base,false)) if (auto* actor=ref->As<RE::Actor>()) {
					actor->SetPosition(player->GetPosition()+RE::NiPoint3{std::sin(h)*250+std::cos(h)*i*80,std::cos(h)*250-std::sin(h)*i*80,0},true);
					if (a_arg=="dummy") { actor->EnableAI(false); actor->AsActorValueOwner()->SetBaseActorValue(RE::ActorValue::kHealth,10000); }
					if (a_arg=="bandit") Execute("startcombat player",actor);
					g_spawned.push_back(actor->CreateRefHandle()); g_result["spawned"].push_back(actor->GetFormID()); ++made;
				}
				return done(made==n,std::format("Spawned {} {}; Clear spawned disables these actors without deleting them",made,a_arg));
			}
			if (a_command=="clearspawn" && a_arg.empty() && a_value.empty()) {
				for (auto h : g_spawned) if (auto actor=h.get()) { actor->StopCombat(); actor->Disable(); }
				g_spawned.clear();
                for (auto h : g_objects) if (auto ref=h.get()) { ref->Disable(); ref->SetDelete(true); }
                g_objects.clear(); return done(true,"Disabled panel-spawned actors (not deleted)");
			}
			if (a_command=="target" && a_value.empty()) {
				const std::map<std::string,std::string> actions{{"kill","kill"},{"revive","resurrect 1"},{"heal","resethealth"}};
				const auto op=actions.find(a_arg); if (op==actions.end()) return done(false,"Target action must be kill, revive or heal");
				auto* target=Bridge::Target();
				if (!target || !target->Is3DLoaded()) return done(false,"Select a loaded actor in the WoW target frame first");
				if (target==player && a_arg!="heal") return done(false,"Kill/revive requires a target other than the player");
				if (a_arg=="revive" && !target->IsDead()) return done(false,"Target is already alive");
				if (a_arg!="revive" && target->IsDead()) return done(false,"Target is dead; revive it first");
				Execute(op->second,target);
				return done(true,std::format("Target {} requested: {} ({:08X}); native essential/protected rules apply",a_arg,target->GetName(),target->GetFormID()));
			}
			if (a_command=="time" && a_value.empty() && Number(a_arg,n,0,23)) {
				Execute(std::format("set gamehour to {}",n)); return done(true,std::format("World time set to {}:00",n));
			}
			if (a_command=="weather" && a_arg=="clear" && a_value.empty()) {
				Execute("fw 0000081A"); return done(true,"SkyrimClear weather requested (outdoors)");
			}
			if (a_command=="bounty" && a_arg=="clear" && a_value.empty()) {
				for (auto id : kCrimeFactions) if (!RE::TESForm::LookupByID<RE::TESFaction>(id)) return done(false,"Skyrim hold crime faction unavailable");
				for (auto id : kCrimeFactions) {
					Execute(std::format("player.setcrimegold 0 {:08X}",id));
					Execute(std::format("player.setcrimegoldviolent 0 {:08X}",id));
				}
				return done(true,"Cleared violent and nonviolent bounties in all nine holds");
			}
			if (a_command=="god" && a_arg.empty() && a_value.empty()) {
				Execute("tgm"); return done(true,"Native god mode toggled; WoW spell requirements still apply");
			}
			if (a_command == "magicka" && a_value.empty() && Number(a_arg, n, 0, 1000000)) {
				av->SetActorValue(RE::ActorValue::kMagicka, static_cast<float>(n));
				av->RestoreActorValue(RE::ActorValue::kMagicka, 1000000.0f);
				return done(true, std::format("magicka base {} refilled; current {}", n, Cast::Mana()));
			}
			if (a_command == "reset" && a_arg.empty() && a_value.empty()) {
				++g_formRequest;
				Mounts::Reset();
				Cast::ResetForTesting();
				SkyrimAuras::Reset();   // republish unrelated native effects after __WBResetCombat clears the UI rows
				return done(true, "cooldowns, casts, WoW auras and forms reset");
			}
			if (a_command == "talents" && a_arg == "reset" && a_value.empty()) {
				Result(call,true,"Resetting talents...",true);
				Progression::ResetTalents([call](bool ok, std::size_t count, std::string error) {
					Result(call, ok, ok ? std::format("talents reset; {} class spells known", count) : error);
				});
				return true;
			}
			if (a_command == "talentpoints" && a_value.empty() && Number(a_arg, n, 0, 51)) {   // their points are their level - 9
				Execute(std::format("player.setlevel {}", n + 9));
				return done(player->GetLevel() == n + 9, std::format("level {}: {} talent points in all", player->GetLevel(), n));
			}
			return done(false, "invalid command or arguments; wb help (level 1..65535, skills 0..100, magicka 0..1000000)");
		}

		bool Run(const RE::SCRIPT_PARAMETER* a_params, RE::SCRIPT_FUNCTION::ScriptData* a_data, RE::TESObjectREFR* a_this,
			RE::TESObjectREFR* a_containing, RE::Script* a_script, RE::ScriptLocals* a_locals, double& a_result, std::uint32_t& a_offset)
		{
			// kChar parameters are strings, including numeric tokens. Skyrim's compiler bounds each line to 512 bytes.
			char command[512]{}, arg[512]{}, value[512]{};
			if (!RE::Script::ParseParameters(a_params, a_data, a_offset, a_this, a_containing, a_script, a_locals, command, arg, value)) {
				Print("arguments could not be parsed; wb help");
				a_result = 0;
				return true;
			}
			try { a_result = Dispatch(command, arg, value) ? 1 : 0; }
			catch (const std::exception& e) { Result(g_calls, false, e.what()); a_result = 0; }
			return true;
		}
	}

	void Print(std::string_view a_message)
	{
		logger::info("wb: {}", a_message);
		if (auto* console = RE::ConsoleLog::GetSingleton()) { console->Print("wb: %s", std::string(a_message).c_str()); }
	}

	json Snapshot() { return g_result; }
	void CancelPending() { Bridge::CancelNPCScanCost(); { std::lock_guard lock(g_levelLock); g_levelRequest.reset(); g_levelActive=false; } ++g_formRequest; g_spawned.clear(); g_objects.clear(); g_vendor.reset(); g_vendorChest.reset(); g_lootChest.reset();
		if (g_vendorFaction && g_lentChest) g_vendorFaction->vendorData.merchantContainer=g_lentChest;
		g_lentChest=nullptr; g_vendorFaction=nullptr; }

	bool Command(std::string a_command,std::string a_arg,std::string a_value)
	{
		if (a_command.size()>128 || a_arg.size()>128 || a_value.size()>128) { Result(++g_calls,false,"Testing command arguments are too long"); return false; }
		try { return Dispatch(std::move(a_command),std::move(a_arg),std::move(a_value)); }
		catch (const std::exception& e) { Result(g_calls,false,e.what()); return false; }
	}

	bool Install()
	{
		// TestSeenData is a developer command of no use in play (other mods take it the same way); 1.7.104's console
		// table has no unused entry. Its opcode stays; the vanilla string-parameter compiler, then ParseParameters.
		auto* entry = RE::SCRIPT_FUNCTION::LocateConsoleCommand("TestSeenData");
		auto* parser = RE::SCRIPT_FUNCTION::LocateConsoleCommand("SetINISetting");
		if (!entry || !parser || RE::SCRIPT_FUNCTION::LocateConsoleCommand("wb")) {
			const auto* first = RE::SCRIPT_FUNCTION::GetFirstConsoleCommand();
			logger::error("console: wb not registered (TestSeenData {}, SetINISetting {}, wb {}; first command {})", entry != nullptr, parser != nullptr,
				RE::SCRIPT_FUNCTION::LocateConsoleCommand("wb") != nullptr, first && first->functionName ? first->functionName : "?");
			return false;
		}
		static RE::SCRIPT_PARAMETER params[] = {
			{ "subcommand", RE::SCRIPT_PARAM_TYPE::kChar, true }, { "argument", RE::SCRIPT_PARAM_TYPE::kChar, true },
			{ "value", RE::SCRIPT_PARAM_TYPE::kChar, true }
		};
		auto command = *entry;
		command.functionName = "wb", command.shortName = "";
		command.helpString = "WoWBridge testing commands: wb help";
		command.referenceFunction = false;
		command.SetParameters(params);
		command.executeFunction = Run;
		command.compileFunction = parser->compileFunction;
		command.conditionFunction = nullptr;
		REL::safe_write(reinterpret_cast<std::uintptr_t>(entry), &command, sizeof(command));
		logger::info("console: registered wb in place of TestSeenData");
		return true;
	}

	void Execute(std::string_view a_command, RE::TESObjectREFR* a_target)
	{
		auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
		auto* script = factory ? factory->Create() : nullptr;
		if (!script) { logger::error("console: cannot create Script form"); return; }
		logger::info("console: {}", a_command);
		script->SetCommand(a_command);
		script->CompileAndRun(a_target);
		delete script;
	}
}
