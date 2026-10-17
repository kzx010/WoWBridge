#pragma once

// Their Skyrim quests in the client's Quest Log and quest watch: the quests Skyrim's journal shows (running, with an
// objective displayed), grouped by Skyrim's kind of quest, their journal text and objectives; a new quest, a
// completed objective, a completed or failed quest told as the client tells them. Main thread.
namespace Quests
{
	void Tick();    // every two seconds: what changed is published
	void Reset();   // a load or a new game: published again, quietly
	struct Quest
	{
		RE::FormID                                   id = 0;
		std::string                                  title, header, text;
		std::vector<std::tuple<std::string, bool, bool>> objectives;   // text, done, failed
		bool                                         failed = false;
	};
	void Publish(const std::vector<Quest>& a_quests, double a_holdSeconds = 0);   // the suite's quests hold the log a while
	struct CompassTarget { RE::FormID quest, reference; std::string name; RE::NiPoint3 position; };
	// Live engine targets, independent of the quest-log fixture/hold timer.
	std::vector<CompassTarget> CompassTargets(RE::PlayerCharacter* a_player, bool a_exteriorMap = false);
	std::optional<RE::NiPoint3> CompassPosition(RE::PlayerCharacter* a_player, RE::TESObjectREFR* a_ref, const RE::TeleportPath* a_path, bool a_exteriorMap = false);
}
