#include "Avatar.h"

#include "Character.h"
#include "Melee.h"

#include "Config.h"
#include "Frame.h"
#include "Surrogate.h"
#include "render/ItemPreview.h"

#include "wowcore/mpq.hpp"

namespace Avatar
{
	namespace
	{
		std::atomic<const Loaded*> g_data = nullptr, g_mountData = nullptr;
		std::atomic<std::uint32_t> g_mountDisplay = 0;
		std::atomic<bool>          g_suspended = false;
		bool                       g_drawn = false;   // her sheath state (main thread)
		bool                       g_shownDrawn = false;   // where the weapon is drawn: in her hand or not, so far
		double                     g_drawSwitchAt = -1;    // a sheath animation's moment the weapon changes place

		struct Action
		{
			std::int32_t animation = -1;
			bool         loop = false;
			double       since = 0;
			bool         upperBody = false;
		};
		Action g_action;   // main thread
		std::atomic<std::uint32_t> g_display = 0;   // her form's creature display; 0: herself
		std::mutex                 g_lock;
		Pose                       g_pose;
        struct MountCameraState {
            std::vector<std::pair<RE::Setting*,float>> settings;
            RE::ThirdPersonState* third=nullptr;
            float expected=0, actual=0, zoom=0, mountedZoom=0;
        };
        std::optional<MountCameraState> g_mountCamera;
        void MountCamera(float height) {
            if (g_mountCamera) {
                for (const auto& [setting,value] : g_mountCamera->settings) setting->data.f=value;
                if (auto* third=g_mountCamera->third) {
                    third->posOffsetExpected.z=g_mountCamera->expected;
                    third->posOffsetActual.z=g_mountCamera->actual;
                    if (std::abs(third->targetZoomOffset-g_mountCamera->mountedZoom)<.001f)
                        third->targetZoomOffset=g_mountCamera->zoom;
                }
                g_mountCamera.reset();
            }
            if (height<=0) return;
            MountCameraState saved;
            auto* ini=RE::INISettingCollection::GetSingleton();
            for (const auto name : {"fOverShoulderPosZ:Camera","fOverShoulderCombatPosZ:Camera"}) {
                if (auto* setting=ini ? ini->GetSetting(name) : nullptr) {
                    saved.settings.emplace_back(setting,setting->data.f); setting->data.f+=height;
                }
            }
            auto* camera=RE::PlayerCamera::GetSingleton();
            if (camera && camera->currentState && camera->currentState->id==RE::CameraState::kThirdPerson) {
                saved.third=static_cast<RE::ThirdPersonState*>(camera->currentState.get());
                auto* third=saved.third;
                saved.expected=third->posOffsetExpected.z; saved.actual=third->posOffsetActual.z;
                saved.zoom=third->targetZoomOffset;
                saved.mountedZoom=std::max(saved.zoom,(8.f*kUnitsPerYard-155.f)/(15.f*kUnitsPerYard-155.f));
                third->posOffsetExpected.z+=height; third->posOffsetActual.z+=height;
                third->targetZoomOffset=saved.mountedZoom;
            }
            g_mountCamera=std::move(saved);
        }

		// What each batch draws with: by the texture's type - the composited body (1), hair (6), a cape or an item's own
		// skin (2) - or the file it names (0).
		void Resolve(Loaded& a_data, Part& a_part, const wowcore::Mpq& a_mpq)
		{
			const auto& m = a_part.model;
			a_part.batchImages.clear();
			for (const auto& b : m.batches) {
				const auto lookup = b.textureIndex < m.textureLookup.size() ? m.textureLookup[b.textureIndex] : 0;
				const auto* tex = lookup < m.textures.size() ? &m.textures[lookup] : nullptr;
				std::string path;
				if (!tex) {
					a_part.batchImages.push_back(nullptr);
					continue;
				}
				if (tex->type == 1 && !a_part.attached) {
					a_part.batchImages.push_back(&a_data.look.skin);
					continue;
				}
				path = tex->type == 0 ? tex->filename : tex->type == 6 ? a_data.look.hair :
				       tex->type == 2 ? (a_part.attached ? a_part.attached->texture : a_data.look.cape) : std::string();
				if (const auto r = a_data.look.replaceableTextures.find(tex->type); path.empty() && r != a_data.look.replaceableTextures.end()) {
					path = r->second;   // a creature's skin (her form: types 11-13)
				}
				if (path.empty()) {
					a_part.batchImages.push_back(nullptr);
					continue;
				}
				auto it = a_data.images.find(path);
				if (it == a_data.images.end()) {
					it = a_data.images.emplace(path, wowcore::loadImage(a_mpq, path)).first;
				}
				a_part.batchImages.push_back(it->second.rgba.empty() ? nullptr : &it->second);
			}
		}

