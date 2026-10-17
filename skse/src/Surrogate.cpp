#include "Surrogate.h"
#include "Character.h"
#include "Config.h"
#include "render/PlayerWeapons.h"
#include "wowcore/dbc.hpp"
#include "wowcore/model.hpp"
#include "wowcore/mpq.hpp"

namespace Surrogate
{
	namespace
	{
		bool g_hidden = false;
		std::optional<float> g_nativeScale;

		void Apply(RE::NiAVObject* a_root, bool a_hidden)
		{
			if (!a_root) {
				return;
			}
			RE::BSVisit::TraverseScenegraphGeometries(a_root, [a_hidden](RE::BSGeometry* a_geo) {
				a_geo->SetAppCulled(a_hidden && !PlayerWeapons::KeepsVisible(a_geo));
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}
	}

	void SetBodyHidden(bool a_hidden)
	{
		g_hidden = a_hidden;
		if (!a_hidden) PlayerWeapons::Reset();
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
		// Keep the save's Skyrim race and abilities; Tauren dialogue uses the Orc race's deep voices.
		if (Character::Looks().race == 6) if (auto* orc = RE::TESForm::LookupByID<RE::TESRace>(0x13747)) race = orc;
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

	std::optional<Size> ClientSize(std::uint32_t a_race, std::uint32_t a_sex)
	{
		static const auto sizes = [] {
			std::map<std::pair<std::uint32_t, std::uint32_t>, Size> out;
			try {
				const wowcore::Mpq mpq(Config::ClientData());
				const auto races = wowcore::Dbc::load(mpq, R"(DBFilesClient\ChrRaces.dbc)");
				const auto displays = wowcore::Dbc::load(mpq, R"(DBFilesClient\CreatureDisplayInfo.dbc)");
				const auto models = wowcore::Dbc::load(mpq, R"(DBFilesClient\CreatureModelData.dbc)");
				if (!races || !displays || !models) throw std::runtime_error("race size tables unavailable");
				for (const auto race : {1u,3u,4u,6u}) for (const auto sex : {0u, 1u}) try {
					const auto r = races->rowOf(race);
					if (!r) throw std::runtime_error("race size row unavailable");
					Size size;
					size.display = races->u32(*r, 4 + sex);
					const auto d = displays->rowOf(size.display);
					if (!d) throw std::runtime_error("race display unavailable");
					size.model = displays->u32(*d, 1);
					const auto m = models->rowOf(size.model);
					if (!m) throw std::runtime_error("race model unavailable");
					size.modelScale = displays->f32(*d, 4) * models->f32(*m, 4);
					const auto body = wowcore::loadModel(mpq, models->str(*m, 2));
					const int stand = body ? wowcore::findSequence(*body, 0) : -1;
					if (stand < 0) throw std::runtime_error("race Stand bounds unavailable");
					const auto& bounds = body->sequences[stand];
					size.height = (bounds.boundMax.z - bounds.boundMin.z) * size.modelScale;
					std::vector<glm::mat4> bones;
					wowcore::poseBones(*body, stand, 0, 0, bones);
					const auto helm = wowcore::attachmentMatrix(*body, bones, 11);
					size.helm = helm ? (*helm)[3].z * size.modelScale : 0;
					if (!std::isfinite(size.helm) || size.helm <= 0) throw std::runtime_error("race Helm attachment unavailable");
					if (!std::isfinite(size.height) || size.height <= 0 || !std::isfinite(size.modelScale) || size.modelScale <= 0)
						throw std::runtime_error("invalid race size");
					out[{race, sex}] = size;
					logger::info("surrogate: race {} sex {} display {} model {} scale {} Stand height {} yards, Helm {} yards",
						race, sex, size.display, size.model, size.modelScale, size.height, size.helm);
				} catch (const std::exception& e) { logger::error("surrogate: race {} sex {}: {}", race, sex, e.what()); }   // the others stand
			} catch (const std::exception& e) { logger::error("surrogate: {}", e.what()); out.clear(); }
			return out;
		}();
		const auto it = sizes.find({a_race, a_sex});
		return it == sizes.end() ? std::nullopt : std::optional(it->second);
	}

	namespace
	{
		// Skyrim's human races (Nord, Imperial, Breton, Redguard): their height multipliers by sex averaged (Skyrim.esm
		// RACE DATA: 1.03/1.03, 1/1, 1/.95, 1.005/1).
		constexpr float kHumanRaceHeight[2] = { (1.03f + 1.f + 1.f + 1.005f) / 4, (1.03f + 1.f + .95f + 1.f) / 4 };
		constexpr float kSkyrimHeight = 128.f;     // a height-1.0 actor's height in units (GetHeight), either sex
		constexpr float kHeadPerScale = 120.5f;    // its head node above its feet, either sex (wow_scale_probe, Oct 16)
		constexpr float kUnitsPerYard = 64.f;      // Avatar::kUnitsPerYard
	}

	std::optional<float> DrawScale(std::uint32_t a_race, std::uint32_t a_sex)
	{
		const auto size = ClientSize(a_race, a_sex), human = ClientSize(1, a_sex);
		if (!size || !human || a_sex > 1) return std::nullopt;
		return size->modelScale * kSkyrimHeight * kHumanRaceHeight[a_sex] / (human->height * kUnitsPerYard);
	}

	std::optional<float> HeadHeight(std::uint32_t a_race, std::uint32_t a_sex)
	{
		const auto size = ClientSize(a_race, a_sex), human = ClientSize(1, a_sex);
		if (!size || !human || a_sex > 1) return std::nullopt;
		return kHeadPerScale * kHumanRaceHeight[a_sex] * size->helm / human->helm;
	}

	std::optional<float> HeadFactor(std::uint32_t a_race, std::uint32_t a_sex)
	{
		const auto size = ClientSize(a_race, a_sex);
		const auto draw = DrawScale(a_race, a_sex), head = HeadHeight(a_race, a_sex);
		if (!size || !draw || !head) return std::nullopt;
		return *head / (size->helm / size->modelScale * *draw * kUnitsPerYard);
	}

	std::optional<float> ReferenceScale(std::uint32_t a_race, std::uint32_t a_sex)
	{
		// Their own Skyrim size for every race: Skyrim moves, reaches and casts from a larger actor faster, farther and
		// higher, and 1.12's races all run 7 yards a second; the Human the player is drawn as stands as tall as Skyrim's
		// average human. The chase camera follows their 1.12 head instead (HeadFactor). (A head-matched surrogate was
		// tried on October 16: their run speed grew with its scale.)
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !ClientSize(a_race, a_sex)) return std::nullopt;
		return g_nativeScale ? *g_nativeScale : player->GetReferenceRuntimeData().refScale / 100.f;
	}

	bool SetSize(std::uint32_t a_race, std::uint32_t a_sex)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		const auto target = ReferenceScale(a_race, a_sex);
		if (!player || !target) return false;
		// The save's own reference scale (also what an older WoWBridge left scaled is returned to).
		const float reference = player->GetReferenceRuntimeData().refScale / 100.f;
		if (!g_nativeScale) g_nativeScale = reference;
		const float scale = *target;
		if (std::abs(reference - scale) > .005f) {
			player->SetScale(scale);
			player->DoReset3D(false);   // their height and character controller follow the scale only when their 3D is rebuilt
		}
		return true;
	}

