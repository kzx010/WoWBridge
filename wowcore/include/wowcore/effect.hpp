#pragma once
// WoW 1.12 effect models running (spell visuals, M8): what each batch looks like at a moment (its colour and
// transparency tracks, its texture animation), bones that face the camera, and the particles and ribbons the model's
// emitters give off - simulated in the effect's world (the model-to-world matrix given each step: yards times the
// world's scale), so the same simulation feeds the native preview and the game.
//
// Semantics from the M2 format as wowdev documents it and the client's own files show it (tools/patch_wowee.py
// patch_m2): emission rate in particles a second, speed in yards a second, sizes in yards; a plane emitter's area is its
// length (x) and width (y) and its particles leave within a cone (vertical range: the polar angle from +z; horizontal
// range: the azimuth); a sphere's area is its inner and outer radius, its particles leaving outward (or up, flag 0x100);
// colour, alpha and size go start -> middle -> end across a particle's life about its midpoint; texture cells run head
// cells then decay cells; type-1 particles are stretched back along their flight (tail length).
// Particle lifespan does not extend an impact kit's model lifetime: the host removes the whole instance when its
// one-shot sequence ends. emitting=false is for a stopped emitter whose existing particles are allowed to finish.
#include "wowcore/model.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <random>
#include <vector>

namespace wowcore {

// A batch at a moment: colour (its colour track's rgb and alpha times its transparency), texture-coordinate matrix
// (its texture animation: translation, rotation about the centre, scale).
struct BatchLook {
    glm::vec4 color{1.0f};
    glm::mat3 uv{1.0f};
};
BatchLook batchLook(const Model& model, const wowee::pipeline::M2Batch& batch, int sequence, float timeMs, float globalMs);

// Turns the billboarded bones (flag 0x8, all ways; 0x10/0x20/0x40 about their x/y/z) of a posed model to face the eye.
// eye and up are in the model's own space.
void billboardBones(const Model& model, std::vector<glm::mat4>& bones, const glm::vec3& eye, const glm::vec3& up);

// What to draw for the particles and ribbons: textured quads with a colour, in world space.
struct EffectQuad {
    glm::vec3 corner[4];
    glm::vec2 uv[4];
    glm::vec4 color;
    uint16_t texture = 0;      // the model's texture index
    uint16_t blend = 0;        // M2 blend mode
};

class Emitters {
public:
    explicit Emitters(const Model& model, uint32_t seed = 1);

    // Moves the particles on and gives off new ones (emitting: false lets the last ones finish), and lays the ribbons
    // along where their bones are. dt in seconds; world: model -> world.
    void update(float dt, const std::vector<glm::mat4>& bones, const glm::mat4& world, int sequence, float timeMs, float globalMs,
                bool emitting = true);

    // Quads facing the camera (right and up in world space) for every live particle, and the ribbons' strips.
    void quads(const glm::vec3& right, const glm::vec3& up, std::vector<EffectQuad>& out) const;

    bool idle() const;   // nothing live

private:
    struct Particle {
        glm::vec3 pos, vel, origin;
        float life = 0, maxLife = 1;
        float phase = 0;       // twinkle
        float spin0 = 0;       // starting angle
        int emitter = 0;
        bool shown = true;     // twinkle's share
    };
    struct RibbonPoint {
        glm::vec3 pos, up;
        float age = 0;
    };
    struct Ribbon {
        std::vector<RibbonPoint> points;   // newest first
        float sinceEdge = 0;
        glm::vec4 color{1.0f};
        float above = 0, below = 0;
    };

    const Model& model_;
    std::mt19937 rng_;
    std::vector<float> owed_;   // particles owed per emitter (fractions carry over)
    std::vector<Particle> particles_;
    std::vector<Ribbon> ribbons_;
    float worldScale_ = 1.0f;
};
}  // namespace wowcore
