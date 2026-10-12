#include "wowcore/effect.hpp"

#include "rendering/m2_track_sampler.hpp"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace wowcore {
namespace track = wowee::rendering::m2_track;
using wowee::pipeline::M2AnimationTrack;
using wowee::pipeline::M2FBlock;

namespace {
float value(const M2AnimationTrack& t, int seq, float timeMs, float globalMs, const Model& m, float fallback) {
    return t.hasData() ? track::sampleFloat(t, seq, timeMs, globalMs, m.globalSequenceDurations, fallback) : fallback;
}

// Start -> middle -> end across a life (0..1), about the midpoint.
template <class T>
T ramp(const std::vector<T>& v, const std::vector<float>& times, float life) {
    if (v.empty()) return T(1.0f);
    if (v.size() < 3) return v[0];
    const float mid = times.size() >= 2 ? std::clamp(times[1], 0.01f, 0.99f) : 0.5f;
    return life <= mid ? glm::mix(v[0], v[1], life / mid) : glm::mix(v[1], v[2], (life - mid) / (1.0f - mid));
}

// The texture cell a particle shows at this point of its life: head cells up to the midpoint, decay cells after.
int cellAt(const wowee::pipeline::M2ParticleEmitter& e, float life, float mid, int cells) {
    const auto run = [&](const uint16_t* range, float part) {
        const int first = range[0], last = range[1], times = std::max<int>(range[2], 1);
        if (last <= first) return first;
        const int span = last - first + 1;
        return first + static_cast<int>(part * span * times) % span;
    };
    const int cell = life <= mid ? run(e.headCells, life / std::max(mid, 1e-3f)) : run(e.decayCells, (life - mid) / std::max(1.0f - mid, 1e-3f));
    return std::clamp(cell, 0, std::max(cells - 1, 0));
}
}  // namespace

BatchLook batchLook(const Model& m, const wowee::pipeline::M2Batch& b, int seq, float timeMs, float globalMs) {
    BatchLook out;
    const auto& globals = m.globalSequenceDurations;
    if (b.colorIndex < m.colorAlphaTracks.size()) {
        const glm::vec3 rest = b.colorIndex < m.colorRGB.size() ? m.colorRGB[b.colorIndex] : glm::vec3(1.0f);
        const glm::vec3 rgb = b.colorIndex < m.colorRGBTracks.size() && m.colorRGBTracks[b.colorIndex].hasData()
            ? track::sampleVec3(m.colorRGBTracks[b.colorIndex], seq, timeMs, globalMs, globals, rest) : rest;
        out.color = glm::vec4(rgb, value(m.colorAlphaTracks[b.colorIndex], seq, timeMs, globalMs, m, 1.0f));
    }
    if (b.transparencyIndex < m.textureWeightLookup.size()) {
        const auto t = m.textureWeightLookup[b.transparencyIndex];
        if (t < m.textureWeightTracks.size()) out.color.a *= value(m.textureWeightTracks[t], seq, timeMs, globalMs, m, 1.0f);
    }
    if (b.textureAnimIndex < m.textureTransformLookup.size()) {
        const auto t = m.textureTransformLookup[b.textureAnimIndex];
        if (t < m.textureTransforms.size()) {
            const auto& tt = m.textureTransforms[t];
            const glm::vec3 move = track::sampleVec3(tt.translation, seq, timeMs, globalMs, globals, glm::vec3(0.0f));
            const glm::quat turn = track::sampleQuat(tt.rotation, seq, timeMs, globalMs, globals);
            const glm::vec3 size = track::sampleVec3(tt.scale, seq, timeMs, globalMs, globals, glm::vec3(1.0f));
            // M2 applies translation in the texture's local coordinates, before rotation/scale about (0.5, 0.5):
            // uv' = T(c) * R * S * T(move) * T(-c) uv. In the cooldown model the translation keys compensate
            // its rotating quadrants; adding them after rotation makes the sweep's edge drift off the shared centre.
            const glm::mat3 r = glm::mat3_cast(turn);
            glm::mat3 rs(1.0f);
            rs[0] = glm::vec3(r[0][0] * size.x, r[0][1] * size.x, 0.0f);
            rs[1] = glm::vec3(r[1][0] * size.y, r[1][1] * size.y, 0.0f);
            const glm::vec2 c(0.5f);
            const glm::vec2 local = glm::vec2(move) - c;
            const glm::vec2 shift = c + glm::vec2(rs[0]) * local.x + glm::vec2(rs[1]) * local.y;
            rs[2] = glm::vec3(shift, 1.0f);
            out.uv = rs;
        }
    }
    return out;
}