		// Her looks, kept once dressed: herself (0) and each form's creature (its display), so that a shift is at once, as
		// the client's (dressing one takes a second and more). Filled by the loading thread; never freed but for her own
		// look when her gear changes, and that one only at the next change (the renderer may be drawing it this frame).
		std::mutex                                             g_looksLock;
		std::map<std::uint32_t, std::unique_ptr<const Loaded>> g_looks;
		std::set<std::uint32_t>                                g_mountRequests;
		std::mutex                                             g_loading;   // one dressing at a time
		std::uint64_t                                          g_outfitRevision = 0;   // protected by g_looksLock

		// She is shown as the look for the display she is in, once it is dressed.
		void Publish()
		{
			std::lock_guard lock(g_looksLock);
			if (const auto it = g_looks.find(g_display.load()); it != g_looks.end()) {
				g_data = it->second.get();
			}
			if (const auto it = g_looks.find(g_mountDisplay.load()); g_mountDisplay && it != g_looks.end()) {
				g_mountData = it->second.get();
			}
		}

		void Load(std::uint32_t display, const std::vector<wowcore::Worn>& a_items, const wowcore::Appearance& a_looks, std::uint64_t a_revision)
		{
			if (!display) {
				std::lock_guard lock(g_looksLock);
				if (a_revision != g_outfitRevision) { return; }
			}
			if (Character::Get().name.empty()) {   // no character file (logged): nothing to dress
				return;
			}
			const auto       start = std::chrono::steady_clock::now();
			const wowcore::Mpq mpq(Config::ClientData());
			auto             data = std::make_unique<Loaded>();
			data->display = display;
			data->appearance = a_looks;
			data->items = a_items;
			// In a form she is its creature (wowcore::dressCreature: its model, skins and scale), else herself in her gear.
			auto       creature = display ? wowcore::dressCreature(mpq, display) : std::nullopt;
			if (creature) {
				data->look = std::move(*creature);
			} else {
				data->look = wowcore::dressCharacter(mpq, a_looks, data->items);
			}
			auto body = wowcore::loadModel(mpq, data->look.body);
			if (!body) {
				logger::error("avatar: no body model {}", data->look.body);
				return;
			}
			data->body.model = std::move(*body);
			data->selection = wowcore::loadImage(mpq, R"(Textures\UnitSelectTexture.blp)");
			data->areaOk = wowcore::loadImage(mpq, R"(Interface\SpellShadow\Spell-Shadow-Acceptable.blp)");
			data->areaBad = wowcore::loadImage(mpq, R"(Interface\SpellShadow\Spell-Shadow-Unacceptable.blp)");
			Resolve(*data, data->body, mpq);
			data->attached.reserve(data->look.attached.size());
			for (const auto& a : data->look.attached) {
				auto model = wowcore::loadModel(mpq, a.model);
				if (!model) {
					logger::warn("avatar: no model {}", a.model);
					continue;
				}
				auto& part = data->attached.emplace_back();
				part.model = std::move(*model);
				part.attached = &a;
				Resolve(*data, part, mpq);
			}
			logger::info("avatar: {} with {} geosets, {} attached models, {} images in {} ms", data->look.body, data->look.geosets.size(),
				data->attached.size(), data->images.size(),
				std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
			{
				std::lock_guard lock(g_looksLock);
				// Equipment may have changed while MPQ assets loaded. An obsolete body must never
				// replace the latest outfit (or publish after a newer request waiting on g_loading).
				if (!creature && a_revision != g_outfitRevision) { return; }
				static std::unique_ptr<const Loaded> retired;   // her look before her gear changed: drawn maybe this frame
				auto& look = g_looks[display];
				retired = std::move(look);
				look = std::move(data);
			}
			Publish();
		}

		// In the background, one at a time: a look dressed (and shown if she is in it).
		void Dress(std::vector<std::uint32_t> a_displays)
		{
			const auto items = Character::Worn();   // the request's outfit, not whatever a queued loader finds later
			const auto looks = Character::Looks();
			std::uint64_t revision;
			{
				std::lock_guard lock(g_looksLock);
				revision = g_outfitRevision;
			}
			std::thread([displays = std::move(a_displays), items, looks, revision] {
				std::lock_guard lock(g_loading);
				for (const auto display : displays) {
					try {
						Load(display, items, looks, revision);
					} catch (const std::exception& e) {   // an exception leaving a thread ends the game
						logger::error("{}: loading failed: {}", "avatar", e.what());
					}
                    std::lock_guard lock(g_looksLock);
                    g_mountRequests.erase(display);
				}
			}).detach();
		}

		// What the WoW body is doing, from what the Skyrim player does: standing, walking, running, backing away,
		// jumping and falling (AnimationData ids).
		struct Motion
		{
			std::uint32_t animation = 0;
			bool          loop = true;
			float         facing = 0;
			float         speed = 0;   // yards a second over the ground
		};

		// WoW's chase camera: centred behind her (no shoulder offset), out to 15 yards at most, starting about 5.5 yards
		// back (the 1.12 client's framing of her: 260 pixels a yard at 1440 lines). Zooming in past the nearest goes to
		// first person, as in WoW.
		bool g_cameraSettings = false, g_cameraZoomed = false;
		std::vector<std::pair<RE::Setting*, float>> g_nativeCameraFloats;
		std::vector<std::pair<RE::Setting*, bool>> g_nativeCameraBools;
		void WowCamera()
		{
			if (!g_cameraSettings) {
				g_cameraSettings = true;
				auto* ini = RE::INISettingCollection::GetSingleton();
				const std::pair<const char*, float> kValues[] = { { "fOverShoulderPosX:Camera", 0.0f }, { "fOverShoulderCombatPosX:Camera", 0.0f },
					{ "fOverShoulderHorsePosX:Camera", 0.0f }, { "fVanityModeMaxDist:Camera", 15.0f * kUnitsPerYard } };
				for (const auto& [name, value] : kValues) {
					if (auto* setting = ini ? ini->GetSetting(name) : nullptr) {
						g_nativeCameraFloats.emplace_back(setting, setting->data.f);
						setting->data.f = value;
					}
				}
				// Nor does WoW's screen pulse and blur as health runs low (the user: the screen shook as she was hit).
				auto* prefs = RE::INIPrefSettingCollection::GetSingleton();
				for (auto* setting : { ini ? ini->GetSetting("bLowHealthIModEnabled:Display") : nullptr,
						 prefs ? prefs->GetSetting("bLowHealthIModEnabled:Display") : nullptr }) {
					if (setting) {
						g_nativeCameraBools.emplace_back(setting, setting->data.b);
						setting->data.b = false;
					}
				}
			}
			auto*       camera = RE::PlayerCamera::GetSingleton();
			auto*       state = camera ? camera->currentState.get() : nullptr;
			if (!g_cameraZoomed && state && state->id == RE::CameraState::kThirdPerson) {
				g_cameraZoomed = true;
				auto*       third = static_cast<RE::ThirdPersonState*>(state);
				const float minDist = 155.0f, maxDist = 15.0f * kUnitsPerYard;   // fVanityModeMinDist .. MaxDist
				third->targetZoomOffset = third->currentZoomOffset = (5.5f * kUnitsPerYard - minDist) / (maxDist - minDist);
			}
		}

		Motion ChooseMotion(RE::PlayerCharacter* a_player, double a_dt)
		{
			static RE::NiPoint3 last = a_player->GetPosition();
			static bool         wasAirborne = false;
			static double       airborneFor = 0;
			const auto          pos = a_player->GetPosition();
			const float         dx = pos.x - last.x, dy = pos.y - last.y;
			last = pos;
			const float heading = a_player->GetHeading(false);
			static float lastHeading = heading;
			const float  turned = std::remainder(heading - lastHeading, 2.0f * RE::NI_PI);   // clockwise, radians
			lastHeading = heading;
			Motion m{ 0, true, heading };
			if (a_player->IsDead()) {
				return { 1, false, heading };
			}
			const float speed = a_dt > 0 ? std::hypot(dx, dy) / static_cast<float>(a_dt) / kUnitsPerYard : 0.0f;   // yards a second
			m.speed = speed;
			// Skyrim's sneak, shown as WoW's stealth stance and walk (without stealth's transparency: the user's choice).
			const bool sneaking = a_player->IsSneaking();
			// In Skyrim's furniture: a chair (SitChairMed) or a bed (SleepDown, Sleep, SleepUp).
			using Sit = RE::SIT_SLEEP_STATE;
			switch (a_player->AsActorState()->GetSitSleepState()) {
			case Sit::kWantToSit:
			case Sit::kWaitingForSitAnim:
			case Sit::kIsSitting:
				return { 103, true, heading };
			case Sit::kWantToSleep:
			case Sit::kWaitingForSleepAnim:
				return { 99, false, heading };
			case Sit::kIsSleeping:
				return { 100, true, heading };
			case Sit::kWantToWake:
				return { 101, false, heading };
			default:
				break;
			}
			if (a_player->AsActorState()->IsSwimming()) {
				const float along = (dx * std::sin(heading) + dy * std::cos(heading)) / std::max(std::hypot(dx, dy), 1e-4f);
				if (speed <= 0.5f) {
					return { 41, true, heading };   // SwimIdle
				}
				if (along < -0.3f) {
					return { 45, true, heading };   // SwimBackwards
				}
				return { 42, true, std::atan2(dx, dy) };   // Swim, towards where she goes
			}
			const bool  airborne = a_player->IsInMidair();
			if (airborne) {
				airborneFor = wasAirborne ? airborneFor + a_dt : 0.0;
				wasAirborne = true;
				return { airborneFor < 0.3 ? 37u : 38u, airborneFor >= 0.3, heading };   // JumpStart, then Jump (in the air)
			}
			wasAirborne = false;
			if (speed > 0.5f) {
				// Moving away from where she faces is backing up (WalkBackwards), her back to the way she goes; otherwise
				// she turns to the way she goes (sideways when strafing, half-way on a diagonal).
				const float along = (dx * std::sin(heading) + dy * std::cos(heading)) / std::max(std::hypot(dx, dy), 1e-4f);
				if (along < -0.3f) {
					m.animation = 13;
					m.facing = std::atan2(-dx, -dy);
				} else {
					// Run or Walk as her run/walk mode says (WoW's TOGGLERUN, numpad /; Skyrim's own run toggle), never by
					// how fast she goes: slowed, she runs slower (the animation's rate follows her speed).
					const auto* controls = RE::PlayerControls::GetSingleton();
					m.animation = !controls || controls->data.running ? 5u : 4u;
					m.facing = std::atan2(dx, dy);
				}
				if (sneaking && m.animation != 13) {
					m.animation = 119;   // StealthWalk
				}
			} else if (a_dt > 0 && std::abs(turned) / a_dt > 1.0) {
				m.animation = turned > 0 ? 12u : 11u;   // turning on the spot: ShuffleRight, ShuffleLeft
			} else if (sneaking) {
				m.animation = 120;   // StealthStand
			}
			return m;
		}
	}

