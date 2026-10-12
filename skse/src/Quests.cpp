#include "Quests.h"

#include "Frame.h"
#include "LuaText.h"
#include "ui/WowUi.h"

namespace Quests
{
	namespace
	{
		using State = RE::QUEST_OBJECTIVE_STATE;

		double g_next = 0;
		bool   g_quiet = true;
		std::map<RE::FormID, std::vector<bool>> g_shown;   // quest -> its objectives done, as last published
		std::string g_last;

		// Skyrim's kinds of quest, as the client's zone headers.
		const char* Header(RE::QUEST_DATA::Type a_type)
		{
			using T = RE::QUEST_DATA::Type;
			switch (a_type) {
			case T::kMainQuest: return "Main Quest";
			case T::kMagesGuild: return "College of Winterhold";
			case T::kThievesGuild: return "Thieves Guild";
			case T::kDarkBrotherhood: return "Dark Brotherhood";
			case T::kCompanionsQuest: return "The Companions";
			case T::kDaedric: return "Daedric";
			case T::kSideQuest: return "Side Quests";
			case T::kCivilWar: return "Civil War";
			case T::kDLC01_Vampire: return "Dawnguard";
			case T::kDLC02_Dragonborn: return "Dragonborn";
			default: return "Miscellaneous";
			}
		}

		// An objective's text with Skyrim's alias tags (<Alias=Lucan>) read as their names.
		std::string Plain(std::string_view a_text)
		{
			std::string out;
			for (std::size_t i = 0; i < a_text.size(); ++i) {
				if (a_text[i] == '<') {
					const auto close = a_text.find('>', i);
					const auto eq = a_text.find('=', i);
					if (close != std::string_view::npos) {
						if (eq != std::string_view::npos && eq < close) {
							out += a_text.substr(eq + 1, close - eq - 1);
						}
						i = close;
						continue;
					}
				}
				out += a_text[i];
			}
			return out;
		}

		std::vector<Quest> Gather()
		{
			std::vector<Quest> out;
			for (auto* quest : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESQuest>()) {
				if (!quest || !quest->IsRunning() || !quest->GetFullName() || !*quest->GetFullName()) {
					continue;
				}
				Quest q{ quest->GetFormID(), quest->GetFullName(), Header(quest->GetType()) };
				bool shown = false;
				for (auto* o : quest->objectives) {
					const auto state = o ? o->state.get() : State::kDormant;
					if (state != State::kDisplayed && state != State::kCompletedDisplayed && state != State::kFailedDisplayed) {
						continue;
					}
					shown = true;
					q.objectives.emplace_back(Plain(o->displayText.c_str()), state == State::kCompletedDisplayed, state == State::kFailedDisplayed);
				}
				if (!shown) {
					continue;
				}
				RE::BSString text;
				quest->GetJournalTextForInstance(text, 0);
				q.text = Plain(text.c_str());
				out.push_back(std::move(q));
			}
			std::ranges::stable_sort(out, {}, &Quest::header);
			return out;
		}

		std::string ObjectiveText(RE::TESQuest* a_quest, const RE::BGSInstancedQuestObjective& a_instance)
		{
			std::string text = a_instance.Objective->displayText.c_str();
			const RE::BGSQuestInstanceText* stored = nullptr;
			for (const auto* data : a_quest->instanceData) if (data && data->id == a_instance.instanceID) { stored = data; break; }
			for (std::size_t at = 0; (at = text.find("<Alias=", at)) != std::string::npos;) {
				const auto end = text.find('>', at);
				if (end == std::string::npos) break;
				const auto aliasName = text.substr(at + 7, end - at - 7);
				std::string name = aliasName;
				for (const auto* alias : a_quest->aliases) {
					if (!alias || aliasName != alias->aliasName.c_str()) continue;
					RE::TESForm* named = nullptr;
					if (stored) for (const auto& s : stored->stringData)
						if (s.aliasID == alias->aliasID) { named = RE::TESForm::LookupByID(s.fullNameFormID); break; }
					if (named && named->GetName() && *named->GetName()) name = named->GetName();
					else if (const auto ref = a_quest->GetAliasedRef(alias->aliasID).get(); ref && *ref->GetDisplayFullName()) name = ref->GetDisplayFullName();
					break;
				}
				text.replace(at, end - at + 1, name); at += name.size();
			}
			return Plain(text);
		}

		void System(const std::string& a_text)
		{
			WowUi::Run(std::format("__WBFire('CHAT_MSG_SYSTEM', {}, '', '', '', '', '', 0, 0, '')", LuaQuote(a_text)));
		}
	}