Emitters::Emitters(const Model& model, uint32_t seed)
    : model_(model), rng_(seed), owed_(model.particleEmitters.size(), 0.0f), ribbons_(model.ribbonEmitters.size()) {}

bool Emitters::idle() const {
    if (!particles_.empty()) return false;
    for (const auto& r : ribbons_) if (!r.points.empty()) return false;
    return true;
}

void Emitters::update(float dt, const std::vector<glm::mat4>& bones, const glm::mat4& world, int seq, float timeMs, float globalMs,
                      bool emitting) {
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const auto rand = [&](float lo, float hi) { return lo + (hi - lo) * unit(rng_); };
    worldScale_ = glm::length(glm::vec3(world[0]));
    const glm::vec3 down(0.0f, 0.0f, -1.0f);

    // Live particles: gravity, drag, flight; the spent ones go.
    for (auto& p : particles_) {
        const auto& e = model_.particleEmitters[p.emitter];
        const float gravity = value(e.gravity, seq, timeMs, globalMs, model_, 0.0f) * worldScale_;
        const float decel = value(e.deceleration, seq, timeMs, globalMs, model_, 0.0f) * worldScale_;
        p.vel += down * gravity * dt;
        if (decel != 0.0f && glm::dot(p.vel, p.vel) > 1e-8f) p.vel -= glm::normalize(p.vel) * decel * dt;
        const float slow = e.drag > 0.0f ? std::exp(-e.drag * p.life) : 1.0f;
        p.pos += p.vel * slow * dt;
        p.life += dt;
    }
    std::erase_if(particles_, [](const Particle& p) { return p.life >= p.maxLife; });

    // New ones, at each emitter's rate (fractions owed to the next step).
    for (size_t i = 0; i < model_.particleEmitters.size(); ++i) {
        const auto& e = model_.particleEmitters[i];
        const bool on = emitting && (!e.enabledTrack.hasData() || value(e.enabledTrack, seq, timeMs, globalMs, model_, 1.0f) > 0.5f);
        const float rate = value(e.emissionRate, seq, timeMs, globalMs, model_, 0.0f);
        const float life = value(e.lifespan, seq, timeMs, globalMs, model_, 1.0f);
        if (!on || rate <= 0.0f || life <= 0.0f) {
            owed_[i] = 0.0f;
            continue;
        }
        owed_[i] += rate * dt;
        const glm::mat4 at = world * (e.bone < bones.size() ? bones[e.bone] : glm::mat4(1.0f));
        const glm::mat3 turn(at);
        const float speed = value(e.emissionSpeed, seq, timeMs, globalMs, model_, 0.0f);
        const float vary = value(e.speedVariation, seq, timeMs, globalMs, model_, 0.0f);
        const float vertical = value(e.verticalRange, seq, timeMs, globalMs, model_, 0.0f);
        const float horizontal = value(e.horizontalRange, seq, timeMs, globalMs, model_, 0.0f);
        const float length = value(e.emissionAreaLength, seq, timeMs, globalMs, model_, 0.0f);
        const float width = value(e.emissionAreaWidth, seq, timeMs, globalMs, model_, 0.0f);
        for (; owed_[i] >= 1.0f && particles_.size() < 2048; owed_[i] -= 1.0f) {
            Particle p;
            p.emitter = static_cast<int>(i);
            p.maxLife = life;
            p.phase = rand(0.0f, glm::two_pi<float>());
            p.spin0 = rand(0.0f, glm::two_pi<float>());
            p.shown = unit(rng_) < e.twinklePercent;
            glm::vec3 local, dir;
            if (e.emitterType == 2) {   // sphere: somewhere between its radii, leaving outward (or up)
                const float elevation = rand(-vertical, vertical), azimuth = rand(-horizontal, horizontal) * 0.5f;
                const glm::vec3 out(std::cos(elevation) * std::cos(azimuth), std::cos(elevation) * std::sin(azimuth), std::sin(elevation));
                local = e.position + out * rand(std::min(length, width), std::max(length, width));
                dir = (e.flags & 0x100) ? glm::vec3(0.0f, 0.0f, 1.0f) : out;
            } else {                    // plane: somewhere on it, leaving within the cone about +z
                local = e.position + glm::vec3(rand(-length, length) * 0.5f, rand(-width, width) * 0.5f, 0.0f);
                const float polar = rand(-vertical, vertical), azimuth = rand(-horizontal, horizontal) * 0.5f;
                dir = glm::vec3(std::sin(polar) * std::cos(azimuth), std::sin(polar) * std::sin(azimuth), std::cos(polar));
            }
            p.pos = p.origin = glm::vec3(at * glm::vec4(local, 1.0f));
            const glm::vec3 worldDir = turn * dir;
            p.vel = (glm::dot(worldDir, worldDir) > 1e-8f ? glm::normalize(worldDir) : worldDir) * speed * worldScale_ * (1.0f + rand(-vary, vary));
            particles_.push_back(p);
        }
    }

    // Ribbons: a new edge so many times a second, the newest following its bone; edges older than their life go. An
    // effect that has ended takes its ribbons with it at once (the client's Rejuvenation rings are whole at 2.75 s of
    // its 3 and gone at 3.0), while its particles run out.
    for (size_t i = 0; i < model_.ribbonEmitters.size(); ++i) {
        const auto& e = model_.ribbonEmitters[i];
        auto& r = ribbons_[i];
        if (!emitting) {
            r.points.clear();
            continue;
        }
        for (auto& p : r.points) p.age += dt;
        std::erase_if(r.points, [&](const RibbonPoint& p) { return p.age > e.edgeLifetime; });
        const bool shown = emitting && (!e.visibilityTrack.hasData() || value(e.visibilityTrack, seq, timeMs, globalMs, model_, 1.0f) > 0.5f);
        if (!shown || e.bone >= bones.size()) continue;
        const glm::mat4 at = world * bones[e.bone];
        const RibbonPoint head{ glm::vec3(at * glm::vec4(e.position, 1.0f)), glm::normalize(glm::mat3(at) * glm::vec3(0.0f, 0.0f, 1.0f)), 0.0f };
        r.sinceEdge += dt;
        if (r.points.empty() || r.sinceEdge >= 1.0f / std::max(e.edgesPerSecond, 1.0f)) {
            r.points.insert(r.points.begin(), head);
            r.sinceEdge = 0.0f;
        } else {
            r.points.front() = head;
        }
        const glm::vec3 rgb = track::sampleVec3(e.colorTrack, seq, timeMs, globalMs, model_.globalSequenceDurations, glm::vec3(1.0f));
        r.color = glm::vec4(rgb, value(e.alphaTrack, seq, timeMs, globalMs, model_, 1.0f));
        r.above = value(e.heightAboveTrack, seq, timeMs, globalMs, model_, 0.0f) * worldScale_;
        r.below = value(e.heightBelowTrack, seq, timeMs, globalMs, model_, 0.0f) * worldScale_;
    }
}