	void Init(std::span<const std::uint32_t> a_forms)
	{
		std::vector<std::uint32_t> displays{ 0 };   // herself first, then her forms' creatures ahead of her first shift
		displays.insert(displays.end(), a_forms.begin(), a_forms.end());
		Dress(std::move(displays));
	}

	void SetForm(std::uint32_t a_display)
	{
		if (g_display.exchange(a_display) == a_display) {
			return;
		}
		bool dressed;
		{
			std::lock_guard lock(g_looksLock);
			dressed = g_looks.contains(a_display);
		}
		if (dressed) {
			Publish();
		} else {
			Dress({ a_display });
		}
	}

	void PrepareMount(std::uint32_t display)
	{
		if (!display || MountReady(display)) return;
        {
            std::lock_guard lock(g_looksLock);
            if (!g_mountRequests.insert(display).second) return;
        }
		Dress({display});
	}
	bool MountReady(std::uint32_t display)
	{
		std::lock_guard lock(g_looksLock);
		const auto it = g_looks.find(display);
		return display && it != g_looks.end() && wowcore::findSequence(it->second->body.model, 5) >= 0 &&
			std::ranges::any_of(it->second->body.model.attachments, [](const auto& a) { return a.id == 0; });
	}
	void SetMount(std::uint32_t display)
	{
		g_mountDisplay = display;
		if (!display) { g_mountData = nullptr; MountCamera(0); }
		else {
            Publish();
            const auto* data=g_mountData.load();
            if (data) {
                std::vector<glm::mat4> bones;
                wowcore::poseBones(data->body.model,wowcore::sequenceWithFallback(data->body.model,0),0,0,bones);
                const auto saddle=wowcore::mountedRiderMatrix(data->body.model,bones,data->look.scale);
                if (saddle) MountCamera((*saddle)[3].z*kUnitsPerYard);
            }
        }
	}
	const Loaded* MountData() { return g_mountData.load(); }

