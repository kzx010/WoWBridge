#include "wowcore/model_layers.hpp"

#include "rendering/m2_track_sampler.hpp"
#include "wowcore/mpq.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace wowcore {
namespace track = wowee::rendering::m2_track;

namespace {
bool arrayFits(const std::vector<uint8_t>& bytes, size_t at, size_t count, size_t stride) {
    return at <= bytes.size() && count <= (bytes.size() - at) / stride;
}

uint32_t read32(const std::vector<uint8_t>& bytes, size_t at) {
    return uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 | uint32_t(bytes[at + 2]) << 16 | uint32_t(bytes[at + 3]) << 24;
}

// The vendored loader includes a vanilla float track's last key, but its byte-track reader still treats a range's
// maximum as exclusive. Complete the particle enabled tracks here: otherwise the morph puff loses its off keys
// (at 1400/1700 ms) and keeps emitting until the model ends. A one-key range (minimum == maximum) matters too.
void completeEnabledTracks(const std::vector<uint8_t>& bytes, Model& model) {
    if (model.version != 256 || !arrayFits(bytes, 0x13C, 2, 4)) return;
    const uint32_t count = read32(bytes, 0x13C), emitters = read32(bytes, 0x140);
    if (count != model.particleEmitters.size() || !arrayFits(bytes, emitters, count, 0x1F8)) return;
    for (size_t i = 0; i < count; ++i) {
        const size_t at = size_t(emitters) + i * 0x1F8 + 0x1DC;   // vanilla enabled M2Track (28 bytes)
        const uint32_t ranges = read32(bytes, at + 4), rangesAt = read32(bytes, at + 8);
        const uint32_t times = read32(bytes, at + 12), timesAt = read32(bytes, at + 16);
        const uint32_t keys = read32(bytes, at + 20), keysAt = read32(bytes, at + 24);
        auto& t = model.particleEmitters[i].enabledTrack;
        if (!ranges || ranges != t.sequences.size() || !arrayFits(bytes, rangesAt, ranges, 8) ||
            !arrayFits(bytes, timesAt, times, 4) || !arrayFits(bytes, keysAt, keys, 1)) continue;
        for (size_t s = 0; s < ranges; ++s) {
            const uint32_t first = read32(bytes, size_t(rangesAt) + s * 8), last = read32(bytes, size_t(rangesAt) + s * 8 + 4);
            if (first > last || last >= times || last >= keys) continue;
            auto& seq = t.sequences[s];
            if (seq.timestamps.size() != last - first || seq.floatValues.size() != last - first) continue;
            const uint32_t start = read32(bytes, size_t(timesAt) + size_t(first) * 4);
            const uint32_t end = read32(bytes, size_t(timesAt) + size_t(last) * 4);
            if (end < start) continue;
            seq.timestamps.push_back(end - start);
            seq.floatValues.push_back(bytes[size_t(keysAt) + last] ? 1.0f : 0.0f);
        }
    }
}
}  // namespace

std::optional<Model> loadModel(const Mpq& mpq, std::string path) {
    if (path.size() > 4) {
        std::string ext = path.substr(path.size() - 4);
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".mdx" || ext == ".mdl") path = path.substr(0, path.size() - 4) + ".m2";
    }
    const auto bytes = mpq.read(path);
    if (!bytes) return std::nullopt;
    auto model = wowee::pipeline::M2Loader::load(*bytes);
    completeEnabledTracks(*bytes, model);
    // An effect may be all particles and ribbons (Rejuvenation's impact): nothing to show is no geometry and no emitters.
    if (model.vertices.empty() && model.particleEmitters.empty() && model.ribbonEmitters.empty()) return std::nullopt;
    model.name = path;
    return model;
}

int findSequence(const Model& model, uint32_t animationId, uint32_t variation) {
    for (size_t i = 0; i < model.sequences.size(); ++i)
        if (model.sequences[i].id == animationId && model.sequences[i].variationIndex == variation) return static_cast<int>(i);
    return -1;
}

int sequenceWithFallback(const Model& model, uint32_t animationId) {
    int seq = findSequence(model, animationId);
    if (seq < 0 && animationId == 119) seq = findSequence(model, 4);
    return seq >= 0 ? seq : std::max(findSequence(model, 0), 0);
}

