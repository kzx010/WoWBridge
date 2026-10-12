#pragma once

#include <nlohmann/json.hpp>

namespace Console
{
	bool Command(std::string a_command, std::string a_arg = {}, std::string a_value = {}); // validated panel/console commands
	bool Install();   // register wb in the native console command table
	void Print(std::string_view a_message);
	void CancelPending();   // load/new game: a form reply must not mutate the next game
	nlohmann::json Snapshot();   // SelfTest: actual native command executions and their last result
	// Compile and run a console command (works from the main menu, e.g. `coc`).
	void Execute(std::string_view a_command, RE::TESObjectREFR* a_target = nullptr);
}
