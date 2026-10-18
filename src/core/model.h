#pragma once
#include "core/math.h"

#include <array>
#include <map>
#include <string>
#include <vector>

namespace rig {

struct Transform {
    float x = 0, y = 0, rotation = 0, scaleX = 1, scaleY = 1;
    float shearX = 0, shearY = 0;  // degrees: extra rotation of the local X / Y axis
    Affine matrix() const;
};
// Decompose an affine matrix into a transform (shearX is always 0; any skew goes to shearY).
Transform decompose(const Affine& m);

struct Color {
    float v[4] = {1, 1, 1, 1};
    Color() = default;
    Color(float r, float g, float b, float a) : v{r, g, b, a} {}
    float& operator[](int i) { return v[i]; }
    float operator[](int i) const { return v[i]; }
    Color operator+(const Color& o) const { return {v[0] + o.v[0], v[1] + o.v[1], v[2] + o.v[2], v[3] + o.v[3]}; }
    Color operator-(const Color& o) const { return {v[0] - o.v[0], v[1] - o.v[1], v[2] - o.v[2], v[3] - o.v[3]}; }
    Color operator*(float s) const { return {v[0] * s, v[1] * s, v[2] * s, v[3] * s}; }
    bool operator==(const Color& o) const { return v[0] == o.v[0] && v[1] == o.v[1] && v[2] == o.v[2] && v[3] == o.v[3]; }
};

struct Bone {
    std::string name;
    int parent = -1;
    float length = 0;
    Transform setup;
    bool skinRequired = false;  // only active when an active skin lists it
};

struct Image {
    std::string path;  // relative to the project file when saved
    int width = 0, height = 0;
};

enum class AttachmentType { Region, Mesh, Clipping, BoundingBox, Path };

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

    // Mesh, clipping and bounding box attachments share the vertex storage. Polygons
    // (clipping / bounding box) have no uvs or triangles; their vertices are the outline.
    std::vector<Vec2> uvs;
    std::vector<int> triangles;
    std::vector<Vec2> vertices;                      // unweighted: slot-bone local
    std::vector<std::vector<VertexWeight>> weights;  // weighted: one list per vertex
    int hullLength = 0;  // mesh: the first hullLength vertices form the outline in order (0 = unknown)
    int endSlot = -1;    // clipping: last slot (index) that is clipped, -1 = all following slots
    // Path: vertices are Bezier control points, three per path point: in-handle, point, out-handle.
    bool closed = false;
    // Skin placeholder: has no content of its own; the active skins provide an attachment
    // registered under this attachment's name for this slot.
    bool skinPlaceholder = false;

    bool isWeighted() const { return !weights.empty(); }
    // Polygons and paths have no uvs or triangles (paths are edited like polygons).
    bool isPolygon() const {
        return type == AttachmentType::Clipping || type == AttachmentType::BoundingBox || type == AttachmentType::Path;
    }
    bool hasVertices() const { return type != AttachmentType::Region; }
    size_t vertexCount() const { return isWeighted() ? weights.size() : vertices.size(); }
};

struct Slot {
    std::string name;
    int bone = 0;
    std::vector<Attachment> attachments;
    int activeAttachment = -1;  // setup-pose attachment, -1 = none
    Color color;                // tint multiplied with the attachment's pixels
};

// Event definition; keys may override its values.
struct EventDef {
    std::string name;
    int intValue = 0;
    float floatValue = 0;
    std::string stringValue;
    std::string audioPath;  // optional sound played by the event
    float volume = 1, balance = 0;
};

// ---- curves ----

enum class Curve { Linear, Stepped, Bezier };

// Interpolation from a key to the next one. Bezier control points are normalised:
// x = fraction of the time between the keys, y = fraction of the value change.
struct CurveShape {
    Curve type = Curve::Linear;
    float cx1 = 0.25f, cy1 = 0.0f, cx2 = 0.75f, cy2 = 1.0f;
    static CurveShape linear() { return {}; }
    static CurveShape stepped() { CurveShape c; c.type = Curve::Stepped; return c; }
    static CurveShape bezier(float x1, float y1, float x2, float y2) { return {Curve::Bezier, x1, y1, x2, y2}; }
};
// Maps a time fraction in [0,1] to a value fraction (Bezier y may leave [0,1] for overshoot).
float applyCurve(const CurveShape& c, float t);

// ---- keys & timelines ----

struct FloatKey { float time = 0; float value = 0; CurveShape curve; };
struct Vec2Key { float time = 0; Vec2 value; CurveShape curve; };  // one curve drives both components
struct ColorKey { float time = 0; Color value; CurveShape curve; };
struct IntKey { float time = 0; int value = -1; };
// Per-vertex offsets added to a mesh / polygon attachment (slot-bone space for unweighted
// vertices, blended bone space for weighted ones).
struct DeformKey { float time = 0; std::vector<Vec2> value; CurveShape curve; };
// Draw order as slot indices from back to front; always a permutation of all slots.
struct DrawOrderKey { float time = 0; std::vector<int> order; };
struct EventKey {
    float time = 0;
    int event = 0;  // index into Skeleton::events
    int intValue = 0;
    float floatValue = 0;
    std::string stringValue;
};

