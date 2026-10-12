#pragma once

#include <algorithm>
#include <cmath>
#include <string_view>

namespace wowcore {
// Skyrim units. Preserve a valid controller diameter, fall back to horizontal
// object bounds for ragdolls, and keep even a chicken's ring visible.
inline float selectionCircleRadius(float controllerDiameter, float boundsWidth) {
    const float size = std::isfinite(controllerDiameter) && controllerDiameter > 0
        ? controllerDiameter : boundsWidth;
    return std::isfinite(size) ? std::max(16.0f, size) : 16.0f;
}

inline bool isMovementCommand(std::string_view command) {
    return command == "MOVEFORWARD" || command == "MOVEBACKWARD" ||
        command == "TURNLEFT" || command == "TURNRIGHT" ||
        command == "STRAFELEFT" || command == "STRAFERIGHT";
}
}
