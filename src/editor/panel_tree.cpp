// Tree view: skeleton → bone hierarchy (with slots and attachments), draw order, animations, images.
#include "editor/editor.h"

#include "core/mesh_tools.h"
#include "editor/channels.h"
#include "editor/icons.h"
#include "editor/theme.h"
#include "platform/file_dialog.h"

#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>

namespace fs = std::filesystem;
using namespace rig;

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

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

namespace {
// One row of the tree table: [visibility][key][tree node with icon].
struct RowResult {
    bool open = false;
    bool clicked = false;
    bool doubleClicked = false;
    bool rightClicked = false;
};

RowResult treeRow(const void* id, Icon icon, const std::string& label, bool selected, bool leaf, bool defaultOpen,
                  ImU32 textColor = 0, ImU32 iconCol = 0) {
    RowResult r;
    ImGui::TableSetColumnIndex(2);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_NavLeftJumpsBackHere;
    if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (defaultOpen) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    if (selected) flags |= ImGuiTreeNodeFlags_Selected;
    // Leave room for the icon in front of the label.
    float h = ImGui::GetTextLineHeight();
    float space = ImGui::CalcTextSize(" ").x;
    std::string padded(size_t(std::ceil((h + 4) / std::max(space, 1.0f))), ' ');
    padded += label;
    if (textColor) ImGui::PushStyleColor(ImGuiCol_Text, textColor);
    r.open = ImGui::TreeNodeEx(id, flags, "%s", padded.c_str());
    if (textColor) ImGui::PopStyleColor();
    r.clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen();
    r.doubleClicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    r.rightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    ImVec2 min = ImGui::GetItemRectMin();
    float x = min.x + ImGui::GetTreeNodeToLabelSpacing();
    drawIcon(ImGui::GetWindowDrawList(), icon, ImVec2(x + h * 0.5f, min.y + ImGui::GetItemRectSize().y * 0.5f), h * 0.85f,
             iconCol ? iconCol : iconColor(icon));
    if (leaf) r.open = false;
    return r;
}
} // namespace

