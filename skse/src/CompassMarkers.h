#pragma once
#include <nlohmann/json.hpp>

// Scaleform stays on the UI thread; callers receive owned marker values only.
namespace CompassMarkers {
    void Update(bool a_rendered);
    nlohmann::json Snapshot();
}
