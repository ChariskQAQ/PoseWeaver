#include "editor/editor.h"

#include <functional>

using namespace rig;

// Moves a slot to another bone while keeping its attachments where they are in the setup pose.
static void moveSlotToBone(Skeleton& sk, int slotIndex, int bone) {
    Slot& slot = sk.slots[slotIndex];
    if (slot.bone == bone) return;
    Pose setup = makeSetupPose(sk);
    Affine change = setup.world[bone].inverse() * setup.world[slot.bone];
    for (auto& att : slot.attachments) {
        if (att.type == AttachmentType::Region) {
            att.transform = decompose(change * att.transform.matrix());
        } else if (!att.isWeighted()) {
            for (auto& v : att.vertices) v = change.apply(v);
        }
    }
    slot.bone = bone;
}

void Editor::drawHierarchy() {
    if (!ImGui::Begin("层级###Hierarchy")) {
        ImGui::End();
        return;
    }
    bool setup = mode_ == Mode::Setup;

    ImGui::BeginDisabled(!setup);
    if (ImGui::Button("+ 子骨骼")) {
        checkpoint();
        int parent = sel_.bone;
        Transform t;
        if (parent >= 0) t.x = sk_.bones[parent].length;  // start at the parent's tip
        sel_.bone = sk_.addBone("bone", parent, t, 50);
        sel_.slot = -1;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("在所选骨骼末端添加子骨骼（未选择时添加根骨骼）");
    ImGui::SameLine();
    if (ImGui::Button("+ 插槽") && sel_.bone >= 0) {
        checkpoint();
        Slot s;
        s.name = sk_.uniqueSlotName("slot");
        s.bone = sel_.bone;
        sk_.slots.push_back(s);
        sel_.slot = (int)sk_.slots.size() - 1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("删除")) deleteSelection();
    ImGui::Separator();

    // Drop here to make a bone a root bone.
    ImGui::Selectable("(骨架)", false);
    if (setup && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("BONE")) {
            int b = *(const int*)p->Data;
            checkpoint();
            sk_.reparentBone(b, -1);
        }
        ImGui::EndDragDropTarget();
    }

    std::function<void(int)> drawBone = [&](int b) {
        const Bone& bone = sk_.bones[b];
        std::vector<int> kids = sk_.children(b);
        std::vector<int> slots;
        for (size_t s = 0; s < sk_.slots.size(); ++s)
            if (sk_.slots[s].bone == b) slots.push_back((int)s);

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen |
                                   ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if (kids.empty() && slots.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
        if (sel_.bone == b && sel_.slot < 0) flags |= ImGuiTreeNodeFlags_Selected;
        bool keyed = false;
        if (Animation* an = currentAnimation()) keyed = an->bones.count(b) > 0;
        if (keyed && mode_ == Mode::Animate) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(140, 230, 170, 255));
        bool open = ImGui::TreeNodeEx((void*)(intptr_t)(b + 1), flags, "%s", bone.name.c_str());
        if (keyed && mode_ == Mode::Animate) ImGui::PopStyleColor();
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            sel_.bone = b;
            if (tool_ != Tool::Weights) {  // the weights tool paints the selected bone onto the selected mesh
                sel_.slot = -1;
                sel_.vertices.clear();
            }
        }
        if (setup && ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("BONE", &b, sizeof(int));
            ImGui::Text("骨骼 %s", bone.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (setup && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("BONE")) {
                int dragged = *(const int*)p->Data;
                if (sk_.isAncestor(dragged, b)) {
                    setStatus("不能把骨骼拖到它自己的子骨骼下");
                } else {
                    checkpoint();
                    sk_.reparentBone(dragged, b);
                }
            }
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SLOT")) {
                checkpoint();
                moveSlotToBone(sk_, *(const int*)p->Data, b);
            }
            ImGui::EndDragDropTarget();
        }
        if (!open) return;
        for (int s : slots) {
            const Slot& slot = sk_.slots[s];
            ImGui::PushID(s);
            std::string label = "  [" + slot.name + "]";
            int a = s < (int)pose_.slotAttachment.size() ? pose_.slotAttachment[s] : -1;
            if (a >= 0 && a < (int)slot.attachments.size()) label += "  " + slot.attachments[a].name;
            ImGui::TreeNodeEx("slot", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                          ImGuiTreeNodeFlags_SpanAvailWidth |
                                          (sel_.slot == s ? ImGuiTreeNodeFlags_Selected : 0),
                              "%s", label.c_str());
            if (ImGui::IsItemClicked()) {
                sel_.slot = s;
                sel_.bone = slot.bone;
                sel_.vertices.clear();
            }
            if (setup && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("SLOT", &s, sizeof(int));
                ImGui::Text("插槽 %s", slot.name.c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
        }
        for (int k : kids) drawBone(k);
        ImGui::TreePop();
    };
    for (size_t i = 0; i < sk_.bones.size(); ++i)
        if (sk_.bones[i].parent < 0) drawBone((int)i);

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("绘制顺序（上方在前）", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (int s = (int)sk_.slots.size() - 1; s >= 0; --s) {
            ImGui::PushID(1000000 + s);
            ImGui::BeginDisabled(!setup);
            if (ImGui::ArrowButton("up", ImGuiDir_Up)) moveSlot(s, +1);
            ImGui::SameLine(0, 2);
            if (ImGui::ArrowButton("down", ImGuiDir_Down)) moveSlot(s, -1);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (s < (int)sk_.slots.size() && ImGui::Selectable(sk_.slots[s].name.c_str(), sel_.slot == s)) {
                sel_.slot = s;
                sel_.bone = sk_.slots[s].bone;
                sel_.vertices.clear();
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
}
