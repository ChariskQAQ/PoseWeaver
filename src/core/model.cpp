#include "core/model.h"
#include "core/pose.h"

#include <algorithm>
#include <functional>

namespace rig {

Transform decompose(const Affine& m) {
    Transform t;
    t.x = m.tx;
    t.y = m.ty;
    t.rotation = radToDeg(std::atan2(m.c, m.a));
    t.scaleX = std::sqrt(m.a * m.a + m.c * m.c);
    t.scaleY = t.scaleX > 1e-8f ? m.det() / t.scaleX : 1.0f;
    return t;
}

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

int Skeleton::addBone(const std::string& name, int parent, const Transform& setup, float length) {
    Bone b;
    b.name = uniqueBoneName(name);
    b.parent = parent;
    b.setup = setup;
    b.length = length;
    bones.push_back(b);
    return (int)bones.size() - 1;
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
        std::map<int, SlotTimeline> st;
        for (auto& [i, tl] : anim.slots)
            if (slotRemap[i] >= 0) st[slotRemap[i]] = std::move(tl);
        anim.slots = std::move(st);
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

// ---- keys ----

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
    keys.insert(std::upper_bound(keys.begin(), keys.end(), time,
                                 [](float t, const K& key) { return t < key.time; }),
                k);
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
void setAttachmentKey(Animation& anim, int slot, float time, int attachment) { upsert(anim.slots[slot].attachment, time, attachment); }

bool removeKey(Animation& anim, int index, Channel ch, float time) {
    bool ok = false;
    if (ch == Channel::Attachment) {
        auto it = anim.slots.find(index);
        if (it == anim.slots.end()) return false;
        ok = eraseAt(it->second.attachment, time);
        if (it->second.empty()) anim.slots.erase(it);
        return ok;
    }
    auto it = anim.bones.find(index);
    if (it == anim.bones.end()) return false;
    BoneTimeline& tl = it->second;
    if (ch == Channel::Rotate) ok = eraseAt(tl.rotate, time);
    if (ch == Channel::Translate) ok = eraseAt(tl.translate, time);
    if (ch == Channel::Scale) ok = eraseAt(tl.scale, time);
    if (tl.empty()) anim.bones.erase(it);
    return ok;
}

bool hasKey(const Animation& anim, int index, Channel ch, float time) {
    if (ch == Channel::Attachment) {
        auto it = anim.slots.find(index);
        return it != anim.slots.end() && contains(it->second.attachment, time);
    }
    auto it = anim.bones.find(index);
    if (it == anim.bones.end()) return false;
    if (ch == Channel::Rotate) return contains(it->second.rotate, time);
    if (ch == Channel::Translate) return contains(it->second.translate, time);
    return contains(it->second.scale, time);
}

void setKeyCurve(Animation& anim, int bone, Channel ch, float time, Curve curve) {
    auto it = anim.bones.find(bone);
    if (it == anim.bones.end()) return;
    auto apply = [&](auto& keys) {
        for (auto& k : keys)
            if (std::fabs(k.time - time) < kKeyEpsilon) k.curve = curve;
    };
    if (ch == Channel::Rotate) apply(it->second.rotate);
    if (ch == Channel::Translate) apply(it->second.translate);
    if (ch == Channel::Scale) apply(it->second.scale);
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
    keys.insert(std::upper_bound(keys.begin(), keys.end(), to,
                                 [](float t, const K& key) { return t < key.time; }),
                moved);
}

static void shiftBone(BoneTimeline& tl, float from, float to) {
    shiftKey(tl.rotate, from, to);
    shiftKey(tl.translate, from, to);
    shiftKey(tl.scale, from, to);
}

void moveKeys(Animation& anim, float from, float to) {
    for (auto& [i, tl] : anim.bones) shiftBone(tl, from, to);
    for (auto& [i, tl] : anim.slots) shiftKey(tl.attachment, from, to);
}

void moveRowKeys(Animation& anim, bool isSlot, int index, float from, float to) {
    if (isSlot) {
        auto it = anim.slots.find(index);
        if (it != anim.slots.end()) shiftKey(it->second.attachment, from, to);
    } else {
        auto it = anim.bones.find(index);
        if (it != anim.bones.end()) shiftBone(it->second, from, to);
    }
}

static void addTimes(std::vector<float>& out, const BoneTimeline& tl) {
    for (auto& k : tl.rotate) out.push_back(k.time);
    for (auto& k : tl.translate) out.push_back(k.time);
    for (auto& k : tl.scale) out.push_back(k.time);
}

static void uniqueSorted(std::vector<float>& t) {
    std::sort(t.begin(), t.end());
    t.erase(std::unique(t.begin(), t.end(), [](float a, float b) { return std::fabs(a - b) < kKeyEpsilon; }),
            t.end());
}

std::vector<float> keyTimes(const Animation& anim) {
    std::vector<float> t;
    for (auto& [i, tl] : anim.bones) addTimes(t, tl);
    for (auto& [i, tl] : anim.slots)
        for (auto& k : tl.attachment) t.push_back(k.time);
    uniqueSorted(t);
    return t;
}

std::vector<float> rowKeyTimes(const Animation& anim, bool isSlot, int index) {
    std::vector<float> t;
    if (isSlot) {
        auto it = anim.slots.find(index);
        if (it != anim.slots.end())
            for (auto& k : it->second.attachment) t.push_back(k.time);
    } else {
        auto it = anim.bones.find(index);
        if (it != anim.bones.end()) addTimes(t, it->second);
    }
    uniqueSorted(t);
    return t;
}

} // namespace rig
