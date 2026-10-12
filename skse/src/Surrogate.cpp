#include "Surrogate.h"

namespace Surrogate
{
	namespace
	{
		bool g_hidden = false;

		void Apply(RE::NiAVObject* a_root, bool a_hidden)
		{
			if (!a_root) {
				return;
			}
			RE::BSVisit::TraverseScenegraphGeometries(a_root, [a_hidden](RE::BSGeometry* a_geo) {
				a_geo->SetAppCulled(a_hidden);
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}
	}

	void SetBodyHidden(bool a_hidden)
	{
		g_hidden = a_hidden;
		if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
			Apply(pc->Get3D(false), a_hidden);
			Apply(pc->Get3D(true), a_hidden);
		}
	}

	bool BodyHidden() { return g_hidden; }

	bool SetSex(bool a_female)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* base = player ? player->GetActorBase() : nullptr;
		auto* race = player ? player->GetRace() : nullptr;
		if (!base || !race || !race->defaultVoiceTypes[a_female ? 1 : 0]) {
			logger::warn("surrogate: player base or matching race voice unavailable");
			return false;
		}
		base->SetFlagBit(RE::ACTOR_BASE_DATA::Flag::kFemale, a_female, true);
		base->voiceType = race->defaultVoiceTypes[a_female ? 1 : 0];   // the virtual setter's slot differs on 1.7.104
		logger::info("surrogate: {} voice {:08X}", a_female ? "female" : "male", base->voiceType->GetFormID());
		Refresh();
		return true;
	}

	void Refresh()
	{
		if (g_hidden) {
			SetBodyHidden(true);
		}
	}
}
