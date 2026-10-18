#include "editor/editor.h"

#include "core/clipping.h"
#include "core/constraints.h"
#include "core/mesh_tools.h"
#include "editor/theme.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

using namespace rig;

static Vec2 toVec(ImVec2 v) { return {v.x, v.y}; }
static ImVec2 toIm(Vec2 v) { return {v.x, v.y}; }
static float dist(ImVec2 a, ImVec2 b) { return length(toVec(a) - toVec(b)); }

static ImU32 weightColor(float w, float alpha) {
    w = std::clamp(w, 0.0f, 1.0f);
    return (ImU32)ImColor::HSV(0.66f * (1.0f - w), 0.85f, 0.95f, alpha);
}

// Barycentric coordinates of p in triangle abc; false if p is outside.
static bool barycentric(Vec2 p, Vec2 a, Vec2 b, Vec2 c, float& u, float& v, float& w) {
    float d = cross(b - a, c - a);
    if (std::fabs(d) < 1e-9f) return false;
    v = cross(p - a, c - a) / d;
    w = cross(b - a, p - a) / d;
    u = 1 - v - w;
    return u >= 0 && v >= 0 && w >= 0;
}

ImVec2 Editor::toScreen(Vec2 w) const {
    return {canvasOrigin_.x + canvasSize_.x * 0.5f + viewOffset_.x + w.x * zoom_,
            canvasOrigin_.y + canvasSize_.y * 0.5f + viewOffset_.y - w.y * zoom_};
}

Vec2 Editor::toWorld(ImVec2 s) const {
    return {(s.x - canvasOrigin_.x - canvasSize_.x * 0.5f - viewOffset_.x) / zoom_,
            -(s.y - canvasOrigin_.y - canvasSize_.y * 0.5f - viewOffset_.y) / zoom_};
}

// ---------------------------------------------------------------- picking

