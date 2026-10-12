#pragma once
// WoW 1.12 models (M2 v256, WoWee's loader): loading from the client's MPQs, animation sequences by id, the bone
// matrices of a pose, and where an attachment point is.
#include "pipeline/m2_loader.hpp"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

namespace wowcore {
class Mpq;

using Model = wowee::pipeline::M2Model;

// A model by its path in the client's data (the tables name them .mdx; the files are .m2).
std::optional<Model> loadModel(const Mpq& mpq, std::string path);

// The index of an animation (AnimationData.dbc id: 0 Stand, 4 Walk, 5 Run, ...) in the model's sequences, or -1.
int findSequence(const Model& model, uint32_t animationId, uint32_t variation = 0);

// World animation selection: missing StealthWalk uses Walk, then missing poses use Stand.
int sequenceWithFallback(const Model& model, uint32_t animationId);

// The model-space matrix of every bone in a pose (WoW's T(pivot) T R S T(-pivot), parents before children).
// timeMs within the sequence; globalMs drives the bones on global sequences.
// eye and up (in the model's own space), when given, turn its billboarded bones to face that eye (flag 0x8: all ways;
// 0x10/0x20/0x40: about their x/y/z) as they are posed, so what hangs below them turns with them.
void poseBones(const Model& model, int sequence, float timeMs, float globalMs, std::vector<glm::mat4>& bones,
               const glm::vec3* eye = nullptr, const glm::vec3* up = nullptr);

// Duration in ms for a switch: the entering (next) sequence's blendTime. No transition (0) for the same
// sequence or an invalid index. Sequence arguments are model indices, as for poseBones, not animation ids.
uint32_t blendDurationMs(const Model& model, int previousSequence, int nextSequence);

// Blend two sampled poses in local bone space, then compose parents, pivots and billboards as poseBones does.
// progress is elapsed / blendDurationMs, clamped to [0,1] (NaN acts as 0); the 1.12 smoothstep curve gives the
// next pose's weight. 0 is exactly the previous pose, 1 exactly the next. For a zero duration, pass 1.
// Both times are sequence-local; the host handles looping/clamping and may advance both during the switch.
// A single globalMs drives both samples, so global-sequence tracks keep their independent clock.
void poseBonesBlended(const Model& model, int previousSequence, float previousTimeMs,
                      int nextSequence, float nextTimeMs, float progress, float globalMs,
                      std::vector<glm::mat4>& bones, const glm::vec3* eye = nullptr, const glm::vec3* up = nullptr);

// When an animation event fires in a sequence (ms into it; the M2's events: "$CSS" a swing's whoosh, "$HIT" the blow
// landing, ...), or nothing if the sequence has none.
std::optional<uint32_t> eventTime(const Model& model, int sequence, const std::string& id);
// Where an attachment point (0 Head ... 1 right hand, 5/6 shoulders, 11 helm, 26 back sheath) is, in model space.
std::optional<glm::mat4> attachmentMatrix(const Model& model, const std::vector<glm::mat4>& bones, uint32_t id);
// The mount's authored saddle (attachment 0), in world yards. Rider scale stays
// independent: mount display scaling changes the saddle position, not rider size.
std::optional<glm::mat4> mountedRiderMatrix(const Model& mount,
    const std::vector<glm::mat4>& bones, float scale);
}  // namespace wowcore
