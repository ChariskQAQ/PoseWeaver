#pragma once
#include "core/model.h"

#include <array>

namespace rig {

struct Pose;

// Applies IK, transform and path constraints in skeleton order (called by updateWorld).
void applyConstraints(const Skeleton& sk, Pose& pose);

// Sets a bone's world transform by changing its applied local transform, then updates its
// descendants. Used by the solvers; the bone keeps any scale/shear the matrix carries.
void setBoneWorld(const Skeleton& sk, Pose& pose, int bone, const Affine& world);

// ---- posing ----

// Cyclic coordinate descent: local rotations for `chain` (bones from the top of the chain to
// the tip bone, each the parent of the next) so the tip of the last bone reaches `target`
// (world). Starts from `pose`; returns one local rotation per chain bone, kept continuous
// with the starting rotations.
std::vector<float> solveChainIk(const Skeleton& sk, const Pose& pose, const std::vector<int>& chain, Vec2 target,
                                int iterations = 12);

// ---- paths ----

// Bezier segments of a path attachment's world-space control points (3 per path point).
struct PathCurve {
    std::vector<std::array<Vec2, 4>> segments;
    std::vector<float> lengths;  // cumulative arc length at the end of each segment
    float total = 0;
};
PathCurve buildPathCurve(const std::vector<Vec2>& controlPoints, bool closed);
// Point and tangent angle (degrees) at arc length `s` (clamped / wrapped when closed).
void evaluatePath(const PathCurve& path, float s, bool closed, Vec2& point, float& tangentDeg);
// Polyline approximation for drawing.
std::vector<Vec2> samplePath(const PathCurve& path, int perSegment = 16);

// ---- physics ----

// Simulation state of every physics constraint. Owned by whoever advances time (the editor
// or a game runtime), because physics depends on the previous frames, not only on `time`.
struct PhysicsState {
    struct Body {
        bool initialised = false;
        Vec2 lastOrigin, lastTip;  // bone positions at the previous step (world)
        Vec2 offset, velocity;     // translation lag
        Vec2 tipOffset, tipVelocity;  // tip lag, gives the rotation
        float remainder = 0;       // unsimulated time
    };
    std::vector<Body> bodies;
    void reset() { bodies.clear(); }
};
// Advances the physics constraints by `dt` seconds (fixed 60 Hz steps) and applies them to the
// pose. dt = 0 only re-applies the current state.
void applyPhysics(const Skeleton& sk, Pose& pose, PhysicsState& state, float dt);

} // namespace rig
