#include "core/pose.h"
#include "core/constraints.h"

#include <algorithm>

namespace rig {

const std::vector<Vec2>* findDeform(const Pose& pose, int slot, int attachment) {
    auto it = pose.deform.find({slot, attachment});
    return it == pose.deform.end() ? nullptr : &it->second;
}

void resetToSetup(const Skeleton& sk, Pose& pose) {
    pose.deform.clear();
    pose.local.resize(sk.bones.size());
    pose.world.assign(sk.bones.size(), Affine{});
    for (size_t i = 0; i < sk.bones.size(); ++i) pose.local[i] = sk.bones[i].setup;
    pose.constraints.resize(sk.constraints.size());
    for (size_t i = 0; i < sk.constraints.size(); ++i) pose.constraints[i] = sk.constraints[i].values;
    pose.slotAttachment.resize(sk.slots.size());
    pose.slotColor.resize(sk.slots.size());
    pose.drawOrder.resize(sk.slots.size());
    for (size_t i = 0; i < sk.slots.size(); ++i) {
        pose.slotAttachment[i] = sk.slots[i].activeAttachment;
        pose.slotColor[i] = sk.slots[i].color;
        pose.drawOrder[i] = (int)i;
    }
}

// Index of the last key whose time <= t, or -1 when t is before the first key.
template <typename K>
static int keyBefore(const std::vector<K>& keys, float t) {
    auto it = std::upper_bound(keys.begin(), keys.end(), t,
                               [](float time, const K& k) { return time < k.time; });
    return (int)(it - keys.begin()) - 1;
}

template <typename K, typename V>
static V sampleLerp(const std::vector<K>& keys, float t, V fallback) {
    if (keys.empty()) return fallback;
    int i = keyBefore(keys, t);
    if (i < 0) return keys.front().value;
    if (i + 1 >= (int)keys.size()) return keys.back().value;
    const K& a = keys[i];
    const K& b = keys[i + 1];
    float span = b.time - a.time;
    float f = applyCurve(a.curve, span > 1e-8f ? (t - a.time) / span : 0.0f);
    return a.value + (b.value - a.value) * f;
}

float sample(const std::vector<FloatKey>& keys, float t, float fallback) { return sampleLerp(keys, t, fallback); }
Vec2 sample(const std::vector<Vec2Key>& keys, float t, Vec2 fallback) { return sampleLerp(keys, t, fallback); }
Color sample(const std::vector<ColorKey>& keys, float t, const Color& fallback) { return sampleLerp(keys, t, fallback); }

int sample(const std::vector<IntKey>& keys, float t, int fallback) {
    if (keys.empty()) return fallback;
    int i = keyBefore(keys, t);
    return keys[i < 0 ? 0 : i].value;
}

void applyAnimation(const Skeleton& sk, const Animation& anim, float time, Pose& pose) {
    for (auto& [bi, tl] : anim.bones) {
        if (bi < 0 || bi >= (int)sk.bones.size()) continue;
        const Transform& s = sk.bones[bi].setup;
        Transform& l = pose.local[bi];
        l.rotation = s.rotation + sample(tl.rotate, time, 0.0f);
        Vec2 tr = sample(tl.translate, time, Vec2{});
        l.x = s.x + tr.x;
        l.y = s.y + tr.y;
        Vec2 sc = sample(tl.scale, time, Vec2{1, 1});
        l.scaleX = s.scaleX * sc.x;
        l.scaleY = s.scaleY * sc.y;
        Vec2 sh = sample(tl.shear, time, Vec2{});
        l.shearX = s.shearX + sh.x;
        l.shearY = s.shearY + sh.y;
    }
    for (auto& [si, tl] : anim.slots) {
        if (si < 0 || si >= (int)sk.slots.size()) continue;
        int a = sample(tl.attachment, time, sk.slots[si].activeAttachment);
        pose.slotAttachment[si] = a < (int)sk.slots[si].attachments.size() ? a : -1;
        pose.slotColor[si] = sample(tl.color, time, sk.slots[si].color);
        for (auto& [ai, keys] : tl.deform) {
            if (keys.empty() || ai < 0 || ai >= (int)sk.slots[si].attachments.size()) continue;
            size_t n = sk.slots[si].attachments[ai].vertexCount();
            int i = keyBefore(keys, time);
            const DeformKey& k0 = keys[std::max(i, 0)];
            if (k0.value.size() != n) continue;
            std::vector<Vec2> offsets = k0.value;
            if (i >= 0 && i + 1 < (int)keys.size() && keys[i + 1].value.size() == n) {
                const DeformKey& k1 = keys[i + 1];
                float span = k1.time - k0.time;
                float f = applyCurve(k0.curve, span > 1e-8f ? (time - k0.time) / span : 0.0f);
                for (size_t v = 0; v < n; ++v) offsets[v] = k0.value[v] + (k1.value[v] - k0.value[v]) * f;
            }
            pose.deform[{si, ai}] = std::move(offsets);
        }
    }
    for (auto& [ci, keys] : anim.constraints) {
        if (ci < 0 || ci >= (int)pose.constraints.size() || keys.empty()) continue;
        int i = keyBefore(keys, time);
        const ConstraintKey& k0 = keys[std::max(i, 0)];
        ConstraintValues v = k0.value;
        if (i >= 0 && i + 1 < (int)keys.size()) {
            const ConstraintKey& k1 = keys[i + 1];
            float span = k1.time - k0.time;
            float f = applyCurve(k0.curve, span > 1e-8f ? (time - k0.time) / span : 0.0f);
            for (int j = 0; j < 6; ++j) v[j] = k0.value[j] + (k1.value[j] - k0.value[j]) * f;
        }
        if (sk.constraints[ci].type == ConstraintType::Ik) v[2] = k0.value[2] < 0 ? -1.0f : 1.0f;  // bend is stepped
        pose.constraints[ci] = v;
    }
    // Draw order is stepped: the last key at or before `time` wins (before the first key: setup).
    const DrawOrderKey* order = nullptr;
    for (auto& k : anim.drawOrder)
        if (k.time <= time + kKeyEpsilon) order = &k;
    if (order && order->order.size() == sk.slots.size()) pose.drawOrder = order->order;
}

const Attachment* poseAttachment(const Skeleton& sk, const Pose& pose, int slot) {
    if (slot < 0 || slot >= (int)sk.slots.size()) return nullptr;
    int bone = sk.slots[slot].bone;
    if (bone < (int)pose.boneActive.size() && !pose.boneActive[bone]) return nullptr;
    int a = slot < (int)pose.slotAttachment.size() ? pose.slotAttachment[slot] : sk.slots[slot].activeAttachment;
    return sk.resolve(pose.skins, slot, a);
}

void updateActivity(const Skeleton& sk, Pose& pose) {
    auto inSkins = [&](auto member) {
        for (int s : pose.skins)
            if (s >= 0 && s < (int)sk.skins.size() && member(sk.skins[s])) return true;
        return false;
    };
    pose.boneActive.assign(sk.bones.size(), 1);
    for (int i : sk.updateOrder()) {
        const Bone& b = sk.bones[i];
        bool active = b.parent < 0 || pose.boneActive[b.parent];
        if (active && b.skinRequired)
            active = inSkins([&](const Skin& s) { return std::find(s.bones.begin(), s.bones.end(), i) != s.bones.end(); });
        pose.boneActive[i] = active;
    }
    pose.constraintActive.assign(sk.constraints.size(), 1);
    for (size_t c = 0; c < sk.constraints.size(); ++c) {
        const Constraint& con = sk.constraints[c];
        bool active = !con.skinRequired || inSkins([&](const Skin& s) {
            return std::find(s.constraints.begin(), s.constraints.end(), (int)c) != s.constraints.end();
        });
        for (int b : con.bones) active = active && b < (int)pose.boneActive.size() && pose.boneActive[b];
        pose.constraintActive[c] = active;
    }
}

void updateWorld(const Skeleton& sk, Pose& pose) {
    updateActivity(sk, pose);
    pose.world.resize(sk.bones.size());
    pose.applied = pose.local;
    for (int i : sk.updateOrder()) {
        Affine localM = pose.applied[i].matrix();
        int p = sk.bones[i].parent;
        pose.world[i] = p >= 0 ? pose.world[p] * localM : localM;
    }
    if (pose.constraints.size() != sk.constraints.size()) {
        pose.constraints.resize(sk.constraints.size());
        for (size_t i = 0; i < sk.constraints.size(); ++i) pose.constraints[i] = sk.constraints[i].values;
    }
    applyConstraints(sk, pose);
}

void updateSubtree(const Skeleton& sk, Pose& pose, int bone) {
    for (int i : sk.updateOrder()) {
        if (!sk.isAncestor(bone, i)) continue;
        Affine localM = pose.applied[i].matrix();
        int p = sk.bones[i].parent;
        pose.world[i] = p >= 0 ? pose.world[p] * localM : localM;
    }
}

Pose makeSetupPose(const Skeleton& sk, const std::vector<int>& skins) {
    Pose p;
    p.skins = skins;
    resetToSetup(sk, p);
    updateWorld(sk, p);
    return p;
}

Pose makeAnimatedPose(const Skeleton& sk, const Animation* anim, float time, const std::vector<int>& skins) {
    Pose p;
    p.skins = skins;
    resetToSetup(sk, p);
    if (anim) applyAnimation(sk, *anim, time, p);
    updateWorld(sk, p);
    return p;
}

Vec2 boneTip(const Bone& bone, const Affine& world) { return world.apply({bone.length, 0}); }

Affine deformBasis(const Pose& pose, const Slot& slot, const Attachment& att, int vertex) {
    if (!att.isWeighted()) {
        Affine m = pose.world[slot.bone];
        m.tx = m.ty = 0;
        return m;
    }
    Affine m{0, 0, 0, 0, 0, 0};
    for (auto& w : att.weights[vertex]) {
        const Affine& b = pose.world[w.bone];
        m.a += b.a * w.weight;
        m.b += b.b * w.weight;
        m.c += b.c * w.weight;
        m.d += b.d * w.weight;
    }
    return m;
}

std::vector<Vec2> computeVertices(const Skeleton& sk, const Pose& pose, const Slot& slot, const Attachment& att,
                                  const std::vector<Vec2>* deform) {
    std::vector<Vec2> out;
    const Affine& boneWorld = pose.world[slot.bone];
    if (att.type == AttachmentType::Region) {
        float w = 0, h = 0;
        if (att.image >= 0 && att.image < (int)sk.images.size()) {
            w = (float)sk.images[att.image].width;
            h = (float)sk.images[att.image].height;
        }
        Affine m = boneWorld * att.transform.matrix();
        out = {m.apply({-w / 2, h / 2}), m.apply({w / 2, h / 2}),
               m.apply({w / 2, -h / 2}), m.apply({-w / 2, -h / 2})};
        return out;
    }
    if (deform && deform->size() != att.vertexCount()) deform = nullptr;
    if (att.isWeighted()) {
        out.resize(att.weights.size());
        for (size_t v = 0; v < att.weights.size(); ++v) {
            Vec2 p;
            Vec2 off = deform ? (*deform)[v] : Vec2{};
            for (auto& w : att.weights[v]) p += pose.world[w.bone].apply(w.local + off) * w.weight;
            out[v] = p;
        }
    } else {
        out.resize(att.vertices.size());
        for (size_t v = 0; v < att.vertices.size(); ++v)
            out[v] = boneWorld.apply(deform ? att.vertices[v] + (*deform)[v] : att.vertices[v]);
    }
    return out;
}

void computeGeometry(const Skeleton& sk, const Pose& pose, const Slot& slot, const Attachment& att,
                     Geometry& out, const std::vector<Vec2>* deform) {
    out.positions = computeVertices(sk, pose, slot, att, deform);
    if (att.type == AttachmentType::Region) {
        out.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        out.triangles = {0, 1, 2, 0, 2, 3};
    } else if (att.isPolygon()) {
        out.uvs.clear();
        out.triangles.clear();
    } else {
        out.uvs = att.uvs;
        out.triangles = att.triangles;
    }
}

} // namespace rig
