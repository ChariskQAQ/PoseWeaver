#include "core/diagnostics.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

namespace rig {
namespace {
bool finite(float v) { return std::isfinite(v); }
bool finite(Vec2 v) { return finite(v.x) && finite(v.y); }
bool finite(const Transform& t) {
    return finite(t.x) && finite(t.y) && finite(t.rotation) && finite(t.scaleX) && finite(t.scaleY) && finite(t.shearX) && finite(t.shearY);
}
bool finite(const Color& c) { return finite(c[0]) && finite(c[1]) && finite(c[2]) && finite(c[3]); }
bool finite(const ConstraintValues& v) { return std::all_of(v.begin(), v.end(), [](float x) { return finite(x); }); }
bool finite(const std::vector<Vec2>& v) { return std::all_of(v.begin(), v.end(), [](Vec2 x) { return finite(x); }); }
bool validCurve(const CurveShape& c) {
    return (c.type == Curve::Linear || c.type == Curve::Stepped || c.type == Curve::Bezier) && finite(c.cx1) && finite(c.cx2) && finite(c.cy1) && finite(c.cy2) && c.cx1 >= 0 && c.cx1 <= 1 && c.cx2 >= 0 && c.cx2 <= 1;
}
template<class K> bool validKeyCurves(const K& key) { return validCurve(key.curve); }
template<class K> bool validComponents(const K& key,unsigned mask) {
    return key.componentMask != 0 && !(key.componentMask & ~mask) && validCurve(key.curve) &&
        (!key.separatedCurves || std::all_of(key.componentCurves.begin(),key.componentCurves.end(),validCurve));
}
bool validKeyCurves(const Vec2Key& key) { return validComponents(key,3); }
bool validKeyCurves(const ColorKey& key) { return validComponents(key,15); }
bool reference(int index, size_t size) { return index >= 0 && (size_t)index < size; }
} // namespace

std::vector<Diagnostic> diagnose(const Skeleton& sk, const std::string& directory, bool resources) {
    std::vector<Diagnostic> result;
    auto add = [&](const char* code, const char* message, IssueTarget target, int index, int sub = -1,
                   int vertex = -1, Severity severity = Severity::Error, std::string detail = {}) {
        result.push_back({severity, code, message, std::move(detail), target, index, sub, vertex});
    };
    auto checkResource = [&](const std::string& path, IssueTarget target, int index, const char* code, const char* message) {
        if (!resources) return;
        std::error_code ec;
        std::filesystem::path file = std::filesystem::u8path(path);
        if (file.is_relative() && !directory.empty()) file = std::filesystem::u8path(directory) / file;
        if (path.empty() || !std::filesystem::is_regular_file(file, ec)) {
            add(code, message, target, index, -1, -1, Severity::Warning);
            result.back().path = file.u8string();
        }
    };
    std::string effectsError;
    if (!validRenderEffects(sk.effects, &effectsError)) add("INVALID_RENDER_EFFECTS", "Invalid light / pixel settings.", IssueTarget::Skeleton, -1, -1, -1, Severity::Error, effectsError);
    for (int i = 0; i < (int)sk.images.size(); ++i)
        checkResource(sk.images[i].path, IssueTarget::Image, i, "MISSING_IMAGE", "Image file is missing.");
    for (int i = 0; i < (int)sk.events.size(); ++i) {
        const auto& e = sk.events[i];
        if (!e.audioPath.empty()) checkResource(e.audioPath, IssueTarget::Event, i, "MISSING_AUDIO", "Audio file is missing.");
        if (!finite(e.floatValue) || !finite(e.volume) || !finite(e.balance))
            add("NON_FINITE", "Numeric values must be finite.", IssueTarget::Event, i);
    }
    std::vector<char> state(sk.bones.size(), 0);
    for (int i = 0; i < (int)sk.bones.size(); ++i) {
        const auto& b = sk.bones[i];
        if (b.parent < -1 || b.parent >= (int)sk.bones.size() || b.parent == i)
            add("INVALID_REFERENCE", "Reference is invalid.", IssueTarget::Bone, i);
        if (!finite(b.setup) || !finite(b.length)) add("NON_FINITE", "Numeric values must be finite.", IssueTarget::Bone, i);
        std::vector<int> chain;
        int node = i;
        while (reference(node, sk.bones.size()) && state[node] == 0) {
            state[node] = 1;
            chain.push_back(node);
            node = sk.bones[node].parent;
        }
        if (reference(node, sk.bones.size()) && state[node] == 1)
            add("BONE_CYCLE", "Bone hierarchy contains a cycle.", IssueTarget::Bone, node);
        for (int bIndex : chain) state[bIndex] = 2;
    }
    auto attachment = [&](const Attachment& a, IssueTarget target, int index, int sub) {
        if (a.image < -1 || a.image >= (int)sk.images.size())
            add("INVALID_REFERENCE", "Reference is invalid.", target, index, sub);
        if (a.skinPlaceholder) return;
        if (a.linkedAttachment < -1 || a.linkedSkin < -1 || (a.linkedAttachment<0 && a.linkedSkin!=-1))
            add("INVALID_REFERENCE","Invalid linked mesh source.",target,index,sub);
        if (a.linkedAttachment>=0) {
            int slot=target==IssueTarget::Attachment ? index : sk.skins[index].attachments[sub].slot;
            if (!sk.meshSource(slot,a)) add("LINKED_MESH","Linked mesh source is missing, is not a mesh, or contains a cycle.",target,index,sub);
            return;
        }
        if (!finite(a.transform) || !finite(a.vertices) || !finite(a.uvs))
            add("NON_FINITE", "Numeric values must be finite.", target, index, sub);
        if (!a.hasVertices()) return;
        const size_t count = a.vertexCount();
        if (a.type == AttachmentType::Mesh && (a.uvs.size() != count || a.triangles.size() % 3 != 0))
            add("MESH_TOPOLOGY", "Mesh vertex or triangle counts are invalid.", target, index, sub);
        if (a.hullLength < 0 || a.hullLength > (int)count || a.endSlot < -1 || a.endSlot >= (int)sk.slots.size())
            add("INVALID_REFERENCE", "Reference is invalid.", target, index, sub);
        for (int triangle : a.triangles)
            if (!reference(triangle, count)) {
                add("MESH_TOPOLOGY", "Mesh vertex or triangle counts are invalid.", target, index, sub);
                break;
            }
        for (int v = 0; v < (int)a.weights.size(); ++v) {
            float sum = 0;
            bool valid = true;
            std::set<int> bones;
            bool repeated = false;
            for (const auto& w : a.weights[v]) {
                valid = valid && reference(w.bone, sk.bones.size()) && finite(w.weight) && finite(w.local) && w.weight >= 0;
                repeated = !bones.insert(w.bone).second || repeated;
                sum += w.weight;
            }
            if (!valid) add("INVALID_WEIGHT", "Weight or influence bone is invalid.", target, index, sub, v);
            else if (!finite(sum) || std::fabs(sum - 1) > 1e-4f)
                add("WEIGHT_SUM", "Vertex weights do not sum to one.", target, index, sub, v, Severity::Warning);
            if (repeated) add("DUPLICATE_WEIGHT", "Vertex repeats an influence bone.", target, index, sub, v, Severity::Warning);
        }
    };
    for (int s = 0; s < (int)sk.slots.size(); ++s) {
        const auto& slot = sk.slots[s];
        if (!reference(slot.bone, sk.bones.size()) || slot.activeAttachment < -1 || slot.activeAttachment >= (int)slot.attachments.size())
            add("INVALID_REFERENCE", "Reference is invalid.", IssueTarget::Slot, s);
        if (!finite(slot.color)) add("NON_FINITE", "Numeric values must be finite.", IssueTarget::Slot, s);
        if (slot.blend < BlendMode::Normal || slot.blend > BlendMode::Screen)
            add("INVALID_BLEND_MODE", "Slot blend mode is invalid.", IssueTarget::Slot, s);
        for (int a = 0; a < (int)slot.attachments.size(); ++a) attachment(slot.attachments[a], IssueTarget::Attachment, s, a);
    }
    for (int s = 0; s < (int)sk.skins.size(); ++s) {
        const auto& skin = sk.skins[s];
        for (int b : skin.bones)
            if (!reference(b, sk.bones.size())) add("INVALID_REFERENCE", "Reference is invalid.", IssueTarget::Skin, s);
        for (int c : skin.constraints)
            if (!reference(c, sk.constraints.size())) add("INVALID_REFERENCE", "Reference is invalid.", IssueTarget::Skin, s);
        for (int a = 0; a < (int)skin.attachments.size(); ++a) {
            const auto& entry = skin.attachments[a];
            if (!reference(entry.slot, sk.slots.size())) add("INVALID_REFERENCE", "Reference is invalid.", IssueTarget::Skin, s, a);
            attachment(entry.attachment, IssueTarget::Skin, s, a);
        }
    }
    for (int i = 0; i < (int)sk.constraints.size(); ++i) {
        const auto& c = sk.constraints[i];
        bool valid = !c.bones.empty();
        for (int b : c.bones) valid = valid && reference(b, sk.bones.size());
        if (c.type != ConstraintType::Physics)
            valid = valid && reference(c.target, c.type == ConstraintType::Path ? sk.slots.size() : sk.bones.size());
        if (!valid) add("INVALID_REFERENCE", "Reference is invalid.", IssueTarget::Constraint, i);
        const float parameters[] = {c.offsetRotation, c.offsetX, c.offsetY, c.offsetScaleX, c.offsetScaleY, c.offsetShearY,
                                    c.inertia, c.strength, c.damping, c.mass, c.limit};
        if (!finite(c.values) || !std::all_of(std::begin(parameters), std::end(parameters), [](float v) { return finite(v); }))
            add("NON_FINITE", "Numeric values must be finite.", IssueTarget::Constraint, i);
    }
    for (int i = 0; i < (int)sk.animations.size(); ++i) {
        const auto& animation = sk.animations[i];
        auto animationError = [&](const char* code, const char* message, IssueTarget target = IssueTarget::Animation,
                                  int index = -1, int sub = -1, float time = -1) {
            add(code, message, target, index < 0 ? i : index, sub);
            result.back().animation = i;
            if (finite(time) && time >= 0) result.back().time = time;
        };
        if (!finite(animation.duration) || animation.duration < 0) animationError("INVALID_KEY", "Animation time, value or curve is invalid.");
        auto keys = [&](const auto& values, IssueTarget target, int index, int sub = -1) {
            float previous = -1;
            for (const auto& key : values) {
                if (!finite(key.time) || key.time < 0 || key.time < previous || !finite(key.value) || !validKeyCurves(key)) {
                    animationError("INVALID_KEY", "Animation time, value or curve is invalid.", target, index, sub, key.time);
                    break;
                }
                previous = key.time;
            }
        };
        for (const auto& [bone, timeline] : animation.bones) {
            if (!reference(bone, sk.bones.size())) { animationError("INVALID_REFERENCE", "Reference is invalid."); continue; }
            keys(timeline.rotate, IssueTarget::Bone, bone); keys(timeline.translate, IssueTarget::Bone, bone);
            keys(timeline.scale, IssueTarget::Bone, bone); keys(timeline.shear, IssueTarget::Bone, bone);
        }
        for (const auto& [slot, timeline] : animation.slots) {
            if (!reference(slot, sk.slots.size())) { animationError("INVALID_REFERENCE", "Reference is invalid."); continue; }
            keys(timeline.color, IssueTarget::Slot, slot);
            for (const auto& key : timeline.attachment)
                if (!finite(key.time) || key.time < 0 || key.value < -1 || key.value >= (int)sk.slots[slot].attachments.size())
                    animationError("INVALID_KEY", "Animation time, value or curve is invalid.", IssueTarget::Slot, slot, -1, key.time);
            for (const auto& [a, deform] : timeline.deform) {
                if (!reference(a, sk.slots[slot].attachments.size())) { animationError("INVALID_REFERENCE", "Reference is invalid."); continue; }
                keys(deform, IssueTarget::Attachment, slot, a);
                const auto& att = sk.slots[slot].attachments[a];
                for (const auto& key : deform) {
                    const auto* target=key.skin==-2 ? &att : sk.attachmentAt(slot,a,key.skin);
                    if (key.skin < -2 || !target) { animationError("INVALID_REFERENCE","Deform skin target is missing.",IssueTarget::Attachment,slot,a,key.time); continue; }
                    if (target->linkedAttachment>=0) target=sk.meshSource(slot,*target);
                    if (target && !target->skinPlaceholder && key.value.size() != target->vertexCount())
                        animationError("DEFORM_TOPOLOGY", "Deform vertex count does not match its attachment.", IssueTarget::Attachment, slot, a, key.time);
                }
            }
        }
        for (const auto& [constraint, timeline] : animation.constraints) {
            if (!reference(constraint, sk.constraints.size())) { animationError("INVALID_REFERENCE", "Reference is invalid."); continue; }
            keys(timeline, IssueTarget::Constraint, constraint);
        }
        for (const auto& key : animation.events)
            if (!finite(key.time) || key.time < 0 || !finite(key.floatValue) || !reference(key.event, sk.events.size()))
                animationError("INVALID_KEY", "Animation time, value or curve is invalid.",
                               reference(key.event, sk.events.size()) ? IssueTarget::Event : IssueTarget::Animation,
                               reference(key.event, sk.events.size()) ? key.event : i, -1, key.time);
        for (const auto& key : animation.drawOrder) {
            std::set<int> seen(key.order.begin(), key.order.end());
            if (!finite(key.time) || key.time < 0 || seen.size() != sk.slots.size() || key.order.size() != sk.slots.size() ||
                (!seen.empty() && (*seen.begin() < 0 || *seen.rbegin() >= (int)sk.slots.size())))
                animationError("DRAW_ORDER", "Draw order must contain each slot exactly once.", IssueTarget::Animation, i, -1, key.time);
        }
    }
    return result;
}

bool validateSkeleton(const Skeleton& sk, std::string* error) {
    for (const auto& issue : diagnose(sk, {}, false))
        if (issue.severity == Severity::Error) {
            if (error) *error = issue.code + ": " + issue.message;
            return false;
        }
    return true;
}

} // namespace rig
