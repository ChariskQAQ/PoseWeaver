#include "core/model.h"
#include "core/pose.h"

#include <algorithm>
#include <functional>
#include <iterator>

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

std::string Skeleton::uniqueConstraintName(const std::string& base) const {
    return uniqueName(base, [&](const std::string& n) {
        for (auto& c : constraints)
            if (c.name == n) return true;
        return false;
    });
}

ConstraintValues defaultConstraintValues(ConstraintType type) {
    switch (type) {
    case ConstraintType::Ik: return {1, 0, 1, 0, 0, 0};
    case ConstraintType::Transform: return {1, 1, 1, 1, 1, 1};
    case ConstraintType::Path: return {0, 0, 1, 1, 1, 0};
    case ConstraintType::Physics: return {1, 0, 0, 0, 0, 0};
    }
    return {};
}

int constraintValueCount(ConstraintType type) {
    switch (type) {
    case ConstraintType::Ik: return 3;
    case ConstraintType::Transform: return 6;
    case ConstraintType::Path: return 5;
    case ConstraintType::Physics: return 3;
    }
    return 0;
}

const char* constraintValueName(ConstraintType type, int i) {
    static const char* ik[] = {"混合", "软化", "弯曲方向"};
    static const char* tr[] = {"旋转混合", "X 混合", "Y 混合", "缩放 X 混合", "缩放 Y 混合", "斜切混合"};
    static const char* path[] = {"位置", "间距", "旋转混合", "X 混合", "Y 混合"};
    static const char* phys[] = {"混合", "风", "重力"};
    if (i < 0 || i >= constraintValueCount(type)) return "";
    switch (type) {
    case ConstraintType::Ik: return ik[i];
    case ConstraintType::Transform: return tr[i];
    case ConstraintType::Path: return path[i];
    case ConstraintType::Physics: return phys[i];
    }
    return "";
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

// Clipping attachments reference their end slot by index.
static void remapPathTargets(std::vector<Constraint>& constraints, const std::vector<int>& slotRemap) {
    for (auto& c : constraints)
        if (c.type == ConstraintType::Path && c.target >= 0)
            c.target = c.target < (int)slotRemap.size() ? slotRemap[c.target] : -1;
}

static void remapSkinSlots(std::vector<Skin>& skins, const std::vector<int>& slotRemap) {
    for (auto& skin : skins) {
        std::vector<SkinEntry> kept;
        for (auto& e : skin.attachments)
            if (e.slot >= 0 && e.slot < (int)slotRemap.size() && slotRemap[e.slot] >= 0) {
                e.slot = slotRemap[e.slot];
                kept.push_back(std::move(e));
            }
        skin.attachments = std::move(kept);
    }
}

static void remapClipEnds(std::vector<Slot>& slots, const std::vector<int>& slotRemap) {
    for (auto& s : slots)
        for (auto& a : s.attachments)
            if (a.type == AttachmentType::Clipping && a.endSlot >= 0)
                a.endSlot = a.endSlot < (int)slotRemap.size() ? slotRemap[a.endSlot] : -1;
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
    remapClipEnds(slots, slotRemap);
    remapSkinSlots(skins, slotRemap);
    for (auto& skin : skins) {
        std::vector<int> kept;
        for (int b : skin.bones)
            if (b < (int)remap.size() && remap[b] >= 0) kept.push_back(remap[b]);
        skin.bones = kept;
        for (auto& e : skin.attachments)  // weights on removed bones: same rules as slot attachments
            for (auto& list : e.attachment.weights) {
                list.erase(std::remove_if(list.begin(), list.end(), [&](const VertexWeight& w) { return w.bone >= (int)remap.size() || remap[w.bone] < 0; }),
                           list.end());
                float total = 0;
                for (auto& w : list) total += w.weight;
                for (auto& w : list) {
                    w.bone = remap[w.bone];
                    if (total > 0) w.weight /= total;
                }
            }
    }

    for (int c = (int)constraints.size() - 1; c >= 0; --c) {
        Constraint& con = constraints[c];
        std::vector<int> kept;
        for (int b : con.bones)
            if (b < (int)remap.size() && remap[b] >= 0) kept.push_back(remap[b]);
        con.bones = kept;
        if (con.type == ConstraintType::Path) {
            con.target = con.target >= 0 && con.target < (int)slotRemap.size() ? slotRemap[con.target] : -1;
        } else if (con.target >= 0) {
            con.target = con.target < (int)remap.size() ? remap[con.target] : -1;
        }
        bool needsTarget = con.type != ConstraintType::Physics;
        if (con.bones.empty() || (needsTarget && con.target < 0)) removeConstraint(c);
    }
}

Attachment* Skin::find(int slot, const std::string& placeholder) {
    for (auto& e : attachments)
        if (e.slot == slot && e.placeholder == placeholder) return &e.attachment;
    return nullptr;
}

const Attachment* Skin::find(int slot, const std::string& placeholder) const {
    return const_cast<Skin*>(this)->find(slot, placeholder);
}

std::string Skeleton::uniqueSkinName(const std::string& base) const {
    return uniqueName(base, [&](const std::string& n) {
        for (auto& s : skins)
            if (s.name == n) return true;
        return false;
    });
}

void Skeleton::removeSkin(int skin) {
    if (skin >= 0 && skin < (int)skins.size()) skins.erase(skins.begin() + skin);
}

Attachment* Skeleton::resolve(const std::vector<int>& activeSkins, int slot, int index) {
    if (slot < 0 || slot >= (int)slots.size() || index < 0 || index >= (int)slots[slot].attachments.size()) return nullptr;
    Attachment& a = slots[slot].attachments[index];
    if (!a.skinPlaceholder) return &a;
    for (auto it = activeSkins.rbegin(); it != activeSkins.rend(); ++it)
        if (*it >= 0 && *it < (int)skins.size())
            if (Attachment* found = skins[*it].find(slot, a.name)) return found;
    return nullptr;
}

const Attachment* Skeleton::resolve(const std::vector<int>& activeSkins, int slot, int index) const {
    return const_cast<Skeleton*>(this)->resolve(activeSkins, slot, index);
}

void Skeleton::removeConstraint(int c) {
    if (c < 0 || c >= (int)constraints.size()) return;
    constraints.erase(constraints.begin() + c);
    for (auto& skin : skins) {
        std::vector<int> kept;
        for (int i : skin.constraints)
            if (i != c) kept.push_back(i > c ? i - 1 : i);
        skin.constraints = kept;
    }
    for (auto& an : animations) {
        std::map<int, std::vector<ConstraintKey>> keys;
        for (auto& [i, k] : an.constraints)
            if (i != c) keys[i > c ? i - 1 : i] = std::move(k);
        an.constraints = std::move(keys);
    }
}

void Skeleton::swapConstraints(int a, int b) {
    if (a < 0 || b < 0 || a >= (int)constraints.size() || b >= (int)constraints.size() || a == b) return;
    std::swap(constraints[a], constraints[b]);
    for (auto& skin : skins)
        for (int& i : skin.constraints) i = i == a ? b : (i == b ? a : i);
    for (auto& an : animations) {
        auto ia = an.constraints.find(a), ib = an.constraints.find(b);
        std::vector<ConstraintKey> ka, kb;
        bool ha = ia != an.constraints.end(), hb = ib != an.constraints.end();
        if (ha) ka = std::move(ia->second);
        if (hb) kb = std::move(ib->second);
        an.constraints.erase(a);
        an.constraints.erase(b);
        if (ha) an.constraints[b] = std::move(ka);
        if (hb) an.constraints[a] = std::move(kb);
    }
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
    remapClipEnds(slots, slotRemap);
    remapPathTargets(constraints, slotRemap);
    remapSkinSlots(skins, slotRemap);
}

void Skeleton::swapSlots(int a, int b) {
    if (a < 0 || b < 0 || a >= (int)slots.size() || b >= (int)slots.size() || a == b) return;
    std::swap(slots[a], slots[b]);
    std::vector<int> slotRemap(slots.size());
    for (int i = 0; i < (int)slots.size(); ++i) slotRemap[i] = i == a ? b : (i == b ? a : i);
    remapSlots(animations, slotRemap);
    remapClipEnds(slots, slotRemap);
    remapPathTargets(constraints, slotRemap);
    remapSkinSlots(skins, slotRemap);
}

void Skeleton::removeAttachment(int slot, int attachment) {
    if (slot < 0 || slot >= (int)slots.size()) return;
    Slot& s = slots[slot];
    if (attachment < 0 || attachment >= (int)s.attachments.size()) return;
    if (s.attachments[attachment].skinPlaceholder) {
        const std::string name = s.attachments[attachment].name;
        for (auto& skin : skins)
            skin.attachments.erase(std::remove_if(skin.attachments.begin(), skin.attachments.end(),
                                                  [&](const SkinEntry& e) { return e.slot == slot && e.placeholder == name; }),
                                   skin.attachments.end());
    }
    s.attachments.erase(s.attachments.begin() + attachment);
    if (s.activeAttachment == attachment) s.activeAttachment = -1;
    else if (s.activeAttachment > attachment) --s.activeAttachment;
    for (auto& an : animations) {
        auto it = an.slots.find(slot);
        if (it == an.slots.end()) continue;
        for (auto& k : it->second.attachment) {
            if (k.value == attachment) k.value = -1;
            else if (k.value > attachment) --k.value;
        }
        std::map<int, std::vector<DeformKey>> deform;
        for (auto& [a, keys] : it->second.deform)
            if (a != attachment) deform[a > attachment ? a - 1 : a] = std::move(keys);
        it->second.deform = std::move(deform);
        if (it->second.empty()) an.slots.erase(it);
    }
}

int Skeleton::dropStaleDeformKeys(int slot, int attachment) {
    if (slot < 0 || slot >= (int)slots.size() || attachment < 0 || attachment >= (int)slots[slot].attachments.size()) return 0;
    size_t n = slots[slot].attachments[attachment].vertexCount();
    int dropped = 0;
    for (auto& an : animations) {
        auto it = an.slots.find(slot);
        if (it == an.slots.end()) continue;
        auto d = it->second.deform.find(attachment);
        if (d == it->second.deform.end()) continue;
        auto& keys = d->second;
        size_t before = keys.size();
        keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const DeformKey& k) { return k.value.size() != n; }), keys.end());
        dropped += (int)(before - keys.size());
        if (keys.empty()) it->second.deform.erase(d);
        if (it->second.empty()) an.slots.erase(it);
    }
    return dropped;
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