void Emitters::quads(const glm::vec3& right, const glm::vec3& up, std::vector<EffectQuad>& out) const {
    const glm::vec3 facing = glm::normalize(glm::cross(right, up));
    for (const auto& p : particles_) {
        const auto& e = model_.particleEmitters[p.emitter];
        if (!p.shown) continue;
        const float life = std::clamp(p.life / p.maxLife, 0.0f, 1.0f);
        const float mid = e.particleColor.timestamps.size() >= 2 ? e.particleColor.timestamps[1] : 0.5f;
        EffectQuad q;
        q.color = glm::vec4(ramp(e.particleColor.vec3Values, e.particleColor.timestamps, life),
                            ramp(e.particleAlpha.floatValues, e.particleAlpha.timestamps, life));
        float size = ramp(e.particleScale.floatValues, e.particleScale.timestamps, life) * worldScale_;
        if (e.twinkleSpeed > 0.0f && e.twinkleScaleMax > e.twinkleScaleMin) {
            const float wave = 0.5f + 0.5f * std::sin(p.phase + e.twinkleSpeed * p.life);
            size *= e.twinkleScaleMin + (e.twinkleScaleMax - e.twinkleScaleMin) * wave;
        }
        // The texture cell.
        const int cols = std::max<int>(e.textureCols, 1), rows = std::max<int>(e.textureRows, 1);
        const int cell = cellAt(e, life, mid, cols * rows);
        const glm::vec2 c0(static_cast<float>(cell % cols) / cols, static_cast<float>(cell / cols) / rows);
        const glm::vec2 c1 = c0 + glm::vec2(1.0f / cols, 1.0f / rows);
        q.uv[0] = { c0.x, c0.y }, q.uv[1] = { c1.x, c0.y }, q.uv[2] = { c1.x, c1.y }, q.uv[3] = { c0.x, c1.y };
        q.texture = e.texture;
        q.blend = e.blendingType;
        if (e.particleType == 1) {   // stretched from where it was born to where it is
            const glm::vec3 along = p.pos - p.origin;
            glm::vec3 side = glm::cross(along, facing);
            side = glm::dot(side, side) > 1e-8f ? glm::normalize(side) * size : right * size;
            q.corner[0] = p.pos + side, q.corner[1] = p.pos - side, q.corner[2] = p.origin - side, q.corner[3] = p.origin + side;
        } else {
            const float angle = p.spin0 * (e.spin != 0.0f ? 1.0f : 0.0f) + e.spin * glm::two_pi<float>() * p.life;
            glm::vec3 x = right, y = up;
            if (e.flags & 0x1000) x = glm::vec3(1, 0, 0), y = glm::vec3(0, 1, 0);   // lying flat
            const glm::vec3 rx = (x * std::cos(angle) + y * std::sin(angle)) * size;
            const glm::vec3 ry = (y * std::cos(angle) - x * std::sin(angle)) * size;
            q.corner[0] = p.pos - rx + ry, q.corner[1] = p.pos + rx + ry, q.corner[2] = p.pos + rx - ry, q.corner[3] = p.pos - rx - ry;
        }
        out.push_back(q);
    }
    for (size_t i = 0; i < ribbons_.size(); ++i) {
        const auto& r = ribbons_[i];
        const auto& e = model_.ribbonEmitters[i];
        const float lifetime = std::max(e.edgeLifetime, 1e-3f);
        const auto material = e.materialIndex < model_.materials.size() ? model_.materials[e.materialIndex].blendMode : 4;
        for (size_t k = 0; k + 1 < r.points.size(); ++k) {
            const auto& a = r.points[k];
            const auto& b = r.points[k + 1];
            EffectQuad q;
            q.color = r.color;
            q.texture = e.textureIndex;
            q.blend = material;
            const float u0 = a.age / lifetime, u1 = b.age / lifetime;
            q.corner[0] = a.pos + a.up * r.above, q.corner[1] = b.pos + b.up * r.above;
            q.corner[2] = b.pos - b.up * r.below, q.corner[3] = a.pos - a.up * r.below;
            q.uv[0] = { u0, 0.0f }, q.uv[1] = { u1, 0.0f }, q.uv[2] = { u1, 1.0f }, q.uv[3] = { u0, 1.0f };
            out.push_back(q);
        }
    }
}
}  // namespace wowcore
