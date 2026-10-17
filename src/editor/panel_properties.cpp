// Properties of the item selected in the tree, plus the Weights view.
#include "editor/editor.h"

#include "core/mesh_tools.h"
#include "editor/icons.h"
#include "editor/theme.h"
#include "platform/file_dialog.h"

#include <imgui_stdlib.h>

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;
using namespace rig;

// Two-column label/value layout used by every properties section.
static bool beginProps(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.5f);
    ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
    return true;
}
static void propLabel(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

// Header: icon, type, name and the rename / delete buttons on the right.
bool Editor::propertiesHeader(int icon, const std::string& type, const std::string& name, bool canRename, bool canDelete) {
    float fh = ImGui::GetFrameHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddRectFilled(p, p + ImVec2(w, fh + 4), IM_COL32(48, 48, 51, 255), 3);
    ImGui::SetCursorScreenPos(p + ImVec2(4, 2));
    iconInline((Icon)icon);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", type.c_str());
    ImGui::SameLine();
    ImGui::TextUnformatted(name.c_str());
    bool deleted = false;
    float right = p.x + w - 4;
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(right - (fh * 1.15f + 3) * 2, p.y + 2));
    ImGui::BeginDisabled(!canRename);
    if (ImGui::Button("改", ImVec2(fh * 1.15f, fh))) openRename_ = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("重命名 (F2)");
    ImGui::EndDisabled();
    ImGui::SameLine(0, 3);
    ImGui::BeginDisabled(!canDelete);
    if (ImGui::Button("删", ImVec2(fh * 1.15f, fh))) deleted = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("删除 (Del)");
    ImGui::EndDisabled();
    ImGui::SetCursorScreenPos(p + ImVec2(0, fh + 10));
    return deleted;
}