void setDeformKey(Animation& anim, int slot, int attachment, float time, const std::vector<Vec2>& offsets) {
    upsert(anim.slots[slot].deform[attachment], time, offsets);
}

void setConstraintKey(Animation& anim, int constraint, float time, const ConstraintValues& values) {
    upsert(anim.constraints[constraint], time, values);
}

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

bool removeKey(Animation& anim, int index, Channel ch, float time, int sub) {
    switch (ch) {
    case Channel::Constraint: {
        auto it = anim.constraints.find(index);
        if (it == anim.constraints.end()) return false;
        bool ok = eraseAt(it->second, time);
        if (it->second.empty()) anim.constraints.erase(it);
        return ok;
    }
    case Channel::Deform: {
        auto it = anim.slots.find(index);
        if (it == anim.slots.end()) return false;
        bool ok = false;
        for (auto& [a, keys] : it->second.deform)
            if (sub < 0 || a == sub) ok = eraseAt(keys, time) || ok;
        for (auto d = it->second.deform.begin(); d != it->second.deform.end();)
            d = d->second.empty() ? it->second.deform.erase(d) : std::next(d);
        if (it->second.empty()) anim.slots.erase(it);
        return ok;
    }
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

bool hasKey(const Animation& anim, int index, Channel ch, float time, int sub) {
    switch (ch) {
    case Channel::Constraint: {
        auto it = anim.constraints.find(index);
        return it != anim.constraints.end() && contains(it->second, time);
    }
    case Channel::Deform: {
        auto it = anim.slots.find(index);
        if (it == anim.slots.end()) return false;
        for (auto& [a, keys] : it->second.deform)
            if ((sub < 0 || a == sub) && contains(keys, time)) return true;
        return false;
    }
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
static CurveShape* curveAt(Animation& anim, int index, Channel ch, float time, int sub) {
    if (ch == Channel::Constraint) {
        auto it = anim.constraints.find(index);
        if (it == anim.constraints.end()) return nullptr;
        ConstraintKey* k = findAt(it->second, time);
        return k ? &k->curve : nullptr;
    }
    if (ch == Channel::Deform) {
        auto it = anim.slots.find(index);
        if (it == anim.slots.end()) return nullptr;
        auto d = it->second.deform.find(sub);
        if (d == it->second.deform.end()) return nullptr;
        DeformKey* k = findAt(d->second, time);
        return k ? &k->curve : nullptr;
    }
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

void setKeyCurve(Animation& anim, int index, Channel ch, float time, const CurveShape& curve, int sub) {
    if (ch == Channel::Deform && sub < 0) {  // every deformed attachment of the slot
        auto it = anim.slots.find(index);
        if (it == anim.slots.end()) return;
        for (auto& [a, keys] : it->second.deform)
            if (CurveShape* c = curveAt(anim, index, ch, time, a)) *c = curve;
        return;
    }
    if (CurveShape* c = curveAt(anim, index, ch, time, sub)) *c = curve;
}

const CurveShape* keyCurve(const Animation& anim, int index, Channel ch, float time, int sub) {
    return curveAt(const_cast<Animation&>(anim), index, ch, time, sub);
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
    for (auto& [a, keys] : tl.deform) shiftKey(keys, from, to);
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
    for (auto& [c, keys] : anim.constraints) shiftKey(keys, from, to);
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

void moveChannelKey(Animation& anim, int index, Channel ch, float from, float to, int sub) {
    switch (ch) {
    case Channel::Constraint: {
        auto it = anim.constraints.find(index);
        if (it != anim.constraints.end()) shiftKey(it->second, from, to);
        return;
    }
    case Channel::Deform: {
        auto it = anim.slots.find(index);
        if (it == anim.slots.end()) return;
        for (auto& [a, keys] : it->second.deform)
            if (sub < 0 || a == sub) shiftKey(keys, from, to);
        return;
    }
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
    for (auto& [a, keys] : tl.deform)
        for (auto& k : keys) out.push_back(k.time);
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
    for (auto& [c, keys] : anim.constraints)
        for (auto& k : keys) t.push_back(k.time);
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

template <typename K>
static void copyAt(const std::vector<K>& src, std::vector<K>& dst, float time, float newTime) {
    for (const K& k : src) {
        if (std::fabs(k.time - time) >= kKeyEpsilon) continue;
        K copy = k;
        copy.time = newTime;
        eraseAt(dst, newTime);
        insertSorted(dst, copy);
        return;
    }
}

void copyKey(const Animation& src, Animation& dst, int index, Channel ch, int sub, float time, float newTime) {
    switch (ch) {
    case Channel::DrawOrder: copyAt(src.drawOrder, dst.drawOrder, time, newTime); return;
    case Channel::Event:
        for (const EventKey& k : src.events) {
            if (std::fabs(k.time - time) >= kKeyEpsilon || (index >= 0 && k.event != index)) continue;
            EventKey copy = k;
            copy.time = newTime;
            dst.events.erase(std::remove_if(dst.events.begin(), dst.events.end(),
                                            [&](const EventKey& o) { return o.event == k.event && std::fabs(o.time - newTime) < kKeyEpsilon; }),
                             dst.events.end());
            insertSorted(dst.events, copy);
        }
        return;
    case Channel::Constraint: {
        auto it = src.constraints.find(index);
        if (it != src.constraints.end()) copyAt(it->second, dst.constraints[index], time, newTime);
        if (dst.constraints.count(index) && dst.constraints[index].empty()) dst.constraints.erase(index);
        return;
    }
    case Channel::Attachment:
    case Channel::Color:
    case Channel::Deform: {
        auto it = src.slots.find(index);
        if (it == src.slots.end()) return;
        SlotTimeline& d = dst.slots[index];
        if (ch == Channel::Attachment) copyAt(it->second.attachment, d.attachment, time, newTime);
        if (ch == Channel::Color) copyAt(it->second.color, d.color, time, newTime);
        if (ch == Channel::Deform)
            for (auto& [a, keys] : it->second.deform)
                if (sub < 0 || a == sub) {
                    copyAt(keys, d.deform[a], time, newTime);
                    if (d.deform[a].empty()) d.deform.erase(a);
                }
        if (d.empty()) dst.slots.erase(index);
        return;
    }
    default: {
        auto it = src.bones.find(index);
        if (it == src.bones.end()) return;
        BoneTimeline& d = dst.bones[index];
        if (ch == Channel::Rotate) copyAt(it->second.rotate, d.rotate, time, newTime);
        if (ch == Channel::Translate) copyAt(it->second.translate, d.translate, time, newTime);
        if (ch == Channel::Scale) copyAt(it->second.scale, d.scale, time, newTime);
        if (ch == Channel::Shear) copyAt(it->second.shear, d.shear, time, newTime);
        if (d.empty()) dst.bones.erase(index);
    }
    }
}

template <typename K>
static void mergeKeys(std::vector<K>& dst, const std::vector<K>& src, float offset) {
    for (const K& k : src) copyAt(src, dst, k.time, k.time + offset);
}

void mergeAnimation(Animation& dst, const Animation& src, float offset) {
    for (auto& [b, tl] : src.bones) {
        BoneTimeline& d = dst.bones[b];
        mergeKeys(d.rotate, tl.rotate, offset);
        mergeKeys(d.translate, tl.translate, offset);
        mergeKeys(d.scale, tl.scale, offset);
        mergeKeys(d.shear, tl.shear, offset);
    }
    for (auto& [s, tl] : src.slots) {
        SlotTimeline& d = dst.slots[s];
        mergeKeys(d.attachment, tl.attachment, offset);
        mergeKeys(d.color, tl.color, offset);
        for (auto& [a, keys] : tl.deform) mergeKeys(d.deform[a], keys, offset);
    }
    mergeKeys(dst.drawOrder, src.drawOrder, offset);
    for (auto& [c, keys] : src.constraints) mergeKeys(dst.constraints[c], keys, offset);
    for (const EventKey& k : src.events) {
        Animation one;
        one.events.push_back(k);
        copyKey(one, dst, k.event, Channel::Event, -1, k.time, k.time + offset);
    }
}

std::vector<const EventKey*> eventsInRange(const Animation& anim, float from, float to) {
    std::vector<const EventKey*> out;
    for (auto& k : anim.events)
        if ((from < 0 ? k.time >= 0 : k.time > from) && k.time <= to) out.push_back(&k);
    return out;
}

} // namespace rig
