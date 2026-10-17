#include "editor/editor.h"

#include <imgui_stdlib.h>

#include <algorithm>
#include <cstdio>

using namespace rig;

static void drawDiamond(ImDrawList* dl, ImVec2 c, float r, ImU32 fill, ImU32 outline) {
    ImVec2 p[4] = {{c.x, c.y - r}, {c.x + r, c.y}, {c.x, c.y + r}, {c.x - r, c.y}};
    dl->AddConvexPolyFilled(p, 4, fill);
    dl->AddPolyline(p, 4, outline, ImDrawFlags_Closed, 1.0f);
}

void Editor::drawTimeline() {
    if (!ImGui::Begin("时间轴###Timeline")) {
        ImGui::End();
        return;
    }

    // ---- animation list & transport ----
    std::string current = currentAnimation() ? currentAnimation()->name : "(无动画)";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (ImGui::BeginCombo("##anim", current.c_str())) {
        for (int i = 0; i < (int)sk_.animations.size(); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(sk_.animations[i].name.c_str(), i == anim_)) {
                anim_ = i;
                time_ = std::min(time_, sk_.animations[i].duration);
                mode_ = Mode::Animate;
                if (tool_ > Tool::Scale) tool_ = Tool::Rotate;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("+ 新动画")) {
        checkpoint();
        Animation an;
        int n = (int)sk_.animations.size() + 1;
        an.name = "animation" + std::to_string(n);
        sk_.animations.push_back(an);
        anim_ = (int)sk_.animations.size() - 1;
        time_ = 0;
        mode_ = Mode::Animate;
        if (tool_ > Tool::Scale) tool_ = Tool::Rotate;
    }
    Animation* an = currentAnimation();
    ImGui::BeginDisabled(!an);
    ImGui::SameLine();
    if (ImGui::Button("重命名") && an) {
        renameBuf_ = an->name;
        ImGui::OpenPopup("rename");
    }
    ImGui::SameLine();
    if (ImGui::Button("删除动画") && an) {
        checkpoint();
        sk_.animations.erase(sk_.animations.begin() + anim_);
        anim_ = std::min(anim_, (int)sk_.animations.size() - 1);
        an = currentAnimation();
        playing_ = false;
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("rename")) {
        ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##name", &renameBuf_, ImGuiInputTextFlags_EnterReturnsTrue) && an && !renameBuf_.empty()) {
            checkpoint();
            an->name = renameBuf_;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!an) {
        ImGui::TextDisabled("还没有动画。点“+ 新动画”开始，或在 Animate 模式下直接拖动骨骼。");
        ImGui::End();
        return;
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::Button(playing_ ? "暂停" : "播放")) {
        playing_ = !playing_;
        mode_ = Mode::Animate;
        if (playing_ && time_ >= an->duration) time_ = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button("|<")) time_ = 0;
    ImGui::SameLine();
    ImGui::Checkbox("循环", &loop_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
    float dur = an->duration;
    if (ImGui::DragFloat("时长(秒)", &dur, 0.01f, 0.05f, 600.0f, "%.2f")) an->duration = std::max(0.05f, dur);
    if (ImGui::IsItemActivated()) checkpoint();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
    ImGui::DragFloat("FPS", &fps_, 1, 1, 120, "%.0f");
    fps_ = std::clamp(fps_, 1.0f, 120.0f);
    ImGui::SameLine();
    ImGui::Text("%.3fs / 第 %d 帧", time_, (int)std::round(time_ * fps_));
    if (mode_ != Mode::Animate) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "  当前是 Setup 模式（Tab 切换）");
    }

    // ---- rows ----
    std::vector<TimelineRow> rows;
    rows.push_back({TimelineRow::Summary, -1});
    for (int b = 0; b < (int)sk_.bones.size(); ++b)
        if (an->bones.count(b) || b == sel_.bone) rows.push_back({TimelineRow::Bone, b});
    for (int s = 0; s < (int)sk_.slots.size(); ++s)
        if (an->slots.count(s) || s == sel_.slot) rows.push_back({TimelineRow::Slot, s});

    auto rowTimes = [&](const TimelineRow& r) {
        if (r.kind == TimelineRow::Summary) return keyTimes(*an);
        return rowKeyTimes(*an, r.kind == TimelineRow::Slot, r.index);
    };

    const float rowH = ImGui::GetFrameHeight();
    const float rulerH = rowH;
    const float nameW = ImGui::GetFontSize() * 9;
    ImGui::BeginChild("dopesheet", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float width = std::max(ImGui::GetContentRegionAvail().x, nameW + 100);
    float height = rulerH + rowH * rows.size();
    float keysX0 = origin.x + nameW, keysX1 = origin.x + width - 10;
    float pps = (keysX1 - keysX0) / an->duration;  // pixels per second
    auto timeToX = [&](float t) { return keysX0 + t * pps; };
    auto xToTime = [&](float x) { return std::clamp(snapTime((x - keysX0) / pps), 0.0f, an->duration); };

    ImGui::InvisibleButton("##sheet", ImVec2(width, height), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();
    ImVec2 mouse = ImGui::GetIO().MousePos;
    int hoverRow = (int)std::floor((mouse.y - origin.y - rulerH) / rowH);
    bool inKeys = mouse.x >= keysX0 - 6;

    // Find the key under the mouse in the hovered row.
    float hitTime = -1;
    if (hovered && hoverRow >= 0 && hoverRow < (int)rows.size() && inKeys) {
        for (float t : rowTimes(rows[hoverRow]))
            if (std::fabs(timeToX(t) - mouse.x) < rowH * 0.4f) hitTime = t;
    }

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        mode_ = Mode::Animate;
        if (tool_ > Tool::Scale) tool_ = Tool::Rotate;
        playing_ = false;
        if (hoverRow >= 0 && hoverRow < (int)rows.size()) {
            const TimelineRow& r = rows[hoverRow];
            if (r.kind == TimelineRow::Bone) {
                sel_.bone = r.index;
                sel_.slot = -1;
            } else if (r.kind == TimelineRow::Slot) {
                sel_.slot = r.index;
                sel_.bone = sk_.slots[r.index].bone;
            }
        }
        if (hitTime >= 0) {
            tlDrag_ = TimelineDrag{};
            tlDrag_.movingKey = true;
            tlDrag_.row = rows[hoverRow];
            tlDrag_.from = hitTime;
            time_ = hitTime;
        } else if (inKeys) {
            tlDrag_ = TimelineDrag{};
            tlDrag_.scrubbing = true;
        }
    }
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (tlDrag_.scrubbing) {
            time_ = xToTime(mouse.x);
        } else if (tlDrag_.movingKey) {
            float t = xToTime(mouse.x);
            if (std::fabs(t - tlDrag_.from) > kKeyEpsilon) {
                if (!tlDrag_.checkpointed) {
                    checkpoint();
                    tlDrag_.checkpointed = true;
                }
                if (tlDrag_.row.kind == TimelineRow::Summary)
                    moveKeys(*an, tlDrag_.from, t);
                else
                    moveRowKeys(*an, tlDrag_.row.kind == TimelineRow::Slot, tlDrag_.row.index, tlDrag_.from, t);
                tlDrag_.from = t;
                time_ = t;
            }
        }
    } else {
        tlDrag_.scrubbing = tlDrag_.movingKey = false;
    }

    if (hovered && hitTime >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        tlMenuRow_ = rows[hoverRow];
        tlMenuTime_ = hitTime;
        ImGui::OpenPopup("keymenu");
    }
    if (ImGui::BeginPopup("keymenu")) {
        auto forRowBones = [&](auto&& fn) {
            if (tlMenuRow_.kind == TimelineRow::Bone) fn(tlMenuRow_.index);
            if (tlMenuRow_.kind == TimelineRow::Summary) {
                std::vector<int> bones;
                for (auto& [b, tl] : an->bones) bones.push_back(b);
                for (int b : bones) fn(b);
            }
        };
        if (ImGui::MenuItem("删除关键帧")) {
            checkpoint();
            forRowBones([&](int b) {
                for (Channel ch : {Channel::Rotate, Channel::Translate, Channel::Scale}) removeKey(*an, b, ch, tlMenuTime_);
            });
            if (tlMenuRow_.kind == TimelineRow::Slot) removeKey(*an, tlMenuRow_.index, Channel::Attachment, tlMenuTime_);
            if (tlMenuRow_.kind == TimelineRow::Summary) {
                std::vector<int> slots;
                for (auto& [s, tl] : an->slots) slots.push_back(s);
                for (int s : slots) removeKey(*an, s, Channel::Attachment, tlMenuTime_);
            }
        }
        if (tlMenuRow_.kind != TimelineRow::Slot) {
            if (ImGui::MenuItem("曲线: 线性")) {
                checkpoint();
                forRowBones([&](int b) {
                    for (Channel ch : {Channel::Rotate, Channel::Translate, Channel::Scale})
                        setKeyCurve(*an, b, ch, tlMenuTime_, Curve::Linear);
                });
            }
            if (ImGui::MenuItem("曲线: 阶梯")) {
                checkpoint();
                forRowBones([&](int b) {
                    for (Channel ch : {Channel::Rotate, Channel::Translate, Channel::Scale})
                        setKeyCurve(*an, b, ch, tlMenuTime_, Curve::Stepped);
                });
            }
        }
        ImGui::EndPopup();
    }

    // ---- drawing ----
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
    ImU32 dimCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    dl->AddRectFilled(origin, {origin.x + width, origin.y + rulerH}, ImGui::GetColorU32(ImGuiCol_FrameBg));

    // Ruler ticks: one per frame when there is room, otherwise every N frames.
    int totalFrames = (int)std::round(an->duration * fps_);
    int every = 1;
    while (pps / fps_ * every < 6) every *= 2;
    int labelEvery = every;
    while (pps / fps_ * labelEvery < 40) labelEvery *= 2;
    for (int f = 0; f <= totalFrames; f += every) {
        float x = timeToX(f / fps_);
        bool label = f % labelEvery == 0;
        dl->AddLine({x, origin.y + rulerH * (label ? 0.35f : 0.7f)}, {x, origin.y + rulerH}, dimCol);
        if (label) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d", f);
            dl->AddText({x + 2, origin.y}, dimCol, buf);
        }
        dl->AddLine({x, origin.y + rulerH}, {x, origin.y + height}, IM_COL32(255, 255, 255, label ? 18 : 7));
    }
    dl->AddLine({timeToX(an->duration), origin.y}, {timeToX(an->duration), origin.y + height}, IM_COL32(255, 90, 90, 120));

    for (size_t i = 0; i < rows.size(); ++i) {
        const TimelineRow& r = rows[i];
        float y = origin.y + rulerH + rowH * i;
        bool selected = (r.kind == TimelineRow::Bone && r.index == sel_.bone && sel_.slot < 0) ||
                        (r.kind == TimelineRow::Slot && r.index == sel_.slot);
        if (selected) dl->AddRectFilled({origin.x, y}, {origin.x + width, y + rowH}, IM_COL32(255, 170, 40, 30));
        else if (i % 2) dl->AddRectFilled({origin.x, y}, {origin.x + width, y + rowH}, IM_COL32(255, 255, 255, 6));
        std::string label = r.kind == TimelineRow::Summary ? "全部"
                            : r.kind == TimelineRow::Bone  ? sk_.bones[r.index].name
                                                           : "[" + sk_.slots[r.index].name + "]";
        dl->AddText({origin.x + 6, y + (rowH - ImGui::GetFontSize()) / 2},
                    r.kind == TimelineRow::Summary ? dimCol : textCol, label.c_str());
        ImU32 fill = r.kind == TimelineRow::Summary ? IM_COL32(200, 200, 200, 255)
                     : r.kind == TimelineRow::Slot  ? IM_COL32(120, 200, 255, 255)
                                                    : IM_COL32(255, 200, 60, 255);
        for (float t : rowTimes(r)) {
            bool atTime = std::fabs(t - time_) < kKeyEpsilon;
            drawDiamond(dl, {timeToX(t), y + rowH / 2}, rowH * 0.28f, fill,
                        atTime ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 200));
        }
    }

    float px = timeToX(time_);
    dl->AddLine({px, origin.y}, {px, origin.y + height}, IM_COL32(90, 200, 255, 255), 1.5f);
    dl->AddTriangleFilled({px - 5, origin.y}, {px + 5, origin.y}, {px, origin.y + 7}, IM_COL32(90, 200, 255, 255));

    ImGui::EndChild();
    ImGui::End();
}
