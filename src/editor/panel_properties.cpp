// Properties of the item selected in the tree, plus the Weights view.
#include "editor/editor.h"

#include "core/mesh_tools.h"
#include "editor/channels.h"
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
    case Selection::Event: propertiesEvent(sel_.index); break;
    case Selection::Constraint: propertiesConstraint(sel_.index); break;
    case Selection::Skin: propertiesSkin(sel_.index); break;
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
        case Selection::Event: renameBuf_ = sk_.events[sel_.index].name; break;
        case Selection::Constraint: renameBuf_ = sk_.constraints[sel_.index].name; break;
        case Selection::Skin: renameBuf_ = sk_.skins[sel_.index].name; break;
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
    if (ImGui::Button("新建皮肤")) {
        selectSkin(createSkin("skin"));
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("新建事件")) {
        checkpoint();
        EventDef e;
        e.name = sk_.uniqueEventName("event");
        sk_.events.push_back(e);
        selectEvent((int)sk_.events.size() - 1);
        return;
    }
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
        propLabel("皮肤专用");
        ImGui::BeginDisabled(!setup);
        bool req = bone.skinRequired;
        if (ImGui::Checkbox("##skinreq", &req)) {
            checkpoint();
            bone.skinRequired = req;
            updatePose();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("勾选后，只有激活的皮肤包含这根骨骼时它（及其子骨骼、插槽）才生效");
        ImGui::EndDisabled();
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
        if (ImGui::Button("新建约束...")) ImGui::OpenPopup("newcon");
        if (ImGui::BeginPopup("newcon")) {
            if (ImGui::MenuItem("IK 约束")) selectConstraint(createConstraint(ConstraintType::Ik));
            if (ImGui::MenuItem("变换约束")) selectConstraint(createConstraint(ConstraintType::Transform));
            if (ImGui::MenuItem("路径约束")) selectConstraint(createConstraint(ConstraintType::Path));
            if (ImGui::MenuItem("物理约束")) selectConstraint(createConstraint(ConstraintType::Physics));
            ImGui::EndPopup();
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
        {
            // Setup: edits the setup colour. Animate: shows the animated colour and keys edits.
            Color c = s < (int)pose_.slotColor.size() ? pose_.slotColor[s] : slot.color;
            float col[4] = {c[0], c[1], c[2], c[3]};
            if (!setup) {
                KeyState st = slotColorKeyState(s);
                if (keyButton("kcol", st == KeyState::Keyed ? theme::kKeyKeyed : theme::kKeyNone,
                              st == KeyState::Keyed ? "当前帧已有颜色关键帧" : "为当前颜色打关键帧")) {
                    checkpoint();
                    setSlotColor(s, c);
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-FLT_MIN);
            }
            bool changed = ImGui::ColorEdit4("##col", col, ImGuiColorEditFlags_AlphaBar);
            if (ImGui::IsItemActivated()) checkpoint();
            if (changed) setSlotColor(s, Color(col[0], col[1], col[2], col[3]));
        }
        propLabel("附件");
        ImGui::TextUnformatted(current >= 0 && current < (int)slot.attachments.size() ? slot.attachments[current].name.c_str() : "(无)");
        propLabel("绘制顺序");
        int pos = (int)(std::find(pose_.drawOrder.begin(), pose_.drawOrder.end(), s) - pose_.drawOrder.begin());
        ImGui::Text("%d / %d", (int)sk_.slots.size() - pos, (int)sk_.slots.size());
        ImGui::SameLine();
        if (ImGui::ArrowButton("front", ImGuiDir_Up)) moveDrawOrder(s, +1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(setup ? "往前（盖住更多）" : "往前，并打绘制顺序关键帧");
        ImGui::SameLine(0, 2);
        if (ImGui::ArrowButton("back", ImGuiDir_Down)) moveDrawOrder(s, -1);
        ImGui::EndTable();
    }
    ImGui::TextDisabled("在树里点附件左侧的圆点切换显示的附件。");
    if (setup) {
        if (ImGui::Button("+ 剪裁")) {
            createPolygonAttachment(s, AttachmentType::Clipping);
            return;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("新建剪裁插槽：只显示多边形内部的部分");
        ImGui::SameLine();
        if (ImGui::Button("+ 边界框")) {
            createPolygonAttachment(s, AttachmentType::BoundingBox);
            return;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("新建边界框插槽：导出给游戏做碰撞/点击检测");
        ImGui::SameLine();
        if (ImGui::Button("+ 路径")) {
            checkpoint();
            int ps = addPathSlot(slot.bone, {-100, 0}, {100, 0});
            selectAttachment(ps, 0);
            setTool(Tool::MeshEdit);
            setStatus("已添加路径：拖方点/圆点编辑，Alt+点击加点；再给骨骼建路径约束");
            return;
        }
        ImGui::SameLine();
    }
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
    bool setup = mode_ == Mode::Setup;
    if (slot.attachments[a].skinPlaceholder) {
        propertiesPlaceholder(s, a);
        if (!editableAttachment(s, a)) return;
        ImGui::Spacing();
    }
    Attachment& att = *editableAttachment(s, a);
    bool mesh = att.type == AttachmentType::Mesh;
    const char* typeName = mesh ? "网格" : att.type == AttachmentType::Clipping ? "剪裁"
                         : att.type == AttachmentType::BoundingBox  ? "边界框"
                         : att.type == AttachmentType::Path         ? "路径"
                                                                    : "图片";
    Icon icon = mesh ? Icon::Mesh : att.type == AttachmentType::Clipping ? Icon::Clipping
              : att.type == AttachmentType::BoundingBox ? Icon::BoundingBox
              : att.type == AttachmentType::Path        ? Icon::PathC
                                                        : Icon::Region;
    if (slot.attachments[a].skinPlaceholder) {
        ImGui::SeparatorText((std::string("皮肤中的") + typeName + "：" + att.name).c_str());
    } else if (propertiesHeader((int)icon, typeName, att.name, true, setup)) {
        deleteSelection();
        return;
    }
    if (setup && !slot.attachments[a].skinPlaceholder && !sk_.skins.empty()) {
        if (ImGui::Button("移入皮肤...")) ImGui::OpenPopup("toskin");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("把这个附件放进某个皮肤，插槽里留下同名占位；其他皮肤可提供各自的版本");
        if (ImGui::BeginPopup("toskin")) {
            for (int k = 0; k < (int)sk_.skins.size(); ++k)
                if (ImGui::MenuItem(sk_.skins[k].name.c_str())) moveAttachmentToSkin(s, a, k);
            ImGui::EndPopup();
            return;
        }
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
        if (att.hasVertices()) {
            propLabel("顶点");
            if (mesh)
                ImGui::Text("%d（轮廓 %d）  三角形 %d", (int)att.vertexCount(), att.hullLength, (int)att.triangles.size() / 3);
            else
                ImGui::Text("%d", (int)att.vertexCount());
            propLabel("蒙皮");
            ImGui::TextUnformatted(att.isWeighted() ? "已绑定骨骼" : "未绑定");
        }
        if (att.type == AttachmentType::Path) {
            propLabel("闭合");
            bool closed = att.closed;
            ImGui::BeginDisabled(!setup);
            if (ImGui::Checkbox("##closed", &closed)) {
                checkpoint();
                att.closed = closed;
            }
            ImGui::EndDisabled();
            propLabel("点数");
            ImGui::Text("%d", (int)att.vertexCount() / 3);
        }
        if (att.type == AttachmentType::Clipping) {
            propLabel("结束插槽");
            const char* current = att.endSlot >= 0 && att.endSlot < (int)sk_.slots.size() ? sk_.slots[att.endSlot].name.c_str() : "(到最后)";
            ImGui::BeginDisabled(!setup);
            if (ImGui::BeginCombo("##end", current)) {
                if (ImGui::Selectable("(到最后)", att.endSlot < 0)) {
                    checkpoint();
                    att.endSlot = -1;
                }
                // Only slots drawn after the clipping slot can be clipped.
                bool after = false;
                for (int o : pose_.drawOrder) {
                    if (o == s) {
                        after = true;
                        continue;
                    }
                    if (!after) continue;
                    ImGui::PushID(o);
                    if (ImGui::Selectable(sk_.slots[o].name.c_str(), att.endSlot == o)) {
                        checkpoint();
                        att.endSlot = o;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
        }
        ImGui::EndTable();
    }

    // Animate mode: deform keys for vertex attachments.
    if (!setup) {
        if (att.hasVertices()) {
            Animation* an = currentAnimation();
            bool keyed = an && hasKey(*an, s, Channel::Deform, time_, a);
            ImGui::SeparatorText("变形");
            ImGui::TextDisabled("用网格工具 (M) 拖动顶点，会在当前帧打变形关键帧。");
            ImGui::BeginDisabled(!keyed);
            if (ImGui::Button("删除当前帧的变形关键帧") && an) {
                checkpoint();
                removeKey(*an, s, Channel::Deform, time_, a);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("网格工具 (M)")) setTool(Tool::MeshEdit);
        } else {
            ImGui::TextDisabled("附件只能在 Setup 模式下编辑。");
        }
        return;
    }

    if (att.type == AttachmentType::Region) {
        ImGui::TextDisabled("选中后用视口工具栏的旋转/移动/缩放调整位置。");
    }
    if (att.type == AttachmentType::Region || mesh) {
        ImGui::SeparatorText(mesh ? "重新生成网格" : "转为网格");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        ImGui::DragFloat("轮廓容差", &outlineTolerance_, 0.1f, 0.5f, 20.0f, "%.1f px");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        ImGui::DragFloat("外扩", &outlinePadding_, 0.1f, 0.0f, 20.0f, "%.1f px");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        ImGui::SliderInt("内部密度", &meshDensity_, 0, 20);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("长边方向上的内部点数量，0 = 只有轮廓");
        if (ImGui::Button("按图片轮廓自动生成") && autoMesh(s, a)) setTool(Tool::MeshEdit);
        if (att.type == AttachmentType::Region) {
            ImGui::SameLine();
            if (ImGui::Button("规则网格...")) ImGui::OpenPopup("grid");
            if (ImGui::BeginPopup("grid")) {
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
                ImGui::InputInt("列", &gridCols_);
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
                ImGui::InputInt("行", &gridRows_);
                gridCols_ = std::clamp(gridCols_, 1, 64);
                gridRows_ = std::clamp(gridRows_, 1, 64);
                if (ImGui::Button("生成")) {
                    const Texture* tex = att.image >= 0 && att.image < (int)textures_.size() ? &textures_[att.image] : nullptr;
                    const unsigned char* rgba = tex && !tex->rgba.empty() ? tex->rgba.data() : nullptr;
                    Attachment copy = att;
                    if (att.image >= 0 && convertRegionToMesh(copy, sk_.images[att.image], rgba, gridCols_, gridRows_)) {
                        ensureHull(copy);
                        checkpoint();
                        att = std::move(copy);
                        setTool(Tool::MeshEdit);
                    } else {
                        setStatus("生成网格失败：图片为空或完全透明");
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }
    }
    if (!att.hasVertices()) return;

    ImGui::SeparatorText("编辑顶点");
    ImGui::TextDisabled("网格工具 (M)：拖动顶点，Alt+点击加顶点，Del 删除所选，空白处框选。");
    if (ImGui::Button("网格工具 (M)")) setTool(Tool::MeshEdit);
    ImGui::SameLine();
    ImGui::BeginDisabled(sel_.vertices.empty());
    if (ImGui::Button("删除所选顶点")) meshDeleteSelectedVertices();
    ImGui::EndDisabled();
    if (mesh) {
        ImGui::SameLine();
        if (ImGui::Button("重新三角化")) {
            Attachment copy = att;
            if ((copy.hullLength >= 3 || ensureHull(copy)) && retriangulate(sk_, slot, copy)) {
                checkpoint();
                att = std::move(copy);
                afterTopologyChange(s, a);
            } else {
                setStatus("无法三角化：外轮廓自相交或有多个外轮廓");
            }
        }
        ImGui::Checkbox("变形模式（拖动顶点不改变 UV，图片跟着拉伸）", &meshDeformMode_);
    }
    if (ImGui::Button(att.isWeighted() ? "权重 (G)" : "绑定骨骼 (G)")) setTool(Tool::Weights);
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

void Editor::propertiesEvent(int e) {
    EventDef& ev = sk_.events[e];
    if (propertiesHeader((int)Icon::Event, "事件", ev.name, true, true)) {
        deleteSelection();
        return;
    }
    if (beginProps("event")) {
        propLabel("整数");
        int iv = ev.intValue;
        bool ci = ImGui::DragInt("##int", &iv);
        if (ImGui::IsItemActivated()) checkpoint();
        if (ci) ev.intValue = iv;
        propLabel("浮点");
        float fv = ev.floatValue;
        bool cf = ImGui::DragFloat("##float", &fv, 0.05f);
        if (ImGui::IsItemActivated()) checkpoint();
        if (cf) ev.floatValue = fv;
        propLabel("字符串");
        std::string sv = ev.stringValue;
        if (ImGui::InputText("##string", &sv, ImGuiInputTextFlags_EnterReturnsTrue)) {
            checkpoint();
            ev.stringValue = sv;
        }
        propLabel("音频");
        std::string audio = ev.audioPath.empty() ? "(无)" : fs::u8path(ev.audioPath).filename().u8string();
        if (ImGui::SmallButton((audio + "##audio").c_str())) {
            std::string path = openFileDialog("音频 (wav/ogg/mp3)", "*.wav;*.ogg;*.mp3");
            if (!path.empty()) {
                checkpoint();
                ev.audioPath = path;
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("选择音频文件（WAV / MP3 / FLAC）");
        if (!ev.audioPath.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("试听")) audio_->play(ev.audioPath, 0, ev.volume, ev.balance, 1);
            ImGui::SameLine();
            if (ImGui::SmallButton("x##noaudio")) {
                checkpoint();
                ev.audioPath.clear();
            }
            const AudioSamples* smp = ev.audioPath.empty() ? nullptr : audio_->samples(ev.audioPath);
            propLabel("时长");
            if (smp) ImGui::Text("%.2f 秒", smp->duration());
            else ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "无法读取");
            propLabel("音量");
            float vol = ev.volume;
            bool cv = ImGui::SliderFloat("##vol", &vol, 0, 1, "%.2f");
            if (ImGui::IsItemActivated()) checkpoint();
            if (cv) ev.volume = vol;
            propLabel("左右平衡");
            float bal = ev.balance;
            bool cb = ImGui::SliderFloat("##bal", &bal, -1, 1, "%.2f");
            if (ImGui::IsItemActivated()) checkpoint();
            if (cb) ev.balance = bal;
        }
        ImGui::EndTable();
    }
    int uses = 0;
    for (auto& an : sk_.animations)
        for (auto& k : an.events) uses += k.event == e;
    ImGui::TextDisabled("被 %d 个关键帧使用。整数/浮点/字符串是默认值，插入关键帧时复制。", uses);
    if (mode_ == Mode::Animate) {
        if (ImGui::Button("在当前帧插入该事件")) keyEvent(e);
    } else {
        ImGui::TextDisabled("切到 Animate 模式后可在时间轴上插入。");
    }
}

void Editor::propertiesConstraint(int ci) {
    Constraint& c = sk_.constraints[ci];
    bool setup = mode_ == Mode::Setup;
    if (propertiesHeader((int)constraintIcon(c.type), constraintTypeName(c.type), c.name, true, setup)) {
        deleteSelection();
        return;
    }
    auto boneCombo = [&](const char* id, int current, auto&& onPick, bool allowNone) {
        const char* label = current >= 0 && current < (int)sk_.bones.size() ? sk_.bones[current].name.c_str() : "(无)";
        if (!ImGui::BeginCombo(id, label)) return;
        if (allowNone && ImGui::Selectable("(无)", current < 0)) onPick(-1);
        for (int b = 0; b < (int)sk_.bones.size(); ++b) {
            ImGui::PushID(b);
            if (ImGui::Selectable(sk_.bones[b].name.c_str(), b == current)) onPick(b);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    };

    if (beginProps("con")) {
        propLabel("皮肤专用");
        ImGui::BeginDisabled(!setup);
        bool req = c.skinRequired;
        if (ImGui::Checkbox("##skinreq", &req)) {
            checkpoint();
            c.skinRequired = req;
            updatePose();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("勾选后，只有激活的皮肤包含这个约束时它才生效");
        ImGui::EndDisabled();
        propLabel("顺序");
        ImGui::Text("%d / %d", ci + 1, (int)sk_.constraints.size());
        ImGui::SameLine();
        ImGui::BeginDisabled(!setup);
        if (ImGui::ArrowButton("up", ImGuiDir_Up) && ci > 0) {
            checkpoint();
            sk_.swapConstraints(ci, ci - 1);
            selectConstraint(ci - 1);
            ImGui::EndDisabled();
            ImGui::EndTable();
            return;
        }
        ImGui::SameLine(0, 2);
        if (ImGui::ArrowButton("down", ImGuiDir_Down) && ci + 1 < (int)sk_.constraints.size()) {
            checkpoint();
            sk_.swapConstraints(ci, ci + 1);
            selectConstraint(ci + 1);
            ImGui::EndDisabled();
            ImGui::EndTable();
            return;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("约束按顺序依次生效");

        // Target
        if (c.type == ConstraintType::Ik || c.type == ConstraintType::Transform) {
            propLabel("目标骨骼");
            boneCombo("##target", c.target, [&](int b) {
                if (b >= 0 && std::find(c.bones.begin(), c.bones.end(), b) == c.bones.end()) {
                    checkpoint();
                    c.target = b;
                } else if (b >= 0) {
                    setStatus("目标不能是被约束的骨骼");
                }
            }, false);
        } else if (c.type == ConstraintType::Path) {
            propLabel("路径插槽");
            const char* label = c.target >= 0 && c.target < (int)sk_.slots.size() ? sk_.slots[c.target].name.c_str() : "(无)";
            if (ImGui::BeginCombo("##pslot", label)) {
                for (int s = 0; s < (int)sk_.slots.size(); ++s) {
                    bool hasPath = false;
                    for (auto& a : sk_.slots[s].attachments) hasPath |= a.type == AttachmentType::Path;
                    if (!hasPath) continue;
                    ImGui::PushID(s);
                    if (ImGui::Selectable(sk_.slots[s].name.c_str(), s == c.target)) {
                        checkpoint();
                        c.target = s;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
        }
        ImGui::EndDisabled();

        // Constrained bones
        propLabel(c.type == ConstraintType::Physics ? "骨骼" : "约束骨骼");
        ImGui::BeginDisabled(!setup);
        for (size_t i = 0; i < c.bones.size(); ++i) {
            ImGui::PushID((int)i);
            if (ImGui::SmallButton("x") && c.bones.size() > 1) {
                checkpoint();
                c.bones.erase(c.bones.begin() + i);
                ImGui::PopID();
                break;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(sk_.bones[c.bones[i]].name.c_str());
            ImGui::PopID();
        }
        bool canAdd = c.type == ConstraintType::Transform || c.type == ConstraintType::Path ||
                      (c.type == ConstraintType::Ik && c.bones.size() < 2);
        if (canAdd) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            boneCombo("##add", -1, [&](int b) {
                if (b < 0 || b == c.target || std::find(c.bones.begin(), c.bones.end(), b) != c.bones.end()) return;
                checkpoint();
                if (c.type == ConstraintType::Ik) {
                    // Two-bone IK: parent first.
                    if (sk_.bones[b].parent == c.bones[0]) c.bones.push_back(b);
                    else if (sk_.bones[c.bones[0]].parent == b) c.bones.insert(c.bones.begin(), b);
                    else setStatus("两骨骼 IK 需要父子相连的骨骼");
                } else {
                    c.bones.push_back(b);
                }
            }, false);
        }
        ImGui::EndDisabled();

        // Type specific settings (Setup only)
        ImGui::BeginDisabled(!setup);
        auto check = [&](const char* label, bool& v) {
            propLabel(label);
            bool x = v;
            if (ImGui::Checkbox((std::string("##") + label).c_str(), &x)) {
                checkpoint();
                v = x;
            }
        };
        auto drag = [&](const char* label, float& v, float speed, const char* fmt) {
            propLabel(label);
            float x = v;
            bool changed = ImGui::DragFloat((std::string("##") + label).c_str(), &x, speed, 0, 0, fmt);
            if (ImGui::IsItemActivated()) checkpoint();
            if (changed) v = x;
        };
        switch (c.type) {
        case ConstraintType::Ik:
            check("拉伸", c.stretch);
            check("压缩", c.compress);
            check("等比缩放", c.uniform);
            break;
        case ConstraintType::Transform:
            drag("旋转偏移", c.offsetRotation, 0.5f, "%.1f°");
            drag("X 偏移", c.offsetX, 0.5f, "%.1f");
            drag("Y 偏移", c.offsetY, 0.5f, "%.1f");
            drag("缩放 X 偏移", c.offsetScaleX, 0.01f, "%.2f");
            drag("缩放 Y 偏移", c.offsetScaleY, 0.01f, "%.2f");
            drag("斜切偏移", c.offsetShearY, 0.5f, "%.1f°");
            check("局部", c.local);
            check("相对", c.relative);
            break;
        case ConstraintType::Path: {
            auto combo = [&](const char* label, int current, std::initializer_list<const char*> names) {
                propLabel(label);
                int v = current;
                std::vector<const char*> items(names);
                if (ImGui::Combo((std::string("##") + label).c_str(), &v, items.data(), (int)items.size()) && v != current) {
                    checkpoint();
                    return v;
                }
                return current;
            };
            c.positionMode = (PathPositionMode)combo("位置模式", (int)c.positionMode, {"固定长度", "百分比"});
            c.spacingMode = (PathSpacingMode)combo("间距模式", (int)c.spacingMode, {"骨骼长度", "固定", "百分比"});
            c.rotateMode = (PathRotateMode)combo("旋转模式", (int)c.rotateMode, {"切线", "链", "链 + 缩放"});
            drag("旋转偏移", c.offsetRotation, 0.5f, "%.1f°");
            break;
        }
        case ConstraintType::Physics:
            check("X", c.physX);
            check("Y", c.physY);
            check("旋转", c.physRotate);
            drag("惯性", c.inertia, 0.01f, "%.2f");
            drag("强度", c.strength, 1.0f, "%.0f");
            drag("阻尼", c.damping, 0.01f, "%.2f");
            drag("质量", c.mass, 0.01f, "%.2f");
            drag("速度上限", c.limit, 10.0f, "%.0f");
            break;
        }
        ImGui::EndDisabled();
        ImGui::EndTable();
    }

    // Keyable values: Setup edits the setup values; Animate keys them at the current time.
    ImGui::SeparatorText(setup ? "数值（Setup）" : "数值（会打关键帧）");
    ConstraintValues v = ci < (int)pose_.constraints.size() ? pose_.constraints[ci] : c.values;
    if (!setup) {
        Animation* an = currentAnimation();
        bool keyed = an && hasKey(*an, ci, Channel::Constraint, time_);
        if (keyButton("kc", keyed ? theme::kKeyKeyed : theme::kKeyNone, keyed ? "当前帧已有约束关键帧" : "为当前数值打关键帧"))
            keyConstraint(ci);
        ImGui::SameLine();
        ImGui::TextDisabled(keyed ? "当前帧有关键帧" : "当前帧无关键帧");
    }
    if (beginProps("vals")) {
        for (int i = 0; i < constraintValueCount(c.type); ++i) {
            propLabel(constraintValueName(c.type, i));
            ImGui::PushID(i);
            bool changed = false;
            float x = v[i];
            if (c.type == ConstraintType::Ik && i == 2) {
                int bend = x < 0 ? 1 : 0;
                const char* items[] = {"正向", "反向"};
                if (ImGui::Combo("##bend", &bend, items, 2)) {
                    checkpoint();
                    x = bend ? -1.0f : 1.0f;
                    changed = true;
                }
            } else {
                bool isMix = (c.type == ConstraintType::Ik && i == 0) || c.type == ConstraintType::Transform ||
                             (c.type == ConstraintType::Path && i >= 2) || (c.type == ConstraintType::Physics && i == 0);
                if (isMix) changed = ImGui::SliderFloat("##v", &x, 0.0f, 1.0f, "%.2f");
                else changed = ImGui::DragFloat("##v", &x, c.type == ConstraintType::Path && i < 2 && c.positionMode == PathPositionMode::Percent ? 0.005f : 0.5f);
                if (ImGui::IsItemActivated()) checkpoint();
            }
            if (changed) {
                v[i] = x;
                setConstraintValues(ci, v);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (c.type == ConstraintType::Physics) {
        ImGui::TextDisabled(physicsOn_ ? "物理预览已开启：拖动父骨骼或播放动画观察效果。" : "物理预览已关闭（视口工具栏 → 物理预览）。");
        ImGui::BeginDisabled(setup || !currentAnimation());
        if (ImGui::Button("烘焙到当前动画")) bakePhysics(ci);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("把物理模拟逐帧写成旋转/移动关键帧，并在该动画里把物理混合设为 0（Animate 模式）");
        ImGui::EndDisabled();
    }
    if (c.type == ConstraintType::Path) ImGui::TextDisabled("选中路径附件后用网格工具 (M) 编辑：方点是路径点，圆点是手柄，Alt+点击加点。");
}

void Editor::propertiesSkin(int k) {
    Skin& skin = sk_.skins[k];
    bool setup = mode_ == Mode::Setup;
    if (propertiesHeader((int)Icon::Skin, "皮肤", skin.name, true, setup)) {
        deleteSelection();
        return;
    }
    bool active = std::find(activeSkins_.begin(), activeSkins_.end(), k) != activeSkins_.end();
    if (ImGui::Checkbox("激活", &active)) toggleSkin(k);
    ImGui::SameLine();
    if (ImGui::Button("只激活这个")) {
        activeSkins_ = {k};
        physics_.reset();
        updatePose();
    }
    ImGui::SameLine();
    if (setup && ImGui::Button("复制皮肤")) {
        checkpoint();
        Skin copy = skin;
        copy.name = sk_.uniqueSkinName(skin.name + "_copy");
        sk_.skins.push_back(copy);
        selectSkin((int)sk_.skins.size() - 1);
        return;
    }
    ImGui::TextDisabled("可同时激活多个皮肤组合使用，同一占位由后激活的皮肤提供。");

    // Attachments
    ImGui::SeparatorText("附件");
    if (skin.attachments.empty()) ImGui::TextDisabled("还没有附件：选中插槽里的附件 → “移入皮肤...”。");
    for (size_t i = 0; i < skin.attachments.size(); ++i) {
        const SkinEntry& e = skin.attachments[i];
        if (e.slot < 0 || e.slot >= (int)sk_.slots.size()) continue;
        ImGui::PushID((int)i);
        std::string label = sk_.slots[e.slot].name + " / " + e.placeholder + "  →  " + e.attachment.name;
        if (ImGui::Selectable(label.c_str())) {
            if (!active) toggleSkin(k);
            const auto& list = sk_.slots[e.slot].attachments;
            for (size_t a = 0; a < list.size(); ++a)
                if (list[a].skinPlaceholder && list[a].name == e.placeholder) selectAttachment(e.slot, (int)a);
            ImGui::PopID();
            return;
        }
        ImGui::PopID();
    }

    // Bones and constraints this skin activates
    auto memberList = [&](const char* title, std::vector<int>& members, int count, auto&& nameOf, auto&& markRequired) {
        ImGui::SeparatorText(title);
        for (size_t i = 0; i < members.size(); ++i) {
            ImGui::PushID((int)i);
            ImGui::BeginDisabled(!setup);
            if (ImGui::SmallButton("x")) {
                checkpoint();
                members.erase(members.begin() + i);
                ImGui::EndDisabled();
                ImGui::PopID();
                updatePose();
                return;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextUnformatted(nameOf(members[i]).c_str());
            ImGui::PopID();
        }
        ImGui::BeginDisabled(!setup);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo((std::string("##add") + title).c_str(), "+ 添加")) {
            for (int i = 0; i < count; ++i) {
                if (std::find(members.begin(), members.end(), i) != members.end()) continue;
                ImGui::PushID(i);
                if (ImGui::Selectable(nameOf(i).c_str())) {
                    checkpoint();
                    members.push_back(i);
                    markRequired(i);  // adding to a skin only matters for skin-required items
                    updatePose();
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
    };
    memberList("皮肤骨骼", skin.bones, (int)sk_.bones.size(), [&](int b) { return sk_.bones[b].name; },
               [&](int b) { sk_.bones[b].skinRequired = true; });
    memberList("皮肤约束", skin.constraints, (int)sk_.constraints.size(), [&](int c) { return sk_.constraints[c].name; },
               [&](int c) { sk_.constraints[c].skinRequired = true; });
}

void Editor::propertiesPlaceholder(int s, int a) {
    Slot& slot = sk_.slots[s];
    Attachment& ph = slot.attachments[a];
    bool setup = mode_ == Mode::Setup;
    if (propertiesHeader((int)Icon::Skin, "皮肤占位", ph.name, true, setup)) {
        deleteSelection();
        return;
    }
    ImGui::TextDisabled("插槽 %s 里的占位，实际内容来自激活的皮肤。", slot.name.c_str());
    if (!ImGui::BeginTable("phskins", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("皮肤", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("附件", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9);
    ImGui::TableHeadersRow();
    const Attachment* current = editableAttachment(s, a);
    for (int k = 0; k < (int)sk_.skins.size(); ++k) {
        Skin& skin = sk_.skins[k];
        Attachment* entry = skin.find(s, ph.name);
        ImGui::PushID(k);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        bool active = std::find(activeSkins_.begin(), activeSkins_.end(), k) != activeSkins_.end();
        ImGui::TextColored(active ? ImVec4(1, 1, 1, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "%s", skin.name.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(entry ? entry->name.c_str() : "(无)");
        ImGui::TableSetColumnIndex(2);
        if (entry && ImGui::SmallButton("编辑")) {
            activeSkins_ = {k};
            updatePose();
        }
        if (setup) {
            ImGui::SameLine();
            if (ImGui::SmallButton("图片...")) {
                std::string path = openFileDialog("图片 (png/jpg/bmp/tga)", "*.png;*.jpg;*.jpeg;*.bmp;*.tga");
                if (!path.empty() && importImage(path)) {
                    // importImage made a new slot with a region; take its attachment for this skin.
                    Attachment img = sk_.slots.back().attachments[0];
                    sk_.slots.pop_back();
                    if (current && current->type == AttachmentType::Region) img.transform = current->transform;
                    Skin& target = sk_.skins[k];
                    if (Attachment* existing = target.find(s, ph.name)) *existing = img;
                    else target.attachments.push_back({s, ph.name, img});
                    activeSkins_ = {k};
                    selectAttachment(s, a);
                    updatePose();
                    ImGui::PopID();
                    ImGui::EndTable();
                    return;
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("用一张图片作为这个皮肤的附件（位置沿用当前显示的附件）");
            if (current && current != entry) {
                ImGui::SameLine();
                if (ImGui::SmallButton("复制")) {
                    checkpoint();
                    Attachment copy = *current;
                    if (entry) *entry = copy;
                    else skin.attachments.push_back({s, ph.name, copy});
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("把当前显示的附件复制到这个皮肤（之后可换图或改网格）");
            }
            if (entry) {
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    checkpoint();
                    skin.attachments.erase(std::remove_if(skin.attachments.begin(), skin.attachments.end(),
                                                          [&](const SkinEntry& e) { return e.slot == s && e.placeholder == ph.name; }),
                                           skin.attachments.end());
                    updatePose();
                }
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (!current) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "激活的皮肤都没有提供这个附件，插槽什么都不显示。");
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
    if (sel_.slot < 0 || !att || !att->hasVertices()) {
        ImGui::TextWrapped("在视口或树中选择一个网格（或剪裁/边界框）附件。\n图片附件需要先在属性里“自动生成网格”。");
        ImGui::End();
        return;
    }
    const Slot& slot = sk_.slots[sel_.slot];
    ImGui::Text("附件: %s", att->name.c_str());

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
    auto isLocked = [&](int b) { return std::find(lockedBones_.begin(), lockedBones_.end(), b) != lockedBones_.end(); };
    const std::vector<int>& selVerts = sel_.vertices;
    if (ImGui::BeginTable("weights", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                          ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 8))) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetTextLineHeight());
        ImGui::TableSetupColumn("骨骼", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(selVerts.empty() ? "平均" : "所选", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.5f);
        ImGui::TableSetupColumn("锁", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableHeadersRow();
        for (int b : bones) {
            // Average over the selected vertices (or all of them).
            float total = 0;
            int count = 0;
            if (selVerts.empty()) {
                for (size_t v = 0; v < att->vertexCount(); ++v, ++count) total += vertexWeight(*att, (int)v, b);
            } else {
                for (int v : selVerts) {
                    total += vertexWeight(*att, v, b);
                    ++count;
                }
            }
            float avg = count ? total / count : 0;
            ImGui::TableNextRow();
            ImGui::PushID(b);
            ImGui::TableSetColumnIndex(0);
            ImVec4 c = ImColor::HSV(0.66f * (1 - avg), 0.85f, 0.95f);
            ImGui::ColorButton("##w", c, ImGuiColorEditFlags_NoTooltip, ImVec2(1, 1) * ImGui::GetTextLineHeight());
            ImGui::TableSetColumnIndex(1);
            if (ImGui::Selectable(sk_.bones[b].name.c_str(), sel_.bone == b)) {
                int keepSlot = sel_.slot;
                auto keepVerts = sel_.vertices;
                selectBone(b);
                sel_.slot = keepSlot;
                sel_.vertices = keepVerts;
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%.0f%%", avg * 100);
            ImGui::TableSetColumnIndex(3);
            bool locked = isLocked(b);
            if (ImGui::Checkbox("##lock", &locked)) {
                if (locked) lockedBones_.push_back(b);
                else lockedBones_.erase(std::remove(lockedBones_.begin(), lockedBones_.end(), b), lockedBones_.end());
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("锁定后，涂抹/平滑/设置都不会改变这根骨骼的权重");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("在树里选其他骨骼即可给它刷权重（会加入列表）。");

    ImGui::SeparatorText("笔刷 (G)");
    ImGui::SliderFloat("半径", &brushRadius_, 5, 200, "%.0f px");
    ImGui::SliderFloat("强度", &brushStrength_, 0.05f, 2.0f, "%.2f");
    ImGui::TextDisabled("左键涂抹增加，Ctrl+左键减少。");

    ImGui::SeparatorText(selVerts.empty() ? "数值（先用网格工具 M 选顶点）" : "数值");
    ImGui::BeginDisabled(selVerts.empty() || sel_.bone < 0);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    ImGui::SliderFloat("##value", &setWeightValue_, 0, 1, "%.2f");
    ImGui::SameLine();
    if (ImGui::Button("设为所选骨骼的权重")) {
        checkpoint();
        for (int v : selVerts) setWeight(sk_, slot, *att, v, sel_.bone, setWeightValue_, lockedBones_);
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("整理");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    ImGui::SliderFloat("平滑程度", &smoothAmount_, 0.05f, 1.0f, "%.2f");
    ImGui::SameLine();
    if (ImGui::Button(selVerts.empty() ? "平滑全部" : "平滑所选")) {
        checkpoint();
        smoothWeights(sk_, slot, *att, selVerts, smoothAmount_, lockedBones_);
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
    ImGui::DragFloat("##thr", &pruneThreshold_, 0.005f, 0.0f, 0.5f, "< %.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
    ImGui::SliderInt("##max", &pruneMax_, 1, 8, "最多 %d");
    ImGui::SameLine();
    if (ImGui::Button("修剪")) {
        checkpoint();
        int removed = pruneWeights(*att, pruneThreshold_, pruneMax_);
        setStatus("已移除 " + std::to_string(removed) + " 个影响");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("去掉低于阈值的影响，并限制每个顶点的骨骼数");

    ImGui::SeparatorText("镜像");
    const char* axes[] = {"网格中心", "所选骨骼原点", "世界 x = 0"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    ImGui::Combo("对称轴", &mirrorAxis_, axes, 3);
    int dir = mirrorLeftToRight_ ? 0 : 1;
    const char* dirs[] = {"左 → 右", "右 → 左"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    if (ImGui::Combo("方向", &dir, dirs, 2)) mirrorLeftToRight_ = dir == 0;
    if (ImGui::Button("镜像权重")) mirrorSelectedWeights();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("按骨骼名字配对左右骨骼（_l/_r、left/right、L_/R_、左/右），把一侧的权重复制到另一侧");

    ImGui::Spacing();
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