	void RestoreSize()
	{
		if (auto* player = RE::PlayerCharacter::GetSingleton(); player && g_nativeScale)
			if (std::abs(player->GetReferenceRuntimeData().refScale / 100.f - *g_nativeScale) > .005f) {
				player->SetScale(*g_nativeScale);
				player->DoReset3D(false);
			}
	}
	void PrepareLoad() { g_nativeScale.reset(); }
	void SaveSize(SKSE::SerializationInterface* a_intfc)
	{
		if (g_nativeScale && (!a_intfc->OpenRecord(kSizeRecord, 1) ||
			!a_intfc->WriteRecordData(&*g_nativeScale, sizeof(float)))) logger::error("surrogate: native scale was not saved");
	}
	void LoadSize(SKSE::SerializationInterface* a_intfc, std::uint32_t a_length, std::uint32_t a_version)
	{
		float scale = 0;
		if (a_version == 1 && a_length == sizeof(scale) && a_intfc->ReadRecordData(&scale, sizeof(scale)) == sizeof(scale) &&
			std::isfinite(scale) && scale > 0) g_nativeScale = scale;
		else logger::error("surrogate: invalid saved native scale");
	}

	void Refresh()
	{
		if (g_hidden) {
			SetBodyHidden(true);
		}
	}
}
