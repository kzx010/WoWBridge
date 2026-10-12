#include "Mounts.h"
#include "Console.h"
#include "Integration.h"

#include "Cast.h"
#include "Bags.h"
#include "LuaText.h"
#include "ui/WowUi.h"
#include "Character.h"
#include "Forms.h"
#include "Progression.h"
#include "Service.h"
#include "SkyrimAuras.h"
#include "Surrogate.h"

#include <charconv>

namespace Console
{
	namespace
	{
		using json = nlohmann::json;
		json g_result = json::object();
		std::uint64_t g_calls = 0, g_formRequest = 0;
		std::vector<RE::ActorHandle> g_spawned;
		const std::map<std::string,std::string> kPlaces{{"riverwood","coc Riverwood"},{"whiterun","cow WhiterunWorld 6 -2"},
			{"solitude","cow SolitudeWorld -13 25"},{"windhelm","cow WindhelmWorld 33 9"},{"riften","cow RiftenWorld 42 -24"},
			{"markarth","cow MarkarthWorld -43 1"},{"dawnstar","cow Tamriel 6 24"},{"blackreach","cow Blackreach 0 0"},{"qasmoke","coc QASmoke"}};

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
				Print("mount KEY|none | form cat|bear|travel|aquatic|none | give ENTRY | give set NAME | sets | magicka N | reset | talents reset | talentpoints N");
				Print("xp SKILL N | heal | equip ENTRY | nativeitem HEX COUNT | learnwow ID | learnskyrim HEX");
				Print("damage PERCENT | speed PERCENT | multipliers | travel PLACE | spawn dummy|wolf|chicken COUNT | clearspawn");
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
			if (a_command == "sets" && a_arg.empty() && a_value.empty()) {
				for (const auto& [key, set] : Character::Sets()) {
					std::string entries;
					for (const auto entry : set.entries) { entries += std::format(" {}", entry); }
					Print(std::format("{}: {} —{}", key, set.name, entries));
				}
				return done(!Character::Sets().empty(), std::format("{} equipment sets in gear.json", Character::Sets().size()));
			}
			if (a_command == "give" && ((a_value.empty() && Number(a_arg, n, 1, UINT32_MAX)) || (a_arg == "set" && !a_value.empty()))) {
				std::vector<std::uint32_t> entries{ n };
				if (a_arg == "set") {
					const auto it = Character::Sets().find(a_value);
					if (it == Character::Sets().end()) { return done(false, "unknown equipment set; wb sets lists keys"); }
					entries = it->second.entries;
				}
				auto* data = RE::TESDataHandler::GetSingleton();
				std::vector<RE::TESBoundObject*> forms;
				// Resolve the whole set before giving anything, so a mismatched ESP cannot give half a set.
				for (const auto entry : entries) {
					const auto it = Character::Gear().find(entry);
					auto* found = it != Character::Gear().end() && data ? data->LookupForm(it->second.form, "WoWBridge.esp") : nullptr;
					auto* form = found ? found->As<RE::TESBoundObject>() : nullptr;
					if (!form) { return done(false, std::format("give {}: no gear.json item or installed ESP form", entry)); }
					forms.push_back(form);
				}
				for (auto* form : forms) { player->AddObjectToContainer(form, nullptr, 1, nullptr); }
				return done(true, a_arg == "set" ? std::format("gave set {} ({} items); equip them in inventory", a_value, forms.size()) :
					std::format("gave WoW entry {} ({:08X})", n, forms.front()->GetFormID()));
			}
			if (a_command=="catalog" && a_arg.empty() && a_value.empty()) {
				json catalog={{"sets",json::array()},{"gear",json::array()},{"skills",json::array()},{"places",json::array()}};
				for (const auto& [key,set] : Character::Sets()) catalog["sets"].push_back({{"key",key},{"name",set.name},{"count",set.entries.size()}});
				std::ifstream in("Data/SKSE/Plugins/WoWBridge/gear.json");
				if (in) { const auto gear=json::parse(in); for (const auto& [key,item] : gear.at("items").items()) catalog["gear"].push_back({{"entry",std::stoul(key)},{"name",item.at("name")}}); }
				for (const auto& skill : kSkills) catalog["skills"].push_back({{"key",skill.name},{"value",av->GetActorValue(skill.value)}});
				for (const auto& [key,command] : kPlaces) catalog["places"].push_back({{"key",key}});
				WowUi::Run("__WBTestCatalog("+Bags::ItemLua(catalog)+")"); return done(true,"Testing tools ready");
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
			if (a_command=="learnwow" && a_value.empty() && Number(a_arg,n,1,INT32_MAX)) {
				if (WowUi::Eval(std::format("(__WB.classRows and __WB.classRows[{}] and not __WB.classRows[{}].passive) and 1 or 0",n,n))!="1") return done(false,"Choose a non-passive packaged class spell");
				const int spell=static_cast<int>(n); Progression::Acquire(std::span<const int>(&spell,1));
				return done(true,std::format("Learned WoW spell {}; book refreshes shortly",n));
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
			if (a_command=="equip" && a_value.empty() && Number(a_arg,n,1,UINT32_MAX)) {
				const auto it=Character::Gear().find(n);
				auto* form=it==Character::Gear().end() ? nullptr : RE::TESDataHandler::GetSingleton()->LookupForm(it->second.form,"WoWBridge.esp");
				auto* item=form ? form->As<RE::TESBoundObject>() : nullptr;
				if (!item) return done(false,"Unknown packaged WoW equipment entry");
				if (player->GetItemCount(item)==0) player->AddObjectToContainer(item,nullptr,1,nullptr);
				RE::ActorEquipManager::GetSingleton()->EquipObject(player,item,nullptr,1,nullptr,false,true,false,true);
				return done(true,std::format("Equipped WoW entry {}",n));
			}
			if (a_command=="travel" && a_value.empty()) {
				const auto place=kPlaces.find(a_arg); if (place==kPlaces.end()) return done(false,"Unknown testing destination");
				Cast::Stop(); WowUi::Run("WBTestPanel:Hide()"); Execute(place->second); return done(true,"Travel to "+a_arg);
			}
			if (a_command=="spawn" && Number(a_value,n,1,5)) {
				const std::map<std::string,RE::FormID> types{{"wolf",0x23ABE},{"dummy",0x23ABE},{"chicken",0xA91A0}};
				const auto it=types.find(a_arg); auto* base=it==types.end() ? nullptr : RE::TESForm::LookupByID<RE::TESNPC>(it->second);
				if (!base) return done(false,"Spawn type must be wolf, dummy or chicken");
				g_result["spawned"]=json::array();
				int made=0; const float h=player->GetHeading(false);
				for (std::uint32_t i=0;i<n;++i) if (auto ref=player->PlaceObjectAtMe(base,false)) if (auto* actor=ref->As<RE::Actor>()) {
					actor->SetPosition(player->GetPosition()+RE::NiPoint3{std::sin(h)*250+std::cos(h)*i*80,std::cos(h)*250-std::sin(h)*i*80,0},true);
					if (a_arg=="dummy") { actor->EnableAI(false); actor->AsActorValueOwner()->SetBaseActorValue(RE::ActorValue::kHealth,10000); }
					g_spawned.push_back(actor->CreateRefHandle()); g_result["spawned"].push_back(actor->GetFormID()); ++made;
				}
				return done(made==n,std::format("Spawned {} {}; Clear spawned removes only this panel's actors",made,a_arg));
			}
			if (a_command=="clearspawn" && a_arg.empty() && a_value.empty()) {
				for (auto h : g_spawned) if (auto actor=h.get()) { actor->StopCombat(); actor->Disable(); }
				g_spawned.clear(); return done(true,"Cleared panel-spawned actors");
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
			if (a_command == "talentpoints" && a_value.empty() && Number(a_arg, n, 1, 51)) {   // her points are her level - 9
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
	void CancelPending() { ++g_formRequest; g_spawned.clear(); }

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
