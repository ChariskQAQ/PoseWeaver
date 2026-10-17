#include "editor/editor.h"

#include "core/mesh_tools.h"
#include "platform/file_dialog.h"

#include <imgui_stdlib.h>

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;
using namespace rig;

// DragFloat that takes an undo snapshot when the user starts dragging/typing.
// Returns true when the value changed this frame.
template <typename Fn>
static bool editFloat(const char* label, float value, float speed, const char* fmt, Fn&& checkpoint, float* out) {
    float v = value;
    bool changed = ImGui::DragFloat(label, &v, speed, 0, 0, fmt);
    if (ImGui::IsItemActivated()) checkpoint();
    if (changed) *out = v;
    return changed;
}

void Editor::drawInspector() {
    if (!ImGui::Begin("属性###Inspector")) {
        ImGui::End();
        return;
    }
    if (sel_.slot >= 0 && sel_.slot < (int)sk_.slots.size()) {
        inspectSlot(sel_.slot);
        if (sel_.bone >= 0 && ImGui::CollapsingHeader(("骨骼: " + sk_.bones[sel_.bone].name).c_str()))
            inspectBone(sel_.bone);
    } else if (sel_.bone >= 0 && sel_.bone < (int)sk_.bones.size()) {
        inspectBone(sel_.bone);
    } else {
        ImGui::TextWrapped("未选择任何对象。\n\n"
                           "开始：\n"
                           "1. 把 PNG 拖进窗口（或 文件 → 导入图片）\n"
                           "2. 按 B，从根骨骼拖出骨骼\n"
                           "3. 选中图片 → 转为网格 → 绑定骨骼\n"
                           "4. 按 Tab 进入 Animate 模式，拖动骨骼打关键帧");
    }
    ImGui::End();
}

void Editor::inspectBone(int b) {
    Bone& bone = sk_.bones[b];
    bool setup = mode_ == Mode::Setup;
    auto cp = [&] { checkpoint(); };

    ImGui::PushID("bone");
    std::string name = bone.name;
    if (ImGui::InputText("名称", &name, ImGuiInputTextFlags_EnterReturnsTrue) && !name.empty() && name != bone.name) {
        checkpoint();
        bone.name = sk_.uniqueBoneName(name);
    }
    ImGui::Text("父骨骼: %s", bone.parent >= 0 ? sk_.bones[bone.parent].name.c_str() : "(无)");

    if (setup) {
        ImGui::SeparatorText("Setup 姿态");
        Transform& t = bone.setup;
        editFloat("X", t.x, 0.5f, "%.2f", cp, &t.x);
        editFloat("Y", t.y, 0.5f, "%.2f", cp, &t.y);
        editFloat("旋转", t.rotation, 0.5f, "%.2f°", cp, &t.rotation);
        editFloat("缩放 X", t.scaleX, 0.01f, "%.3f", cp, &t.scaleX);
        editFloat("缩放 Y", t.scaleY, 0.01f, "%.3f", cp, &t.scaleY);
        float len = bone.length;
        if (editFloat("长度", len, 0.5f, "%.1f", cp, &len)) bone.length = std::max(0.0f, len);
        ImGui::PopID();
        return;
    }

    ImGui::SeparatorText("动画姿态");
    Animation* an = currentAnimation();
    if (!an) {
        ImGui::TextDisabled("先在时间轴面板新建一个动画");
        ImGui::PopID();
        return;
    }
    ImGui::Text("时间 %.3f s（第 %d 帧）", time_, (int)std::round(time_ * fps_));
    Transform t = pose_.local[b];
    auto keyButton = [&](const char* id, Channel ch) {
        bool has = hasKey(*an, b, ch, time_);
        ImGui::PushID(id);
        if (has) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 200, 60, 255));
        bool clicked = ImGui::SmallButton(has ? "◆" : "◇");
        if (has) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(has ? "删除此关键帧" : "在当前时间打关键帧");
        ImGui::PopID();
        if (!clicked) return;
        if (has) {
            checkpoint();
            removeKey(*an, b, ch, time_);
        } else {
            keyBone(b, ch == Channel::Rotate, ch == Channel::Translate, ch == Channel::Scale);
        }
    };
    keyButton("kr", Channel::Rotate);
    ImGui::SameLine();
    if (editFloat("旋转", t.rotation, 0.5f, "%.2f°", cp, &t.rotation)) setBoneLocal(b, t, true, false, false);
    keyButton("kt", Channel::Translate);
    ImGui::SameLine();
    float xy[2] = {t.x, t.y};
    bool changed = ImGui::DragFloat2("位置", xy, 0.5f, 0, 0, "%.2f");
    if (ImGui::IsItemActivated()) checkpoint();
    if (changed) {
        t.x = xy[0];
        t.y = xy[1];
        setBoneLocal(b, t, false, true, false);
    }
    keyButton("ks", Channel::Scale);
    ImGui::SameLine();
    float sc[2] = {t.scaleX, t.scaleY};
    changed = ImGui::DragFloat2("缩放", sc, 0.01f, 0, 0, "%.3f");
    if (ImGui::IsItemActivated()) checkpoint();
    if (changed) {
        t.scaleX = sc[0];
        t.scaleY = sc[1];
        setBoneLocal(b, t, false, false, true);
    }
    if (ImGui::Button("全部打关键帧 (K)")) keyBone(b, true, true, true);

    // Curve of the keys at the current time.
    bool anyKey = hasKey(*an, b, Channel::Rotate, time_) || hasKey(*an, b, Channel::Translate, time_) ||
                  hasKey(*an, b, Channel::Scale, time_);
    if (anyKey) {
        ImGui::SameLine();
        if (ImGui::Button("曲线...")) ImGui::OpenPopup("curve");
        if (ImGui::BeginPopup("curve")) {
            Curve c = Curve::Linear;
            bool pick = false;
            if (ImGui::MenuItem("线性")) pick = true;
            if (ImGui::MenuItem("阶梯（保持到下一帧）")) {
                pick = true;
                c = Curve::Stepped;
            }
            if (pick) {
                checkpoint();
                for (Channel ch : {Channel::Rotate, Channel::Translate, Channel::Scale}) setKeyCurve(*an, b, ch, time_, c);
            }
            ImGui::EndPopup();
        }
    }
    ImGui::PopID();
}