void Editor::drawTree() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    bool visible = ImGui::Begin("树###Tree");
    ImGui::PopStyleVar();
    if (!visible) {
        ImGui::End();
        return;
    }
    const bool setup = mode_ == Mode::Setup;
    const bool animate = !setup;
    Animation* an = currentAnimation();

    // ---- search & filters ----
    float fh = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (fh * 1.15f + 3) * 4);
    ImGui::InputTextWithHint("##search", "搜索名称", &treeFilter_);
    ImGui::SameLine(0, 3);
    if (iconOnlyButton("fb", Icon::Bone, treeShowBones_, "显示骨骼")) treeShowBones_ = !treeShowBones_;
    ImGui::SameLine(0, 3);
    if (iconOnlyButton("fs", Icon::Slot, treeShowSlots_, "显示插槽")) treeShowSlots_ = !treeShowSlots_;
    ImGui::SameLine(0, 3);
    if (iconOnlyButton("fa", Icon::Region, treeShowAttachments_, "显示附件")) treeShowAttachments_ = !treeShowAttachments_;
    ImGui::SameLine(0, 3);
    if (iconOnlyButton("imp", Icon::Image, false, "导入图片 (Ctrl+I)")) importImageDialog();
    const std::string filter = lower(treeFilter_);
    auto matches = [&](const std::string& name) { return filter.empty() || lower(name).find(filter) != std::string::npos; };

    // A bone is shown if it, a descendant bone, or one of their slots/attachments matches the filter.
    std::vector<int> boneVisible(sk_.bones.size(), -1);
    std::function<bool(int)> boneMatches = [&](int b) -> bool {
        if (boneVisible[b] >= 0) return boneVisible[b] != 0;
        bool m = matches(sk_.bones[b].name);
        for (size_t s = 0; !m && s < sk_.slots.size(); ++s) {
            if (sk_.slots[s].bone != b) continue;
            m = matches(sk_.slots[s].name);
            for (auto& a : sk_.slots[s].attachments) m = m || matches(a.name);
        }
        for (int c : sk_.children(b)) m = boneMatches(c) || m;
        boneVisible[b] = m ? 1 : 0;
        return m;
    };

    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                         ImGuiTableFlags_NoPadOuterX;
    if (!ImGui::BeginTable("tree", 3, tf)) {
        ImGui::End();
        return;
    }
    float dotW = ImGui::GetTextLineHeight() * 1.2f;
    // Tree indentation must apply to the name column, not the visibility/key columns.
    ImGui::TableSetupColumn("vis", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_IndentDisable, dotW);
    ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_IndentDisable, animate ? fh : 1.0f);
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_IndentEnable);
    ImGui::TableSetupScrollFreeze(0, 0);

    auto visCell = [&](const char* id, bool on, const char* tip) {
        ImGui::TableSetColumnIndex(0);
        return dotToggle(id, on, tip);
    };
    auto keyCell = [&](const char* id, KeyState st, const char* tip) {
        if (!animate) return false;
        ImGui::TableSetColumnIndex(1);
        ImU32 col = st == KeyState::Changed ? theme::kKeyChanged : st == KeyState::Keyed ? theme::kKeyKeyed : IM_COL32(110, 110, 110, 255);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        float h = ImGui::GetTextLineHeight();
        ImGui::PushID(id);
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::InvisibleButton("##k", ImVec2(h, h));
        drawIcon(ImGui::GetWindowDrawList(), Icon::Key, p + ImVec2(h * 0.5f, h * 0.5f), h * 0.75f, col);
        if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        ImGui::PopID();
        ImGui::PopStyleVar();
        return clicked;
    };
    auto scrollIfSelected = [&](bool selected) {
        if (selected && scrollTreeToSelection_) {
            ImGui::SetScrollHereY(0.4f);
            scrollTreeToSelection_ = false;
        }
    };
    // Open the ancestors of the selection so it is visible in the tree.
    std::vector<bool> forceOpen(sk_.bones.size(), false);
    if (scrollTreeToSelection_ && sel_.bone >= 0)
        for (int b = sk_.bones[sel_.bone].parent; b >= 0; b = sk_.bones[b].parent) forceOpen[b] = true;

    bool deferDelete = false;  // deleting while iterating the tree would invalidate indices

    // ---- skeleton root ----
    ImGui::TableNextRow();
    RowResult root = treeRow("skeleton", Icon::Skeleton, "骨架", sel_.kind == Selection::Skeleton, false, true);
    if (root.clicked) {
        sel_.clear();
        sel_.kind = Selection::Skeleton;
    }
    if (!root.open) {
        ImGui::EndTable();
        ImGui::End();
        return;
    }

    // ---- bones / slots / attachments ----
    std::function<void(int)> drawBone = [&](int b) {
        if (!boneMatches(b)) return;
        const Bone& bone = sk_.bones[b];
        bool open = true;
        ImGui::PushID(b);
        if (treeShowBones_) {
            ImGui::TableNextRow();
            bool hidden = hiddenBones_.count(b) > 0;
            if (visCell("v", !hidden, "在视口中显示/隐藏（仅编辑器）")) hidden ? (void)hiddenBones_.erase(b) : (void)hiddenBones_.insert(b);
            if (animate) {
                KeyState st = KeyState::None;
                for (Channel ch : {Channel::Rotate, Channel::Translate, Channel::Scale}) {
                    KeyState k = boneKeyState(b, ch);
                    if (k == KeyState::Changed || (k == KeyState::Keyed && st == KeyState::None)) st = k;
                }
                if (keyCell("k", st, st == KeyState::Changed ? "为未打帧的改动打关键帧" : "为该骨骼打全部关键帧")) {
                    auto it = pending_.find(b);
                    if (it != pending_.end()) keyBone(b, it->second.rot, it->second.trans, it->second.scale);
                    else keyBone(b, true, true, true);
                }
            }
            bool selected = sel_.kind == Selection::Bone && sel_.bone == b;
            if (forceOpen[b]) ImGui::SetNextItemOpen(true);
            bool keyed = animate && an && an->bones.count(b);
            RowResult r = treeRow((void*)(intptr_t)(b + 1), Icon::Bone, bone.name, selected, false, true,
                                  hidden ? IM_COL32(140, 140, 140, 255) : 0, keyed ? theme::kRotate : 0);
            scrollIfSelected(selected);
            open = r.open;
            if (r.clicked) selectBone(b);
            if (r.rightClicked) {
                selectBone(b);
                ImGui::OpenPopup("ctx");
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
            if (ImGui::BeginPopup("ctx")) {
                if (ImGui::MenuItem("新建子骨骼", nullptr, false, setup)) {
                    checkpoint();
                    Transform t;
                    t.x = bone.length;
                    selectBone(sk_.addBone("bone", b, t, 50));
                }
                if (ImGui::BeginMenu("新建约束", setup)) {
                    if (ImGui::MenuItem("IK 约束")) selectConstraint(createConstraint(ConstraintType::Ik));
                    if (ImGui::MenuItem("变换约束")) selectConstraint(createConstraint(ConstraintType::Transform));
                    if (ImGui::MenuItem("路径约束")) selectConstraint(createConstraint(ConstraintType::Path));
                    if (ImGui::MenuItem("物理约束")) selectConstraint(createConstraint(ConstraintType::Physics));
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem("新建插槽", nullptr, false, setup)) {
                    checkpoint();
                    Slot s;
                    s.name = sk_.uniqueSlotName("slot");
                    s.bone = b;
                    sk_.slots.push_back(s);
                    selectSlot((int)sk_.slots.size() - 1);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("重命名", "F2")) openRename_ = true;
                if (ImGui::MenuItem("删除", "Del", false, setup && sk_.bones.size() > 1)) deferDelete = true;
                ImGui::EndPopup();
            }
        }
        if (open) {
            for (int s = 0; s < (int)sk_.slots.size(); ++s) {
                const Slot& slot = sk_.slots[s];
                if (slot.bone != b) continue;
                bool slotMatch = matches(slot.name) || matches(sk_.bones[b].name);
                for (auto& a : slot.attachments) slotMatch = slotMatch || matches(a.name);
                if (!slotMatch) continue;
                ImGui::PushID(1000 + s);
                bool slotOpen = true;
                int current = s < (int)pose_.slotAttachment.size() ? pose_.slotAttachment[s] : slot.activeAttachment;
                if (treeShowSlots_) {
                    ImGui::TableNextRow();
                    bool hidden = hiddenSlots_.count(s) > 0;
                    if (visCell("v", !hidden, "在视口中显示/隐藏（仅编辑器）")) hidden ? (void)hiddenSlots_.erase(s) : (void)hiddenSlots_.insert(s);
                    if (keyCell("k", slotKeyState(s), "为当前显示的附件打关键帧")) setSlotAttachment(s, current);
                    bool selected = sel_.kind == Selection::Slot && sel_.slot == s;
                    RowResult r = treeRow("slot", Icon::Slot, slot.name, selected, slot.attachments.empty(), false,
                                          hidden ? IM_COL32(140, 140, 140, 255) : 0);
                    scrollIfSelected(selected);
                    slotOpen = r.open || !treeShowSlots_;
                    if (r.clicked) selectSlot(s);
                    if (setup && ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("SLOT", &s, sizeof(int));
                        ImGui::Text("插槽 %s", slot.name.c_str());
                        ImGui::EndDragDropSource();
                    }
                }
                if (slotOpen && treeShowAttachments_) {
                    for (int a = 0; a < (int)sk_.slots[s].attachments.size(); ++a) {
                        const Attachment& att = sk_.slots[s].attachments[a];
                        ImGui::PushID(a);
                        ImGui::TableNextRow();
                        bool active = current == a;
                        if (visCell("v", active, active ? "隐藏该附件" : "显示该附件（Animate 模式下会打关键帧）"))
                            setSlotAttachment(s, active ? -1 : a);
                        bool selected = sel_.kind == Selection::Attachment && sel_.slot == s && sel_.attachment == a;
                        // Placeholders show what the active skins put there.
                        const Attachment* shown = att.skinPlaceholder ? sk_.resolve(activeSkins_, s, a) : &att;
                        Icon icon = att.skinPlaceholder && !shown       ? Icon::Skin
                                    : shown->type == AttachmentType::Mesh ? Icon::Mesh
                                    : shown->type == AttachmentType::Clipping ? Icon::Clipping
                                    : shown->type == AttachmentType::BoundingBox ? Icon::BoundingBox
                                    : shown->type == AttachmentType::Path        ? Icon::PathC
                                                                                 : Icon::Region;
                        std::string label = att.name;
                        if (att.skinPlaceholder) label += shown ? "  [皮肤: " + shown->name + "]" : "  [皮肤: 无]";
                        RowResult r = treeRow("att", icon, label,
                                              selected, true, false, active ? 0 : IM_COL32(150, 150, 150, 255));
                        scrollIfSelected(selected);
                        if (r.clicked) selectAttachment(s, a);
                        if (r.doubleClicked && att.hasVertices()) setTool(Tool::MeshEdit);
                        ImGui::PopID();
                    }
                }
                if (treeShowSlots_ && slotOpen && !sk_.slots[s].attachments.empty()) ImGui::TreePop();
                ImGui::PopID();
            }
            for (int c : sk_.children(b)) drawBone(c);
            if (treeShowBones_) ImGui::TreePop();
        }
        ImGui::PopID();
    };
    for (size_t i = 0; i < sk_.bones.size(); ++i)
        if (sk_.bones[i].parent < 0) drawBone((int)i);

    // ---- draw order (front first; in Animate mode this is the animated order and edits are keyed) ----
    ImGui::TableNextRow();
    if (animate && an &&
        keyCell("dok", hasKey(*an, -1, Channel::DrawOrder, time_) ? KeyState::Keyed : KeyState::None, "为当前绘制顺序打关键帧")) {
        checkpoint();
        setDrawOrderKey(*an, time_, pose_.drawOrder);
    }
    RowResult dor = treeRow("draworder", Icon::DrawOrder, "绘制顺序", false, sk_.slots.empty(), false);
    int moveFrom = -1, moveTo = -1;
    if (dor.open) {
        for (int i = (int)pose_.drawOrder.size() - 1; i >= 0; --i) {
            int s = pose_.drawOrder[i];
            ImGui::PushID(5000 + s);
            ImGui::TableNextRow();
            bool selected = (sel_.kind == Selection::Slot) && sel_.slot == s;
            RowResult r = treeRow("o", Icon::Slot, sk_.slots[s].name, selected, true, false);
            if (r.clicked) selectSlot(s);
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("DRAWORDER", &s, sizeof(int));
                ImGui::Text("移动 %s", sk_.slots[s].name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("DRAWORDER")) {
                    moveFrom = *(const int*)p->Data;
                    moveTo = s;
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    if (moveFrom >= 0 && moveFrom != moveTo) {
        // Move `moveFrom` to the draw order position currently held by `moveTo`.
        std::vector<int> order = pose_.drawOrder;
        int to = (int)(std::find(order.begin(), order.end(), moveTo) - order.begin());
        order.erase(std::find(order.begin(), order.end(), moveFrom));
        order.insert(order.begin() + to, moveFrom);
        checkpoint();
        if (setup) {
            // Setup order is the slot array itself: apply the permutation with swaps.
            for (int pos = 0; pos < (int)order.size(); ++pos) {
                int cur = order[pos];
                if (cur == pos) continue;
                sk_.swapSlots(pos, cur);
                for (int& o : order) o = o == pos ? cur : (o == cur ? pos : o);
            }
            hiddenSlots_.clear();
            selectSlot(to);
        } else {
            if (!an) an = &sk_.animations[createAnimation()];
            setDrawOrderKey(*an, time_, order);
            updatePose();
        }
    }

    // ---- skins ----
    ImGui::TableNextRow();
    RowResult skins = treeRow("skins", Icon::Skin, "皮肤", false, sk_.skins.empty(), false);
    if (skins.doubleClicked && setup) selectSkin(createSkin("skin"));
    if (skins.rightClicked) ImGui::OpenPopup("skinctx");
    if (ImGui::BeginPopup("skinctx")) {
        if (ImGui::MenuItem("新建皮肤", nullptr, false, setup)) selectSkin(createSkin("skin"));
        ImGui::EndPopup();
    }
    if (skins.open) {
        for (int i = 0; i < (int)sk_.skins.size(); ++i) {
            ImGui::PushID(17000 + i);
            ImGui::TableNextRow();
            auto pos = std::find(activeSkins_.begin(), activeSkins_.end(), i);
            bool active = pos != activeSkins_.end();
            if (visCell("v", active, "激活/取消这个皮肤（可同时激活多个，后激活的优先）")) toggleSkin(i);
            bool selected = sel_.kind == Selection::Skin && sel_.index == i;
            std::string label = sk_.skins[i].name;
            if (active && activeSkins_.size() > 1) label += "  #" + std::to_string(pos - activeSkins_.begin() + 1);
            RowResult r = treeRow("sk", Icon::Skin, label, selected, true, false, active ? 0 : IM_COL32(160, 160, 160, 255));
            if (r.clicked) selectSkin(i);
            if (r.doubleClicked) {
                activeSkins_ = {i};
                updatePose();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }

    // ---- constraints ----
    ImGui::TableNextRow();
    RowResult cons = treeRow("constraints", Icon::Constraint, "约束", false, sk_.constraints.empty(), false);
    if (cons.rightClicked) ImGui::OpenPopup("conctx");
    if (ImGui::BeginPopup("conctx")) {
        ImGui::TextDisabled("约束所选骨骼");
        if (ImGui::MenuItem("IK 约束", nullptr, false, setup)) selectConstraint(createConstraint(ConstraintType::Ik));
        if (ImGui::MenuItem("变换约束", nullptr, false, setup)) selectConstraint(createConstraint(ConstraintType::Transform));
        if (ImGui::MenuItem("路径约束", nullptr, false, setup)) selectConstraint(createConstraint(ConstraintType::Path));
        if (ImGui::MenuItem("物理约束", nullptr, false, setup)) selectConstraint(createConstraint(ConstraintType::Physics));
        ImGui::EndPopup();
    }
    if (cons.open) {
        for (int i = 0; i < (int)sk_.constraints.size(); ++i) {
            ImGui::PushID(12000 + i);
            ImGui::TableNextRow();
            const Constraint& c = sk_.constraints[i];
            if (animate && an && keyCell("k", hasKey(*an, i, Channel::Constraint, time_) ? KeyState::Keyed : KeyState::None, "为约束的当前数值打关键帧"))
                keyConstraint(i);
            bool selected = sel_.kind == Selection::Constraint && sel_.index == i;
            RowResult r = treeRow("c", constraintIcon(c.type), c.name, selected, true, false);
            if (r.clicked) selectConstraint(i);
            ImGui::PopID();
        }
        ImGui::TreePop();
    }

    // ---- animations ----
    ImGui::TableNextRow();
    RowResult anims = treeRow("animations", Icon::Animation, "动画", false, false, true);
    if (anims.doubleClicked) selectAnimation(createAnimation());
    if (anims.rightClicked) ImGui::OpenPopup("animctx");
    if (ImGui::BeginPopup("animctx")) {
        if (ImGui::MenuItem("新建动画")) selectAnimation(createAnimation());
        ImGui::EndPopup();
    }
    if (anims.open) {
        for (int i = 0; i < (int)sk_.animations.size(); ++i) {
            ImGui::PushID(9000 + i);
            ImGui::TableNextRow();
            bool active = anim_ == i;
            if (visCell("v", active, "设为当前动画")) selectAnimation(i);
            bool selected = sel_.kind == Selection::Animation && sel_.index == i;
            RowResult r = treeRow("a", Icon::Animation, sk_.animations[i].name, selected, true, false,
                                  active ? 0 : IM_COL32(160, 160, 160, 255));
            if (r.clicked) selectAnimation(i);
            ImGui::PopID();
        }
        ImGui::TreePop();
    }

    // ---- events ----
    ImGui::TableNextRow();
    RowResult evs = treeRow("events", Icon::Event, "事件", false, false, false);
    if (evs.doubleClicked) {
        checkpoint();
        EventDef e;
        e.name = sk_.uniqueEventName("event");
        sk_.events.push_back(e);
        selectEvent((int)sk_.events.size() - 1);
    }
    if (evs.rightClicked) ImGui::OpenPopup("evctx");
    if (ImGui::BeginPopup("evctx")) {
        if (ImGui::MenuItem("新建事件")) {
            checkpoint();
            EventDef e;
            e.name = sk_.uniqueEventName("event");
            sk_.events.push_back(e);
            selectEvent((int)sk_.events.size() - 1);
        }
        ImGui::EndPopup();
    }
    if (evs.open) {
        for (int i = 0; i < (int)sk_.events.size(); ++i) {
            ImGui::PushID(15000 + i);
            ImGui::TableNextRow();
            if (animate && an && keyCell("k", hasKey(*an, i, Channel::Event, time_) ? KeyState::Keyed : KeyState::None,
                                         "在当前帧插入该事件"))
                keyEvent(i);
            bool selected = sel_.kind == Selection::Event && sel_.index == i;
            RowResult r = treeRow("e", Icon::Event, sk_.events[i].name, selected, true, false);
            if (r.clicked) selectEvent(i);
            ImGui::PopID();
        }
        ImGui::TreePop();
    }

    // ---- images ----
    ImGui::TableNextRow();
    RowResult imgs = treeRow("images", Icon::Image, "图片", false, sk_.images.empty(), false);
    if (imgs.open) {
        for (int i = 0; i < (int)sk_.images.size(); ++i) {
            ImGui::PushID(20000 + i);
            ImGui::TableNextRow();
            bool missing = i >= (int)textures_.size() || !textures_[i].id;
            bool selected = sel_.kind == Selection::Image && sel_.index == i;
            RowResult r = treeRow("i", Icon::Image, fs::u8path(sk_.images[i].path).filename().u8string(), selected, true,
                                  false, missing ? IM_COL32(240, 90, 90, 255) : 0);
            if (r.clicked) {
                sel_.clear();
                sel_.kind = Selection::Image;
                sel_.index = i;
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }

    ImGui::TreePop();  // skeleton
    ImGui::EndTable();
    if (deferDelete) deleteSelection();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::IsKeyPressed(ImGuiKey_F2, false) &&
        !ImGui::GetIO().WantTextInput)
        openRename_ = true;
    ImGui::End();
}
