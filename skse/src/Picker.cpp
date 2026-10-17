#include "Picker.h"

#include "render/WorldPass.h"

namespace Picker
{
	namespace
	{
		// Layers a cursor pick should stop at (SmoothCam's set: static, anim static, clutter, biped, trees,
		// props, terrain, ground, char controller) plus dead bipeds, and the loose things the player can take or hold:
		// weapons on the ground (5) and large clutter (29).
		constexpr std::uint64_t kLayers = (1ull << 1) | (1ull << 2) | (1ull << 4) | (1ull << 5) | (1ull << 8) | (1ull << 9) |
		                                  (1ull << 10) | (1ull << 13) | (1ull << 17) | (1ull << 29) | (1ull << 30) | (1ull << 32) |
		                                  (1ull << 33);
		// Units: biped, char controller, dead biped, biped without controller.
		constexpr std::uint64_t kUnits = (1ull << 8) | (1ull << 30) | (1ull << 32) | (1ull << 33);

		// Closest-hit collector; hkpRayHitCollector's vtable is { AddRayHit, dtor } (CommonLib, SmoothCam).
		class Collector : public RE::hkpRayHitCollector
		{
		public:
			void AddRayHit(const RE::hkpCdBody& a_body, const RE::hkpShapeRayCastCollectorOutput& a_out) override
			{
				const RE::hkpCdBody* root = &a_body;
				while (root->parent) {
					root = root->parent;
				}
				const auto* collidable = static_cast<const RE::hkpCollidable*>(root);
				const auto  layer = static_cast<std::uint32_t>(collidable->GetCollisionLayer());
				if (layer >= 64 || !(layers & (1ull << layer)) || a_out.hitFraction >= fraction) {
					return;
				}
				auto* ref = RE::TESHavokUtilities::FindCollidableRef(*collidable);
				if (ref && (ref == RE::PlayerCharacter::GetSingleton() || ref == ignore)) {
					return;  // the surrogate is invisible; never pick it
				}
				fraction = a_out.hitFraction;
				earlyOutHitFraction = fraction;
				this->ref = ref;
				this->layer = static_cast<int>(layer);
				const auto* owner = collidable->GetOwner<RE::hkpWorldObject>();
				auto*       wrapper = owner ? reinterpret_cast<RE::bhkWorldObject*>(owner->userData) : nullptr;
				body = wrapper ? wrapper->AsBhkRigidBody() : nullptr;
				float n[4];
				_mm_storeu_ps(n, a_out.normal.quad);  // clang's __m128 has no MSVC m128_f32 union
				normal = { n[0], n[1], n[2] };
			}

			std::uint64_t      layers = kLayers;
			const RE::TESObjectREFR* ignore = nullptr;
			float              fraction = 1.0f;
			RE::TESObjectREFR* ref = nullptr;
			int                layer = -1;
			RE::bhkRigidBody*  body = nullptr;
			RE::NiPoint3       normal;
		};
	}

	namespace
	{
		bool Invert4(const double a_m[4][4], double a_out[4][4])
		{
			double a[4][8];
			for (int r = 0; r < 4; ++r) {
				for (int c = 0; c < 4; ++c) {
					a[r][c] = a_m[r][c];
					a[r][c + 4] = r == c ? 1.0 : 0.0;
				}
			}
			for (int c = 0; c < 4; ++c) {
				int piv = c;
				for (int r = c + 1; r < 4; ++r) {
					if (std::abs(a[r][c]) > std::abs(a[piv][c])) {
						piv = r;
					}
				}
				if (std::abs(a[piv][c]) < 1e-12) {
					return false;
				}
				std::swap(a[c], a[piv]);
				const double inv = 1.0 / a[c][c];
				for (double& v : a[c]) {
					v *= inv;
				}
				for (int r = 0; r < 4; ++r) {
					if (r != c && a[r][c] != 0.0) {
						const double f = a[r][c];
						for (int k = 0; k < 8; ++k) {
							a[r][k] -= f * a[c][k];
						}
					}
				}
			}
			for (int r = 0; r < 4; ++r) {
				for (int c = 0; c < 4; ++c) {
					a_out[r][c] = a[r][c + 4];
				}
			}
			return true;
		}
	}