void Editor::drawProperties() {
    if (!ImGui::Begin("属性###Properties")) {
        ImGui::End();
        return;
    }
    switch (sel_.kind) {
    case Selection::Skeleton: propertiesSkeleton(); break;
    case Selection::Bone: propertiesBone(sel_.bone); break;
    case Selection::Slot: propertiesSlot(sel_.slot); break;
    case Selection::Attachment: propertiesAttachment(sel_.slot, sel_.attachment); break;
    case Selection::Animation: propertiesAnimation(sel_.index); break;
    case Selection::Image: propertiesImage(sel_.index); break;
    default:
        ImGui::TextDisabled("未选择");
        ImGui::Spacing();
        ImGui::TextWrapped("1. 把 PNG 拖进窗口导入图片\n"
                           "2. 按 N（建骨骼），从 root 拖出骨骼\n"
                           "3. 选中附件 → 转为网格 → 绑定骨骼 → G 刷权重\n"
                           "4. Tab 进入 Animate 模式，用 C / V / X 摆姿势并打关键帧");
        break;
    }

    // Rename dialog shared by the tree (F2) and the properties header.
    if (openRename_) {
        openRename_ = false;
        switch (sel_.kind) {
        case Selection::Bone: renameBuf_ = sk_.bones[sel_.bone].name; break;
        case Selection::Slot: renameBuf_ = sk_.slots[sel_.slot].name; break;
        case Selection::Attachment: renameBuf_ = sk_.slots[sel_.slot].attachments[sel_.attachment].name; break;
        case Selection::Animation: renameBuf_ = sk_.animations[sel_.index].name; break;
        default: renameBuf_.clear(); break;
        }
        if (!renameBuf_.empty()) ImGui::OpenPopup("重命名");
    }
    if (ImGui::BeginPopupModal("重命名", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
        bool enter = ImGui::InputText("##rename", &renameBuf_, ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter || ImGui::Button("确定")) {
            renameSelection(renameBuf_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
}

void Editor::propertiesSkeleton() {
    propertiesHeader((int)Icon::Skeleton, "骨架", projectPath_.empty() ? "未命名" : fs::u8path(projectPath_).stem().u8string(),
                     false, false);
    if (beginProps("skel")) {
        propLabel("骨骼");
        ImGui::Text("%d", (int)sk_.bones.size());
        propLabel("插槽");
        ImGui::Text("%d", (int)sk_.slots.size());
        propLabel("动画");
        ImGui::Text("%d", (int)sk_.animations.size());
        propLabel("帧率");
        ImGui::DragFloat("##fps", &fps_, 1, 1, 120, "%.0f FPS");
        fps_ = std::clamp(fps_, 1.0f, 120.0f);
        ImGui::EndTable();
    }
    if (ImGui::Button("新建动画")) selectAnimation(createAnimation());
    ImGui::SameLine();
    if (ImGui::Button("导入图片...")) importImageDialog();
}

void Editor::propertiesBone(int b) {
    Bone& bone = sk_.bones[b];
    bool setup = mode_ == Mode::Setup;
    if (propertiesHeader((int)Icon::Bone, "骨骼", bone.name, true, setup && sk_.bones.size() > 1)) {
        deleteSelection();
        return;
    }
    if (beginProps("bone")) {
        propLabel("父骨骼");
        ImGui::AlignTextToFramePadding();
        if (bone.parent >= 0) {
            if (ImGui::SmallButton(sk_.bones[bone.parent].name.c_str())) {
                selectBone(bone.parent);
                ImGui::EndTable();
                return;
            }
        } else {
            ImGui::TextDisabled("(无)");
        }
        propLabel("长度");
        ImGui::BeginDisabled(!setup);
        float len = bone.length;
        bool changed = ImGui::DragFloat("##len", &len, 0.5f, 0, 100000, "%.1f");
        if (ImGui::IsItemActivated()) checkpoint();
        if (changed) bone.length = std::max(0.0f, len);
        ImGui::EndDisabled();
        const Transform& t = setup ? bone.setup : pose_.local[b];
        propLabel(setup ? "Setup" : "当前");
        ImGui::TextDisabled("旋转 %.1f°  位置 %.1f, %.1f  缩放 %.2f, %.2f", t.rotation, t.x, t.y, t.scaleX, t.scaleY);
        ImGui::EndTable();
    }
    ImGui::TextDisabled("变换数值在视口底部的工具栏里编辑。");
    if (setup) {
        if (ImGui::Button("新建子骨骼")) {
            checkpoint();
            Transform t;
            t.x = bone.length;
            selectBone(sk_.addBone("bone", b, t, 50));
            return;
        }
        ImGui::SameLine();
        if (ImGui::Button("新建插槽")) {
            checkpoint();
            Slot s;
            s.name = sk_.uniqueSlotName(bone.name + "_slot");
            s.bone = b;
            sk_.slots.push_back(s);
            selectSlot((int)sk_.slots.size() - 1);
        }
    } else if (currentAnimation()) {
        if (ImGui::Button("为该骨骼打全部关键帧")) keyBone(b, true, true, true);
    }
}

void Editor::propertiesSlot(int s) {
    Slot& slot = sk_.slots[s];
    bool setup = mode_ == Mode::Setup;
    if (propertiesHeader((int)Icon::Slot, "插槽", slot.name, true, true)) {
        deleteSelection();
        return;
    }
    int current = s < (int)pose_.slotAttachment.size() ? pose_.slotAttachment[s] : slot.activeAttachment;
    if (beginProps("slot")) {
        propLabel("骨骼");
        if (ImGui::SmallButton(sk_.bones[slot.bone].name.c_str())) {
            selectBone(slot.bone);
            ImGui::EndTable();
            return;
        }
        propLabel("颜色");
        ImGui::BeginDisabled(!setup);
        float col[4] = {slot.color[0], slot.color[1], slot.color[2], slot.color[3]};
        if (ImGui::ColorEdit4("##col", col, ImGuiColorEditFlags_AlphaBar)) std::copy(col, col + 4, slot.color);
        if (ImGui::IsItemActivated()) checkpoint();
        ImGui::EndDisabled();
        propLabel("附件");
        ImGui::TextUnformatted(current >= 0 && current < (int)slot.attachments.size() ? slot.attachments[current].name.c_str() : "(无)");
        propLabel("绘制顺序");
        ImGui::Text("%d / %d", (int)sk_.slots.size() - s, (int)sk_.slots.size());
        ImGui::SameLine();
        ImGui::BeginDisabled(!setup);
        if (ImGui::ArrowButton("front", ImGuiDir_Up)) moveSlot(s, +1);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("往前（盖住更多）");
        ImGui::SameLine(0, 2);
        if (ImGui::ArrowButton("back", ImGuiDir_Down)) moveSlot(s, -1);
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    ImGui::TextDisabled("在树里点附件左侧的圆点切换显示的附件。");
    if (setup && ImGui::Button("添加图片附件...")) {
        std::string path = openFileDialog("图片 (png/jpg/bmp/tga)", "*.png;*.jpg;*.jpeg;*.bmp;*.tga");
        if (!path.empty()) {
            int before = (int)sk_.slots.size();
            int keepBone = sel_.bone;
            sel_.bone = slot.bone;
            if (importImage(path) && (int)sk_.slots.size() == before + 1) {
                // importImage created a new slot; move its attachment into this one instead.
                Attachment att = sk_.slots.back().attachments[0];
                sk_.slots.pop_back();
                sk_.slots[s].attachments.push_back(att);
                sk_.slots[s].activeAttachment = (int)sk_.slots[s].attachments.size() - 1;
                selectAttachment(s, sk_.slots[s].activeAttachment);
            } else {
                sel_.bone = keepBone;
            }
        }
    }
}

void Editor::propertiesAttachment(int s, int a) {
    Slot& slot = sk_.slots[s];
    Attachment& att = slot.attachments[a];
    bool setup = mode_ == Mode::Setup;
    bool mesh = att.type == AttachmentType::Mesh;
    if (propertiesHeader((int)(mesh ? Icon::Mesh : Icon::Region), mesh ? "网格" : "图片", att.name, true, setup)) {
        deleteSelection();
        return;
    }
    if (beginProps("att")) {
        propLabel("插槽");
        if (ImGui::SmallButton(slot.name.c_str())) {
            selectSlot(s);
            ImGui::EndTable();
            return;
        }
        if (att.image >= 0 && att.image < (int)sk_.images.size()) {
            const Image& im = sk_.images[att.image];
            propLabel("图片");
            ImGui::TextUnformatted(fs::u8path(im.path).filename().u8string().c_str());
            propLabel("尺寸");
            ImGui::Text("%d x %d", im.width, im.height);
        }
        if (mesh) {
            propLabel("顶点");
            ImGui::Text("%d   三角形 %d", (int)att.vertexCount(), (int)att.triangles.size() / 3);
            propLabel("蒙皮");
            ImGui::TextUnformatted(att.isWeighted() ? "已绑定骨骼" : "未绑定");
        }
        ImGui::EndTable();
    }
    if (!setup) {
        ImGui::TextDisabled("附件只能在 Setup 模式下编辑。");
        return;
    }
    if (!mesh) {
        ImGui::TextDisabled("选中后用视口工具栏的旋转/移动/缩放调整位置。");
        ImGui::SeparatorText("转为网格");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        ImGui::InputInt("列", &gridCols_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        ImGui::InputInt("行", &gridRows_);
        gridCols_ = std::clamp(gridCols_, 1, 64);
        gridRows_ = std::clamp(gridRows_, 1, 64);
        if (ImGui::Button("转为网格（按透明区域裁剪）")) {
            const Texture* tex = att.image >= 0 && att.image < (int)textures_.size() ? &textures_[att.image] : nullptr;
            const unsigned char* rgba = tex && !tex->rgba.empty() ? tex->rgba.data() : nullptr;
            Attachment copy = att;
            if (att.image >= 0 && convertRegionToMesh(copy, sk_.images[att.image], rgba, gridCols_, gridRows_)) {
                checkpoint();
                att = std::move(copy);
                setTool(Tool::MeshEdit);
                setStatus("已生成网格：网格工具 (M) 调整顶点，然后在权重视图 (G) 绑定骨骼");
            } else {
                setStatus("生成网格失败：图片为空或完全透明");
            }
        }
        return;
    }
    if (ImGui::Button("编辑网格 (M)")) setTool(Tool::MeshEdit);
    ImGui::SameLine();
    if (ImGui::Button(att.isWeighted() ? "权重 (G)" : "绑定骨骼 (G)")) setTool(Tool::Weights);
    ImGui::TextDisabled("网格工具：拖动顶点，空白处框选，Shift 加选。");
}

void Editor::propertiesAnimation(int i) {
    Animation& an = sk_.animations[i];
    if (propertiesHeader((int)Icon::Animation, "动画", an.name, true, true)) {
        deleteSelection();
        return;
    }
    if (beginProps("anim")) {
        propLabel("时长");
        float dur = an.duration;
        bool changed = ImGui::DragFloat("##dur", &dur, 0.01f, 0.05f, 600.0f, "%.2f 秒");
        if (ImGui::IsItemActivated()) checkpoint();
        if (changed) an.duration = std::max(0.05f, dur);
        propLabel("帧数");
        ImGui::Text("%d 帧 @ %.0f FPS", (int)std::round(an.duration * fps_), fps_);
        propLabel("关键帧");
        ImGui::Text("%d 个时间点", (int)keyTimes(an).size());
        ImGui::EndTable();
    }
    if (anim_ != i && ImGui::Button("设为当前动画")) selectAnimation(i);
}

void Editor::propertiesImage(int i) {
    const Image& im = sk_.images[i];
    if (propertiesHeader((int)Icon::Image, "图片", fs::u8path(im.path).filename().u8string(), false, true)) {
        deleteSelection();
        return;
    }
    int users = 0;
    for (auto& s : sk_.slots)
        for (auto& a : s.attachments) users += a.image == i;
    if (beginProps("img")) {
        propLabel("路径");
        ImGui::TextWrapped("%s", im.path.c_str());
        propLabel("尺寸");
        ImGui::Text("%d x %d", im.width, im.height);
        propLabel("使用");
        ImGui::Text("%d 个附件", users);
        ImGui::EndTable();
    }
    bool missing = i >= (int)textures_.size() || !textures_[i].id;
    if (missing) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "文件找不到或无法读取");
    if (ImGui::Button("重新加载图片")) reloadTextures();
    if (i < (int)textures_.size() && textures_[i].id) {
        float w = ImGui::GetContentRegionAvail().x;
        float scale = std::min(1.0f, w / std::max(1, im.width));
        ImGui::Image((ImTextureID)(intptr_t)textures_[i].id, ImVec2(im.width * scale, im.height * scale));
    }
}

// ---------------------------------------------------------------- weights view

void Editor::drawWeightsView() {
    bool visible = ImGui::Begin("权重###Weights");
    if (focusWeights_ > 0) {
        // Bring the tab to front once it exists (it is created the first time the tool is used).
        ImGui::SetWindowFocus();
        --focusWeights_;
    }
    if (!visible) {
        ImGui::End();
        return;
    }
    Attachment* att = selectedAttachment();
    if (sel_.slot < 0 || !att || att->type != AttachmentType::Mesh) {
        ImGui::TextWrapped("在视口或树中选择一个网格附件。\n图片附件需要先在属性里“转为网格”。");
        ImGui::End();
        return;
    }
    const Slot& slot = sk_.slots[sel_.slot];
    ImGui::Text("网格: %s", att->name.c_str());

    if (!att->isWeighted()) {
        ImGui::SeparatorText("绑定骨骼");
        if (bindBones_.empty()) {
            for (size_t i = 0; i < sk_.bones.size(); ++i)
                if (sk_.isAncestor(slot.bone, (int)i) && sk_.bones[i].length > 0) bindBones_.push_back((int)i);
            if (bindBones_.empty()) bindBones_.push_back(slot.bone);
        }
        ImGui::BeginChild("bindlist", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 8), ImGuiChildFlags_Borders);
        for (size_t i = 0; i < sk_.bones.size(); ++i) {
            bool on = std::find(bindBones_.begin(), bindBones_.end(), (int)i) != bindBones_.end();
            if (ImGui::Checkbox((sk_.bones[i].name + "##bind" + std::to_string(i)).c_str(), &on)) {
                if (on) bindBones_.push_back((int)i);
                else bindBones_.erase(std::remove(bindBones_.begin(), bindBones_.end(), (int)i), bindBones_.end());
            }
        }
        ImGui::EndChild();
        ImGui::BeginDisabled(bindBones_.empty());
        if (ImGui::Button("绑定并自动计算权重", ImVec2(-FLT_MIN, 0))) {
            checkpoint();
            bindMesh(sk_, slot, *att, bindBones_);
            int keepSlot = sel_.slot;
            selectBone(bindBones_.front());
            sel_.slot = keepSlot;
            setStatus("已绑定：选一根骨骼，在网格上涂抹调整权重");
        }
        ImGui::EndDisabled();
        ImGui::End();
        return;
    }

    std::vector<int> bones = meshBones(*att);
    if (sel_.bone >= 0 && std::find(bones.begin(), bones.end(), sel_.bone) == bones.end()) bones.push_back(sel_.bone);
    if (ImGui::BeginTable("weights", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                          ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 8))) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetTextLineHeight());
        ImGui::TableSetupColumn("骨骼", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("平均", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.5f);
        ImGui::TableHeadersRow();
        for (int b : bones) {
            float total = 0;
            for (size_t v = 0; v < att->vertexCount(); ++v) total += vertexWeight(*att, (int)v, b);
            float avg = att->vertexCount() ? total / att->vertexCount() : 0;
            ImGui::TableNextRow();
            ImGui::PushID(b);
            ImGui::TableSetColumnIndex(0);
            ImVec4 c = ImColor::HSV(0.66f * (1 - avg), 0.85f, 0.95f);
            ImGui::ColorButton("##w", c, ImGuiColorEditFlags_NoTooltip, ImVec2(1, 1) * ImGui::GetTextLineHeight());
            ImGui::TableSetColumnIndex(1);
            if (ImGui::Selectable(sk_.bones[b].name.c_str(), sel_.bone == b, ImGuiSelectableFlags_SpanAllColumns)) {
                int keepSlot = sel_.slot;
                selectBone(b);
                sel_.slot = keepSlot;
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%.0f%%", avg * 100);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("在树里选其他骨骼即可给它刷权重（会加入列表）。");
    ImGui::SliderFloat("笔刷半径", &brushRadius_, 5, 200, "%.0f px");
    ImGui::SliderFloat("笔刷强度", &brushStrength_, 0.05f, 2.0f, "%.2f");
    ImGui::TextDisabled("左键涂抹增加，Ctrl+左键减少。");
    if (ImGui::Button("重新自动权重")) {
        checkpoint();
        std::vector<int> mb = meshBones(*att);
        bindMesh(sk_, slot, *att, mb);
    }
    ImGui::SameLine();
    if (ImGui::Button("解除绑定")) {
        checkpoint();
        unbindMesh(sk_, slot, *att);
        bindBones_.clear();
    }
    ImGui::End();
}