// Rotate/translate/shear values are added to the setup pose; scale values multiply it.
struct BoneTimeline {
    std::vector<FloatKey> rotate;
    std::vector<Vec2Key> translate;
    std::vector<Vec2Key> scale;
    std::vector<Vec2Key> shear;
    bool empty() const { return rotate.empty() && translate.empty() && scale.empty() && shear.empty(); }
};

struct SlotTimeline {
    std::vector<IntKey> attachment;
    std::vector<ColorKey> color;                    // absolute colour, replaces the setup colour
    std::map<int, std::vector<DeformKey>> deform;  // per attachment index
    bool empty() const {
        if (!attachment.empty() || !color.empty()) return false;
        for (auto& [a, keys] : deform)
            if (!keys.empty()) return false;
        return true;
    }
};

// ---- skins ----

struct SkinEntry {
    int slot = 0;
    std::string placeholder;  // name of the placeholder attachment in the slot
    Attachment attachment;
};

struct Skin {
    std::string name;
    std::vector<SkinEntry> attachments;
    std::vector<int> bones;        // skin-required bones this skin activates
    std::vector<int> constraints;  // skin-required constraints this skin activates

    Attachment* find(int slot, const std::string& placeholder);
    const Attachment* find(int slot, const std::string& placeholder) const;
};

// ---- constraints ----

enum class ConstraintType { Ik, Transform, Path, Physics };
enum class PathPositionMode { Fixed, Percent };
enum class PathSpacingMode { Length, Fixed, Percent };
enum class PathRotateMode { Tangent, Chain, ChainScale };

// Keyable constraint values, meaning per type (see constraintValueNames):
//   Ik:        mix, softness, bendDirection (stepped)
//   Transform: mixRotate, mixX, mixY, mixScaleX, mixScaleY, mixShearY
//   Path:      position, spacing, mixRotate, mixX, mixY
//   Physics:   mix, wind, gravity
using ConstraintValues = std::array<float, 6>;

struct Constraint {
    ConstraintType type = ConstraintType::Ik;
    std::string name;
    std::vector<int> bones;  // constrained bones (Ik: 1 or 2, parent first; Physics: 1)
    int target = -1;         // Ik / Transform: target bone; Path: slot holding the path attachment
    ConstraintValues values{1, 0, 1, 1, 1, 1};

    // Ik
    bool compress = false, stretch = false, uniform = false;
    // Transform: offsets added to the target's values
    float offsetRotation = 0, offsetX = 0, offsetY = 0, offsetScaleX = 0, offsetScaleY = 0, offsetShearY = 0;
    bool local = false, relative = false;
    // Path
    PathPositionMode positionMode = PathPositionMode::Percent;
    PathSpacingMode spacingMode = PathSpacingMode::Length;
    PathRotateMode rotateMode = PathRotateMode::Tangent;
    // Physics
    bool physX = true, physY = true, physRotate = true;
    float inertia = 1, strength = 100, damping = 0.25f, mass = 1, limit = 5000;  // damping 1 = no wobble
    bool skinRequired = false;  // only active when an active skin lists it
};
// Sensible default values for a new constraint of `type`.
ConstraintValues defaultConstraintValues(ConstraintType type);
// Number of keyable values for a type and their display names.
int constraintValueCount(ConstraintType type);
const char* constraintValueName(ConstraintType type, int index);

struct ConstraintKey { float time = 0; ConstraintValues value{}; CurveShape curve; };

enum class Channel { Rotate, Translate, Scale, Shear, Attachment, Color, DrawOrder, Event, Deform, Constraint };
inline bool isBoneChannel(Channel c) { return c == Channel::Rotate || c == Channel::Translate || c == Channel::Scale || c == Channel::Shear; }
inline bool isSlotChannel(Channel c) { return c == Channel::Attachment || c == Channel::Color || c == Channel::Deform; }

struct Animation {
    std::string name;
    float duration = 1.0f;
    std::map<int, BoneTimeline> bones;
    std::map<int, SlotTimeline> slots;
    std::vector<DrawOrderKey> drawOrder;
    std::vector<EventKey> events;  // sorted by time
    std::map<int, std::vector<ConstraintKey>> constraints;  // per constraint index
};

struct Skeleton {
    std::vector<Bone> bones;
    std::vector<Slot> slots;  // order = setup draw order (back to front)
    std::vector<Image> images;
    std::vector<EventDef> events;
    std::vector<Constraint> constraints;  // applied in this order after the bone hierarchy
    std::vector<Skin> skins;
    std::vector<Animation> animations;

