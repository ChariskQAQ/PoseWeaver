#include "core/merge.h"

#include <algorithm>

namespace rig {

namespace {
struct Maps {
    std::vector<int> bone, slot, image, event, constraint;
};

int map(const std::vector<int>& m, int i) { return i >= 0 && i < (int)m.size() ? m[i] : -1; }

void remapAttachment(Attachment& a, const Maps& m) {
    a.image = a.image >= 0 ? map(m.image, a.image) : -1;
    for (auto& list : a.weights)
        for (auto& w : list) w.bone = map(m.bone, w.bone);
    if (a.endSlot >= 0) a.endSlot = map(m.slot, a.endSlot);
}

Animation remapAnimation(const Animation& src, const Maps& m, int dstSlotCount, int srcSlotCount) {
    Animation out;
    out.name = src.name;
    out.duration = src.duration;
    for (auto& [b, tl] : src.bones) out.bones[map(m.bone, b)] = tl;
    for (auto& [s, tl] : src.slots) out.slots[map(m.slot, s)] = tl;
    for (auto& [c, keys] : src.constraints) out.constraints[map(m.constraint, c)] = keys;
    for (EventKey k : src.events) {
        k.event = map(m.event, k.event);
        out.events.push_back(k);
    }
    // Draw order keys cover every slot of the merged skeleton: existing slots keep their setup
    // order underneath, the imported slots follow in the keyed order.
    for (const DrawOrderKey& k : src.drawOrder) {
        if ((int)k.order.size() != srcSlotCount) continue;
        DrawOrderKey dk;
        dk.time = k.time;
        for (int i = 0; i < dstSlotCount - srcSlotCount; ++i) dk.order.push_back(i);
        for (int s : k.order) dk.order.push_back(map(m.slot, s));
        out.drawOrder.push_back(dk);
    }
    return out;
}
} // namespace

std::string mergeSkeleton(Skeleton& dst, const Skeleton& src) {
    Maps m;
    int newImages = 0, sharedEvents = 0, mergedAnims = 0, mergedSkins = 0, renamed = 0;

    for (const Image& im : src.images) {
        int found = -1;
        for (size_t i = 0; i < dst.images.size(); ++i)
            if (dst.images[i].path == im.path) found = (int)i;
        if (found < 0) {
            dst.images.push_back(im);
            found = (int)dst.images.size() - 1;
            ++newImages;
        }
        m.image.push_back(found);
    }

    // Bones (parents always come from the same imported skeleton).
    int boneBase = (int)dst.bones.size();
    for (size_t i = 0; i < src.bones.size(); ++i) m.bone.push_back(boneBase + (int)i);
    for (const Bone& b : src.bones) {
        Bone nb = b;
        nb.name = dst.uniqueBoneName(b.name);
        renamed += nb.name != b.name;
        nb.parent = b.parent >= 0 ? map(m.bone, b.parent) : -1;
        dst.bones.push_back(nb);
    }

    int slotBase = (int)dst.slots.size();
    for (size_t i = 0; i < src.slots.size(); ++i) m.slot.push_back(slotBase + (int)i);
    for (const Slot& s : src.slots) {
        Slot ns = s;
        ns.name = dst.uniqueSlotName(s.name);
        renamed += ns.name != s.name;
        ns.bone = map(m.bone, s.bone);
        for (auto& a : ns.attachments) remapAttachment(a, m);
        dst.slots.push_back(ns);
    }

    for (const EventDef& e : src.events) {
        int found = -1;
        for (size_t i = 0; i < dst.events.size(); ++i)
            if (dst.events[i].name == e.name) found = (int)i;
        if (found < 0) {
            dst.events.push_back(e);
            found = (int)dst.events.size() - 1;
        } else {
            ++sharedEvents;
        }
        m.event.push_back(found);
    }

    int conBase = (int)dst.constraints.size();
    for (size_t i = 0; i < src.constraints.size(); ++i) m.constraint.push_back(conBase + (int)i);
    for (const Constraint& c : src.constraints) {
        Constraint nc = c;
        nc.name = dst.uniqueConstraintName(c.name);
        for (int& b : nc.bones) b = map(m.bone, b);
        nc.target = c.type == ConstraintType::Path ? map(m.slot, c.target) : (c.target >= 0 ? map(m.bone, c.target) : -1);
        dst.constraints.push_back(nc);
    }

    for (const Skin& skin : src.skins) {
        Skin* target = nullptr;
        for (auto& s : dst.skins)
            if (s.name == skin.name) target = &s;
        if (!target) {
            dst.skins.push_back(Skin{skin.name, {}, {}, {}});
            target = &dst.skins.back();
        } else {
            ++mergedSkins;
        }
        for (int b : skin.bones) target->bones.push_back(map(m.bone, b));
        for (int c : skin.constraints) target->constraints.push_back(map(m.constraint, c));
        for (SkinEntry e : skin.attachments) {
            e.slot = map(m.slot, e.slot);
            remapAttachment(e.attachment, m);
            target->attachments.push_back(std::move(e));
        }
    }

    for (const Animation& an : src.animations) {
        Animation mapped = remapAnimation(an, m, (int)dst.slots.size(), (int)src.slots.size());
        Animation* existing = nullptr;
        for (auto& d : dst.animations)
            if (d.name == an.name) existing = &d;
        if (existing) {
            // Existing draw order keys do not know the new slots: extend them with the imported setup order.
            for (auto& k : existing->drawOrder)
                for (int s = (int)k.order.size(); s < (int)dst.slots.size(); ++s) k.order.push_back(s);
            mergeAnimation(*existing, mapped, 0);
            existing->duration = std::max(existing->duration, an.duration);
            ++mergedAnims;
        } else {
            dst.animations.push_back(std::move(mapped));
        }
    }
    // Animations that existed before and have draw order keys need the new slots too.
    for (auto& an : dst.animations)
        for (auto& k : an.drawOrder)
            for (int s = (int)k.order.size(); s < (int)dst.slots.size(); ++s) k.order.push_back(s);

    std::string report = "导入了 " + std::to_string(src.bones.size()) + " 根骨骼、" + std::to_string(src.slots.size()) +
                         " 个插槽、" + std::to_string(src.animations.size()) + " 个动画";
    if (newImages) report += "，新图片 " + std::to_string(newImages);
    if (mergedAnims) report += "，合并同名动画 " + std::to_string(mergedAnims);
    if (mergedSkins) report += "，合并同名皮肤 " + std::to_string(mergedSkins);
    if (sharedEvents) report += "，共用同名事件 " + std::to_string(sharedEvents);
    if (renamed) report += "，重命名 " + std::to_string(renamed) + " 项";
    return report;
}

} // namespace rig
