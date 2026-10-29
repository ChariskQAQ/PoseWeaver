#pragma once
#include "core/model.h"
#include <utility>

namespace rig::bench {

inline Skeleton fixture(int count, int grid, bool clipping) {
    Skeleton sk;
    sk.addBone("root", -1, {}, 0);
    sk.images.push_back({"synthetic-white", 16, 16});
    if (clipping) {
        Slot slot;
        slot.name = "clip";
        slot.activeAttachment = 0;
        Attachment clip;
        clip.name = "concave";
        clip.type = AttachmentType::Clipping;
        clip.vertices = {{0, 0}, {256, 0}, {256, 128}, {128, 128}, {128, 256}, {0, 256}};
        slot.attachments.push_back(clip);
        sk.slots.push_back(slot);
    }
    Animation animation;
    animation.name = "motion";
    for (int i = 0; i < count; ++i) {
        Transform transform;
        transform.x = (i % 16) * 16.0f + 8;
        transform.y = ((i / 16) % 16) * 16.0f + 8;
        int bone = sk.addBone("bone" + std::to_string(i), 0, transform, 16);
        setRotateKey(animation, bone, 0, -10);
        setRotateKey(animation, bone, 0.5f, 10);
        setRotateKey(animation, bone, 1, -10);
        Slot slot;
        slot.name = "slot" + std::to_string(i);
        slot.bone = bone;
        slot.activeAttachment = 0;
        slot.blend = (BlendMode)(i % 4);
        slot.color[3] = 0.6f;
        Attachment mesh;
        mesh.name = "mesh";
        mesh.type = AttachmentType::Mesh;
        mesh.image = 0;
        for (int y = 0; y < grid; ++y) for (int x = 0; x < grid; ++x) {
            Vec2 local{x * 16.0f / (grid - 1) - 8, y * 16.0f / (grid - 1) - 8};
            mesh.uvs.push_back({float(x) / (grid - 1), float(y) / (grid - 1)});
            mesh.weights.push_back({{bone, local, 0.75f}, {0, {local.x + transform.x, local.y + transform.y}, 0.25f}});
            if (x + 1 < grid && y + 1 < grid) {
                int p = y * grid + x;
                mesh.triangles.insert(mesh.triangles.end(), {p, p + 1, p + grid, p + 1, p + grid + 1, p + grid});
            }
        }
        slot.attachments.push_back(std::move(mesh));
        sk.slots.push_back(std::move(slot));
        if (i % 16 == 0) {
            Constraint constraint;
            constraint.name = "transform" + std::to_string(i);
            constraint.type = ConstraintType::Transform;
            constraint.bones = {bone};
            constraint.target = 0;
            constraint.values = {0.25f, 0, 0, 0, 0, 0};
            sk.constraints.push_back(constraint);
        }
    }
    sk.animations.push_back(std::move(animation));
    return sk;
}

} // namespace rig::bench