    int findBone(const std::string& name) const;
    bool isAncestor(int ancestor, int bone) const;  // true if ancestor == bone too
    std::vector<int> children(int bone) const;
    // Parents always come before children in the returned order.
    std::vector<int> updateOrder() const;

    std::string uniqueBoneName(const std::string& base) const;
    std::string uniqueSlotName(const std::string& base) const;
    std::string uniqueEventName(const std::string& base) const;
    std::string uniqueConstraintName(const std::string& base) const;

    int addBone(const std::string& name, int parent, const Transform& setup, float length);
    // Removes the bone, its descendants, their slots and every reference to them.
    void removeBone(int bone);
    // Changes the parent while keeping the bone's setup world transform.
    bool reparentBone(int bone, int newParent);

    // Slot index changes, keeping every animation reference (timelines, draw order keys) valid.
    void removeSlot(int slot);
    void swapSlots(int a, int b);  // swaps setup draw order positions
    // Removes an event definition and its keys.
    void removeEvent(int event);
    // Removes a constraint and its keys.
    void removeConstraint(int constraint);
    void removeSkin(int skin);
    std::string uniqueSkinName(const std::string& base) const;
    // Attachment shown for slot attachment `index` with `activeSkins` (later skins win), or null
    // when it is a placeholder no active skin fills.
    const Attachment* resolve(const std::vector<int>& activeSkins, int slot, int index) const;
    Attachment* resolve(const std::vector<int>& activeSkins, int slot, int index);
    // Swaps two constraints (application order), keeping their keys.
    void swapConstraints(int a, int b);
    // Removes an attachment from a slot, fixing the active attachment, attachment keys and deform keys.
    void removeAttachment(int slot, int attachment);
    // Drops deform keys whose vertex count no longer matches the attachment (after topology edits).
    int dropStaleDeformKeys(int slot, int attachment);
};

// Insert or replace a key at `time` (within a small epsilon). Keeps keys sorted.
void setRotateKey(Animation& anim, int bone, float time, float value);
void setTranslateKey(Animation& anim, int bone, float time, Vec2 value);
void setScaleKey(Animation& anim, int bone, float time, Vec2 value);
void setShearKey(Animation& anim, int bone, float time, Vec2 value);
void setAttachmentKey(Animation& anim, int slot, float time, int attachment);
void setColorKey(Animation& anim, int slot, float time, const Color& color);
void setDrawOrderKey(Animation& anim, float time, const std::vector<int>& order);
// Events can share a time only if they are different events; same event at same time replaces.
void setEventKey(Animation& anim, float time, int event, const EventDef& def);
void setDeformKey(Animation& anim, int slot, int attachment, float time, const std::vector<Vec2>& offsets);
void setConstraintKey(Animation& anim, int constraint, float time, const ConstraintValues& values);

// `index` = bone for bone channels, slot for slot channels, event for Event, ignored for DrawOrder.
// `sub` = attachment index for Deform.
bool removeKey(Animation& anim, int index, Channel ch, float time, int sub = -1);
bool hasKey(const Animation& anim, int index, Channel ch, float time, int sub = -1);
void setKeyCurve(Animation& anim, int index, Channel ch, float time, const CurveShape& curve, int sub = -1);
const CurveShape* keyCurve(const Animation& anim, int index, Channel ch, float time, int sub = -1);
// Move every key (all channels) located at `from` to `to`.
void moveKeys(Animation& anim, float from, float to);
// Same, restricted to one bone's channels (isSlot = false) or one slot's channels.
void moveRowKeys(Animation& anim, bool isSlot, int index, float from, float to);
// Same, restricted to a single channel of one bone or slot (or the draw order / an event).
void moveChannelKey(Animation& anim, int index, Channel ch, float from, float to, int sub = -1);
// Sorted, de-duplicated times of every key in the animation (or one row of it).
std::vector<float> keyTimes(const Animation& anim);
std::vector<float> rowKeyTimes(const Animation& anim, bool isSlot, int index);
// Copies the key of one channel at `time` from `src` into `dst` at `newTime` (replacing what is
// there). Index / sub as for removeKey; Event with index < 0 copies every event at that time.
void copyKey(const Animation& src, Animation& dst, int index, Channel ch, int sub, float time, float newTime);
// Inserts every key of `src` into `dst`, shifted by `offset` seconds (same-time keys are replaced).
void mergeAnimation(Animation& dst, const Animation& src, float offset);
// Events whose time is in (from, to]; from < 0 includes time 0.
std::vector<const EventKey*> eventsInRange(const Animation& anim, float from, float to);

constexpr float kKeyEpsilon = 1e-4f;

} // namespace rig
