#pragma once
#include "core/model.h"

namespace rig {

// Evaluated state of a skeleton: what gets drawn.
struct Pose {
    std::vector<Transform> local;     // per bone: setup + animation (what gets keyed)
    std::vector<Transform> applied;   // per bone: after constraints (what world is computed from)
    std::vector<Affine> world;        // per bone
    std::vector<int> slotAttachment;  // per slot, -1 = none
    std::vector<Color> slotColor;     // per slot
    std::vector<int> drawOrder;       // slot indices, back to front
    std::map<std::pair<int, int>, std::vector<Vec2>> deform;  // (slot, attachment) -> vertex offsets
    std::vector<ConstraintValues> constraints;                // per constraint: setup or animated values
    std::vector<int> skins;            // active skins (later ones win); set by the caller
    std::vector<char> boneActive;      // per bone: false when skin-required and not in an active skin
    std::vector<char> constraintActive;
};
// The attachment a slot shows in this pose (resolved through the active skins), or null.
const Attachment* poseAttachment(const Skeleton& sk, const Pose& pose, int slot);
// Bone / constraint activity from the pose's active skins (called by updateWorld).
void updateActivity(const Skeleton& sk, Pose& pose);
// Deform offsets of an attachment in this pose, or null.
const std::vector<Vec2>* findDeform(const Pose& pose, int slot, int attachment);

void resetToSetup(const Skeleton& sk, Pose& pose);
void applyAnimation(const Skeleton& sk, const Animation& anim, float time, Pose& pose);
// World transforms from `local`, then every constraint (except physics) in order.
void updateWorld(const Skeleton& sk, Pose& pose);
// World transforms of `bone` and its descendants from `applied` (used by constraint solvers).
void updateSubtree(const Skeleton& sk, Pose& pose, int bone);
Pose makeSetupPose(const Skeleton& sk, const std::vector<int>& skins = {});
Pose makeAnimatedPose(const Skeleton& sk, const Animation* anim, float time, const std::vector<int>& skins = {});

float sample(const std::vector<FloatKey>& keys, float t, float fallback);
Vec2 sample(const std::vector<Vec2Key>& keys, float t, Vec2 fallback);
int sample(const std::vector<IntKey>& keys, float t, int fallback);
Color sample(const std::vector<ColorKey>& keys, float t, const Color& fallback);

// Tip of a bone (end of its length along the local x axis) in world space.
Vec2 boneTip(const Bone& bone, const Affine& world);

struct Geometry {
    std::vector<Vec2> positions;  // world space
    std::vector<Vec2> uvs;
    std::vector<int> triangles;
};
// `deform` (optional) holds per-vertex offsets, see DeformKey.
void computeGeometry(const Skeleton& sk, const Pose& pose, const Slot& slot, const Attachment& att, Geometry& out,
                     const std::vector<Vec2>* deform = nullptr);
std::vector<Vec2> computeVertices(const Skeleton& sk, const Pose& pose, const Slot& slot, const Attachment& att,
                                  const std::vector<Vec2>* deform = nullptr);
// Linear part that maps a deform offset of `vertex` to world space in `pose`.
Affine deformBasis(const Pose& pose, const Slot& slot, const Attachment& att, int vertex);

} // namespace rig
