#include "core/model.h"
#include "core/pose.h"

#include <algorithm>
#include <functional>
#include <iterator>

namespace rig {

const char* blendModeName(BlendMode mode) {
    switch (mode) {
    case BlendMode::Normal: return "normal";
    case BlendMode::Additive: return "additive";
    case BlendMode::Multiply: return "multiply";
    case BlendMode::Screen: return "screen";
    }
    return "invalid";
}

bool parseBlendMode(const std::string& name, BlendMode& mode) {
    for (BlendMode candidate : {BlendMode::Normal, BlendMode::Additive, BlendMode::Multiply, BlendMode::Screen}) {
        if (name == blendModeName(candidate)) { mode = candidate; return true; }
    }
    return false;
}

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
    if (skin<0 || skin>=(int)skins.size()) return;
    auto update=[&](int slot,Attachment& attachment) {
        const Attachment* cursor=&attachment; std::vector<const Attachment*> visited;
        while (cursor && cursor->linkedAttachment>=0) {
            if (cursor->linkedSkin==skin) { detachLinkedMesh(slot,attachment); break; }
            if (std::find(visited.begin(),visited.end(),cursor)!=visited.end()) break;
            visited.push_back(cursor); cursor=attachmentAt(slot,cursor->linkedAttachment,cursor->linkedSkin);
        }
    };
    for (int s=0;s<(int)slots.size();++s) for (auto& attachment : slots[s].attachments) update(s,attachment);
    for (auto& owner : skins) for (auto& entry : owner.attachments) update(entry.slot,entry.attachment);
    skins.erase(skins.begin()+skin);
    for (auto& slot : slots) for (auto& attachment : slot.attachments) if (attachment.linkedSkin>skin) --attachment.linkedSkin;
    for (auto& owner : skins) for (auto& entry : owner.attachments) if (entry.attachment.linkedSkin>skin) --entry.attachment.linkedSkin;
    for (auto& animation : animations) for (auto& [slot,timeline] : animation.slots) for (auto& [attachment,keys] : timeline.deform) {
        keys.erase(std::remove_if(keys.begin(),keys.end(),[&](const DeformKey& key) { return key.skin==skin; }),keys.end());
        for (auto& key : keys) if (key.skin>skin) --key.skin;
    }
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

const Attachment* Skeleton::attachmentAt(int slot,int index,int skin) const {
    if (slot<0 || slot>=(int)slots.size() || index<0 || index>=(int)slots[slot].attachments.size()) return nullptr;
    const auto& a=slots[slot].attachments[index];
    if (skin==-1) return a.skinPlaceholder ? nullptr : &a;
    return skin>=0 && skin<(int)skins.size() && a.skinPlaceholder ? skins[skin].find(slot,a.name) : nullptr;
}
int Skeleton::attachmentSkin(int slot,const Attachment* attachment) const {
    if (slot<0 || slot>=(int)slots.size()) return -1;
    for (int i=0;i<(int)skins.size();++i) for (const auto& entry : skins[i].attachments)
        if (entry.slot==slot && &entry.attachment==attachment) return i;
    return -1;
}
const Attachment* Skeleton::meshSource(int slot,const Attachment& attachment) const {
    const Attachment* current=&attachment;
    std::vector<const Attachment*> visited;
    while (current && current->linkedAttachment>=0) {
        if (current->type!=AttachmentType::Mesh || std::find(visited.begin(),visited.end(),current)!=visited.end()) return nullptr;
        visited.push_back(current);
        current=attachmentAt(slot,current->linkedAttachment,current->linkedSkin);
    }
    return current && current->type==AttachmentType::Mesh && !current->skinPlaceholder ? current : nullptr;
}
void Skeleton::refreshLinkedMesh(int slot,Attachment& attachment) const {
    if (attachment.linkedAttachment<0) return;
    const auto* source=meshSource(slot,attachment); if (!source) return;
    attachment.vertices=source->vertices; attachment.weights=source->weights;
    attachment.uvs=source->uvs; attachment.triangles=source->triangles; attachment.hullLength=source->hullLength;
}
bool Skeleton::detachLinkedMesh(int slot,Attachment& attachment) {
    if (attachment.linkedAttachment<0 || !meshSource(slot,attachment)) return false;
    int target=-1;
    const int targetSkin=attachmentSkin(slot,&attachment);
    for (int i=0;i<(int)slots[slot].attachments.size();++i) if (attachmentAt(slot,i,targetSkin)==&attachment) target=i;
    if (attachment.inheritDeform && target>=0) {
        const Attachment* owner=&attachment; int source=target;
        while (owner->linkedAttachment>=0 && owner->inheritDeform) {
            source=owner->linkedAttachment; owner=attachmentAt(slot,source,owner->linkedSkin);
        }
        const int sourceSkin=attachmentSkin(slot,owner);
        for (auto& animation : animations) {
            auto timeline=animation.slots.find(slot); if (timeline==animation.slots.end()) continue;
            auto sourceTrack=timeline->second.deform.find(source); if (sourceTrack==timeline->second.deform.end()) continue;
            std::vector<DeformKey> copy;
            for (auto key : sourceTrack->second) if (key.skin==sourceSkin) { key.skin=targetSkin; copy.push_back(std::move(key)); }
            if (copy.empty()) for (auto key : sourceTrack->second) if (key.skin==-2) { key.skin=targetSkin; copy.push_back(std::move(key)); }
            auto& destination=timeline->second.deform[target];
            destination.erase(std::remove_if(destination.begin(),destination.end(),[&](const DeformKey& key) { return key.skin==targetSkin; }),destination.end());
            destination.insert(destination.end(),copy.begin(),copy.end());
            std::stable_sort(destination.begin(),destination.end(),[](const DeformKey& a,const DeformKey& b) { return a.time<b.time; });
        }
    }
    refreshLinkedMesh(slot,attachment); attachment.linkedAttachment=attachment.linkedSkin=-1; return true;
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
    auto update=[&](Attachment& linked) {
        const Attachment* cursor=&linked; std::vector<const Attachment*> visited;
        while (cursor && cursor->linkedAttachment>=0) {
            if (cursor->linkedAttachment==attachment) { detachLinkedMesh(slot,linked); break; }
            if (std::find(visited.begin(),visited.end(),cursor)!=visited.end()) break;
            visited.push_back(cursor); cursor=attachmentAt(slot,cursor->linkedAttachment,cursor->linkedSkin);
        }
    };
    for (auto& linked : s.attachments) update(linked);
    for (auto& skin : skins) for (auto& entry : skin.attachments) if (entry.slot==slot) update(entry.attachment);
    if (s.attachments[attachment].skinPlaceholder) {
        const std::string name = s.attachments[attachment].name;
        for (auto& skin : skins)
            skin.attachments.erase(std::remove_if(skin.attachments.begin(), skin.attachments.end(),
                                                  [&](const SkinEntry& e) { return e.slot == slot && e.placeholder == name; }),
                                   skin.attachments.end());
    }
    s.attachments.erase(s.attachments.begin() + attachment);
    for (auto& linked : s.attachments) if (linked.linkedAttachment>attachment) --linked.linkedAttachment;
    for (auto& skin : skins) for (auto& entry : skin.attachments) if (entry.slot==slot && entry.attachment.linkedAttachment>attachment) --entry.attachment.linkedAttachment;
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
    int dropped = 0;
    for (auto& an : animations) {
        auto it = an.slots.find(slot);
        if (it == an.slots.end()) continue;
        for (auto d=it->second.deform.begin();d!=it->second.deform.end();) {
            const int index=d->first; auto& keys=d->second; size_t before=keys.size();
            keys.erase(std::remove_if(keys.begin(),keys.end(),[&](const DeformKey& key) {
                if (index<0 || index>=(int)slots[slot].attachments.size()) return false;
                auto fits=[&](const Attachment* target) {
                    if (!target) return false;
                    if (target->linkedAttachment>=0) target=meshSource(slot,*target);
                    return target && key.value.size()==target->vertexCount();
                };
                if (key.skin!=-2) {
                    const auto* target=attachmentAt(slot,index,key.skin);
                    return target && !fits(target);
                }
                const auto& base=slots[slot].attachments[index];
                if (!base.skinPlaceholder) return !fits(&base);
                bool found=false;
                for (int s=0;s<(int)skins.size();++s) if (const auto* target=attachmentAt(slot,index,s)) { found=true; if (fits(target)) return false; }
                return found;
            }),keys.end());
            dropped+=(int)(before-keys.size());
            d=keys.empty() ? it->second.deform.erase(d) : std::next(d);
        }
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
template<class K,class V> static void setCombinedKey(std::vector<K>& keys,float time,const V& value,unsigned mask) {
    upsert(keys,time,value); findAt(keys,time)->componentMask = mask;
}
void setTranslateKey(Animation& anim, int bone, float time, Vec2 value) { setCombinedKey(anim.bones[bone].translate,time,value,3); }
void setScaleKey(Animation& anim, int bone, float time, Vec2 value) { setCombinedKey(anim.bones[bone].scale,time,value,3); }
void setShearKey(Animation& anim, int bone, float time, Vec2 value) { setCombinedKey(anim.bones[bone].shear,time,value,3); }
void setAttachmentKey(Animation& anim, int slot, float time, int attachment) { upsert(anim.slots[slot].attachment, time, attachment); }
void setColorKey(Animation& anim, int slot, float time, const Color& color) { setCombinedKey(anim.slots[slot].color,time,color,15); }

static float& valueComponent(Vec2& value,int c) { return c ? value.y : value.x; }
static float& valueComponent(Color& value,int c) { return value[c]; }
static float valueComponent(const Vec2& value,int c) { return c ? value.y : value.x; }
static float valueComponent(const Color& value,int c) { return value[c]; }
template<class K> static bool editComponent(std::vector<K>& keys,int component,float time,float value) {
    K* key = findAt(keys,time);
    if (!key) { K fresh; fresh.time = time; fresh.componentMask = 0; insertSorted(keys,fresh); key = findAt(keys,time); }
    valueComponent(key->value,component) = value; key->componentMask |= 1u << component; return true;
}
template<class K> static bool eraseComponent(std::vector<K>& keys,int component,float time) {
    K* key = findAt(keys,time);
    if (!key || !(key->componentMask & (1u << component))) return false;
    key->componentMask &= ~(1u << component);
    if (!key->componentMask) eraseAt(keys,time);
    return true;
}
template<class K> static bool shiftComponent(std::vector<K>& keys,int component,float from,float to) {
    K* source = findAt(keys,from); K* target = findAt(keys,to);
    if (!source || !(source->componentMask & (1u << component)) || (target && (target->componentMask & (1u << component)))) return false;
    K copy = *source; eraseComponent(keys,component,from);
    editComponent(keys,component,to,valueComponent(copy.value,component));
    editComponentCurve(*findAt(keys,to),component) = componentCurve(copy,component); return true;
}
template<class Action> static bool componentKeys(Animation& anim,int index,Channel channel,int component,Action action) {
    if (index < 0 || component < 0) return false;
    if (channel == Channel::Color) return component < 4 && action(anim.slots[index].color);
    if (component > 1 || (channel != Channel::Translate && channel != Channel::Scale && channel != Channel::Shear)) return false;
    auto& timeline = anim.bones[index];
    return action(channel == Channel::Translate ? timeline.translate : channel == Channel::Scale ? timeline.scale : timeline.shear);
}
bool setComponentKey(Animation& anim,int index,Channel channel,int component,float time,float value) {
    if (!std::isfinite(time) || time < 0 || !std::isfinite(value)) return false;
    return componentKeys(anim,index,channel,component,[&](auto& keys) { return editComponent(keys,component,time,value); });
}
bool removeComponentKey(Animation& anim,int index,Channel channel,int component,float time) {
    return componentKeys(anim,index,channel,component,[&](auto& keys) { return eraseComponent(keys,component,time); });
}
bool moveComponentKey(Animation& anim,int index,Channel channel,int component,float from,float to) {
    if (!std::isfinite(to) || to < 0 || std::fabs(from-to) < kKeyEpsilon) return false;
    return componentKeys(anim,index,channel,component,[&](auto& keys) { return shiftComponent(keys,component,from,to); });
}
bool usesComponentKeys(const Skeleton& sk) {
    auto used = [](const auto& keys,unsigned mask) { return std::any_of(keys.begin(),keys.end(),[&](const auto& k) { return k.separatedCurves || k.componentMask != mask; }); };
    for (const auto& animation : sk.animations) {
        for (const auto& [index,timeline] : animation.bones)
            if (used(timeline.translate,3) || used(timeline.scale,3) || used(timeline.shear,3)) return true;
        for (const auto& [index,timeline] : animation.slots) if (used(timeline.color,15)) return true;
    }
    return false;
}

void setDeformKey(Animation& anim, int slot, int attachment, float time, const std::vector<Vec2>& offsets,int skin) {
    auto& keys=anim.slots[slot].deform[attachment];
    keys.erase(std::remove_if(keys.begin(),keys.end(),[&](const DeformKey& key) { return key.skin==skin && std::fabs(key.time-time)<kKeyEpsilon; }),keys.end());
    insertSorted(keys,DeformKey{time,offsets,{},skin});
}
bool hasDeformKey(const Animation& anim,int slot,int attachment,float time,int skin) {
    auto s=anim.slots.find(slot); if (s==anim.slots.end()) return false;
    auto a=s->second.deform.find(attachment); if (a==s->second.deform.end()) return false;
    return std::any_of(a->second.begin(),a->second.end(),[&](const DeformKey& key) { return (key.skin==skin || key.skin==-2) && std::fabs(key.time-time)<kKeyEpsilon; });
}
void removeDeformKey(Animation& anim,int slot,int attachment,float time,int skin) {
    auto s=anim.slots.find(slot); if (s==anim.slots.end()) return;
    auto a=s->second.deform.find(attachment); if (a==s->second.deform.end()) return;
    a->second.erase(std::remove_if(a->second.begin(),a->second.end(),[&](const DeformKey& key) { return (key.skin==skin || key.skin==-2) && std::fabs(key.time-time)<kKeyEpsilon; }),a->second.end());
    if (a->second.empty()) s->second.deform.erase(a);
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
    if (ch == Channel::Translate || ch == Channel::Scale || ch == Channel::Shear || ch == Channel::Color) {
        componentKeys(anim,index,ch,0,[&](auto& keys) {
            if (auto* key = findAt(keys,time)) { key->curve = curve; key->componentCurves.fill(curve); key->separatedCurves = false; }
            return true;
        });
        return;
    }
    if (ch == Channel::Deform) {
        auto it = anim.slots.find(index);
        if (it == anim.slots.end()) return;
        for (auto& [a, keys] : it->second.deform) if (sub<0 || sub==a)
            for (auto& key : keys) if (std::fabs(key.time-time)<kKeyEpsilon) key.curve=curve;
        return;
    }
    if (CurveShape* c = curveAt(anim, index, ch, time, sub)) *c = curve;
}

const CurveShape* keyCurve(const Animation& anim, int index, Channel ch, float time, int sub) {
    return curveAt(const_cast<Animation&>(anim), index, ch, time, sub);
}

template<class K> static void insertMerged(std::vector<K>& keys,const K& key) { eraseAt(keys,key.time); insertSorted(keys,key); }
template<class K> static void mergeComponentKey(std::vector<K>& keys,const K& key,unsigned fullMask,int components) {
    auto* target = findAt(keys,key.time);
    if (!target || key.componentMask == fullMask) { eraseAt(keys,key.time); insertSorted(keys,key); return; }
    for (int c = 0; c < components; ++c) if (key.componentMask & (1u << c)) {
        valueComponent(target->value,c) = valueComponent(key.value,c);
        editComponentCurve(*target,c) = componentCurve(key,c);
    }
    target->componentMask |= key.componentMask;
}
static void insertMerged(std::vector<Vec2Key>& keys,const Vec2Key& key) { mergeComponentKey(keys,key,3,2); }
static void insertMerged(std::vector<ColorKey>& keys,const ColorKey& key) { mergeComponentKey(keys,key,15,4); }
static void insertMerged(std::vector<DeformKey>& keys,const DeformKey& key) {
    keys.erase(std::remove_if(keys.begin(),keys.end(),[&](const DeformKey& other) { return other.skin==key.skin && std::fabs(other.time-key.time)<kKeyEpsilon; }),keys.end());
    insertSorted(keys,key);
}

template <typename K>
static void shiftKey(std::vector<K>& keys, float from, float to) {
    auto it = std::find_if(keys.begin(), keys.end(),
                           [&](const K& k) { return std::fabs(k.time - from) < kKeyEpsilon; });
    if (it == keys.end()) return;
    K moved = *it;
    keys.erase(it);
    moved.time = to;
    insertMerged(keys, moved);
}
static void shiftKey(std::vector<DeformKey>& keys,float from,float to) {
    std::vector<DeformKey> moved;
    keys.erase(std::remove_if(keys.begin(),keys.end(),[&](const DeformKey& key) { if (std::fabs(key.time-from)>=kKeyEpsilon) return false; moved.push_back(key); return true; }),keys.end());
    for (auto& key : moved) { key.time=to; insertMerged(keys,key); }
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
        insertMerged(dst, copy);
        return;
    }
}
static void copyAt(const std::vector<DeformKey>& src,std::vector<DeformKey>& dst,float time,float newTime) {
    std::vector<DeformKey> copied;
    for (auto key : src) if (std::fabs(key.time-time)<kKeyEpsilon) { key.time=newTime; copied.push_back(std::move(key)); }
    for (const auto& key : copied) insertMerged(dst,key);
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
