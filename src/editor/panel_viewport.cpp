#include "editor/editor.h"

#include "core/mesh_tools.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

using namespace rig;

static Vec2 toVec(ImVec2 v) { return {v.x, v.y}; }
static ImVec2 toIm(Vec2 v) { return {v.x, v.y}; }
static float dist(ImVec2 a, ImVec2 b) { return length(toVec(a) - toVec(b)); }

static const ImU32 kSelectColor = IM_COL32(255, 170, 40, 255);
static const ImU32 kBoneColor = IM_COL32(170, 200, 255, 255);

static ImU32 weightColor(float w, float alpha) {
    w = std::clamp(w, 0.0f, 1.0f);
    ImColor c = ImColor::HSV(0.66f * (1.0f - w), 0.85f, 0.95f, alpha);
    return (ImU32)c;
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

// ---------------------------------------------------------------- toolbar

void Editor::drawToolbar() {
    auto toggle = [&](const char* label, bool active, const char* tip) {
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        bool clicked = ImGui::Button(label);
        if (active) ImGui::PopStyleColor();
        if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        ImGui::SameLine();
        return clicked;
    };
    if (toggle("Setup", mode_ == Mode::Setup, "编辑骨骼、图片和网格的初始姿态 (Tab)")) {
        mode_ = Mode::Setup;
        playing_ = false;
    }
    if (toggle("Animate", mode_ == Mode::Animate, "摆姿势并打关键帧 (Tab)")) {
        mode_ = Mode::Animate;
        if (tool_ > Tool::Scale) tool_ = Tool::Rotate;
    }
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (toggle("旋转 R", tool_ == Tool::Rotate, "拖动旋转骨骼，Shift 吸附 15°")) tool_ = Tool::Rotate;
    if (toggle("移动 T", tool_ == Tool::Translate, nullptr)) tool_ = Tool::Translate;
    if (toggle("缩放 S", tool_ == Tool::Scale, "Ctrl 只缩放 X")) tool_ = Tool::Scale;
    ImGui::BeginDisabled(mode_ != Mode::Setup);
    if (toggle("建骨骼 B", tool_ == Tool::CreateBone, "从所选骨骼拖出子骨骼；未选骨骼时建根骨骼"))
        tool_ = Tool::CreateBone;
    if (toggle("网格 M", tool_ == Tool::MeshEdit, "选择/拖动网格顶点，空白处框选")) tool_ = Tool::MeshEdit;
    if (toggle("权重 W", tool_ == Tool::Weights, "给所选骨骼刷权重，Ctrl 减少")) tool_ = Tool::Weights;
    ImGui::EndDisabled();
    ImGui::NewLine();
}

// ---------------------------------------------------------------- picking

int Editor::pickBone(ImVec2 screen) const {
    int best = -1;
    float bestD = 7.0f * uiScale_;
    for (int i = (int)sk_.bones.size() - 1; i >= 0; --i) {
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

int Editor::pickSlot(Vec2 world) const {
    for (int s = (int)sk_.slots.size() - 1; s >= 0; --s) {
        const Slot& slot = sk_.slots[s];
        int a = s < (int)pose_.slotAttachment.size() ? pose_.slotAttachment[s] : -1;
        if (a < 0 || a >= (int)slot.attachments.size()) continue;
        const Attachment& att = slot.attachments[a];
        Geometry g;
        computeGeometry(sk_, pose_, slot, att, g);
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
    const Attachment* att = const_cast<Editor*>(this)->selectedAttachment();
    if (!att || att->type != AttachmentType::Mesh) return -1;
    std::vector<Vec2> verts = computeVertices(sk_, pose_, slot, *att);
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

// ---------------------------------------------------------------- input

void Editor::viewportMouseDown(ImVec2 mouse, bool shift) {
    drag_ = Drag{};
    drag_.startScreen = mouse;
    drag_.startWorld = toWorld(mouse);

    auto beginBoneDrag = [&](int b) {
        drag_.kind = Drag::Bone;
        drag_.pivotWorld = pose_.world[b].origin();
        int p = sk_.bones[b].parent;
        drag_.parentWorld = p >= 0 ? pose_.world[p] : Affine{};
        drag_.startTransform = pose_.local[b];
    };

    switch (tool_) {
    case Tool::Rotate:
    case Tool::Translate:
    case Tool::Scale: {
        int b = pickBone(mouse);
        if (b >= 0) {
            sel_.bone = b;
            sel_.slot = -1;
            beginBoneDrag(b);
            return;
        }
        int s = pickSlot(drag_.startWorld);
        if (s < 0) {
            sel_.clear();
            return;
        }
        sel_.slot = s;
        sel_.bone = sk_.slots[s].bone;
        sel_.vertices.clear();
        Attachment* att = selectedAttachment();
        if (mode_ == Mode::Setup) {
            if (att && att->type == AttachmentType::Region) {
                drag_.kind = Drag::Attachment;
                drag_.parentWorld = pose_.world[sel_.bone];
                drag_.startTransform = att->transform;
                drag_.pivotWorld = drag_.parentWorld.apply({att->transform.x, att->transform.y});
            }
        } else {
            beginBoneDrag(sel_.bone);
        }
        return;
    }
    case Tool::CreateBone:
        drag_.kind = Drag::CreateBone;
        drag_.parentBone = sel_.bone;
        return;
    case Tool::MeshEdit: {
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
            drag_.startVertices = computeVertices(sk_, pose_, sk_.slots[sel_.slot], *selectedAttachment());
            return;
        }
        int s = pickSlot(drag_.startWorld);
        if (s >= 0 && s != sel_.slot) {
            sel_.slot = s;
            sel_.bone = sk_.slots[s].bone;
            sel_.vertices.clear();
            Attachment* att = selectedAttachment();
            if (!att || att->type != AttachmentType::Mesh) setStatus("所选附件不是网格：在属性面板点“转为网格”");
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
                sel_.slot = s;
                att = selectedAttachment();
            }
            if (!att || !att->isWeighted()) {
                setStatus("请选择已绑定骨骼的网格（属性面板 → 绑定骨骼）");
                return;
            }
        }
        if (sel_.bone < 0) {
            setStatus("请先在属性面板的权重列表里选一根骨骼");
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
    // Applies the active transform tool to `t` (a copy of the drag-start transform).
    auto applyTool = [&](Transform& t) {
        if (tool_ == Tool::Rotate) {
            t.rotation += rotatedDelta();
            if (shift) t.rotation = std::round(t.rotation / 15.0f) * 15.0f;
        } else if (tool_ == Tool::Translate) {
            Vec2 l = drag_.parentWorld.inverse().applyVector(worldDelta);
            t.x += l.x;
            t.y += l.y;
        } else if (tool_ == Tool::Scale) {
            float f = scaleFactor();
            t.scaleX *= f;
            if (!ctrl) t.scaleY *= f;
        }
    };

    switch (drag_.kind) {
    case Drag::Bone: {
        ensureCheckpoint();
        Transform t = drag_.startTransform;
        applyTool(t);
        setBoneLocal(sel_.bone, t, tool_ == Tool::Rotate, tool_ == Tool::Translate, tool_ == Tool::Scale);
        break;
    }
    case Drag::Attachment: {
        Attachment* att = selectedAttachment();
        if (!att) break;
        ensureCheckpoint();
        Transform t = drag_.startTransform;
        applyTool(t);
        att->transform = t;
        break;
    }
    case Drag::Vertices: {
        Attachment* att = selectedAttachment();
        if (!att || mode_ != Mode::Setup) break;
        ensureCheckpoint();
        Pose setup = makeSetupPose(sk_);
        for (int v : sel_.vertices)
            if (v < (int)drag_.startVertices.size())
                setVertexSetupWorld(sk_, setup, sk_.slots[sel_.slot], *att, v, drag_.startVertices[v] + worldDelta);
        break;
    }
    case Drag::Paint: {
        Attachment* att = selectedAttachment();
        if (!att || !att->isWeighted()) break;
        ensureCheckpoint();
        const Slot& slot = sk_.slots[sel_.slot];
        std::vector<Vec2> verts = computeVertices(sk_, pose_, slot, *att);
        float r = brushRadius_ * uiScale_;
        for (size_t v = 0; v < verts.size(); ++v) {
            float d = dist(mouse, toScreen(verts[v]));
            if (d > r) continue;
            float amount = brushStrength_ * (1.0f - d / r) * dt * 2.0f;
            paintWeight(sk_, slot, *att, (int)v, sel_.bone, ctrl ? -amount : amount);
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
        float len = length(b - a);
        if (!drag_.moved || len * zoom_ < 4) {
            sel_.bone = pickBone(mouse);
            sel_.slot = -1;
            break;
        }
        checkpoint();
        Pose setup = makeSetupPose(sk_);
        int parent = drag_.parentBone >= 0 && drag_.parentBone < (int)sk_.bones.size() ? drag_.parentBone : -1;
        Affine parentWorld = parent >= 0 ? setup.world[parent] : Affine{};
        Affine world = Affine::fromTRS(a.x, a.y, radToDeg(std::atan2(b.y - a.y, b.x - a.x)), 1, 1);
        Transform local = decompose(parentWorld.inverse() * world);
        sel_.bone = sk_.addBone("bone", parent, local, len);
        sel_.slot = -1;
        break;
    }
    case Drag::BoxSelect: {
        Attachment* att = selectedAttachment();
        if (!att || att->type != AttachmentType::Mesh) break;
        ImVec2 lo{std::min(mouse.x, drag_.startScreen.x), std::min(mouse.y, drag_.startScreen.y)};
        ImVec2 hi{std::max(mouse.x, drag_.startScreen.x), std::max(mouse.y, drag_.startScreen.y)};
        std::vector<Vec2> verts = computeVertices(sk_, pose_, sk_.slots[sel_.slot], *att);
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
    Geometry g;
    for (size_t s = 0; s < sk_.slots.size(); ++s) {
        const Slot& slot = sk_.slots[s];
        int a = s < pose_.slotAttachment.size() ? pose_.slotAttachment[s] : -1;
        if (a < 0 || a >= (int)slot.attachments.size()) continue;
        const Attachment& att = slot.attachments[a];
        computeGeometry(sk_, pose_, slot, att, g);
        bool hasTex = att.image >= 0 && att.image < (int)textures_.size() && textures_[att.image].id;
        if (showImages_ && hasTex && !g.triangles.empty()) {
            ImU32 col = ImGui::ColorConvertFloat4ToU32({slot.color[0], slot.color[1], slot.color[2], slot.color[3]});
            dl->PushTextureID((ImTextureID)(intptr_t)textures_[att.image].id);
            dl->PrimReserve((int)g.triangles.size(), (int)g.positions.size());
            unsigned int base = dl->_VtxCurrentIdx;
            for (size_t v = 0; v < g.positions.size(); ++v)
                dl->PrimWriteVtx(toScreen(g.positions[v]), toIm(g.uvs[v]), col);
            for (int idx : g.triangles) dl->PrimWriteIdx((ImDrawIdx)(base + idx));
            dl->PopTextureID();
        }
        bool selected = (int)s == sel_.slot;
        if (!hasTex || selected) {
            // Outline: for regions the 4 corners, for meshes every edge is drawn in the mesh overlay.
            if (att.type == AttachmentType::Region) {
                ImVec2 p[4];
                for (int i = 0; i < 4; ++i) p[i] = toScreen(g.positions[i]);
                dl->AddPolyline(p, 4, selected ? kSelectColor : IM_COL32(200, 200, 200, 120), ImDrawFlags_Closed,
                                selected ? 2.0f : 1.0f);
            }
        }
    }
}

void Editor::drawMeshOverlay(ImDrawList* dl) {
    if (sel_.slot < 0) return;
    Attachment* att = selectedAttachment();
    if (!att || att->type != AttachmentType::Mesh) return;
    bool editing = tool_ == Tool::MeshEdit || tool_ == Tool::Weights;
    if (!editing && !showMesh_) return;
    const Slot& slot = sk_.slots[sel_.slot];
    std::vector<Vec2> verts = computeVertices(sk_, pose_, slot, *att);
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

    ImU32 lineCol = editing ? IM_COL32(80, 220, 255, 200) : IM_COL32(80, 220, 255, 90);
    for (size_t t = 0; t + 2 < att->triangles.size(); t += 3) {
        ImVec2 p[3] = {sp[att->triangles[t]], sp[att->triangles[t + 1]], sp[att->triangles[t + 2]]};
        dl->AddPolyline(p, 3, lineCol, ImDrawFlags_Closed, 1.0f);
    }
    if (!editing) return;
    for (size_t v = 0; v < sp.size(); ++v) {
        bool sel = std::find(sel_.vertices.begin(), sel_.vertices.end(), (int)v) != sel_.vertices.end();
        dl->AddCircleFilled(sp[v], sel ? 4.5f : 3.0f, sel ? kSelectColor : IM_COL32(255, 255, 255, 230));
    }
}

void Editor::drawBones(ImDrawList* dl) {
    if (!showBones_) return;
    for (size_t i = 0; i < sk_.bones.size(); ++i) {
        const Bone& b = sk_.bones[i];
        bool selected = (int)i == sel_.bone;
        bool influences = false;
        if (tool_ == Tool::Weights) {
            if (Attachment* att = selectedAttachment())
                for (int mb : meshBones(*att)) influences |= mb == (int)i;
        }
        ImU32 col = selected ? kSelectColor : (influences ? IM_COL32(120, 255, 140, 255) : kBoneColor);
        ImU32 fill = (col & 0x00FFFFFF) | 0x50000000;
        ImVec2 o = toScreen(pose_.world[i].origin());
        float lenPx = b.length * zoom_;
        if (lenPx > 4) {
            ImVec2 t = toScreen(boneTip(b, pose_.world[i]));
            Vec2 dir = toVec(t - o) * (1.0f / std::max(dist(t, o), 1e-3f));
            Vec2 perp{-dir.y, dir.x};
            float w = std::min(lenPx * 0.12f, 9.0f);
            Vec2 mid = toVec(o) + dir * std::min(lenPx * 0.18f, 20.0f);
            ImVec2 pts[4] = {o, toIm(mid + perp * w), t, toIm(mid - perp * w)};
            dl->AddConvexPolyFilled(pts, 4, fill);
            dl->AddPolyline(pts, 4, col, ImDrawFlags_Closed, selected ? 2.0f : 1.2f);
        }
        dl->AddCircle(o, 4.5f, col, 0, selected ? 2.0f : 1.5f);
        if (b.parent < 0 || lenPx <= 4) {
            dl->AddLine(o + ImVec2(-7, 0), o + ImVec2(7, 0), col);
            dl->AddLine(o + ImVec2(0, -7), o + ImVec2(0, 7), col);
        }
    }
}

// ---------------------------------------------------------------- window

void Editor::drawViewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool open = ImGui::Begin("视口###Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::End();
        return;
    }
    ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(6, 4));
    ImGui::BeginGroup();
    drawToolbar();
    ImGui::EndGroup();

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

    // Pan & zoom.
    if (hovered && io.MouseWheel != 0) {
        Vec2 before = toWorld(mouse);
        zoom_ = std::clamp(zoom_ * std::pow(1.15f, io.MouseWheel), 0.05f, 40.0f);
        ImVec2 after = toScreen(before);
        viewOffset_.x += mouse.x - after.x;
        viewOffset_.y += mouse.y - after.y;
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right))) {
        viewOffset_.x += io.MouseDelta.x;
        viewOffset_.y += io.MouseDelta.y;
    }

    // Tools.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) viewportMouseDown(mouse, io.KeyShift);
    if (drag_.kind != Drag::None) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            viewportDrag(mouse, io.KeyShift, io.KeyCtrl, io.DeltaTime);
        else
            viewportMouseUp(mouse, io.KeyShift);
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 lo = canvasOrigin_, hi = canvasOrigin_ + canvasSize_;
    dl->PushClipRect(lo, hi, true);
    dl->AddRectFilled(lo, hi, mode_ == Mode::Setup ? IM_COL32(38, 40, 46, 255) : IM_COL32(34, 42, 40, 255));

    // Grid.
    float step = 50;
    while (step * zoom_ < 24) step *= 2;
    while (step * zoom_ > 160) step /= 2;
    Vec2 wlo = toWorld({lo.x, hi.y}), whi = toWorld({hi.x, lo.y});
    for (float x = std::floor(wlo.x / step) * step; x <= whi.x; x += step) {
        float sx = toScreen({x, 0}).x;
        dl->AddLine({sx, lo.y}, {sx, hi.y}, IM_COL32(255, 255, 255, 14));
    }
    for (float y = std::floor(wlo.y / step) * step; y <= whi.y; y += step) {
        float sy = toScreen({0, y}).y;
        dl->AddLine({lo.x, sy}, {hi.x, sy}, IM_COL32(255, 255, 255, 14));
    }
    ImVec2 origin = toScreen({0, 0});
    dl->AddLine({lo.x, origin.y}, {hi.x, origin.y}, IM_COL32(220, 80, 80, 90));
    dl->AddLine({origin.x, lo.y}, {origin.x, hi.y}, IM_COL32(80, 220, 80, 90));

    drawSlots(dl);
    drawMeshOverlay(dl);
    drawBones(dl);

    if (drag_.kind == Drag::CreateBone && drag_.moved) {
        dl->AddLine(drag_.startScreen, mouse, kSelectColor, 2.0f);
        dl->AddCircle(drag_.startScreen, 4.5f, kSelectColor);
    }
    if (drag_.kind == Drag::BoxSelect && drag_.moved) {
        dl->AddRectFilled(drag_.startScreen, mouse, IM_COL32(255, 170, 40, 30));
        dl->AddRect(drag_.startScreen, mouse, kSelectColor);
    }
    if (tool_ == Tool::Weights && hovered)
        dl->AddCircle(mouse, brushRadius_ * uiScale_, io.KeyCtrl ? IM_COL32(120, 160, 255, 200) : IM_COL32(255, 120, 120, 200), 48, 1.5f);

    Vec2 mw = toWorld(mouse);
    char info[96];
    std::snprintf(info, sizeof(info), "x %.1f  y %.1f", mw.x, mw.y);
    dl->AddText({lo.x + 8, hi.y - ImGui::GetTextLineHeight() - 6}, IM_COL32(255, 255, 255, 120), info);
    if (mode_ == Mode::Animate)
        dl->AddRect(lo, hi, IM_COL32(90, 200, 140, 160), 0, 0, 2.0f);
    dl->PopClipRect();
    ImGui::End();
}
