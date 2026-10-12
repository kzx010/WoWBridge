#pragma once
#include "wowcore/model.hpp"

namespace wowcore {
// Replace SpineLow (key bone 4) and its descendants with the action's local
// transforms, composed over the existing locomotion pelvis/legs. Attachments
// then follow the same hand bones. Returns false for a model without a torso.
bool layerUpperBody(const Model& model, int actionSequence, float actionTimeMs, float globalMs,
                    std::vector<glm::mat4>& bones);

}