	void Redress()
	{
		{
			std::lock_guard lock(g_looksLock);
			++g_outfitRevision;
		}
		ItemPreview::Redress();
		Dress({ 0 });
	}

	void Tick()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->Is3DLoaded()) {
			return;
		}
		WowCamera();
		static double last = Frame::Now();
		const double  now = Frame::Now();
		const double  dt = std::min(now - last, 0.25);
		last = now;
		const bool ready = g_data.load() != nullptr;
		// Fully zoomed in, WoW does not draw your own model; nor does Skyrim's first person.
		auto*      camera = RE::PlayerCamera::GetSingleton();
		const bool firstPerson = camera && camera->currentState && camera->currentState->id == RE::CameraState::kFirstPerson;
		const bool active = ready && !g_suspended;
		if (active && !Surrogate::BodyHidden()) {
			Surrogate::SetBodyHidden(true);
		}
		if (active) {
			Surrogate::Refresh();
		}
		Motion     motion = ChooseMotion(player, dt);
		const auto stance = Character::Melee().ready;   // squared up with her weapon while she fights
		if (motion.animation == 0 && Melee::Attacking()) {
			motion.animation = stance;
		}
		std::int32_t upperAnimation = -1;
		float actionTime = 0;
		if (g_action.animation >= 0) {
			const auto* data = g_data.load();
			const int seq = data ? wowcore::findSequence(data->body.model, static_cast<std::uint32_t>(g_action.animation)) : -1;
			actionTime = static_cast<float>((now - g_action.since) * 1000.0);
			if (seq < 0 || (!g_action.loop && actionTime >= data->body.model.sequences[seq].duration)) {
				g_action.animation = -1;
			} else if (g_action.upperBody && g_display == 0) {
				upperAnimation = g_action.animation; // legs keep locomotion while the torso swings
			} else if (g_action.upperBody || motion.animation == 0 || motion.animation == stance || motion.animation == 120) {
				// Creature attacks are authored as full-body poses; keep their swing
				// visible while the native controller continues to translate them.
				motion.animation = static_cast<std::uint32_t>(g_action.animation);
				motion.loop = g_action.loop;
			}
		}
		// Slowed or hastened, a moving animation plays slower or faster in proportion, as the client's does with a snare
		// or a speed buff: here by Skyrim's own speed modifier (a frost slow lowers it). Unhindered it plays at its pace.
		static float rate = 1.0f;
		const float  want = motion.speed > 0.5f ? std::clamp(player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kSpeedMult) / 100.0f, 0.25f, 3.0f) : 1.0f;
		rate += (want - rate) * std::min(1.0f, static_cast<float>(dt) * 8.0f);
		const auto mount = g_mountDisplay.load();
		std::lock_guard lock(g_lock);
		if (mount && g_mountData) {
			if (g_pose.mountDisplay != mount || g_pose.mountAnimation != motion.animation) g_pose.mountTimeMs = 0;
			else g_pose.mountTimeMs += static_cast<float>(dt * 1000.0) * rate;
			g_pose.mountDisplay = mount;
			g_pose.mountAnimation = motion.animation;
			motion.animation = 91; // rider's Mount pose, independent of the mount's locomotion
			motion.loop = true;
			upperAnimation = -1;
		} else g_pose.mountDisplay = 0;
		if (motion.animation != g_pose.animation) {
			g_pose.animation = motion.animation;
			g_pose.timeMs = 0;
		} else {
			g_pose.timeMs += static_cast<float>(dt * 1000.0) * rate;
		}
		g_pose.upperAnimation = upperAnimation;
		g_pose.actionTimeMs = actionTime;
		g_pose.speed = motion.speed;
		g_pose.rate = rate;
		g_pose.loop = motion.loop;
		g_pose.visible = active && !firstPerson;
		g_pose.position = player->GetPosition();
		g_pose.facing = motion.facing;
		if (g_drawSwitchAt >= 0 && now >= g_drawSwitchAt) {
			g_drawSwitchAt = -1;
			g_shownDrawn = g_drawn;
		}
		g_pose.weaponsDrawn = g_shownDrawn;
	}

	void Act(std::int32_t a_animation, bool a_loop, bool a_upperBody)
	{
		g_action = { a_animation, a_loop, Frame::Now(), a_upperBody };
		std::lock_guard lock(g_lock);
		if (!a_upperBody) g_pose.timeMs = 0;   // a new action starts from its beginning, even the same one again
	}

	void StopActing() { g_action.animation = -1; }

	std::optional<double> EventAt(std::uint32_t a_animation, const std::string& a_id)
	{
		const auto* data = g_data.load();
		if (!data) {
			return std::nullopt;
		}
		const auto ms = wowcore::eventTime(data->body.model, wowcore::findSequence(data->body.model, a_animation), a_id);
		return ms ? std::optional<double>(*ms / 1000.0) : std::nullopt;
	}

	void SetWeaponsDrawn(bool a_drawn, bool a_animate)
	{
		if (a_drawn == g_drawn) {
			return;
		}
		g_drawn = a_drawn;
		if (!a_animate) {
			g_shownDrawn = a_drawn;
			g_drawSwitchAt = -1;
			return;
		}
		// Sheath (89) for a weapon on her back, HipSheath (90) for one at her hip (item_template.sheath 3); the weapon
		// changes place at the animation's own moment ($SHR: 433 ms of Sheath's 667).
		const auto hand = Character::InSlot(15);
		const std::int32_t anim = hand && hand->sheath == 3 ? 90 : 89;
		Act(anim, false);
		g_drawSwitchAt = Frame::Now() + EventAt(static_cast<std::uint32_t>(anim), "$SHR").value_or(0.0);
	}
	bool WeaponsDrawn() { return g_drawn; }

	void SetCircle(const RE::NiPoint3& a_at, float a_radius, const std::array<float, 3>& a_color)
	{
		std::lock_guard lock(g_lock);
		g_pose.circleAt = a_at;
		g_pose.circleRadius = a_radius;
		g_pose.circleColor = a_color;
	}

	void SetArea(const RE::NiPoint3& a_at, float a_radius, bool a_inRange)
	{
		std::lock_guard lock(g_lock);
		g_pose.areaAt = a_at;
		g_pose.areaRadius = a_radius;
		g_pose.areaOk = a_inRange;
	}

	void SetAlpha(float a_alpha)
	{
		std::lock_guard lock(g_lock);
		g_pose.alpha = a_alpha;
	}

	void IntegrationChanged(bool active)
	{
		if (active) return;
		for (const auto& [setting, value] : g_nativeCameraFloats) setting->data.f = value;
		for (const auto& [setting, value] : g_nativeCameraBools) setting->data.b = value;
		g_nativeCameraFloats.clear(); g_nativeCameraBools.clear();
		g_cameraSettings = g_cameraZoomed = false;
	}

	void Suspend(bool a_suspended)
	{
		g_suspended = a_suspended;
		if (a_suspended) {
			Surrogate::SetBodyHidden(false);
		}
	}

	Pose Current()
	{
		std::lock_guard lock(g_lock);
		return g_pose;
	}

	const Loaded* Data() { return g_data.load(); }
}
