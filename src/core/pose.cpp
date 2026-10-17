#include "core/pose.h"

#include <algorithm>

namespace rig {

void resetToSetup(const Skeleton& sk, Pose& pose) {
    pose.local.resize(sk.bones.size());
    pose.world.assign(sk.bones.size(), Affine{});
    for (size_t i = 0; i < sk.bones.size(); ++i) pose.local[i] = sk.bones[i].setup;
    pose.slotAttachment.resize(sk.slots.size());
    for (size_t i = 0; i < sk.slots.size(); ++i) pose.slotAttachment[i] = sk.slots[i].activeAttachment;
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
    if (a.curve == Curve::Stepped) return a.value;
    float span = b.time - a.time;
    float f = span > 1e-8f ? (t - a.time) / span : 0.0f;
    return a.value + (b.value - a.value) * f;
}

float sample(const std::vector<FloatKey>& keys, float t, float fallback) { return sampleLerp(keys, t, fallback); }
Vec2 sample(const std::vector<Vec2Key>& keys, float t, Vec2 fallback) { return sampleLerp(keys, t, fallback); }

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
    }
    for (auto& [si, tl] : anim.slots) {
        if (si < 0 || si >= (int)sk.slots.size()) continue;
        int a = sample(tl.attachment, time, sk.slots[si].activeAttachment);
        pose.slotAttachment[si] = a < (int)sk.slots[si].attachments.size() ? a : -1;
    }
}

void updateWorld(const Skeleton& sk, Pose& pose) {
    pose.world.resize(sk.bones.size());
    for (int i : sk.updateOrder()) {
        Affine localM = pose.local[i].matrix();
        int p = sk.bones[i].parent;
        pose.world[i] = p >= 0 ? pose.world[p] * localM : localM;
    }
}

Pose makeSetupPose(const Skeleton& sk) {
    Pose p;
    resetToSetup(sk, p);
    updateWorld(sk, p);
    return p;
}

Pose makeAnimatedPose(const Skeleton& sk, const Animation* anim, float time) {
    Pose p;
    resetToSetup(sk, p);
    if (anim) applyAnimation(sk, *anim, time, p);
    updateWorld(sk, p);
    return p;
}

Vec2 boneTip(const Bone& bone, const Affine& world) { return world.apply({bone.length, 0}); }

std::vector<Vec2> computeVertices(const Skeleton& sk, const Pose& pose, const Slot& slot,
                                  const Attachment& att) {
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
    if (att.isWeighted()) {
        out.resize(att.weights.size());
        for (size_t v = 0; v < att.weights.size(); ++v) {
            Vec2 p;
            for (auto& w : att.weights[v]) p += pose.world[w.bone].apply(w.local) * w.weight;
            out[v] = p;
        }
    } else {
        out.resize(att.vertices.size());
        for (size_t v = 0; v < att.vertices.size(); ++v) out[v] = boneWorld.apply(att.vertices[v]);
    }
    return out;
}

void computeGeometry(const Skeleton& sk, const Pose& pose, const Slot& slot, const Attachment& att,
                     Geometry& out) {
    out.positions = computeVertices(sk, pose, slot, att);
    if (att.type == AttachmentType::Region) {
        out.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        out.triangles = {0, 1, 2, 0, 2, 3};
    } else {
        out.uvs = att.uvs;
        out.triangles = att.triangles;
    }
}

} // namespace rig