void Editor::inspectSlot(int s) {
    Slot& slot = sk_.slots[s];
    bool setup = mode_ == Mode::Setup;
    ImGui::PushID("slot");
    std::string name = slot.name;
    if (ImGui::InputText("插槽名", &name, ImGuiInputTextFlags_EnterReturnsTrue) && !name.empty() && name != slot.name) {
        checkpoint();
        slot.name = sk_.uniqueSlotName(name);
    }
    ImGui::Text("所属骨骼: %s", sk_.bones[slot.bone].name.c_str());
    ImGui::BeginDisabled(!setup);
    float col[4] = {slot.color[0], slot.color[1], slot.color[2], slot.color[3]};
    if (ImGui::ColorEdit4("颜色", col, ImGuiColorEditFlags_AlphaBar)) std::copy(col, col + 4, slot.color);
    if (ImGui::IsItemActivated()) checkpoint();
    ImGui::EndDisabled();

    ImGui::SeparatorText(setup ? "附件（Setup 显示哪个）" : "附件（切换会打关键帧）");
    int current = s < (int)pose_.slotAttachment.size() ? pose_.slotAttachment[s] : slot.activeAttachment;
    if (ImGui::RadioButton("(无)", current == -1) && current != -1) setSlotAttachment(s, -1);
    for (int a = 0; a < (int)slot.attachments.size(); ++a) {
        ImGui::PushID(a);
        const Attachment& att = slot.attachments[a];
        std::string label = att.name + (att.type == AttachmentType::Mesh ? "  (网格)" : "");
        if (ImGui::RadioButton(label.c_str(), current == a) && current != a) setSlotAttachment(s, a);
        ImGui::PopID();
    }
    if (setup) {
        if (ImGui::Button("+ 图片附件...")) {
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
                    sel_.slot = s;
                } else {
                    sel_.bone = keepBone;
                }
                ImGui::PopID();
                return;  // sk_.slots may have reallocated; `slot` is stale
            }
        }
        if (current >= 0) {
            ImGui::SameLine();
            if (ImGui::Button("删除当前附件")) {
                checkpoint();
                slot.attachments.erase(slot.attachments.begin() + current);
                if (slot.activeAttachment == current) slot.activeAttachment = -1;
                else if (slot.activeAttachment > current) --slot.activeAttachment;
                for (auto& an : sk_.animations) {
                    auto it = an.slots.find(s);
                    if (it == an.slots.end()) continue;
                    for (auto& k : it->second.attachment) {
                        if (k.value == current) k.value = -1;
                        else if (k.value > current) --k.value;
                    }
                }
                sel_.vertices.clear();
                updatePose();
                current = -1;
            }
        }
    }
    if (current >= 0 && current < (int)slot.attachments.size()) inspectAttachment(s, current);
    ImGui::PopID();
}

