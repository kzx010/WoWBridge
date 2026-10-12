#include "SpellVisuals.h"

#include "Avatar.h"
#include "Config.h"

#include "wowcore/character.hpp"
#include "wowcore/dbc.hpp"
#include "wowcore/mpq.hpp"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace SpellVisuals
{
	namespace
	{
		// Her caster-form book (data/build_catalog.py SLICE_SPELLS): Wrath, Moonfire, Rejuvenation, Healing Touch,
		// Entangling Roots, Hurricane; Starfire, Regrowth, Faerie Fire, Hibernate, Barkskin, Innervate, Mark and Gift of the
		// Wild, Thorns, Tranquility, Rebirth, Remove Curse, Abolish Poison (and its tick), Cure Poison, Soothe Animal; her
		// forms (the druid's morph puff) and the feral abilities (service feral.FORM_SPELLS, ABILITIES).
		constexpr std::uint32_t kSpells[] = { 9912, 9835, 25299, 25297, 9853, 17402, 25298, 9858, 9907, 18658, 22812, 29166, 9885,
			21850, 9910, 9863, 20748, 2782, 2893, 3137, 8946, 9901, 5487, 9634, 768, 783, 1066, 9881, 9908, 6795, 9898, 8983, 5229,
			22896, 9850, 9830, 9904, 9896, 31018, 9913, 9827, 9867, 9846, 9821, 9892, 17392, 20580 };

		struct KitAssets
		{
			std::vector<std::pair<int, const Asset*>> effects;   // slot, model
		};
		struct SpellAssets
		{
			KitAssets    kits[5];
			const Asset* missile = nullptr;
			const Asset* area = nullptr;
			float        areaSize = 0;   // yards the area model is made for (0: as made)
		};
		struct Library
		{
			std::map<std::string, std::unique_ptr<Asset>> assets;   // by lower-case path
			std::map<std::string, wowcore::Image>         images;
			std::map<std::uint32_t, SpellAssets>          spells;
		};
		std::atomic<const Library*> g_library = nullptr;
		std::mutex                  g_loading;   // one loading at a time
		std::mutex                  g_wantLock;  // the spells wanted so far
		std::set<std::uint32_t>     g_wanted(std::begin(kSpells), std::end(kSpells));

		std::string Lower(std::string a_s)
		{
			std::ranges::transform(a_s, a_s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_s;
		}

		const Asset* LoadAsset(Library& a_lib, const wowcore::Mpq& a_mpq, const std::string& a_path, float a_scale)
		{
			const auto key = Lower(a_path);
			if (auto it = a_lib.assets.find(key); it != a_lib.assets.end()) {
				return it->second.get();
			}
			auto model = wowcore::loadModel(a_mpq, a_path);
			if (!model) {
				logger::warn("spell visuals: no model {}", a_path);
				a_lib.assets[key] = nullptr;   // the tables name a few files the client does not have; nothing shows
				return nullptr;
			}
			auto asset = std::make_unique<Asset>();
			asset->model = std::move(*model);
			asset->scale = a_scale;
			const auto image = [&](const std::string& a_file) -> const wowcore::Image* {
				if (a_file.empty()) {
					return nullptr;
				}
				auto it = a_lib.images.find(Lower(a_file));
				if (it == a_lib.images.end()) {
					it = a_lib.images.emplace(Lower(a_file), wowcore::loadImage(a_mpq, a_file)).first;
				}
				return it->second.rgba.empty() ? nullptr : &it->second;
			};
			const auto& m = asset->model;
			for (const auto& t : m.textures) {
				asset->textureImages.push_back(image(t.filename));
			}
			for (const auto& b : m.batches) {
				const auto lookup = b.textureIndex < m.textureLookup.size() ? m.textureLookup[b.textureIndex] : 0;
				asset->batchImages.push_back(lookup < asset->textureImages.size() ? asset->textureImages[lookup] : nullptr);
			}
			return (a_lib.assets[key] = std::move(asset)).get();
		}

		void Load()
		{
			std::lock_guard    one(g_loading);
			const auto         start = std::chrono::steady_clock::now();
			const wowcore::Mpq mpq(Config::ClientData());
			const wowcore::ClientDb db(mpq);
			auto               lib = std::make_unique<Library>();
			std::set<std::uint32_t> wanted;
			{
				std::lock_guard lock(g_wantLock);
				wanted = g_wanted;
			}
			for (const auto spell : wanted) {
				const auto  kits = db.spellVisualKits(spell);
				auto&       s = lib->spells[spell];
				const std::vector<wowcore::KitEffect>* lists[5] = { &kits.precast, &kits.cast, &kits.impact, &kits.state, &kits.channel };
				for (int k = 0; k < 5; ++k) {
					for (const auto& e : *lists[k]) {
						if (const auto* asset = LoadAsset(*lib, mpq, e.model, e.scale)) {
							s.kits[k].effects.emplace_back(e.slot, asset);
						}
					}
				}
				if (!kits.missile.empty()) {
					s.missile = LoadAsset(*lib, mpq, kits.missile, 1.0f);
				}
				if (!kits.area.empty()) {
					s.area = LoadAsset(*lib, mpq, kits.area, kits.areaScale);
					s.areaSize = kits.areaSize;
				}
			}
			if (const auto up = db.levelUpEffect()) {   // her level up's light (a unit effect the client hardcodes)
				if (const auto* asset = LoadAsset(*lib, mpq, up->model, 1.0f)) {
					lib->spells[kLevelUp].kits[static_cast<int>(Kit::Impact)].effects.emplace_back(2, asset);   // at her feet
				}
			}
			std::size_t models = 0;
			for (const auto& [path, asset] : lib->assets) {
				models += asset != nullptr;
			}
			logger::info("spell visuals: {} spells, {} effect models, {} images in {} ms", lib->spells.size(), models, lib->images.size(),
				std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
			// The one before stays: effects playing hold its models (a library is made once or twice a session).
			g_library = lib.release();
		}

		// What the main thread asks for, applied by the render thread's next step.
		struct Request
		{
			enum class Kind
			{
				Play,
				Stop,
				Missile,
				Area,
				Clear
			} kind = Kind::Play;
			Handle          handle = 0;
			std::uint32_t   spell = 0;
			Kit             kit = Kit::Cast;
			RE::ActorHandle on;
			bool            lasting = false;
			float           seconds = 0;   // a missile's flight; an area's radius (yards)
			RE::NiPoint3    at;            // an area's place
		};
		std::mutex           g_lock;
		std::vector<Request> g_requests;
		std::atomic<Handle>  g_next = 1;

		// One effect model running (render thread).
		struct Running
		{
			Handle                             handle = 0;
			const Asset*                       asset = nullptr;
			int                                slot = 0;
			RE::ActorHandle                    on;            // empty: her
			bool                               lasting = false, stopped = false, missile = false, area = false;
			float                              areaScale = 1;   // its radius over the size it is made for
			float                              age = 0;       // seconds
			int                                seq = 0;       // the sequence playing (index), since seqStart (seconds of age)
			float                              seqStart = 0;
			bool                               decaying = false;
			float                              flight = 0;    // missile
			glm::vec3                          from{ 0.0f }, lastTo{ 0.0f };
			std::unique_ptr<wowcore::Emitters> emitters;
			std::vector<glm::mat4>             bones;
		};
		std::vector<Running>   g_running;
		std::atomic<std::uint32_t> g_count = 0;

		// Her attachment points for a kit's slots: head, chest, base, left hand, right hand, breath, the specials, the world.
		constexpr std::uint32_t kHerPoints[] = { 11, 15, 19, 21, 22, 17, 23, 24, 25, 19 };

		// A Skyrim actor's skeleton node for a slot, else a height on it.
		RE::NiPoint3 ActorPoint(RE::Actor* a_actor, int a_slot)
		{
			const char* const kNodes[] = { "NPC Head [Head]", "NPC Spine2 [Spn2]", nullptr, "NPC L Hand [LHnd]", "NPC R Hand [RHnd]",
				"NPC Head [Head]", nullptr, nullptr, nullptr, nullptr };
			const float kHeights[] = { 0.95f, 0.6f, 0.0f, 0.5f, 0.5f, 0.9f, 0.6f, 0.6f, 0.6f, 0.0f };
			if (auto* root = a_actor->Get3D(false); root && kNodes[a_slot]) {
				if (auto* node = root->GetObjectByName(kNodes[a_slot])) {
					return node->world.translate;
				}
			}
			return a_actor->GetPosition() + RE::NiPoint3{ 0, 0, a_actor->GetHeight() * kHeights[a_slot] };
		}

		glm::mat4 Place(const glm::vec3& a_at, float a_turn, float a_scale)
		{
			return glm::scale(glm::rotate(glm::translate(glm::mat4(1.0f), a_at), a_turn, glm::vec3(0, 0, 1)), glm::vec3(a_scale));
		}

		void Start(const Library& a_lib, const Request& a_r)
		{
			const auto spell = a_lib.spells.find(a_r.spell);
			if (spell == a_lib.spells.end()) {
				return;
			}
			if (a_r.kind == Request::Kind::Area) {
				if (!spell->second.area) {
					return;
				}
				Running r;
				r.handle = a_r.handle, r.asset = spell->second.area, r.lasting = true, r.area = true;
				r.from = glm::vec3(a_r.at.x, a_r.at.y, a_r.at.z);
				r.areaScale = spell->second.areaSize > 0 ? a_r.seconds / spell->second.areaSize : 1.0f;
				r.emitters = std::make_unique<wowcore::Emitters>(r.asset->model, static_cast<std::uint32_t>(r.handle));
				g_running.push_back(std::move(r));
				return;
			}
			if (a_r.kind == Request::Kind::Missile) {
				if (!spell->second.missile) {
					return;
				}
				Running r;
				r.handle = a_r.handle, r.asset = spell->second.missile, r.on = a_r.on, r.missile = true, r.flight = std::max(a_r.seconds, 0.05f);
				r.emitters = std::make_unique<wowcore::Emitters>(r.asset->model, static_cast<std::uint32_t>(r.handle));
				r.from = glm::vec3(std::numeric_limits<float>::quiet_NaN());   // her right hand, at the first step
				g_running.push_back(std::move(r));
				return;
			}
			for (const auto& [slot, asset] : spell->second.kits[static_cast<int>(a_r.kit)].effects) {
				Running r;
				r.handle = a_r.handle, r.asset = asset, r.slot = slot, r.on = a_r.on, r.lasting = a_r.lasting;
				r.emitters = std::make_unique<wowcore::Emitters>(asset->model, static_cast<std::uint32_t>(r.handle * 31 + slot));
				g_running.push_back(std::move(r));
			}
		}
	}

	void Want(const std::vector<std::uint32_t>& a_spells)
	{
		{
			std::lock_guard lock(g_wantLock);
			const auto      before = g_wanted.size();
			g_wanted.insert(a_spells.begin(), a_spells.end());
			if (g_wanted.size() == before) {
				return;
			}
		}
		Init();
	}

	void Init()
	{
		std::thread([] {
			try {
				Load();
			} catch (const std::exception& e) {   // an exception leaving a thread ends the game
				logger::error("{}: loading failed: {}", "spell visuals", e.what());
			}
		}).detach();
	}

	Handle Play(std::uint32_t a_spell, Kit a_kit, RE::ActorHandle a_on, bool a_lasting)
	{
		const Handle h = g_next++;
		std::lock_guard lock(g_lock);
		g_requests.push_back({ Request::Kind::Play, h, a_spell, a_kit, a_on, a_lasting });
		return h;
	}

	void Stop(Handle a_handle)
	{
		if (a_handle) {
			std::lock_guard lock(g_lock);
			g_requests.push_back({ Request::Kind::Stop, a_handle });
		}
	}

	void Missile(std::uint32_t a_spell, RE::ActorHandle a_target, float a_seconds)
	{
		std::lock_guard lock(g_lock);
		g_requests.push_back({ Request::Kind::Missile, g_next++, a_spell, Kit::Cast, a_target, false, a_seconds });
	}

	Handle Area(std::uint32_t a_spell, const RE::NiPoint3& a_at, float a_radiusYards)
	{
		const Handle    h = g_next++;
		std::lock_guard lock(g_lock);
		Request         r{ Request::Kind::Area, h, a_spell, Kit::Channel, {}, true, a_radiusYards };
		r.at = a_at;
		g_requests.push_back(r);
		return h;
	}

	void Clear()
	{
		std::lock_guard lock(g_lock);
		g_requests.push_back({ Request::Kind::Clear });
	}

	std::uint32_t Running() { return g_count; }

	void Step(float a_dt, const Her& a_her, const glm::vec3& a_eye, const glm::vec3& a_right, const glm::vec3& a_up,
		std::vector<DrawModel>& a_models, std::vector<DrawQuads>& a_quads)
	{
		const auto* lib = g_library.load();
		std::vector<Request> requests;
		{
			std::lock_guard lock(g_lock);
			requests.swap(g_requests);
		}
		if (!lib) {
			return;
		}
		for (const auto& r : requests) {
			if (r.kind == Request::Kind::Stop) {
				for (auto& run : g_running) {
					run.stopped = run.stopped || run.handle == r.handle;
				}
			} else if (r.kind == Request::Kind::Clear) {
				g_running.clear();
			} else {
				Start(*lib, r);
			}
		}
		for (auto it = g_running.begin(); it != g_running.end();) {
			auto&       r = *it;
			const auto& m = r.asset->model;
			r.age += a_dt;
			// A lasting effect made with a Hold sequence (Entangling Roots' state) forms once (Stand), holds for as long as
			// its aura lasts (Hold, looping) and fades as it ends (Decay); one without loops its Stand.
			const int hold = r.lasting ? wowcore::findSequence(m, 158) : -1;
			const int decay = hold >= 0 ? wowcore::findSequence(m, 159) : -1;
			if (r.stopped && decay >= 0 && !r.decaying) {
				r.stopped = false, r.decaying = true, r.seq = decay, r.seqStart = r.age;
			}
			const auto  lengthOf = [&](int a_seq) { return m.sequences.empty() ? 1.0f : std::max(m.sequences[a_seq].duration, 1u) / 1000.0f; };
			if (hold >= 0 && !r.decaying && r.seq != hold && r.age - r.seqStart >= lengthOf(r.seq)) {
				r.seq = hold, r.seqStart = r.age;
			}
			const float length = lengthOf(r.seq);
			const float into = r.age - r.seqStart;
			if (!r.lasting && !r.missile && r.age >= length) {
				// Once through: the client's impact kit removes the model instance, including its particles. Adding
				// their lifespan here kept the morph puff past its 2.8 s; Stop on a lasting effect still lets them finish.
				it = g_running.erase(it);
				continue;
			}
			if (r.decaying && into >= length) {
				r.stopped = true;
			}
			const bool looping = (r.lasting && !r.decaying && (hold < 0 || r.seq == hold)) || r.missile;
			// Where it is: on her attachment point, on its actor's node, or along the missile's flight.
			std::optional<glm::mat4> world;
			const float scale = Avatar::kUnitsPerYard * r.asset->scale;
			auto*       actor = r.on ? r.on.get().get() : nullptr;
			if (r.area) {
				// On the ground where it landed, a little above it: its shadow is a plane at its feet, and Skyrim's ground
				// is rougher than the client's.
				world = Place(r.from + glm::vec3(0.0f, 0.0f, 8.0f), 0.0f, scale * r.areaScale);
			} else if (r.missile) {
				const glm::mat4* hand = nullptr;
				std::optional<glm::mat4> handAt;
				if (a_her.body && a_her.bones) {
					handAt = wowcore::attachmentMatrix(*a_her.body, *a_her.bones, 22);
					hand = handAt ? &*handAt : nullptr;
				}
				if (std::isnan(r.from.x)) {
					r.from = hand ? glm::vec3(a_her.world * (*hand)[3]) : glm::vec3(a_her.world[3]) + glm::vec3(0, 0, 100);
				}
				if (actor) {
					const auto chest = ActorPoint(actor, 1);
					r.lastTo = glm::vec3(chest.x, chest.y, chest.z);
				}
				const float     t = std::clamp(r.age / r.flight, 0.0f, 1.0f);
				const glm::vec3 at = glm::mix(r.from, r.lastTo, t), dir = r.lastTo - r.from;
				world = Place(at, std::atan2(dir.y, dir.x), scale);   // the model's +x along its flight
				if (t >= 1.0f) {
					r.stopped = true;
				}
			} else if (!r.on) {
				if (a_her.body && a_her.bones) {
					if (const auto at = wowcore::attachmentMatrix(*a_her.body, *a_her.bones, kHerPoints[r.slot])) {
						world = a_her.world * *at * glm::scale(glm::mat4(1.0f), glm::vec3(r.asset->scale));
					}
				}
			} else if (actor && actor->Is3DLoaded()) {
				const auto p = ActorPoint(actor, r.slot);
				world = Place(glm::vec3(p.x, p.y, p.z), glm::half_pi<float>() - actor->GetHeading(false), scale);
			} else {
				r.stopped = true;
			}
			if (world) {
				// Its pose, billboards facing the eye (in its own space), and its emitters moved on.
				const glm::mat4 inv = glm::inverse(*world);
				const glm::vec3 eye = glm::vec3(inv * glm::vec4(a_eye, 1.0f));
				const glm::vec3 up = glm::normalize(glm::vec3(inv * glm::vec4(a_up, 0.0f)));
				const float     timeMs = looping ? std::fmod(into * 1000.0f, length * 1000.0f) : std::min(into, length) * 1000.0f;
				wowcore::poseBones(m, r.seq, timeMs, r.age * 1000.0f, r.bones, &eye, &up);
				r.emitters->update(a_dt, r.bones, *world, r.seq, timeMs, r.age * 1000.0f, !r.stopped);
				if (!r.stopped && (a_her.visible || r.on || r.area)) {
					a_models.push_back({ r.asset, r.bones, *world, timeMs, r.seq });
				}
			} else {
				r.emitters->update(a_dt, r.bones, glm::mat4(1.0f), 0, 0, 0, false);
			}
			auto& q = a_quads.emplace_back();
			q.asset = r.asset;
			r.emitters->quads(a_right, a_up, q.quads);
			if (r.stopped && r.emitters->idle()) {
				it = g_running.erase(it);
			} else {
				++it;
			}
		}
		g_count = static_cast<std::uint32_t>(g_running.size());
	}
}