	std::optional<RE::NiPoint3> CompassPosition(RE::PlayerCharacter* a_player, RE::TESObjectREFR* a_ref, const RE::TeleportPath* a_path, bool a_exteriorMap)
	{
		// Walled exteriors share geographical coordinates with the parent. Other
		// worlds (Blackreach, Soul Cairn) need their route's door, not a raw target
		// coordinate from an unrelated floor/worldspace.
		const auto mapWorld = [](RE::TESWorldSpace* a_world) {
			for (int depth = 0; a_world && a_world->parentWorld && depth < 16 &&
				a_world->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseSkyCell, RE::TESWorldSpace::ParentUseFlag::kUseLODData); ++depth)
				a_world = a_world->parentWorld;
			return a_world;
		};
		const auto* cell = a_player->GetParentCell();
		const auto here = mapWorld(a_exteriorMap && cell && cell->IsInteriorCell() ?
			a_player->GetPlayerRuntimeData().cachedWorldSpace : a_player->GetWorldspace());
		const auto usable = [&](RE::TESObjectREFR* a_candidate) {
			if (!a_candidate || a_candidate->IsDeleted() || a_candidate->IsDisabled() || !cell || !a_candidate->GetParentCell()) return false;
			if (cell->IsInteriorCell() && !a_exteriorMap) return a_candidate->GetParentCell() == cell;
			return here && !a_candidate->GetParentCell()->IsInteriorCell() && mapWorld(a_candidate->GetWorldspace()) == here;
		};
		if (usable(a_ref)) return a_ref->GetPosition();
		if (a_path) {
			for (const auto& link : a_path->teleportRefs) {
				if (usable(link.ref)) return link.ref->GetPosition();
			}
		}
		// An objective in her own interior has no exterior leg in its current
		// tracking path. Its entrance is represented by her cached exterior pose.
		if (a_exteriorMap && here && cell && cell->IsInteriorCell() && a_ref &&
			!a_ref->IsDeleted() && !a_ref->IsDisabled() && a_ref->GetParentCell() == cell)
			return a_player->GetPlayerRuntimeData().exteriorPosition;
		return std::nullopt;
	}

	std::vector<CompassTarget> CompassTargets(RE::PlayerCharacter* a_player, bool a_exteriorMap)
	{
		std::vector<std::pair<RE::TESQuest*, RE::TESQuestTarget*>> targets;
		{
			RE::BSSpinLockGuard guard(a_player->GetQuestTargetsLock());
			for (const auto& [quest, list] : a_player->GetQuestTargets()) {
				if (!quest || !list || !quest->IsActive() || !quest->IsRunning()) continue;
				for (auto* target : *list) if (target) targets.emplace_back(quest, target);
			}
		} // Engine resolution can re-enter the target cache; never hold its lock then.
		std::vector<CompassTarget> out;
		for (const auto& [quest, target] : targets) {
			RE::ObjectRefHandle handle;
			target->GetTrackingRef(handle, quest);
			const auto ref = handle.get();
			const auto at = CompassPosition(a_player, ref.get(), &target->teleportPath, a_exteriorMap);
			if (!at) continue;
			std::string name;
			for (const auto& instance : a_player->GetPlayerRuntimeData().objectives) {
				const auto* o = instance.Objective;
				if (!o || o->ownerQuest != quest || instance.InstanceState != State::kDisplayed) continue;
				for (std::uint32_t i = 0; o->targets && i < o->numTargets; ++i) {
					if (o->targets[i] == target) { name = ObjectiveText(quest, instance); break; }
				}
				if (!name.empty()) break;
			}
			if (name.empty() && quest->GetFullName()) name = quest->GetFullName();
			if (name.empty()) name = "Quest target";
			out.push_back({quest->GetFormID(), ref ? ref->GetFormID() : 0, std::move(name), *at});
		}
		std::ranges::sort(out, [](const auto& a, const auto& b) {
			return std::tie(a.quest, a.reference, a.name) < std::tie(b.quest, b.reference, b.name);
		});
		return out;
	}

	void Publish(const std::vector<Quest>& a_quests, double a_holdSeconds)
	{
		if (a_holdSeconds > 0) {
			g_next = Frame::Now() + a_holdSeconds;
		}
		std::string lua = "__WBQuests({";
		for (const auto& q : a_quests) {
			lua += std::format("{{ id = {}, title = {}, header = {}, text = {}, failed = {}, objectives = {{", q.id, LuaQuote(q.title),
				LuaQuote(q.header), LuaQuote(q.text), q.failed ? "true" : "false");
			for (const auto& [text, done, failed] : q.objectives) {
				lua += std::format("{{ text = {}, done = {} }},", LuaQuote(text), done ? "true" : "false");
			}
			lua += "} },";
		}
		lua += "})";
		if (lua == g_last) {
			return;
		}
		g_last = lua;
		WowUi::Run(lua);
		// What changed, as the client says it: a quest accepted, an objective done, a quest completed or failed.
		std::map<RE::FormID, std::vector<bool>> now;
		for (const auto& q : a_quests) {
			auto& done = now[q.id];
			for (const auto& o : q.objectives) {
				done.push_back(std::get<1>(o));
			}
			if (g_quiet) {
				continue;
			}
			const auto was = g_shown.find(q.id);
			if (was == g_shown.end()) {
				System(std::format("Quest accepted: {}", q.title));
				continue;
			}
			for (std::size_t i = 0; i < q.objectives.size() && i < was->second.size(); ++i) {
				if (std::get<1>(q.objectives[i]) && !was->second[i]) {
					WowUi::Run(std::format("__WBFire('UI_INFO_MESSAGE', {})", LuaQuote(std::get<0>(q.objectives[i]) + " (Complete)")));
				}
			}
		}
		if (!g_quiet) {
			for (const auto& [id, done] : g_shown) {
				if (!now.contains(id)) {
					auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(id);
					if (quest && quest->GetFullName()) {
						System(std::string(quest->GetFullName()) + (quest->IsCompleted() ? " completed." : " failed."));
					}
				}
			}
		}
		g_shown = std::move(now);
		g_quiet = false;
	}

	void Tick()
	{
		const double now = Frame::Now();
		auto*        player = RE::PlayerCharacter::GetSingleton();
		if (now < g_next || !player || !player->Is3DLoaded() || !WowUi::Ready()) {
			return;
		}
		g_next = now + 2.0;
		Publish(Gather());
	}

	void Reset()
	{
		g_quiet = true;
		g_shown.clear();
		g_last.clear();
		g_next = 0;
	}
}