void Editor::inspectAttachment(int s, int a) {
    Slot& slot = sk_.slots[s];
    Attachment& att = slot.attachments[a];
    bool setup = mode_ == Mode::Setup;
    auto cp = [&] { checkpoint(); };
    ImGui::PushID("att");
    ImGui::SeparatorText(att.type == AttachmentType::Mesh ? "网格附件" : "图片附件");

    std::string name = att.name;
    if (ImGui::InputText("附件名", &name, ImGuiInputTextFlags_EnterReturnsTrue) && !name.empty()) {
        checkpoint();
        att.name = name;
    }
    if (att.image >= 0 && att.image < (int)sk_.images.size()) {
        const Image& im = sk_.images[att.image];
        ImGui::TextDisabled("%s  (%d x %d)", fs::u8path(im.path).filename().u8string().c_str(), im.width, im.height);
    }
    if (!setup) {
        ImGui::TextDisabled("附件的形状只能在 Setup 模式下编辑");
        ImGui::PopID();
        return;
    }

    if (att.type == AttachmentType::Region) {
        Transform& t = att.transform;
        editFloat("X##a", t.x, 0.5f, "%.2f", cp, &t.x);
        editFloat("Y##a", t.y, 0.5f, "%.2f", cp, &t.y);
        editFloat("旋转##a", t.rotation, 0.5f, "%.2f°", cp, &t.rotation);
        editFloat("缩放 X##a", t.scaleX, 0.01f, "%.3f", cp, &t.scaleX);
        editFloat("缩放 Y##a", t.scaleY, 0.01f, "%.3f", cp, &t.scaleY);

        ImGui::SeparatorText("转为网格");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        ImGui::InputInt("列", &gridCols_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
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
                tool_ = Tool::MeshEdit;
                setStatus("已生成网格：可用网格工具 (M) 调整顶点，然后绑定骨骼");
            } else {
                setStatus("生成网格失败：图片为空或完全透明");
            }
        }
        ImGui::PopID();
        return;
    }

    ImGui::Text("顶点 %d   三角形 %d", (int)att.vertexCount(), (int)att.triangles.size() / 3);
    ImGui::TextDisabled("网格工具 (M)：拖动顶点，空白处框选，Shift 加选");

    if (!att.isWeighted()) {
        ImGui::SeparatorText("绑定骨骼");
        if (bindBones_.empty()) {
            for (size_t i = 0; i < sk_.bones.size(); ++i)
                if (sk_.isAncestor(slot.bone, (int)i) && sk_.bones[i].length > 0) bindBones_.push_back((int)i);
            if (bindBones_.empty()) bindBones_.push_back(slot.bone);
        }
        ImGui::BeginChild("bindlist", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 7), ImGuiChildFlags_Borders);
        for (size_t i = 0; i < sk_.bones.size(); ++i) {
            bool on = std::find(bindBones_.begin(), bindBones_.end(), (int)i) != bindBones_.end();
            if (ImGui::Checkbox((sk_.bones[i].name + "##bind" + std::to_string(i)).c_str(), &on)) {
                if (on) bindBones_.push_back((int)i);
                else bindBones_.erase(std::remove(bindBones_.begin(), bindBones_.end(), (int)i), bindBones_.end());
            }
        }
        ImGui::EndChild();
        ImGui::BeginDisabled(bindBones_.empty());
        if (ImGui::Button("绑定并自动计算权重")) {
            checkpoint();
            bindMesh(sk_, slot, att, bindBones_);
            tool_ = Tool::Weights;
            sel_.bone = bindBones_.front();
            setStatus("已绑定：在权重工具 (W) 下给所选骨骼刷权重");
        }
        ImGui::EndDisabled();
        ImGui::PopID();
        return;
    }

    ImGui::SeparatorText("权重");
    ImGui::TextDisabled("选一根骨骼，用权重工具 (W) 在网格上涂抹；Ctrl 减少。\n也可以在层级面板选其他骨骼来添加影响。");
    std::vector<int> bones = meshBones(att);
    if (sel_.bone >= 0 && std::find(bones.begin(), bones.end(), sel_.bone) == bones.end()) bones.push_back(sel_.bone);
    for (int b : bones) {
        float total = 0;
        for (size_t v = 0; v < att.vertexCount(); ++v) total += vertexWeight(att, (int)v, b);
        float avg = att.vertexCount() ? total / att.vertexCount() : 0;
        ImGui::PushID(b);
        ImVec4 c = ImColor::HSV(0.66f * (1 - avg), 0.85f, 0.95f);
        ImGui::ColorButton("##w", c, ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12) * ImGui::GetFontSize() / 13);
        ImGui::SameLine();
        if (ImGui::RadioButton(sk_.bones[b].name.c_str(), sel_.bone == b)) {
            sel_.bone = b;
            tool_ = Tool::Weights;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("平均 %.0f%%", avg * 100);
        ImGui::PopID();
    }
    ImGui::SliderFloat("笔刷半径", &brushRadius_, 5, 200, "%.0f px");
    ImGui::SliderFloat("笔刷强度", &brushStrength_, 0.05f, 2.0f, "%.2f");
    if (ImGui::Button("重新自动权重")) {
        checkpoint();
        std::vector<int> mb = meshBones(att);
        bindMesh(sk_, slot, att, mb);
    }
    ImGui::SameLine();
    if (ImGui::Button("解除绑定")) {
        checkpoint();
        unbindMesh(sk_, slot, att);
        bindBones_.clear();
    }
    ImGui::PopID();
}
