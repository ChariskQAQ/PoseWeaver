// Overlays drawn on top of the viewport: mode label, zoom controls and the main toolbar.
#include "editor/editor.h"

#include "editor/icons.h"
#include "editor/theme.h"

#include <algorithm>
#include <cmath>

using namespace rig;

static constexpr ImGuiWindowFlags kOverlayFlags =
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings;

// Floating, rounded group panel over the viewport.
static bool beginGroup(const char* id, ImVec2 pos, float height) {
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kPanelBg);
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(30, 30, 32, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5, 5));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3, 3));
    bool open = ImGui::BeginChild(id, ImVec2(0, height), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_Borders, kOverlayFlags);
    return open;
}

static void endGroup() {
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

static float groupHeight(int rows) {
    float fh = ImGui::GetFrameHeight();
    return rows * fh + (rows - 1) * 3.0f + 10.0f + 2.0f;
}

void Editor::drawModeLabel() {
    ImGui::SetCursorScreenPos(canvasOrigin_ + ImVec2(10, 6));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::BeginChild("##mode", ImVec2(0, 0), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY, kOverlayFlags);
    ImGui::PushFont(theme::largeFont());
    const char* label = mode_ == Mode::Setup ? "SETUP" : "ANIMATE";
    float h = ImGui::GetTextLineHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 textSize = ImGui::CalcTextSize(label);
    bool clicked = ImGui::InvisibleButton("##toggle", ImVec2(h + 6 + textSize.x, h));
    bool hovered = ImGui::IsItemHovered();
    ImU32 col = hovered ? IM_COL32(255, 255, 255, 235) : IM_COL32(255, 255, 255, 150);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    drawIcon(dl, mode_ == Mode::Setup ? Icon::Skeleton : Icon::Animation, p + ImVec2(h * 0.5f, h * 0.5f), h * 0.8f, col);
    dl->AddText(theme::largeFont(), ImGui::GetFontSize(), p + ImVec2(h + 6, 0), col, label);
    ImGui::PopFont();
    if (hovered) ImGui::SetTooltip("点击切换 Setup / Animate 模式 (Tab)");
    if (clicked) setMode(mode_ == Mode::Setup ? Mode::Animate : Mode::Setup);
    if (mode_ == Mode::Animate) {
        Animation* an = currentAnimation();
        ImGui::TextColored(ImVec4(1, 1, 1, 0.7f), "%s   第 %d 帧", an ? an->name.c_str() : "(无动画)",
                           (int)std::round(time_ * fps_));
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void Editor::drawZoomControls() {
    float h = groupHeight(8);  // taller than the toolbar so the vertical slider is usable
    ImVec2 pos(canvasOrigin_.x + 8, canvasOrigin_.y + canvasSize_.y - h - 8);
    if (!beginGroup("##zoom", pos, h)) {
        endGroup();
        return;
    }
    float fh = ImGui::GetFrameHeight();
    float logZoom = std::log2(zoom_);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 8.0f);
    if (ImGui::VSliderFloat("##z", ImVec2(fh * 1.15f, h - 2 * fh - 18), &logZoom, -4.0f, 5.0f, ""))
        zoomTo(std::pow(2.0f, logZoom));
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("缩放 %.0f%%", zoom_ * 100);
    if (iconOnlyButton("100", Icon::Zoom100, false, "缩放到 100% (Ctrl+F)")) zoomTo(1.0f);
    if (iconOnlyButton("fit", Icon::ZoomFit, false, "适配骨架 (F)")) frameAll();
    endGroup();
}

// Numeric value of the transform tools for the current axes.
namespace {
struct ToolValues {
    bool valid = false;
    bool isAttachment = false;
    int bone = -1;
    Transform local;
    Affine parentWorld;
    Affine world;
};
}

void Editor::drawMainToolbar() {
    const float h = groupHeight(5);
    const float fh = ImGui::GetFrameHeight();
    const bool animate = mode_ == Mode::Animate;
    float x = canvasOrigin_.x + 8 + fh * 1.15f + 10 + 10;
    const float y = canvasOrigin_.y + canvasSize_.y - h - 8;
    auto nextX = [&]() { x = ImGui::GetItemRectMax().x + 6; };

    // ---- tools ----
    if (beginGroup("##tools", ImVec2(x, y), h)) {
        float w = ImGui::CalcTextSize("建骨骼 N").x + fh + 16;
        if (iconButton("pose", Icon::Pose, "姿势 B", tool_ == Tool::Pose, w)) setTool(Tool::Pose);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("拖骨骼原点移动，拖骨骼其它位置让它指向鼠标");
        if (iconButton("weights", Icon::Weights, "权重 G", tool_ == Tool::Weights, w, !animate)) setTool(Tool::Weights);
        if (iconButton("create", Icon::Create, "建骨骼 N", tool_ == Tool::CreateBone, w, !animate)) setTool(Tool::CreateBone);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("先选父骨骼，再单击或拖动创建子骨骼；Shift 保持父骨骼选中");
        if (iconButton("mesh", Icon::MeshTool, "网格 M", tool_ == Tool::MeshEdit, w, !animate)) setTool(Tool::MeshEdit);
    }
    endGroup();
    nextX();

    // ---- transform tools with numeric entry ----
    ToolValues tv;
    if (Attachment* att = transformTargetAttachment()) {
        tv.valid = true;
        tv.isAttachment = true;
        tv.local = att->transform;
        tv.parentWorld = pose_.world[sel_.bone];
        tv.world = tv.parentWorld * tv.local.matrix();
    } else if (sel_.bone >= 0 && sel_.bone < (int)sk_.bones.size() && sel_.kind != Selection::None) {
        tv.valid = true;
        tv.bone = sel_.bone;
        tv.local = pose_.local[sel_.bone];
        int p = sk_.bones[sel_.bone].parent;
        tv.parentWorld = p >= 0 ? pose_.world[p] : Affine{};
        tv.world = pose_.world[sel_.bone];
    }
    auto apply = [&](const Transform& t, bool rot, bool trans, bool scale, bool shear = false) {
        if (tv.isAttachment) {
            if (Attachment* att = transformTargetAttachment()) att->transform = t;
            updatePose();
        } else {
            setBoneLocal(tv.bone, t, rot, trans, scale, shear);
        }
    };
    auto keyColor = [&](Channel ch) {
        switch (boneKeyState(tv.bone, ch)) {
        case KeyState::Changed: return theme::kKeyChanged;
        case KeyState::Keyed: return theme::kKeyKeyed;
        default: return theme::kKeyNone;
        }
    };

    if (beginGroup("##transform", ImVec2(x, y), h)) {
        float btnW = ImGui::CalcTextSize("旋转 C").x + fh + 16;
        float fieldW = ImGui::GetFontSize() * 4.6f;
        ImGui::BeginDisabled(!tv.valid);

        auto row = [&](const char* id, Icon icon, const char* label, Tool tool) {
            ImGui::EndDisabled();
            if (iconButton(id, icon, label, tool_ == tool, btnW)) setTool(tool);
            ImGui::BeginDisabled(!tv.valid);
            ImGui::SameLine();
        };
        auto keyCell = [&](const char* id, Channel ch, const char* name) {
            if (!animate || tv.isAttachment || !tv.valid) {
                ImGui::SameLine();
                ImGui::Dummy(ImVec2(fh, fh));
                return;
            }
            ImGui::SameLine();
            KeyState st = boneKeyState(tv.bone, ch);
            std::string tip = std::string(st == KeyState::Keyed ? "当前帧已有" : st == KeyState::Changed ? "有未打帧的" : "为") +
                              name + (st == KeyState::None ? "打关键帧" : st == KeyState::Changed ? "改动，点击打帧" : "关键帧");
            if (keyButton(id, keyColor(ch), tip.c_str()))
                keyBone(tv.bone, ch == Channel::Rotate, ch == Channel::Translate, ch == Channel::Scale, ch == Channel::Shear);
        };

        // Rotate
        row("rot", Icon::Rotate, "旋转 C", Tool::Rotate);
        {
            float parentRot = tv.parentWorld.rotationDeg();
            bool flipped = tv.parentWorld.det() < 0;
            float v = tv.local.rotation;
            if (axes_ == Axes::World) v = std::fmod(tv.world.rotationDeg() + 360.0f, 360.0f);
            ImGui::SetNextItemWidth(fieldW * 2 + 3);
            bool changed = ImGui::DragFloat("##rotv", &v, 0.5f, 0, 0, "%.2f°");
            if (ImGui::IsItemActivated()) checkpoint();
            if (changed && tv.valid) {
                Transform t = tv.local;
                t.rotation = axes_ == Axes::World ? (flipped ? parentRot - v : v - parentRot) : v;
                if (axes_ == Axes::World) t.rotation = tv.local.rotation + wrapDeg(t.rotation - tv.local.rotation);
                apply(t, true, false, false);
            }
        }
        keyCell("krot", Channel::Rotate, "旋转");

        // Translate
        row("trans", Icon::Translate, "移动 V", Tool::Translate);
        {
            Vec2 v{tv.local.x, tv.local.y};
            Affine localRot = Affine::fromTRS(0, 0, tv.local.rotation, 1, 1);
            if (axes_ == Axes::Local) v = localRot.inverse().applyVector(v);
            if (axes_ == Axes::World) v = tv.world.origin();
            float xy[2] = {v.x, v.y};
            ImGui::SetNextItemWidth(fieldW * 2 + 3);
            bool changed = ImGui::DragFloat2("##transv", xy, 0.5f, 0, 0, "%.1f");
            if (ImGui::IsItemActivated()) checkpoint();
            if (changed && tv.valid) {
                Vec2 n{xy[0], xy[1]};
                if (axes_ == Axes::Local) n = localRot.applyVector(n);
                if (axes_ == Axes::World) n = tv.parentWorld.inverse().apply(n);
                Transform t = tv.local;
                t.x = n.x;
                t.y = n.y;
                apply(t, false, true, false);
            }
        }
        keyCell("ktrans", Channel::Translate, "移动");

        // Scale
        row("scale", Icon::Scale, "缩放 X", Tool::Scale);
        {
            float sx = tv.local.scaleX, sy = tv.local.scaleY;
            if (axes_ == Axes::World) {
                Transform w = decompose(tv.world);
                sx = w.scaleX;
                sy = w.scaleY;
            }
            float xy[2] = {sx, sy};
            ImGui::SetNextItemWidth(fieldW * 2 + 3);
            bool changed = ImGui::DragFloat2("##scalev", xy, 0.01f, 0, 0, "%.3f");
            if (ImGui::IsItemActivated()) checkpoint();
            if (changed && tv.valid) {
                Transform t = tv.local;
                // World scale can't be stored directly: apply the change as a ratio to local scale.
                auto ratio = [](float n, float o) { return std::fabs(o) > 1e-6f ? n / o : 1.0f; };
                t.scaleX = tv.local.scaleX * ratio(xy[0], sx);
                t.scaleY = tv.local.scaleY * ratio(xy[1], sy);
                apply(t, false, false, true);
            }
        }
        keyCell("kscale", Channel::Scale, "缩放");

        // Shear
        row("shear", Icon::Shear, "斜切 Z", Tool::Shear);
        {
            float xy[2] = {tv.local.shearX, tv.local.shearY};
            ImGui::SetNextItemWidth(fieldW * 2 + 3);
            bool changed = ImGui::DragFloat2("##shearv", xy, 0.5f, 0, 0, "%.1f°");
            if (ImGui::IsItemActivated()) checkpoint();
            if (changed && tv.valid) {
                Transform t = tv.local;
                t.shearX = xy[0];
                t.shearY = xy[1];
                apply(t, false, false, false, true);
            }
        }
        keyCell("kshear", Channel::Shear, "斜切");

        // Length (Setup mode bones) or bone name, so the 4th row is never empty.
        ImGui::EndDisabled();
        ImGui::Dummy(ImVec2(btnW, fh));
        ImGui::SameLine();
        if (tv.valid && !tv.isAttachment && mode_ == Mode::Setup) {
            float len = sk_.bones[tv.bone].length;
            ImGui::SetNextItemWidth(fieldW * 2 + 3);
            bool changed = ImGui::DragFloat("##len", &len, 0.5f, 0, 100000, "长度 %.1f");
            if (ImGui::IsItemActivated()) checkpoint();
            if (changed) sk_.bones[tv.bone].length = std::max(0.0f, len);
        } else {
            const char* what = !tv.valid ? "未选择" : tv.isAttachment ? sk_.slots[sel_.slot].name.c_str()
                                                                       : sk_.bones[tv.bone].name.c_str();
            ImGui::TextDisabled("%s", what);
        }
    }
    endGroup();
    nextX();

    // ---- axes ----
    if (beginGroup("##axes", ImVec2(x, y), h)) {
        float w = ImGui::CalcTextSize("父级").x + fh + 16;
        if (iconButton("local", Icon::Local, "局部", axes_ == Axes::Local, w)) axes_ = Axes::Local;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("数值相对于自身坐标轴");
        if (iconButton("parent", Icon::Parent, "父级", axes_ == Axes::Parent, w)) axes_ = Axes::Parent;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("数值相对于父骨骼坐标轴（存储方式）");
        if (iconButton("world", Icon::World, "世界", axes_ == Axes::World, w)) axes_ = Axes::World;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("数值为世界坐标");
    }
    endGroup();
    nextX();

    // ---- keying ----
    if (beginGroup("##keying", ImVec2(x, y), h)) {
        float w = ImGui::CalcTextSize("自动关键帧").x + fh + 16;
        if (iconButton("autokey", Icon::AutoKey, "自动关键帧", autoKey_ && animate, w, animate)) autoKey_ = !autoKey_;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("开启：Animate 模式下的改动立即打关键帧\n关闭：改动先显示为橙色，按 K 或点钥匙打帧");
        bool any = !pending_.empty();
        if (iconButton("keyedit", Icon::Key, "打帧改动 K", false, w, animate && any)) keyPending();
        ImGui::BeginDisabled(!animate);
        if (iconButton("keyall", Icon::Key, "全部打帧", false, w, animate && sel_.bone >= 0) && sel_.bone >= 0)
            keyBone(sel_.bone, true, true, true, true);
        ImGui::EndDisabled();
    }
    endGroup();
    nextX();

    // ---- display options ----
    if (beginGroup("##options", ImVec2(x, y), h)) {
        ImGuiTableFlags tf = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX;
        if (ImGui::BeginTable("opts", 4, tf)) {
            float dot = ImGui::GetTextLineHeight();
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("可见", ImGuiTableColumnFlags_WidthFixed, dot * 1.6f);
            ImGui::TableSetupColumn("可选", ImGuiTableColumnFlags_WidthFixed, dot * 1.6f);
            ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthFixed, dot * 1.6f);
            ImGui::TableNextRow(ImGuiTableRowFlags_None, fh);
            const char* heads[] = {"", "显示", "可选", "名称"};
            for (int c = 0; c < 4; ++c) {
                ImGui::TableSetColumnIndex(c);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", heads[c]);
            }
            auto line = [&](const char* name, bool* vis, bool* selb, bool* names) {
                ImGui::TableNextRow(ImGuiTableRowFlags_None, fh);
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(name);
                bool* cells[3] = {vis, selb, names};
                for (int c = 0; c < 3; ++c) {
                    ImGui::TableSetColumnIndex(c + 1);
                    if (!cells[c]) continue;
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (fh - dot) * 0.5f);
                    ImGui::PushID(name);
                    ImGui::PushID(c);
                    if (dotToggle("t", *cells[c])) *cells[c] = !*cells[c];
                    ImGui::PopID();
                    ImGui::PopID();
                }
            };
            line("骨骼", &view_.bonesVisible, &view_.bonesSelectable, &view_.boneNames);
            line("图片", &view_.imagesVisible, &view_.imagesSelectable, nullptr);
            line("网格线", &view_.meshVisible, nullptr, nullptr);
            ImGui::EndTable();
        }
    }
    endGroup();
}