	// Unprojects with the inverse of the exact matrix the last frame was rendered with (published by the
	// world pass), so picks agree with what is on screen. Both NiCamera::WindowPointToRay and
	// NiCamera::worldToCam read during the update hook disagreed with the rendered view on this runtime.
	bool ScreenRay(float a_x, float a_y, RE::NiPoint3& a_origin, RE::NiPoint3& a_dir)
	{
		const auto v = WorldPass::LastView();
		double     inv[4][4];
		if (!v.valid || !Invert4(v.viewProj, inv)) {
			return false;
		}
		const double nx = (a_x + 0.5) / v.width * 2.0 - 1.0, ny = 1.0 - (a_y + 0.5) / v.height * 2.0;
		auto unproject = [&](double nz) {
			double p[4];
			for (int r = 0; r < 4; ++r) {
				p[r] = inv[r][0] * nx + inv[r][1] * ny + inv[r][2] * nz + inv[r][3];
			}
			return RE::NiPoint3{ static_cast<float>(p[0] / p[3]), static_cast<float>(p[1] / p[3]), static_cast<float>(p[2] / p[3]) };
		};
		const bool reversed = WorldPass::GetInfo().reversedZ;
		const auto nearP = unproject(reversed ? 1.0 : 0.0), farP = unproject(reversed ? 0.01 : 0.99);
		auto d = farP - nearP;
		if (d.Length() < 1e-6f) {
			return false;
		}
		d.Unitize();
		a_origin = nearP + v.cam;
		a_dir = d;
		return true;
	}

	Hit Cast(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, bool a_ground, const RE::TESObjectREFR* a_ignore)
	{
		Hit   result;
		auto* pc = RE::PlayerCharacter::GetSingleton();
		auto* cell = pc ? pc->GetParentCell() : nullptr;
		auto* world = cell ? cell->GetbhkWorld() : nullptr;
		if (!world) {
			return result;
		}
		// hkpWorld::CastRay(input, collector) under the world's read lock (TrueDirectionalMovement's call).
		// NOT bhkWorld::PickObject: on 1.7.104 it writes past CommonLib's 0xD0 bhkPickData and corrupted
		// our stack (pick_center_surface test).
		using CastRay_t = void* (*)(RE::hkpWorld*, RE::hkpWorldRayCastInput&, RE::hkpRayHitCollector&);
		static REL::Relocation<CastRay_t> castRay{ RELOCATION_ID(60552, 61400) };
		const float scale = RE::bhkWorld::GetWorldScale();
		const auto  d = a_to - a_from;
		Collector   collector;
		collector.layers = a_ground ? kLayers & ~kUnits : kLayers;
		collector.ignore = a_ignore;
		RE::hkpWorldRayCastInput input;
		input.from = RE::hkVector4(a_from.x * scale, a_from.y * scale, a_from.z * scale, 0.0f);
		input.to = RE::hkVector4(a_to.x * scale, a_to.y * scale, a_to.z * scale, 0.0f);
		world->worldLock.LockForRead();
		castRay(world->GetWorld1(), input, collector);
		world->worldLock.UnlockForRead();
		if (collector.fraction >= 1.0f) {
			return result;
		}
		result.hit = true;
		result.pos = a_from + d * collector.fraction;
		result.normal = collector.normal;
		result.distance = d.Length() * collector.fraction;
		result.ref = collector.ref;
		result.layer = collector.layer;
		result.body = collector.body;
		return result;
	}

	Hit PickScreen(float a_x, float a_y, float a_maxDistance, bool a_ground, const RE::TESObjectREFR* a_ignore)
	{
		RE::NiPoint3 origin, dir;
		if (!ScreenRay(a_x, a_y, origin, dir)) {
			return {};
		}
		return Cast(origin, origin + dir * a_maxDistance, a_ground, a_ignore);
	}

	bool WorldToScreen(const RE::NiPoint3& a_world, float& a_x, float& a_y)
	{
		const auto v = WorldPass::LastView();
		if (!v.valid) {
			return false;
		}
		const auto rel = a_world - v.cam;
		double     clip[4];
		for (int r = 0; r < 4; ++r) {
			clip[r] = v.viewProj[r][0] * rel.x + v.viewProj[r][1] * rel.y + v.viewProj[r][2] * rel.z + v.viewProj[r][3];
		}
		if (clip[3] <= 1e-3) {
			return false;
		}
		a_x = static_cast<float>((clip[0] / clip[3] * 0.5 + 0.5) * v.width);
		a_y = static_cast<float>((0.5 - clip[1] / clip[3] * 0.5) * v.height);
		return true;
	}
}