namespace {
// The bone's matrix turned to face the eye, keeping where its pivot goes and its scale.
glm::mat4 facing(const glm::mat4& m, const glm::vec3& pivot, uint32_t flags, const glm::vec3& eye, const glm::vec3& up) {
    const glm::vec3 at = glm::vec3(m * glm::vec4(pivot, 1.0f));
    const glm::vec3 scale(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
    glm::vec3 look = eye - at;
    if (glm::dot(look, look) < 1e-8f) return m;
    look = glm::normalize(look);
    glm::mat3 r;
    if (flags & 0x8) {   // all ways: x towards the eye, z the eye's up
        glm::vec3 z = up - glm::dot(up, look) * look;
        z = glm::dot(z, z) > 1e-8f ? glm::normalize(z) : glm::vec3(0.0f, 0.0f, 1.0f);
        r = glm::mat3(look, glm::cross(z, look), z);
    } else {              // about one of its own axes: that axis kept, x turned towards the eye round it
        const int k = (flags & 0x10) ? 0 : (flags & 0x20) ? 1 : 2;
        const glm::vec3 axis = glm::normalize(glm::vec3(m[k]));
        glm::vec3 x = look - glm::dot(look, axis) * axis;
        if (glm::dot(x, x) < 1e-8f || k == 0) return m;
        x = glm::normalize(x);
        r = k == 1 ? glm::mat3(x, axis, glm::cross(x, axis)) : glm::mat3(x, glm::cross(axis, x), axis);
    }
    glm::mat4 out(1.0f);
    out[0] = glm::vec4(r[0] * scale.x, 0.0f);
    out[1] = glm::vec4(r[1] * scale.y, 0.0f);
    out[2] = glm::vec4(r[2] * scale.z, 0.0f);
    out[3] = glm::vec4(at - (glm::mat3(out) * pivot), 1.0f);
    return out;
}
void pose(const Model& model, int previousSequence, float previousTimeMs, int sequence, float timeMs,
          float weight, float globalMs, std::vector<glm::mat4>& bones, const glm::vec3* eye, const glm::vec3* up) {
    bones.assign(model.bones.size(), glm::mat4(1.0f));
    const auto& globals = model.globalSequenceDurations;
    for (size_t i = 0; i < model.bones.size(); ++i) {
        const auto& bone = model.bones[i];
        glm::vec3 t = track::sampleVec3(bone.translation, sequence, timeMs, globalMs, globals, glm::vec3(0.0f));
        glm::quat r = track::sampleQuat(bone.rotation, sequence, timeMs, globalMs, globals);
        glm::vec3 s = track::sampleVec3(bone.scale, sequence, timeMs, globalMs, globals, glm::vec3(1.0f));
        if (weight < 1.0f) {
            const glm::vec3 fromT = track::sampleVec3(bone.translation, previousSequence, previousTimeMs, globalMs, globals, glm::vec3(0.0f));
            const glm::quat fromR = track::sampleQuat(bone.rotation, previousSequence, previousTimeMs, globalMs, globals);
            const glm::vec3 fromS = track::sampleVec3(bone.scale, previousSequence, previousTimeMs, globalMs, globals, glm::vec3(1.0f));
            t = glm::mix(fromT, t, weight);
            r = glm::normalize(glm::slerp(fromR, r, weight));
            s = glm::mix(fromS, s, weight);
        }
        // T(pivot) * T(t) * R * S * T(-pivot), built directly.
        const glm::mat3 rot = glm::mat3_cast(r);
        const glm::vec3 c0 = rot[0] * s.x, c1 = rot[1] * s.y, c2 = rot[2] * s.z;
        glm::mat4 local(1.0f);
        local[0] = glm::vec4(c0, 0.0f);
        local[1] = glm::vec4(c1, 0.0f);
        local[2] = glm::vec4(c2, 0.0f);
        local[3] = glm::vec4(bone.pivot + t - (c0 * bone.pivot.x + c1 * bone.pivot.y + c2 * bone.pivot.z), 1.0f);
        const int parent = bone.parentBone;
        bones[i] = parent >= 0 && static_cast<size_t>(parent) < i ? bones[parent] * local : local;
        if (eye && up && (bone.flags & 0x78)) bones[i] = facing(bones[i], bone.pivot, bone.flags, *eye, *up);
    }
}
}  // namespace

void poseBones(const Model& model, int sequence, float timeMs, float globalMs, std::vector<glm::mat4>& bones,
               const glm::vec3* eye, const glm::vec3* up) {
    pose(model, sequence, timeMs, sequence, timeMs, 1.0f, globalMs, bones, eye, up);
}

bool layerUpperBody(const Model& model, int actionSequence, float actionTimeMs, float globalMs,
                    std::vector<glm::mat4>& bones) {
    if (actionSequence < 0 || static_cast<size_t>(actionSequence) >= model.sequences.size() ||
        bones.size() != model.bones.size()) return false;
    std::vector<bool> upper(model.bones.size(), false);
    bool layered = false;
    for (size_t i = 0; i < model.bones.size(); ++i) {
        const auto& bone = model.bones[i];
        const int parent = bone.parentBone;
        upper[i] = bone.keyBoneId == 4 || (parent >= 0 && static_cast<size_t>(parent) < i && upper[parent]);
        if (!upper[i]) continue;
        layered = true;
        const auto& globals = model.globalSequenceDurations;
        const auto t = track::sampleVec3(bone.translation, actionSequence, actionTimeMs, globalMs, globals, glm::vec3(0));
        const auto r = track::sampleQuat(bone.rotation, actionSequence, actionTimeMs, globalMs, globals);
        const auto s = track::sampleVec3(bone.scale, actionSequence, actionTimeMs, globalMs, globals, glm::vec3(1));
        const glm::mat3 rot = glm::mat3_cast(r);
        const glm::vec3 c0 = rot[0] * s.x, c1 = rot[1] * s.y, c2 = rot[2] * s.z;
        glm::mat4 local(1);
        local[0] = glm::vec4(c0, 0); local[1] = glm::vec4(c1, 0); local[2] = glm::vec4(c2, 0);
        local[3] = glm::vec4(bone.pivot + t - (c0 * bone.pivot.x + c1 * bone.pivot.y + c2 * bone.pivot.z), 1);
        bones[i] = parent >= 0 && static_cast<size_t>(parent) < i ? bones[parent] * local : local;
    }
    return layered;
}

uint32_t blendDurationMs(const Model& model, int previousSequence, int nextSequence) {
    // Vanilla's 68-byte M2SequenceDiskVanilla has a single uint32 blendTime at +0x20; WoWee's loader
    // copies it unchanged into M2Sequence::blendTime (src/pipeline/m2_loader.cpp). Whoa's
    // CM2Model::SetPrimaryBoneSequence copies the old clock to the secondary slot, then sets the
    // deadline and reciprocal duration from sequences[sequenceIndex].blendtime (the NEW sequence):
    // https://github.com/whoahq/whoa/blob/master/src/model/CM2Model.cpp#L1808-L1834
    // Whoa targets 3.3.5; applying that direction to 1.12 is an inference supported by the same single
    // blend-time layout, not an independently verified 1.12 arming call / live switch. Use the entering
    // sequence's time, without substituting later clients' split in/out times.
    if (previousSequence == nextSequence || previousSequence < 0 || nextSequence < 0 ||
        static_cast<size_t>(previousSequence) >= model.sequences.size() ||
        static_cast<size_t>(nextSequence) >= model.sequences.size()) return 0;
    return model.sequences[nextSequence].blendTime;
}

void poseBonesBlended(const Model& model, int previousSequence, float previousTimeMs,
                      int nextSequence, float nextTimeMs, float progress, float globalMs,
                      std::vector<glm::mat4>& bones, const glm::vec3* eye, const glm::vec3* up) {
    // The weighting curve is UNVERIFIED: a smoothstep (3-2t)t^2 cross-fade with shortest-arc slerp, after third-party
    // 1.12 notes we could not check; a linear fade is the alternative. Settle it against the reference client's frames
    // of a switch (Stand to Run). This is a whole-pose blend, not the client's per-bone layered animation state.
    if (!(progress > 0.0f)) {
        poseBones(model, previousSequence, previousTimeMs, globalMs, bones, eye, up);
    } else if (progress >= 1.0f) {
        poseBones(model, nextSequence, nextTimeMs, globalMs, bones, eye, up);
    } else {
        const float weight = progress * progress * (3.0f - 2.0f * progress);
        pose(model, previousSequence, previousTimeMs, nextSequence, nextTimeMs, weight, globalMs, bones, eye, up);
    }
}

std::optional<glm::mat4> attachmentMatrix(const Model& model, const std::vector<glm::mat4>& bones, uint32_t id) {
    for (const auto& a : model.attachments) {
        if (a.id != id) continue;
        const glm::mat4 bone = a.bone < bones.size() ? bones[a.bone] : glm::mat4(1.0f);
        return bone * glm::translate(glm::mat4(1.0f), a.position);
    }
    return std::nullopt;
}
std::optional<uint32_t> eventTime(const Model& model, int sequence, const std::string& id) {
    if (sequence < 0 || static_cast<size_t>(sequence) >= model.sequenceEvents.size()) return std::nullopt;
    for (const auto& [name, ms] : model.sequenceEvents[sequence])
        if (name == id) return ms;
    return std::nullopt;
}
std::optional<glm::mat4> mountedRiderMatrix(const Model& mount,
    const std::vector<glm::mat4>& bones, float scale) {
    if (!std::isfinite(scale) || scale <= 0) return std::nullopt;
    const auto saddle = attachmentMatrix(mount, bones, 0);
    if (!saddle) return std::nullopt;
    // Preserve the animated saddle rotation; scale its translation only.
    auto result = *saddle;
    result[3] = glm::vec4(glm::vec3(result[3]) * scale, 1.0f);
    return result;
}
}  // namespace wowcore
