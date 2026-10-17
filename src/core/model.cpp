#include "core/model.h"
#include "core/pose.h"

#include <algorithm>
#include <functional>

namespace rig {

// ---------------------------------------------------------------- transforms

Affine Transform::matrix() const {
    // The X axis is rotated by rotation + shearX, the Y axis by rotation + 90 + shearY.
    float rx = degToRad(rotation + shearX), ry = degToRad(rotation + 90.0f + shearY);
    return {std::cos(rx) * scaleX, std::cos(ry) * scaleY, std::sin(rx) * scaleX, std::sin(ry) * scaleY, x, y};
}

Transform decompose(const Affine& m) {
    Transform t;
    t.x = m.tx;
    t.y = m.ty;
    t.rotation = radToDeg(std::atan2(m.c, m.a));
    t.scaleX = std::sqrt(m.a * m.a + m.c * m.c);
    float sy = std::sqrt(m.b * m.b + m.d * m.d);
    float by = m.b, dy = m.d;
    if (m.det() < 0) {  // reflection: express it as a negative Y scale
        sy = -sy;
        by = -by;
        dy = -dy;
    }
    t.scaleY = sy;
    t.shearY = wrapDeg(radToDeg(std::atan2(dy, by)) - t.rotation - 90.0f);
    if (std::fabs(t.shearY) < 1e-4f) t.shearY = 0;
    return t;
}

// ---------------------------------------------------------------- curves

static float bezierAxis(float s, float p1, float p2) {
    float u = 1 - s;
    return 3 * u * u * s * p1 + 3 * u * s * s * p2 + s * s * s;
}

float applyCurve(const CurveShape& c, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    if (c.type == Curve::Linear) return t;
    if (c.type == Curve::Stepped) return t >= 1.0f ? 1.0f : 0.0f;
    // Solve x(s) = t for s (x is monotonic because control x values are clamped to [0,1]).
    float x1 = std::clamp(c.cx1, 0.0f, 1.0f), x2 = std::clamp(c.cx2, 0.0f, 1.0f);
    float lo = 0, hi = 1, s = t;
    for (int i = 0; i < 24; ++i) {
        float x = bezierAxis(s, x1, x2);
        if (std::fabs(x - t) < 1e-6f) break;
        if (x < t) lo = s;
        else hi = s;
        s = (lo + hi) * 0.5f;
    }
    return bezierAxis(s, c.cy1, c.cy2);
}

// ---------------------------------------------------------------- skeleton

int Skeleton::findBone(const std::string& name) const {
    for (size_t i = 0; i < bones.size(); ++i)
        if (bones[i].name == name) return (int)i;
    return -1;
}

bool Skeleton::isAncestor(int ancestor, int bone) const {
    for (int b = bone; b >= 0; b = bones[b].parent)
        if (b == ancestor) return true;
    return false;
}

std::vector<int> Skeleton::children(int bone) const {
    std::vector<int> out;
    for (size_t i = 0; i < bones.size(); ++i)
        if (bones[i].parent == bone) out.push_back((int)i);
    return out;
}

std::vector<int> Skeleton::updateOrder() const {
    std::vector<int> order;
    order.reserve(bones.size());
    std::function<void(int)> visit = [&](int parent) {
        for (size_t i = 0; i < bones.size(); ++i) {
            if (bones[i].parent != parent) continue;
            order.push_back((int)i);
            visit((int)i);
        }
    };
    visit(-1);
    return order;
}

static std::string uniqueName(const std::string& base, const std::function<bool(const std::string&)>& taken) {
    if (!taken(base)) return base;
    for (int i = 2;; ++i) {
        std::string n = base + std::to_string(i);
        if (!taken(n)) return n;
    }
}

std::string Skeleton::uniqueBoneName(const std::string& base) const {
    return uniqueName(base, [&](const std::string& n) { return findBone(n) >= 0; });
}

std::string Skeleton::uniqueSlotName(const std::string& base) const {
    return uniqueName(base, [&](const std::string& n) {
        for (auto& s : slots)
            if (s.name == n) return true;
        return false;
    });
}

std::string Skeleton::uniqueEventName(const std::string& base) const {
    return uniqueName(base, [&](const std::string& n) {
        for (auto& e : events)
            if (e.name == n) return true;
        return false;
    });
}

int Skeleton::addBone(const std::string& name, int parent, const Transform& setup, float length) {
    Bone b;
    b.name = uniqueBoneName(name);
    b.parent = parent;
    b.setup = setup;
    b.length = length;
    bones.push_back(b);
    return (int)bones.size() - 1;
}

// Applies an old→new slot index map (-1 = removed) to every animation.
static void remapSlots(std::vector<Animation>& animations, const std::vector<int>& slotRemap) {
    for (auto& an : animations) {
        std::map<int, SlotTimeline> st;
        for (auto& [i, tl] : an.slots)
            if (i < (int)slotRemap.size() && slotRemap[i] >= 0) st[slotRemap[i]] = std::move(tl);
        an.slots = std::move(st);
        for (auto& k : an.drawOrder) {
            std::vector<int> order;
            for (int s : k.order)
                if (s < (int)slotRemap.size() && slotRemap[s] >= 0) order.push_back(slotRemap[s]);
            k.order = std::move(order);
        }
    }
}

void Skeleton::removeBone(int bone) {
    if (bone < 0 || bone >= (int)bones.size()) return;
    Pose setupPose = makeSetupPose(*this);

    std::vector<bool> removed(bones.size(), false);
    for (size_t i = 0; i < bones.size(); ++i) removed[i] = isAncestor(bone, (int)i);

    std::vector<int> remap(bones.size(), -1);
    int next = 0;
    for (size_t i = 0; i < bones.size(); ++i)
        if (!removed[i]) remap[i] = next++;

    // Slots attached to removed bones go away; mesh weights on removed bones are dropped.
    std::vector<Slot> keptSlots;
    std::vector<int> slotRemap(slots.size(), -1);
    for (size_t si = 0; si < slots.size(); ++si) {
        Slot& s = slots[si];
        if (removed[s.bone]) continue;
        for (auto& att : s.attachments) {
            if (!att.isWeighted()) continue;
            std::vector<Vec2> world = computeVertices(*this, setupPose, s, att);
            for (size_t v = 0; v < att.weights.size(); ++v) {
                auto& list = att.weights[v];
                list.erase(std::remove_if(list.begin(), list.end(),
                                          [&](const VertexWeight& w) { return removed[w.bone]; }),
                           list.end());
                float total = 0;
                for (auto& w : list) total += w.weight;
                if (list.empty() || total <= 1e-6f) {
                    VertexWeight w;
                    w.bone = s.bone;
                    w.local = setupPose.world[s.bone].inverse().apply(world[v]);
                    w.weight = 1;
                    list.assign(1, w);
                } else {
                    for (auto& w : list) w.weight /= total;
                }
                for (auto& w : list) w.bone = remap[w.bone];
            }
        }
        s.bone = remap[s.bone];
        slotRemap[si] = (int)keptSlots.size();
        keptSlots.push_back(std::move(s));
    }
    slots = std::move(keptSlots);

    std::vector<Bone> keptBones;
    for (size_t i = 0; i < bones.size(); ++i) {
        if (removed[i]) continue;
        Bone b = bones[i];
        b.parent = b.parent >= 0 ? remap[b.parent] : -1;
        keptBones.push_back(b);
    }
    bones = std::move(keptBones);

    for (auto& anim : animations) {
        std::map<int, BoneTimeline> bt;
        for (auto& [i, tl] : anim.bones)
            if (remap[i] >= 0) bt[remap[i]] = std::move(tl);
        anim.bones = std::move(bt);
    }
    remapSlots(animations, slotRemap);
}

bool Skeleton::reparentBone(int bone, int newParent) {
    if (bone < 0 || bone >= (int)bones.size()) return false;
    if (newParent >= 0 && isAncestor(bone, newParent)) return false;  // would create a cycle
    Pose pose = makeSetupPose(*this);
    Affine parentWorld = newParent >= 0 ? pose.world[newParent] : Affine{};
    bones[bone].setup = decompose(parentWorld.inverse() * pose.world[bone]);
    bones[bone].parent = newParent;
    return true;
}

void Skeleton::removeSlot(int slot) {
    if (slot < 0 || slot >= (int)slots.size()) return;
    std::vector<int> slotRemap(slots.size());
    for (int i = 0; i < (int)slots.size(); ++i) slotRemap[i] = i < slot ? i : (i == slot ? -1 : i - 1);
    slots.erase(slots.begin() + slot);
    remapSlots(animations, slotRemap);
}

void Skeleton::swapSlots(int a, int b) {
    if (a < 0 || b < 0 || a >= (int)slots.size() || b >= (int)slots.size() || a == b) return;
    std::swap(slots[a], slots[b]);
    std::vector<int> slotRemap(slots.size());
    for (int i = 0; i < (int)slots.size(); ++i) slotRemap[i] = i == a ? b : (i == b ? a : i);
    remapSlots(animations, slotRemap);
}

void Skeleton::removeEvent(int event) {
    if (event < 0 || event >= (int)events.size()) return;
    events.erase(events.begin() + event);
    for (auto& an : animations) {
        an.events.erase(std::remove_if(an.events.begin(), an.events.end(),
                                       [&](const EventKey& k) { return k.event == event; }),
                        an.events.end());
        for (auto& k : an.events)
            if (k.event > event) --k.event;
    }
}

// ---------------------------------------------------------------- keys

template <typename K>
static typename std::vector<K>::iterator insertSorted(std::vector<K>& keys, K k) {
    return keys.insert(std::upper_bound(keys.begin(), keys.end(), k.time,
                                        [](float t, const K& key) { return t < key.time; }),
                       std::move(k));
}

template <typename K, typename V>
static void upsert(std::vector<K>& keys, float time, const V& value) {
    for (auto& k : keys) {
        if (std::fabs(k.time - time) < kKeyEpsilon) {
            k.value = value;
            return;
        }
    }
    K k;
    k.time = time;
    k.value = value;
    insertSorted(keys, k);
}

template <typename K>
static K* findAt(std::vector<K>& keys, float time) {
    for (auto& k : keys)
        if (std::fabs(k.time - time) < kKeyEpsilon) return &k;
    return nullptr;
}

template <typename K>
static bool eraseAt(std::vector<K>& keys, float time) {
    auto it = std::find_if(keys.begin(), keys.end(),
                           [&](const K& k) { return std::fabs(k.time - time) < kKeyEpsilon; });
    if (it == keys.end()) return false;
    keys.erase(it);
    return true;
}

template <typename K>
static bool contains(const std::vector<K>& keys, float time) {
    for (auto& k : keys)
        if (std::fabs(k.time - time) < kKeyEpsilon) return true;
    return false;
}

void setRotateKey(Animation& anim, int bone, float time, float value) { upsert(anim.bones[bone].rotate, time, value); }
void setTranslateKey(Animation& anim, int bone, float time, Vec2 value) { upsert(anim.bones[bone].translate, time, value); }
void setScaleKey(Animation& anim, int bone, float time, Vec2 value) { upsert(anim.bones[bone].scale, time, value); }
void setShearKey(Animation& anim, int bone, float time, Vec2 value) { upsert(anim.bones[bone].shear, time, value); }
void setAttachmentKey(Animation& anim, int slot, float time, int attachment) { upsert(anim.slots[slot].attachment, time, attachment); }
void setColorKey(Animation& anim, int slot, float time, const Color& color) { upsert(anim.slots[slot].color, time, color); }

void setDrawOrderKey(Animation& anim, float time, const std::vector<int>& order) {
    if (DrawOrderKey* k = findAt(anim.drawOrder, time)) {
        k->order = order;
        return;
    }
    insertSorted(anim.drawOrder, DrawOrderKey{time, order});
}

void setEventKey(Animation& anim, float time, int event, const EventDef& def) {
    for (auto& k : anim.events)
        if (k.event == event && std::fabs(k.time - time) < kKeyEpsilon) return;  // already there
    EventKey k;
    k.time = time;
    k.event = event;
    k.intValue = def.intValue;
    k.floatValue = def.floatValue;
    k.stringValue = def.stringValue;
    insertSorted(anim.events, k);
}

bool removeKey(Animation& anim, int index, Channel ch, float time) {
    switch (ch) {
    case Channel::DrawOrder: return eraseAt(anim.drawOrder, time);
    case Channel::Event: {
        auto it = std::find_if(anim.events.begin(), anim.events.end(), [&](const EventKey& k) {
            return std::fabs(k.time - time) < kKeyEpsilon && (index < 0 || k.event == index);
        });
        if (it == anim.events.end()) return false;
        anim.events.erase(it);
        return true;
    }
    case Channel::Attachment:
    case Channel::Color: {
        auto it = anim.slots.find(index);
        if (it == anim.slots.end()) return false;
        bool ok = ch == Channel::Attachment ? eraseAt(it->second.attachment, time) : eraseAt(it->second.color, time);
        if (it->second.empty()) anim.slots.erase(it);
        return ok;
    }
    default: {
        auto it = anim.bones.find(index);
        if (it == anim.bones.end()) return false;
        BoneTimeline& tl = it->second;
        bool ok = false;
        if (ch == Channel::Rotate) ok = eraseAt(tl.rotate, time);
        if (ch == Channel::Translate) ok = eraseAt(tl.translate, time);
        if (ch == Channel::Scale) ok = eraseAt(tl.scale, time);
        if (ch == Channel::Shear) ok = eraseAt(tl.shear, time);
        if (tl.empty()) anim.bones.erase(it);
        return ok;
    }
    }
}

bool hasKey(const Animation& anim, int index, Channel ch, float time) {
    switch (ch) {
    case Channel::DrawOrder: return contains(anim.drawOrder, time);
    case Channel::Event:
        for (auto& k : anim.events)
            if (std::fabs(k.time - time) < kKeyEpsilon && (index < 0 || k.event == index)) return true;
        return false;
    case Channel::Attachment:
    case Channel::Color: {
        auto it = anim.slots.find(index);
        if (it == anim.slots.end()) return false;
        return ch == Channel::Attachment ? contains(it->second.attachment, time) : contains(it->second.color, time);
    }
    default: {
        auto it = anim.bones.find(index);
        if (it == anim.bones.end()) return false;
        const BoneTimeline& tl = it->second;
        if (ch == Channel::Rotate) return contains(tl.rotate, time);
        if (ch == Channel::Translate) return contains(tl.translate, time);
        if (ch == Channel::Scale) return contains(tl.scale, time);
        return contains(tl.shear, time);
    }
    }
}

// Pointer to the curve of a curve-carrying key, or null.
static CurveShape* curveAt(Animation& anim, int index, Channel ch, float time) {
    if (ch == Channel::Color) {
        auto it = anim.slots.find(index);
        if (it == anim.slots.end()) return nullptr;
        ColorKey* k = findAt(it->second.color, time);
        return k ? &k->curve : nullptr;
    }
    if (!isBoneChannel(ch)) return nullptr;
    auto it = anim.bones.find(index);
    if (it == anim.bones.end()) return nullptr;
    BoneTimeline& tl = it->second;
    if (ch == Channel::Rotate) {
        FloatKey* k = findAt(tl.rotate, time);
        return k ? &k->curve : nullptr;
    }
    std::vector<Vec2Key>& keys = ch == Channel::Translate ? tl.translate : ch == Channel::Scale ? tl.scale : tl.shear;
    Vec2Key* k = findAt(keys, time);
    return k ? &k->curve : nullptr;
}

void setKeyCurve(Animation& anim, int index, Channel ch, float time, const CurveShape& curve) {
    if (CurveShape* c = curveAt(anim, index, ch, time)) *c = curve;
}

const CurveShape* keyCurve(const Animation& anim, int index, Channel ch, float time) {
    return curveAt(const_cast<Animation&>(anim), index, ch, time);
}

template <typename K>
static void shiftKey(std::vector<K>& keys, float from, float to) {
    auto it = std::find_if(keys.begin(), keys.end(),
                           [&](const K& k) { return std::fabs(k.time - from) < kKeyEpsilon; });
    if (it == keys.end()) return;
    K moved = *it;
    keys.erase(it);
    eraseAt(keys, to);
    moved.time = to;
    insertSorted(keys, moved);
}

static void shiftBone(BoneTimeline& tl, float from, float to) {
    shiftKey(tl.rotate, from, to);
    shiftKey(tl.translate, from, to);
    shiftKey(tl.scale, from, to);
    shiftKey(tl.shear, from, to);
}

static void shiftSlot(SlotTimeline& tl, float from, float to) {
    shiftKey(tl.attachment, from, to);
    shiftKey(tl.color, from, to);
}

// Events at `from` (optionally only one event) move to `to`; several events may share a time.
static void shiftEvents(Animation& anim, int event, float from, float to) {
    std::vector<EventKey> moved;
    anim.events.erase(std::remove_if(anim.events.begin(), anim.events.end(),
                                     [&](const EventKey& k) {
                                         bool hit = std::fabs(k.time - from) < kKeyEpsilon && (event < 0 || k.event == event);
                                         if (hit) moved.push_back(k);
                                         return hit;
                                     }),
                      anim.events.end());
    for (auto& k : moved) {
        anim.events.erase(std::remove_if(anim.events.begin(), anim.events.end(),
                                         [&](const EventKey& o) { return o.event == k.event && std::fabs(o.time - to) < kKeyEpsilon; }),
                          anim.events.end());
        k.time = to;
        insertSorted(anim.events, k);
    }
}

void moveKeys(Animation& anim, float from, float to) {
    for (auto& [i, tl] : anim.bones) shiftBone(tl, from, to);
    for (auto& [i, tl] : anim.slots) shiftSlot(tl, from, to);
    shiftKey(anim.drawOrder, from, to);
    shiftEvents(anim, -1, from, to);
}

void moveRowKeys(Animation& anim, bool isSlot, int index, float from, float to) {
    if (isSlot) {
        auto it = anim.slots.find(index);
        if (it != anim.slots.end()) shiftSlot(it->second, from, to);
    } else {
        auto it = anim.bones.find(index);
        if (it != anim.bones.end()) shiftBone(it->second, from, to);
    }
}

void moveChannelKey(Animation& anim, int index, Channel ch, float from, float to) {
    switch (ch) {
    case Channel::DrawOrder: shiftKey(anim.drawOrder, from, to); return;
    case Channel::Event: shiftEvents(anim, index, from, to); return;
    case Channel::Attachment:
    case Channel::Color: {
        auto it = anim.slots.find(index);
        if (it == anim.slots.end()) return;
        if (ch == Channel::Attachment) shiftKey(it->second.attachment, from, to);
        else shiftKey(it->second.color, from, to);
        return;
    }
    default: {
        auto it = anim.bones.find(index);
        if (it == anim.bones.end()) return;
        if (ch == Channel::Rotate) shiftKey(it->second.rotate, from, to);
        if (ch == Channel::Translate) shiftKey(it->second.translate, from, to);
        if (ch == Channel::Scale) shiftKey(it->second.scale, from, to);
        if (ch == Channel::Shear) shiftKey(it->second.shear, from, to);
    }
    }
}

static void addTimes(std::vector<float>& out, const BoneTimeline& tl) {
    for (auto& k : tl.rotate) out.push_back(k.time);
    for (auto& k : tl.translate) out.push_back(k.time);
    for (auto& k : tl.scale) out.push_back(k.time);
    for (auto& k : tl.shear) out.push_back(k.time);
}

static void addTimes(std::vector<float>& out, const SlotTimeline& tl) {
    for (auto& k : tl.attachment) out.push_back(k.time);
    for (auto& k : tl.color) out.push_back(k.time);
}

static void uniqueSorted(std::vector<float>& t) {
    std::sort(t.begin(), t.end());
    t.erase(std::unique(t.begin(), t.end(), [](float a, float b) { return std::fabs(a - b) < kKeyEpsilon; }),
            t.end());
}

std::vector<float> keyTimes(const Animation& anim) {
    std::vector<float> t;
    for (auto& [i, tl] : anim.bones) addTimes(t, tl);
    for (auto& [i, tl] : anim.slots) addTimes(t, tl);
    for (auto& k : anim.drawOrder) t.push_back(k.time);
    for (auto& k : anim.events) t.push_back(k.time);
    uniqueSorted(t);
    return t;
}

std::vector<float> rowKeyTimes(const Animation& anim, bool isSlot, int index) {
    std::vector<float> t;
    if (isSlot) {
        auto it = anim.slots.find(index);
        if (it != anim.slots.end()) addTimes(t, it->second);
    } else {
        auto it = anim.bones.find(index);
        if (it != anim.bones.end()) addTimes(t, it->second);
    }
    uniqueSorted(t);
    return t;
}

std::vector<const EventKey*> eventsInRange(const Animation& anim, float from, float to) {
    std::vector<const EventKey*> out;
    for (auto& k : anim.events)
        if ((from < 0 ? k.time >= 0 : k.time > from) && k.time <= to) out.push_back(&k);
    return out;
}

} // namespace rig