int Editor::pickBone(ImVec2 screen) const {
    if (!view_.bonesVisible || !view_.bonesSelectable) return -1;
    int best = -1;
    float bestD = 7.0f * uiScale_;
    for (int i = (int)sk_.bones.size() - 1; i >= 0; --i) {
        if (hiddenBones_.count(i)) continue;
        ImVec2 o = toScreen(pose_.world[i].origin());
        float d;
        if (sk_.bones[i].length * zoom_ > 4) {
            ImVec2 t = toScreen(boneTip(sk_.bones[i], pose_.world[i]));
            d = distToSegment(toVec(screen), toVec(o), toVec(t));
        } else {
            d = dist(screen, o);
        }
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

int Editor::attachmentIndex(int slot, const Attachment* att) const {
    if (!att || slot < 0 || slot >= (int)sk_.slots.size()) return -1;
    const auto& list = sk_.slots[slot].attachments;
    if (!list.empty() && att >= &list.front() && att <= &list.back()) return (int)(att - &list.front());
    // A skin attachment stands for the placeholder with the same name in the slot.
    for (const Skin& skin : sk_.skins)
        for (const SkinEntry& e : skin.attachments)
            if (e.slot == slot && &e.attachment == att)
                for (size_t i = 0; i < list.size(); ++i)
                    if (list[i].skinPlaceholder && list[i].name == e.placeholder) return (int)i;
    return -1;
}

const std::vector<Vec2>* Editor::poseDeform(int slot, const Attachment* att) const {
    if (mode_ != Mode::Animate) return nullptr;
    return findDeform(pose_, slot, attachmentIndex(slot, att));
}

std::vector<Vec2> Editor::attachmentVertices(int slot, const Attachment& att) const {
    return computeVertices(sk_, pose_, sk_.slots[slot], att, poseDeform(slot, &att));
}

int Editor::pickSlot(Vec2 world) const {
    if (!view_.imagesVisible || !view_.imagesSelectable) return -1;
    for (int i = (int)pose_.drawOrder.size() - 1; i >= 0; --i) {
        int s = pose_.drawOrder[i];
        if (hiddenSlots_.count(s)) continue;
        const Slot& slot = sk_.slots[s];
        const Attachment* resolved = poseAttachment(sk_, pose_, s);
        if (!resolved) continue;
        const Attachment& att = *resolved;
        if (att.isPolygon()) {
            if (view_.polygonsVisible && pointInPolygon(world, attachmentVertices(s, att))) return s;
            continue;
        }
        Geometry g;
        computeGeometry(sk_, pose_, slot, att, g, poseDeform(s, &att));
        for (size_t t = 0; t + 2 < g.triangles.size(); t += 3) {
            int i0 = g.triangles[t], i1 = g.triangles[t + 1], i2 = g.triangles[t + 2];
            float u, v, w;
            if (!barycentric(world, g.positions[i0], g.positions[i1], g.positions[i2], u, v, w)) continue;
            Vec2 uv = g.uvs[i0] * u + g.uvs[i1] * v + g.uvs[i2] * w;
            bool hasTex = att.image >= 0 && att.image < (int)textures_.size();
            if (!hasTex || textures_[att.image].alphaAt(uv.x, uv.y) > 10) return s;
        }
    }
    return -1;
}

int Editor::pickVertex(ImVec2 screen) const {
    if (sel_.slot < 0) return -1;
    const Slot& slot = sk_.slots[sel_.slot];
    (void)slot;
    const Attachment* att = const_cast<Editor*>(this)->selectedAttachment();
    if (!att || !att->hasVertices()) return -1;
    std::vector<Vec2> verts = attachmentVertices(sel_.slot, *att);
    int best = -1;
    float bestD = 8.0f * uiScale_;
    for (size_t i = 0; i < verts.size(); ++i) {
        float d = dist(screen, toScreen(verts[i]));
        if (d < bestD) {
            bestD = d;
            best = (int)i;
        }
    }
    return best;
}

bool Editor::overBoneTip(ImVec2 screen) const {
    if (mode_ != Mode::Setup || sel_.kind != Selection::Bone || sel_.bone < 0) return false;
    if (tool_ != Tool::Rotate && tool_ != Tool::Translate && tool_ != Tool::Scale && tool_ != Tool::Shear) return false;
    const Bone& b = sk_.bones[sel_.bone];
    if (b.length * zoom_ < 12) return false;
    return dist(screen, toScreen(boneTip(b, pose_.world[sel_.bone]))) < 6.0f * uiScale_;
}

// ---------------------------------------------------------------- input

void Editor::viewportMouseDown(ImVec2 mouse, bool shift, bool ctrl) {
    (void)ctrl;
    drag_ = Drag{};
    drag_.startScreen = mouse;
    drag_.startWorld = toWorld(mouse);
    drag_.tool = tool_;

    auto beginBoneDrag = [&](int b, Tool tool) {
        drag_.kind = Drag::Bone;
        drag_.tool = tool;
        drag_.pivotWorld = pose_.world[b].origin();
        int p = sk_.bones[b].parent;
        drag_.parentWorld = p >= 0 ? pose_.world[p] : Affine{};
        drag_.startTransform = pose_.local[b];
    };
    auto beginAttachmentDrag = [&](Attachment* att) {
        drag_.kind = Drag::Attachment;
        drag_.parentWorld = pose_.world[sel_.bone];
        drag_.startTransform = att->transform;
        drag_.pivotWorld = drag_.parentWorld.apply({att->transform.x, att->transform.y});
    };
    // Drag whatever is selected (Spine lets transform drags start anywhere).
    auto dragSelection = [&]() {
        if (Attachment* att = transformTargetAttachment()) beginAttachmentDrag(att);
        else if (sel_.bone >= 0 && sel_.kind != Selection::None) beginBoneDrag(sel_.bone, tool_);
    };

    switch (tool_) {
    case Tool::Rotate:
    case Tool::Translate:
    case Tool::Scale:
    case Tool::Shear: {
        if (overBoneTip(mouse)) {
            drag_.kind = Drag::BoneLength;
            return;
        }
        int b = pickBone(mouse);
        if (b >= 0) {
            if (b != sel_.bone || sel_.kind != Selection::Bone) selectBone(b);
            beginBoneDrag(b, tool_);
            return;
        }
        int s = pickSlot(drag_.startWorld);
        if (s >= 0 && s != sel_.slot) {
            if (mode_ == Mode::Setup) {
                const Slot& slot = sk_.slots[s];
                int a = s < (int)pose_.slotAttachment.size() ? pose_.slotAttachment[s] : slot.activeAttachment;
                selectAttachment(s, a);
            } else {
                selectBone(sk_.slots[s].bone);  // in Animate mode images pose their bone
            }
        }
        dragSelection();
        return;
    }
    case Tool::Pose: {
        int b = pickBone(mouse);
        if (b < 0) {
            sel_.clear();
            return;
        }
        if (ctrl) {
            // Ctrl+click toggles bones in the multi-selection (posed together as IK chains).
            std::vector<int> multi = sel_.bones;
            if (multi.empty() && sel_.kind == Selection::Bone && sel_.bone >= 0) multi.push_back(sel_.bone);
            auto it = std::find(multi.begin(), multi.end(), b);
            if (it != multi.end()) multi.erase(it);
            else multi.push_back(b);
            selectBone(multi.empty() ? -1 : multi.back());
            sel_.bones = multi;
            return;
        }
        bool inMulti = std::find(sel_.bones.begin(), sel_.bones.end(), b) != sel_.bones.end();
        std::vector<int> multi = inMulti ? sel_.bones : std::vector<int>{b};
        selectBone(b);
        sel_.bones = multi;
        bool onOrigin = dist(mouse, toScreen(pose_.world[b].origin())) < 8.0f * uiScale_;
        beginBoneDrag(b, onOrigin ? Tool::Translate : Tool::Rotate);
        if (onOrigin) return;
        // The dragged bone and its selected ancestors (contiguous) form the IK chain.
        std::vector<int> chain{b};
        for (int p = sk_.bones[b].parent; p >= 0 && std::find(multi.begin(), multi.end(), p) != multi.end(); p = sk_.bones[p].parent)
            chain.insert(chain.begin(), p);
        if (chain.size() >= 2) {
            drag_.kind = Drag::PoseIk;
            drag_.chain = chain;
            drag_.startPose = pose_;
        } else {
            drag_.kind = Drag::PoseRotate;
        }
        return;
    }
    case Tool::CreateBone:
        drag_.kind = Drag::CreateBone;
        drag_.parentBone = sel_.bone;
        return;
    case Tool::MeshEdit: {
        if (ImGui::GetIO().KeyAlt && mode_ == Mode::Setup) {
            meshAddVertexAt(mouse);
            return;
        }
        int v = pickVertex(mouse);
        if (v >= 0) {
            auto it = std::find(sel_.vertices.begin(), sel_.vertices.end(), v);
            if (shift) {
                if (it != sel_.vertices.end()) sel_.vertices.erase(it);
                else sel_.vertices.push_back(v);
            } else if (it == sel_.vertices.end()) {
                sel_.vertices = {v};
            }
            drag_.kind = Drag::Vertices;
            Attachment* att = selectedAttachment();
            drag_.startVertices = attachmentVertices(sel_.slot, *att);
            if (mode_ == Mode::Animate) {
                // Deform: remember the current offsets and how offsets map to world space.
                const std::vector<Vec2>* d = poseDeform(sel_.slot, att);
                drag_.startOffsets = d ? *d : std::vector<Vec2>(att->vertexCount(), Vec2{});
                drag_.deformBases.clear();
                for (int i = 0; i < (int)att->vertexCount(); ++i)
                    drag_.deformBases.push_back(deformBasis(pose_, sk_.slots[sel_.slot], *att, i).inverse());
            } else if (att->type == AttachmentType::Mesh && att->image >= 0 && att->image < (int)sk_.images.size()) {
                drag_.imageMap = imageToLocal(sk_, sk_.slots[sel_.slot], *att, sk_.images[att->image]);
            }
            return;
        }
        int s = pickSlot(drag_.startWorld);
        if (s >= 0 && s != sel_.slot) {
            int a = s < (int)pose_.slotAttachment.size() ? pose_.slotAttachment[s] : sk_.slots[s].activeAttachment;
            selectAttachment(s, a);
            Attachment* att = selectedAttachment();
            if (!att || !att->hasVertices()) setStatus("所选附件不是网格：在属性面板点“自动生成网格”");
            return;
        }
        if (!shift) sel_.vertices.clear();
        drag_.kind = Drag::BoxSelect;
        return;
    }
    case Tool::Weights: {
        Attachment* att = selectedAttachment();
        if (!att || !att->isWeighted()) {
            int s = pickSlot(drag_.startWorld);
            if (s >= 0) {
                int keepBone = sel_.bone;
                selectSlot(s);
                sel_.bone = keepBone;
                att = selectedAttachment();
            }
            if (!att || !att->isWeighted()) {
                setStatus("请选择已绑定骨骼的网格（属性面板 → 绑定骨骼）");
                return;
            }
        }
        if (sel_.bone < 0) {
            setStatus("请先在权重视图里选一根骨骼");
            return;
        }
        drag_.kind = Drag::Paint;
        return;
    }
    }
}

void Editor::viewportDrag(ImVec2 mouse, bool shift, bool ctrl, float dt) {
    if (!drag_.moved && dist(mouse, drag_.startScreen) < 3.0f && drag_.kind != Drag::Paint) return;
    drag_.moved = true;
    auto ensureCheckpoint = [&] {
        if (!drag_.checkpointed) {
            checkpoint();
            drag_.checkpointed = true;
        }
    };
    Vec2 world = toWorld(mouse);
    Vec2 worldDelta = world - drag_.startWorld;

    auto rotatedDelta = [&]() {
        Vec2 a = drag_.startWorld - drag_.pivotWorld, b = world - drag_.pivotWorld;
        float d = wrapDeg(radToDeg(std::atan2(b.y, b.x) - std::atan2(a.y, a.x)));
        return drag_.parentWorld.det() < 0 ? -d : d;
    };
    auto scaleFactor = [&]() {
        float d0 = length(drag_.startWorld - drag_.pivotWorld);
        float d1 = length(world - drag_.pivotWorld);
        return d0 > 1e-3f ? d1 / d0 : 1.0f;
    };
    // Applies the drag's transform tool to `t` (a copy of the drag-start transform).
    auto applyTool = [&](Transform& t) {
        if (drag_.tool == Tool::Rotate) {
            t.rotation += rotatedDelta();
            if (shift) t.rotation = std::round(t.rotation / 15.0f) * 15.0f;
        } else if (drag_.tool == Tool::Translate) {
            Vec2 l = drag_.parentWorld.inverse().applyVector(worldDelta);
            t.x += l.x;
            t.y += l.y;
        } else if (drag_.tool == Tool::Scale) {
            float f = scaleFactor();
            t.scaleX *= f;
            if (!ctrl) t.scaleY *= f;
        } else if (drag_.tool == Tool::Shear) {
            // Dragging around the pivot tilts the Y axis (Ctrl: the X axis).
            float d = rotatedDelta();
            if (ctrl) t.shearX += d;
            else t.shearY += d;
        }
    };

    switch (drag_.kind) {
    case Drag::Bone: {
        ensureCheckpoint();
        Transform t = drag_.startTransform;
        applyTool(t);
        transformBone(sel_.bone, t, drag_.tool == Tool::Rotate, drag_.tool == Tool::Translate, drag_.tool == Tool::Scale,
                      drag_.tool == Tool::Shear);
        break;
    }
    case Drag::PoseIk: {
        ensureCheckpoint();
        std::vector<float> rot = solveChainIk(sk_, drag_.startPose, drag_.chain, world, 16);
        for (size_t i = 0; i < drag_.chain.size(); ++i) {
            Transform t = drag_.startPose.local[drag_.chain[i]];
            t.rotation = rot[i];
            setBoneLocal(drag_.chain[i], t, true, false, false, false);
        }
        break;
    }
    case Drag::PoseRotate: {
        ensureCheckpoint();
        Transform t = drag_.startTransform;
        Vec2 d = world - drag_.pivotWorld;
        float target = radToDeg(std::atan2(d.y, d.x));
        float parentRot = drag_.parentWorld.rotationDeg();
        t.rotation = drag_.parentWorld.det() < 0 ? parentRot - target : target - parentRot;
        // Keep the value continuous with the start rotation (no 360° jumps in keys).
        t.rotation = drag_.startTransform.rotation + wrapDeg(t.rotation - drag_.startTransform.rotation);
        transformBone(sel_.bone, t, true, false, false, false);
        break;
    }
    case Drag::BoneLength: {
        ensureCheckpoint();
        const Affine& w = pose_.world[sel_.bone];
        Vec2 axis = w.applyVector({1, 0});
        float scale = length(axis);
        if (scale > 1e-6f) {
            float along = dot(world - w.origin(), axis * (1.0f / scale));
            sk_.bones[sel_.bone].length = std::max(0.0f, along / scale);
        }
        break;
    }
    case Drag::Attachment: {
        Attachment* att = transformTargetAttachment();
        if (!att) break;
        ensureCheckpoint();
        Transform t = drag_.startTransform;
        applyTool(t);
        att->transform = t;
        break;
    }
    case Drag::Vertices: {
        Attachment* att = selectedAttachment();
        if (!att) break;
        ensureCheckpoint();
        // Moving a path point carries its two handles along.
        std::vector<int> moving = sel_.vertices;
        if (att->type == AttachmentType::Path)
            for (int v : sel_.vertices)
                if (v % 3 == 1)
                    for (int h : {v - 1, v + 1})
                        if (h >= 0 && std::find(moving.begin(), moving.end(), h) == moving.end()) moving.push_back(h);
        if (mode_ == Mode::Animate) {
            // Deform key: offsets in each vertex's own deform space.
            std::vector<Vec2> offsets = drag_.startOffsets;
            for (int v : moving)
                if (v < (int)offsets.size() && v < (int)drag_.deformBases.size())
                    offsets[v] = drag_.startOffsets[v] + drag_.deformBases[v].applyVector(worldDelta);
            if (!currentAnimation()) createAnimation();
            setDeformKey(*currentAnimation(), sel_.slot, attachmentIndex(sel_.slot, att), time_, offsets);
            break;
        }
        Pose setup = makeSetupPose(sk_);
        const Slot& slot = sk_.slots[sel_.slot];
        Affine toLocal = setup.world[slot.bone].inverse();
        bool uvFollows = !meshDeformMode_ && att->type == AttachmentType::Mesh && att->image >= 0 && att->image < (int)sk_.images.size();
        for (int v : moving) {
            if (v >= (int)drag_.startVertices.size()) continue;
            Vec2 target = drag_.startVertices[v] + worldDelta;
            setVertexSetupWorld(sk_, setup, slot, *att, v, target);
            if (uvFollows) setVertexUV(*att, v, toLocal.apply(target), drag_.imageMap, sk_.images[att->image]);
        }
        break;
    }
    case Drag::Paint: {
        Attachment* att = selectedAttachment();
        if (!att || !att->isWeighted()) break;
        ensureCheckpoint();
        const Slot& slot = sk_.slots[sel_.slot];
        std::vector<Vec2> verts = attachmentVertices(sel_.slot, *att);
        float r = brushRadius_ * uiScale_;
        for (size_t v = 0; v < verts.size(); ++v) {
            float d = dist(mouse, toScreen(verts[v]));
            if (d > r) continue;
            float amount = brushStrength_ * (1.0f - d / r) * dt * 2.0f;
            paintWeight(sk_, slot, *att, (int)v, sel_.bone, ctrl ? -amount : amount, lockedBones_);
        }
        break;
    }
    default:
        break;
    }
    updatePose();
}

void Editor::viewportMouseUp(ImVec2 mouse, bool shift) {
    switch (drag_.kind) {
    case Drag::CreateBone: {
        Vec2 a = drag_.startWorld, b = toWorld(mouse);
        float len = drag_.moved ? length(b - a) : 0.0f;
        checkpoint();
        Pose setup = makeSetupPose(sk_);
        int parent = drag_.parentBone >= 0 && drag_.parentBone < (int)sk_.bones.size() ? drag_.parentBone : -1;
        Affine parentWorld = parent >= 0 ? setup.world[parent] : Affine{};
        float angle = len > 0 ? radToDeg(std::atan2(b.y - a.y, b.x - a.x)) : 0.0f;
        Affine world = Affine::fromTRS(a.x, a.y, angle, 1, 1);
        Transform local = decompose(parentWorld.inverse() * world);
        int created = sk_.addBone("bone", parent, local, len);
        // Shift keeps the parent selected so siblings can be created quickly (Spine convention).
        if (!shift) selectBone(created);
        break;
    }
    case Drag::BoxSelect: {
        Attachment* att = selectedAttachment();
        if (!att || !att->hasVertices()) break;
        ImVec2 lo{std::min(mouse.x, drag_.startScreen.x), std::min(mouse.y, drag_.startScreen.y)};
        ImVec2 hi{std::max(mouse.x, drag_.startScreen.x), std::max(mouse.y, drag_.startScreen.y)};
        std::vector<Vec2> verts = attachmentVertices(sel_.slot, *att);
        if (!shift) sel_.vertices.clear();
        for (size_t v = 0; v < verts.size(); ++v) {
            ImVec2 p = toScreen(verts[v]);
            if (p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y &&
                std::find(sel_.vertices.begin(), sel_.vertices.end(), (int)v) == sel_.vertices.end())
                sel_.vertices.push_back((int)v);
        }
        break;
    }
    default:
        break;
    }
    drag_.kind = Drag::None;
}

// ---------------------------------------------------------------- drawing

void Editor::drawSlots(ImDrawList* dl) {
    if (!view_.imagesVisible) return;
    Geometry g, clipped;
    ClipTriangles clip;
    int clipEnd = -2;  // -2: no clipping active
    for (int s : pose_.drawOrder) {
        bool endsHere = clipEnd != -2 && s == clipEnd;
        struct ClipReset {  // stop clipping after the end slot, however this iteration exits
            bool on;
            int& end;
            ClipTriangles& c;
            ~ClipReset() {
                if (on) {
                    end = -2;
                    c.clear();
                }
            }
        } reset{endsHere, clipEnd, clip};
        if (hiddenSlots_.count(s)) continue;
        const Slot& slot = sk_.slots[s];
        const Attachment* resolved = poseAttachment(sk_, pose_, s);
        if (!resolved) continue;
        const Attachment& att = *resolved;
        if (att.isPolygon()) {
            std::vector<Vec2> poly = attachmentVertices(s, att);
            if (att.type == AttachmentType::Clipping) {
                clip = clipTrianglesOf(poly);
                clipEnd = att.endSlot;
            }
            if (att.type == AttachmentType::Path) {
                if (!view_.polygonsVisible) continue;
                bool selected = s == sel_.slot && (sel_.kind == Selection::Slot || sel_.kind == Selection::Attachment);
                std::vector<ImVec2> sp;
                for (Vec2 p : samplePath(buildPathCurve(poly, att.closed))) sp.push_back(toScreen(p));
                if (sp.size() >= 2)
                    dl->AddPolyline(sp.data(), (int)sp.size(), selected ? theme::kSelect : IM_COL32(255, 150, 60, 230),
                                    att.closed ? ImDrawFlags_Closed : 0, (selected ? 2.4f : 1.8f) * uiScale_);
                continue;
            }
            if (view_.polygonsVisible && poly.size() >= 2) {
                bool selected = s == sel_.slot && (sel_.kind == Selection::Slot || sel_.kind == Selection::Attachment);
                std::vector<ImVec2> sp;
                for (Vec2 p : poly) sp.push_back(toScreen(p));
                ImU32 col = att.type == AttachmentType::Clipping ? IM_COL32(200, 110, 255, 220) : IM_COL32(110, 230, 120, 220);
                dl->AddPolyline(sp.data(), (int)sp.size(), selected ? theme::kSelect : col, ImDrawFlags_Closed,
                                (selected ? 2.0f : 1.4f) * uiScale_);
            }
            continue;
        }
        computeGeometry(sk_, pose_, slot, att, g, poseDeform(s, &att));
        if (!clip.empty()) {
            clipGeometry(g, clip, clipped);
            std::swap(g, clipped);
        }
        bool hasTex = att.image >= 0 && att.image < (int)textures_.size() && textures_[att.image].id;
        if (hasTex && !g.triangles.empty()) {
            const Color& c = s < (int)pose_.slotColor.size() ? pose_.slotColor[s] : slot.color;
            ImU32 col = ImGui::ColorConvertFloat4ToU32({c[0], c[1], c[2], c[3]});
            dl->PushTextureID((ImTextureID)(intptr_t)textures_[att.image].id);
            dl->PrimReserve((int)g.triangles.size(), (int)g.positions.size());
            unsigned int base = dl->_VtxCurrentIdx;
            for (size_t v = 0; v < g.positions.size(); ++v)
                dl->PrimWriteVtx(toScreen(g.positions[v]), toIm(g.uvs[v]), col);
            for (int idx : g.triangles) dl->PrimWriteIdx((ImDrawIdx)(base + idx));
            dl->PopTextureID();
        }
        bool selected = s == sel_.slot && (sel_.kind == Selection::Slot || sel_.kind == Selection::Attachment);
        if ((!hasTex || selected) && att.type == AttachmentType::Region && g.positions.size() == 4) {
            ImVec2 p[4];
            for (int i = 0; i < 4; ++i) p[i] = toScreen(g.positions[i]);
            dl->AddPolyline(p, 4, selected ? theme::kSelect : IM_COL32(200, 200, 200, 120), ImDrawFlags_Closed,
                            selected ? 1.5f * uiScale_ : 1.0f);
        }
    }
}

void Editor::drawMeshOverlay(ImDrawList* dl) {
    if (sel_.slot < 0) return;
    Attachment* att = selectedAttachment();
    if (!att || !att->hasVertices()) return;
    bool editing = tool_ == Tool::MeshEdit || tool_ == Tool::Weights;
    if (!editing && !view_.meshVisible) return;
    std::vector<Vec2> verts = attachmentVertices(sel_.slot, *att);
    std::vector<ImVec2> sp(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) sp[i] = toScreen(verts[i]);

    if (tool_ == Tool::Weights && att->isWeighted() && sel_.bone >= 0) {
        ImVec2 white = ImGui::GetIO().Fonts->TexUvWhitePixel;
        dl->PrimReserve((int)att->triangles.size(), (int)sp.size());
        unsigned int base = dl->_VtxCurrentIdx;
        for (size_t v = 0; v < sp.size(); ++v)
            dl->PrimWriteVtx(sp[v], white, weightColor(vertexWeight(*att, (int)v, sel_.bone), 0.65f));
        for (int idx : att->triangles) dl->PrimWriteIdx((ImDrawIdx)(base + idx));
    }

    ImU32 lineCol = editing ? IM_COL32(80, 220, 255, 200) : IM_COL32(80, 220, 255, 80);
    for (size_t t = 0; t + 2 < att->triangles.size(); t += 3) {
        ImVec2 p[3] = {sp[att->triangles[t]], sp[att->triangles[t + 1]], sp[att->triangles[t + 2]]};
        dl->AddPolyline(p, 3, lineCol, ImDrawFlags_Closed, 1.0f);
    }
    if (att->type == AttachmentType::Path) {
        // Path: handle lines from each point to its in/out handles; points drawn as squares.
        for (size_t i = 0; i + 2 < sp.size(); i += 3) {
            dl->AddLine(sp[i], sp[i + 1], IM_COL32(255, 200, 120, 200), 1.2f);
            dl->AddLine(sp[i + 1], sp[i + 2], IM_COL32(255, 200, 120, 200), 1.2f);
        }
        if (!editing) return;
        for (size_t v = 0; v < sp.size(); ++v) {
            bool sel = std::find(sel_.vertices.begin(), sel_.vertices.end(), (int)v) != sel_.vertices.end();
            ImU32 col = sel ? theme::kSelect : IM_COL32(255, 255, 255, 230);
            float r = (v % 3 == 1 ? 4.5f : 3.0f) * uiScale_;
            if (v % 3 == 1) dl->AddRectFilled(sp[v] - ImVec2(r, r), sp[v] + ImVec2(r, r), col);
            else dl->AddCircleFilled(sp[v], r, col);
        }
        return;
    }
    // Outline (hull / polygon) drawn thicker.
    int hull = att->isPolygon() ? (int)sp.size() : att->hullLength;
    if (hull >= 2 && hull <= (int)sp.size())
        dl->AddPolyline(sp.data(), hull, editing ? IM_COL32(255, 255, 255, 210) : IM_COL32(255, 255, 255, 90), ImDrawFlags_Closed,
                        1.8f * uiScale_);
    if (editing && tool_ == Tool::MeshEdit && mode_ == Mode::Setup && ImGui::GetIO().KeyAlt)
        dl->AddText(ImGui::GetIO().MousePos + ImVec2(14, 4), IM_COL32(255, 255, 255, 220), "+ 顶点");
    if (!editing) return;
    for (size_t v = 0; v < sp.size(); ++v) {
        bool sel = std::find(sel_.vertices.begin(), sel_.vertices.end(), (int)v) != sel_.vertices.end();
        dl->AddCircleFilled(sp[v], (sel ? 4.5f : 3.0f) * uiScale_, sel ? theme::kSelect : IM_COL32(255, 255, 255, 230));
    }
}

void Editor::drawBones(ImDrawList* dl) {
    if (!view_.bonesVisible) return;
    std::vector<int> influences;
    if (tool_ == Tool::Weights)
        if (Attachment* att = selectedAttachment()) influences = meshBones(*att);
    const float u = uiScale_;
    for (size_t i = 0; i < sk_.bones.size(); ++i) {
        if (hiddenBones_.count((int)i)) continue;
        if (i < pose_.boneActive.size() && !pose_.boneActive[i]) continue;  // skin bone not in an active skin
        const Bone& b = sk_.bones[i];
        bool selected = ((int)i == sel_.bone && sel_.kind == Selection::Bone) ||
                        std::find(sel_.bones.begin(), sel_.bones.end(), (int)i) != sel_.bones.end();
        bool related = (int)i == sel_.bone && !selected;  // bone of the selected slot
        bool influence = std::find(influences.begin(), influences.end(), (int)i) != influences.end();
        ImU32 col = selected ? theme::kSelect
                    : influence ? IM_COL32(120, 255, 140, 255)
                    : related   ? IM_COL32(150, 225, 245, 255)
                                : theme::kBone;
        ImU32 fill = (col & 0x00FFFFFF) | (selected ? 0xB0000000 : 0x80000000);
        ImVec2 o = toScreen(pose_.world[i].origin());
        float lenPx = b.length * zoom_;
        if (lenPx > 4) {
            ImVec2 t = toScreen(boneTip(b, pose_.world[i]));
            Vec2 dir = toVec(t - o) * (1.0f / std::max(dist(t, o), 1e-3f));
            Vec2 perp{-dir.y, dir.x};
            float w = std::min(lenPx * 0.08f, 6.0f * u);
            Vec2 mid = toVec(o) + dir * std::min(lenPx * 0.12f, 14.0f * u);
            ImVec2 pts[4] = {o, toIm(mid + perp * w), t, toIm(mid - perp * w)};
            dl->AddConvexPolyFilled(pts, 4, fill);
            dl->AddPolyline(pts, 4, theme::kBoneOutline, ImDrawFlags_Closed, 1.0f);
        }
        dl->AddCircleFilled(o, 4.0f * u, theme::kBoneOutline);
        dl->AddCircle(o, 4.0f * u, col, 0, 1.5f * u);
        if (b.parent < 0) {
            dl->AddLine(o + ImVec2(-8 * u, 0), o + ImVec2(8 * u, 0), col, 1.2f);
            dl->AddLine(o + ImVec2(0, -8 * u), o + ImVec2(0, 8 * u), col, 1.2f);
        }
        if (view_.boneNames) {
            ImVec2 at = lenPx > 4 ? toScreen(pose_.world[i].apply({b.length * 0.5f, 0})) : o;
            dl->AddText(at + ImVec2(6 * u, -ImGui::GetFontSize() - 2), IM_COL32(255, 255, 255, 200), b.name.c_str());
        }
        if (selected && overBoneTip(ImGui::GetIO().MousePos))
            dl->AddCircle(toScreen(boneTip(b, pose_.world[i])), 6.0f * u, theme::kAnimEnd, 0, 2.0f);
    }
}

void Editor::drawPoseImages(ImDrawList* dl, const Pose& pose, ImVec4 tint) {
    Geometry g;
    for (int s : pose.drawOrder) {
        if (hiddenSlots_.count(s)) continue;
        const Slot& slot = sk_.slots[s];
        int a = s < (int)pose.slotAttachment.size() ? pose.slotAttachment[s] : -1;
        const Attachment* resolved = poseAttachment(sk_, pose, s);
        if (!resolved) continue;
        const Attachment& att = *resolved;
        if (att.isPolygon() || att.image < 0 || att.image >= (int)textures_.size() || !textures_[att.image].id) continue;
        computeGeometry(sk_, pose, slot, att, g, findDeform(pose, s, a));
        const Color& c = s < (int)pose.slotColor.size() ? pose.slotColor[s] : slot.color;
        ImU32 col = ImGui::ColorConvertFloat4ToU32({c[0] * tint.x, c[1] * tint.y, c[2] * tint.z, c[3] * tint.w});
        dl->PushTextureID((ImTextureID)(intptr_t)textures_[att.image].id);
        dl->PrimReserve((int)g.triangles.size(), (int)g.positions.size());
        unsigned int base = dl->_VtxCurrentIdx;
        for (size_t v = 0; v < g.positions.size(); ++v) dl->PrimWriteVtx(toScreen(g.positions[v]), toIm(g.uvs[v]), col);
        for (int idx : g.triangles) dl->PrimWriteIdx((ImDrawIdx)(base + idx));
        dl->PopTextureID();
    }
}

void Editor::drawGhosts(ImDrawList* dl) {
    Animation* an = currentAnimation();
    if (!ghostOn_ || mode_ != Mode::Animate || !an || !view_.imagesVisible) return;
    float step = std::max(1, ghostStep_) / fps_;
    // Farthest ghosts first so nearer ones draw on top; before = red, after = green.
    for (int side = 0; side < 2; ++side) {
        int count = side == 0 ? ghostBefore_ : ghostAfter_;
        for (int k = count; k >= 1; --k) {
            float t = time_ + (side == 0 ? -k : k) * step;
            if (loop_ && an->duration > 0) t = std::fmod(t + an->duration * 4, an->duration);
            else if (t < 0 || t > an->duration) continue;
            float alpha = ghostAlpha_ * (1.0f - (k - 1) / (float)(count + 1));
            ImVec4 tint = side == 0 ? ImVec4(1.0f, 0.45f, 0.45f, alpha) : ImVec4(0.45f, 1.0f, 0.55f, alpha);
            drawPoseImages(dl, makeAnimatedPose(sk_, an, t, activeSkins_), tint);
        }
    }
}

static void dashed(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float thickness) {
    float len = dist(a, b);
    if (len < 1) return;
    ImVec2 d = (b - a) * (1.0f / len);
    for (float x = 0; x < len; x += 8) {
        float e = std::min(x + 4, len);
        dl->AddLine(a + d * x, a + d * e, col, thickness);
    }
}

void Editor::drawConstraints(ImDrawList* dl) {
    if (!view_.bonesVisible) return;
    const float u = uiScale_;
    for (int i = 0; i < (int)sk_.constraints.size(); ++i) {
        const Constraint& c = sk_.constraints[i];
        bool selected = sel_.kind == Selection::Constraint && sel_.index == i;
        bool related = selected;
        for (int b : c.bones) related |= sel_.bone == b && sel_.kind == Selection::Bone;
        if (c.type != ConstraintType::Path) related |= sel_.bone == c.target && sel_.kind == Selection::Bone;
        switch (c.type) {
        case ConstraintType::Ik: {
            if (c.target < 0 || c.target >= (int)pose_.world.size()) break;
            ImVec2 t = toScreen(pose_.world[c.target].origin());
            ImU32 col = selected ? theme::kSelect : IM_COL32(240, 90, 90, 255);
            dl->AddCircle(t, 9 * u, col, 0, 2.0f);
            dl->AddLine(t - ImVec2(13 * u, 0), t + ImVec2(13 * u, 0), col, 1.5f);
            dl->AddLine(t - ImVec2(0, 13 * u), t + ImVec2(0, 13 * u), col, 1.5f);
            if (related && !c.bones.empty()) {
                int last = c.bones.back();
                dashed(dl, toScreen(boneTip(sk_.bones[last], pose_.world[last])), t, col, 1.5f);
            }
            break;
        }
        case ConstraintType::Transform: {
            if (!related || c.target < 0 || c.target >= (int)pose_.world.size()) break;
            ImVec2 t = toScreen(pose_.world[c.target].origin());
            for (int b : c.bones) dashed(dl, toScreen(pose_.world[b].origin()), t, IM_COL32(240, 200, 80, 230), 1.5f);
            dl->AddRect(t - ImVec2(6 * u, 6 * u), t + ImVec2(6 * u, 6 * u), IM_COL32(240, 200, 80, 255), 0, 0, 2.0f);
            break;
        }
        case ConstraintType::Path: {
            if (!related) break;
            for (int b : c.bones) dl->AddCircle(toScreen(pose_.world[b].origin()), 6 * u, IM_COL32(255, 150, 60, 255), 0, 2.0f);
            break;
        }
        case ConstraintType::Physics: {
            if (c.bones.empty()) break;
            int b = c.bones[0];
            ImVec2 tip = toScreen(boneTip(sk_.bones[b], pose_.world[b]));
            ImU32 col = related ? theme::kSelect : IM_COL32(90, 210, 240, 220);
            ImVec2 pts[7];
            for (int k = 0; k < 7; ++k) pts[k] = tip + ImVec2((k - 3) * 3.0f * u, (k % 2 ? -4.0f : 4.0f) * u + 12 * u);
            dl->AddPolyline(pts, 7, col, 0, 1.6f);
            break;
        }
        }
    }
}

// ---------------------------------------------------------------- window

void Editor::drawViewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool open = ImGui::Begin("视口###Viewport", nullptr,
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::End();
        return;
    }
    canvasOrigin_ = ImGui::GetCursorScreenPos();
    canvasSize_ = ImGui::GetContentRegionAvail();
    canvasSize_.x = std::max(canvasSize_.x, 50.0f);
    canvasSize_.y = std::max(canvasSize_.y, 50.0f);
    ImGui::InvisibleButton("##canvas", canvasSize_,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mouse = io.MousePos;

    // Pan (right/middle drag) & zoom (wheel). A right click without dragging swaps tools.
    if (hovered && io.MouseWheel != 0) {
        Vec2 before = toWorld(mouse);
        zoom_ = std::clamp(zoom_ * std::pow(1.15f, io.MouseWheel), 0.05f, 40.0f);
        ImVec2 after = toScreen(before);
        viewOffset_.x += mouse.x - after.x;
        viewOffset_.y += mouse.y - after.y;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) rightClickStart_ = mouse;
    if (ImGui::IsItemActive() && (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right))) {
        viewOffset_.x += io.MouseDelta.x;
        viewOffset_.y += io.MouseDelta.y;
    }
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && dist(mouse, rightClickStart_) < 4.0f)
        setTool(prevTool_);

    // Tools.
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && pickBone(mouse) < 0 &&
        pickSlot(toWorld(mouse)) < 0 && tool_ != Tool::CreateBone && tool_ != Tool::MeshEdit) {
        sel_.clear();  // double click in empty space deselects
        drag_ = Drag{};
    } else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        viewportMouseDown(mouse, io.KeyShift, io.KeyCtrl);
    }
    if (drag_.kind != Drag::None) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            viewportDrag(mouse, io.KeyShift, io.KeyCtrl, io.DeltaTime);
        else
            viewportMouseUp(mouse, io.KeyShift);
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 lo = canvasOrigin_, hi = canvasOrigin_ + canvasSize_;
    dl->PushClipRect(lo, hi, true);

    // Checkerboard background (tinted in Animate mode).
    bool anim = mode_ == Mode::Animate;
    dl->AddRectFilled(lo, hi, anim ? theme::kCheckerAnimA : theme::kCheckerA);
    float cell = 16.0f * uiScale_;
    ImU32 cb = anim ? theme::kCheckerAnimB : theme::kCheckerB;
    for (int y = 0; lo.y + y * cell < hi.y; ++y)
        for (int x = (y & 1); lo.x + x * cell < hi.x; x += 2)
            dl->AddRectFilled(ImVec2(lo.x + x * cell, lo.y + y * cell), ImVec2(lo.x + (x + 1) * cell, lo.y + (y + 1) * cell), cb);
    ImVec2 origin = toScreen({0, 0});
    dl->AddLine({lo.x, origin.y}, {hi.x, origin.y}, IM_COL32(20, 20, 20, 110));
    dl->AddLine({origin.x, lo.y}, {origin.x, hi.y}, IM_COL32(20, 20, 20, 110));

    drawGhosts(dl);
    drawSlots(dl);
    drawMeshOverlay(dl);
    drawBones(dl);
    drawConstraints(dl);

    if (drag_.kind == Drag::CreateBone) {
        ImVec2 end = drag_.moved ? mouse : drag_.startScreen;
        dl->AddLine(drag_.startScreen, end, theme::kSelect, 2.0f);
        dl->AddCircle(drag_.startScreen, 4.5f * uiScale_, theme::kSelect);
    }
    if (drag_.kind == Drag::BoxSelect && drag_.moved) {
        dl->AddRectFilled(drag_.startScreen, mouse, IM_COL32(64, 214, 255, 30));
        dl->AddRect(drag_.startScreen, mouse, theme::kSelect);
    }
    if (tool_ == Tool::Weights && hovered)
        dl->AddCircle(mouse, brushRadius_ * uiScale_, io.KeyCtrl ? IM_COL32(120, 160, 255, 220) : IM_COL32(255, 120, 120, 220), 48, 1.5f);
    if (anim) dl->AddRect(lo, hi, IM_COL32(90, 200, 160, 120), 0, 0, 2.0f);
    // Events fired during playback flash at the top of the viewport.
    float ey = lo.y + 12;
    for (auto& e : firedEvents_) {
        ImVec2 ts = ImGui::CalcTextSize(e.text.c_str());
        ImVec2 p(lo.x + (canvasSize_.x - ts.x) * 0.5f, ey);
        int alpha = (int)(255 * std::min(1.0f, e.timeLeft / 0.4f));
        dl->AddRectFilled(p - ImVec2(8, 3), p + ts + ImVec2(8, 3), IM_COL32(150, 90, 210, alpha * 3 / 4), 4);
        dl->AddText(p, IM_COL32(255, 255, 255, alpha), e.text.c_str());
        ey += ts.y + 10;
    }
    dl->PopClipRect();

    drawModeLabel();
    drawZoomControls();
    drawMainToolbar();
    ImGui::End();
}
