#pragma once
#include "core/math.h"

#include <map>
#include <string>
#include <vector>

namespace rig {

struct Transform {
    float x = 0, y = 0, rotation = 0, scaleX = 1, scaleY = 1;
    Affine matrix() const { return Affine::fromTRS(x, y, rotation, scaleX, scaleY); }
};
// Decompose an affine matrix into TRS (any shear is dropped).
Transform decompose(const Affine& m);

struct Bone {
    std::string name;
    int parent = -1;
    float length = 0;
    Transform setup;
};

struct Image {
    std::string path;  // relative to the project file when saved
    int width = 0, height = 0;
};

enum class AttachmentType { Region, Mesh };

struct VertexWeight {
    int bone = 0;
    Vec2 local;  // vertex position in the bone's setup-pose local space
    float weight = 0;
};

struct Attachment {
    std::string name;
    AttachmentType type = AttachmentType::Region;
    int image = -1;

    // Region: image centred at the origin, placed by `transform` in slot-bone space.
    Transform transform;

    // Mesh
    std::vector<Vec2> uvs;
    std::vector<int> triangles;
    std::vector<Vec2> vertices;                      // unweighted: slot-bone local
    std::vector<std::vector<VertexWeight>> weights;  // weighted: one list per vertex

    bool isWeighted() const { return !weights.empty(); }
    size_t vertexCount() const { return uvs.size(); }
};

struct Slot {
    std::string name;
    int bone = 0;
    std::vector<Attachment> attachments;
    int activeAttachment = -1;  // setup-pose attachment, -1 = none
    float color[4] = {1, 1, 1, 1};
};

enum class Curve { Linear, Stepped };

struct FloatKey { float time = 0; float value = 0; Curve curve = Curve::Linear; };
struct Vec2Key { float time = 0; Vec2 value; Curve curve = Curve::Linear; };
struct IntKey { float time = 0; int value = -1; };

// Rotate/translate values are added to the setup pose; scale values multiply it.
struct BoneTimeline {
    std::vector<FloatKey> rotate;
    std::vector<Vec2Key> translate;
    std::vector<Vec2Key> scale;
    bool empty() const { return rotate.empty() && translate.empty() && scale.empty(); }
};

struct SlotTimeline {
    std::vector<IntKey> attachment;
    bool empty() const { return attachment.empty(); }
};

enum class Channel { Rotate, Translate, Scale, Attachment };

struct Animation {
    std::string name;
    float duration = 1.0f;
    std::map<int, BoneTimeline> bones;
    std::map<int, SlotTimeline> slots;
};

struct Skeleton {
    std::vector<Bone> bones;
    std::vector<Slot> slots;  // order = draw order
    std::vector<Image> images;
    std::vector<Animation> animations;

    int findBone(const std::string& name) const;
    bool isAncestor(int ancestor, int bone) const;  // true if ancestor == bone too
    std::vector<int> children(int bone) const;
    // Parents always come before children in the returned order.
    std::vector<int> updateOrder() const;

    std::string uniqueBoneName(const std::string& base) const;
    std::string uniqueSlotName(const std::string& base) const;

    int addBone(const std::string& name, int parent, const Transform& setup, float length);
    // Removes the bone, its descendants, their slots and every reference to them.
    void removeBone(int bone);
    // Changes the parent while keeping the bone's setup world transform.
    bool reparentBone(int bone, int newParent);
};

// Insert or replace a key at `time` (within a small epsilon). Keeps keys sorted.
void setRotateKey(Animation& anim, int bone, float time, float value);
void setTranslateKey(Animation& anim, int bone, float time, Vec2 value);
void setScaleKey(Animation& anim, int bone, float time, Vec2 value);
void setAttachmentKey(Animation& anim, int slot, float time, int attachment);
bool removeKey(Animation& anim, int index, Channel ch, float time);  // index = bone or slot
bool hasKey(const Animation& anim, int index, Channel ch, float time);
void setKeyCurve(Animation& anim, int bone, Channel ch, float time, Curve curve);
// Move every key (all channels) located at `from` to `to`.
void moveKeys(Animation& anim, float from, float to);
// Same, restricted to one bone's channels (isSlot = false) or one slot's channels.
void moveRowKeys(Animation& anim, bool isSlot, int index, float from, float to);
// Same, restricted to a single channel of one bone or slot.
void moveChannelKey(Animation& anim, int index, Channel ch, float from, float to);
// Sorted, de-duplicated times of every key in the animation (or one row of it).
std::vector<float> keyTimes(const Animation& anim);
std::vector<float> rowKeyTimes(const Animation& anim, bool isSlot, int index);

constexpr float kKeyEpsilon = 1e-4f;

} // namespace rig
