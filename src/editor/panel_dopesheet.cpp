// Dopesheet: overview row → bone rows → property rows, with playback controls.
#include "editor/editor.h"

#include "editor/icons.h"
#include "editor/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <type_traits>

using namespace rig;

namespace {
enum KeyBits : unsigned { kRot = 1, kTrans = 2, kScale = 4, kAtt = 8 };

struct KeyMark {
    float time;
    unsigned mask;
};

void addMarks(std::vector<KeyMark>& out, float t, unsigned bit) {
    for (auto& m : out)
        if (std::fabs(m.time - t) < kKeyEpsilon) {
            m.mask |= bit;
            return;
        }
    out.push_back({t, bit});
}

void addBone(std::vector<KeyMark>& out, const BoneTimeline& tl, unsigned filter = kRot | kTrans | kScale) {
    if (filter & kRot) for (auto& k : tl.rotate) addMarks(out, k.time, kRot);
    if (filter & kTrans) for (auto& k : tl.translate) addMarks(out, k.time, kTrans);
    if (filter & kScale) for (auto& k : tl.scale) addMarks(out, k.time, kScale);
}

ImU32 maskColor(unsigned mask) {
    switch (mask) {
    case kRot: return theme::kRotate;
    case kTrans: return theme::kTranslate;
    case kScale: return theme::kScale;
    case kAtt: return theme::kAttachment;
    default: return theme::kMixed;
    }
}

unsigned channelBit(Channel ch) {
    switch (ch) {
    case Channel::Rotate: return kRot;
    case Channel::Translate: return kTrans;
    case Channel::Scale: return kScale;
    default: return kAtt;
    }
}

const char* channelName(Channel ch) {
    switch (ch) {
    case Channel::Rotate: return "旋转";
    case Channel::Translate: return "移动";
    case Channel::Scale: return "缩放";
    default: return "附件";
    }
}

Icon channelIcon(Channel ch) {
    switch (ch) {
    case Channel::Rotate: return Icon::Rotate;
    case Channel::Translate: return Icon::Translate;
    case Channel::Scale: return Icon::Scale;
    default: return Icon::Region;
    }
}

void dashedLine(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col) {
    float len = b.x - a.x;
    for (float x = 0; x < len; x += 6) dl->AddLine(ImVec2(a.x + x, a.y), ImVec2(a.x + std::min(x + 3, len), a.y), col, 1.5f);
}
} // namespace

void Editor::drawDopesheet() {
    if (!ImGui::Begin("Dopesheet###Dopesheet")) {
        ImGui::End();
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    const float fh = ImGui::GetFrameHeight();

    // ---- toolbar ----
    {
        Animation* an = currentAnimation();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
        if (ImGui::BeginCombo("##anim", an ? an->name.c_str() : "(无动画)")) {
            for (int i = 0; i < (int)sk_.animations.size(); ++i) {
                ImGui::PushID(i);
                if (ImGui::Selectable(sk_.animations[i].name.c_str(), i == anim_)) selectAnimation(i);
                ImGui::PopID();
            }
            ImGui::Separator();
            if (ImGui::Selectable("+ 新建动画")) selectAnimation(createAnimation());
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, 12);

        std::vector<float> times = an ? keyTimes(*an) : std::vector<float>{};
        float dur = an ? an->duration : 0.0f;
        if (iconOnlyButton("first", Icon::First, false, "第一帧")) setTime(0);
        ImGui::SameLine(0, 2);
        if (iconOnlyButton("prev", Icon::PrevKey, false, "上一个关键帧")) {
            float t = 0;
            for (float k : times)
                if (k < time_ - kKeyEpsilon) t = k;
            setTime(t);
        }
        ImGui::SameLine(0, 2);
        bool back = playing_ && playDir_ < 0, fwd = playing_ && playDir_ > 0;
        if (iconOnlyButton("pb", back ? Icon::Pause : Icon::PlayBack, back, "倒放")) {
            playing_ = !back;
            playDir_ = -1;
            if (playing_ && time_ <= 0) time_ = dur;
        }
        ImGui::SameLine(0, 2);
        if (iconOnlyButton("pf", fwd ? Icon::Pause : Icon::Play, fwd, "播放 / 暂停 (空格)")) {
            playing_ = !fwd;
            playDir_ = 1;
            if (playing_ && time_ >= dur) time_ = 0;
        }
        ImGui::SameLine(0, 2);
        if (iconOnlyButton("next", Icon::NextKey, false, "下一个关键帧")) {
            for (float k : times)
                if (k > time_ + kKeyEpsilon) {
                    setTime(k);
                    break;
                }
        }
        ImGui::SameLine(0, 2);
        if (iconOnlyButton("last", Icon::Last, false, "最后一帧")) setTime(dur);
        ImGui::SameLine(0, 6);
        if (iconOnlyButton("loop", Icon::Loop, loop_, "循环播放")) loop_ = !loop_;
        ImGui::SameLine(0, 12);

        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("当前");
        ImGui::SameLine();
        int frame = (int)std::round(time_ * fps_);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3.5f);
        if (ImGui::DragInt("##frame", &frame, 0.2f, 0, (int)std::round(dur * fps_))) setTime(frame / fps_);
        ImGui::SameLine();
        ImGui::TextDisabled("结束");
        ImGui::SameLine();
        int endFrame = (int)std::round(dur * fps_);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3.5f);
        ImGui::BeginDisabled(!an);
        bool changed = ImGui::DragInt("##end", &endFrame, 0.2f, 1, 100000);
        if (ImGui::IsItemActivated()) checkpoint();
        if (changed && an) an->duration = std::max(1, endFrame) / fps_;
        ImGui::EndDisabled();
        ImGui::SameLine(0, 12);
        ImGui::TextDisabled("速度");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.5f);
        char speedLabel[16];
        std::snprintf(speedLabel, sizeof(speedLabel), "%.0f%%", speed_ * 100);
        if (ImGui::BeginCombo("##speed", speedLabel)) {
            for (float s : {0.1f, 0.25f, 0.5f, 1.0f, 1.5f, 2.0f}) {
                char l[16];
                std::snprintf(l, sizeof(l), "%.0f%%", s * 100);
                if (ImGui::Selectable(l, std::fabs(speed_ - s) < 1e-3f)) speed_ = s;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, 12);
        if (iconOnlyButton("lock", Icon::Lock, sheetLocked_, "锁定：不随选择改变显示的骨骼")) {
            sheetLocked_ = !sheetLocked_;
            sheetLockedBones_.clear();
            if (sheetLocked_) {
                if (sel_.bone >= 0 && sel_.kind != Selection::None) sheetLockedBones_.push_back(sel_.bone);
                else if (an)
                    for (auto& [b, tl] : an->bones) sheetLockedBones_.push_back(b);
            }
        }
        ImGui::SameLine(0, 2);
        if (iconOnlyButton("zfit", Icon::ZoomFit, sheetPps_ == 0, "时间轴适配宽度")) {
            sheetPps_ = 0;
            sheetScroll_ = 0;
        }
    }

    Animation* an = currentAnimation();
    if (!an) {
        ImGui::TextDisabled("没有动画。在树的“动画”节点上双击，或用左上角下拉框新建。");
        ImGui::End();
        return;
    }

    // ---- rows ----
    std::vector<int> bones;
    if (sheetLocked_) {
        for (int b : sheetLockedBones_)
            if (b < (int)sk_.bones.size()) bones.push_back(b);
    } else if (sel_.bone >= 0 && sel_.kind != Selection::None && sel_.kind != Selection::Animation) {
        bones.push_back(sel_.bone);
    } else {
        for (auto& [b, tl] : an->bones) bones.push_back(b);
    }
    std::vector<SheetRow> rows;
    rows.push_back({SheetRow::Overview, -1});
    for (int b : bones) {
        rows.push_back({SheetRow::Bone, b});
        if (collapsedBones_.count(b)) continue;
        auto it = an->bones.find(b);
        if (it == an->bones.end()) continue;
        if (!it->second.rotate.empty()) rows.push_back({SheetRow::Property, b, Channel::Rotate});
        if (!it->second.translate.empty()) rows.push_back({SheetRow::Property, b, Channel::Translate});
        if (!it->second.scale.empty()) rows.push_back({SheetRow::Property, b, Channel::Scale});
    }
    for (auto& [s, tl] : an->slots) {
        if (s >= (int)sk_.slots.size() || tl.attachment.empty()) continue;
        bool shown = std::find(bones.begin(), bones.end(), sk_.slots[s].bone) != bones.end();
        if (shown || (!sheetLocked_ && sel_.kind == Selection::None)) rows.push_back({SheetRow::Slot, s, Channel::Attachment});
    }

    auto rowMarks = [&](const SheetRow& r) {
        std::vector<KeyMark> m;
        if (r.kind == SheetRow::Overview) {
            for (auto& [b, tl] : an->bones) addBone(m, tl);
            for (auto& [s, tl] : an->slots)
                for (auto& k : tl.attachment) addMarks(m, k.time, kAtt);
        } else if (r.kind == SheetRow::Bone) {
            auto it = an->bones.find(r.index);
            if (it != an->bones.end()) addBone(m, it->second);
        } else if (r.kind == SheetRow::Property) {
            auto it = an->bones.find(r.index);
            if (it != an->bones.end()) addBone(m, it->second, channelBit(r.channel));
        } else {
            auto it = an->slots.find(r.index);
            if (it != an->slots.end())
                for (auto& k : it->second.attachment) addMarks(m, k.time, kAtt);
        }
        std::sort(m.begin(), m.end(), [](const KeyMark& a, const KeyMark& b) { return a.time < b.time; });
        return m;
    };

    // ---- geometry ----
    const float rowH = ImGui::GetTextLineHeight() + 8;
    const float rulerH = fh + 4;
    const float nameW = ImGui::GetFontSize() * 10;
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float keysX0 = origin.x + nameW + 8, keysX1 = origin.x + avail.x - 12;
    float fitPps = (keysX1 - keysX0) / std::max(an->duration, 1.0f / fps_);
    float pps = sheetPps_ > 0 ? sheetPps_ : fitPps;
    if (sheetPps_ <= 0) sheetScroll_ = 0;
    auto timeToX = [&](float t) { return keysX0 + (t - sheetScroll_) * pps; };
    auto xToTime = [&](float x, bool snap) {
        float t = sheetScroll_ + (x - keysX0) / pps;
        return std::clamp(snap ? snapTime(t) : t, 0.0f, an->duration);
    };
    auto zoomAt = [&](float mouseX, float wheel) {
        float tAtMouse = sheetScroll_ + (mouseX - keysX0) / pps;
        float n = std::clamp(pps * std::pow(1.2f, wheel), 20.0f, 5000.0f);
        sheetPps_ = n;
        sheetScroll_ = std::max(0.0f, tAtMouse - (mouseX - keysX0) / n);
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
    ImU32 dimCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);

    // ---- ruler ----
    ImGui::InvisibleButton("##ruler", ImVec2(avail.x, rulerH));
    bool rulerHovered = ImGui::IsItemHovered();
    if (rulerHovered && io.MouseWheel != 0) zoomAt(io.MousePos.x, io.MouseWheel);
    if (ImGui::IsItemActivated() && io.MousePos.x >= keysX0 - 4) {
        sheetDrag_ = SheetDrag{};
        sheetDrag_.scrubbing = true;
        playing_ = false;
    }
    dl->AddRectFilled(origin, origin + ImVec2(avail.x, rulerH), IM_COL32(44, 44, 47, 255));
    int every = 1;
    while (pps / fps_ * every < 7) every *= 2;
    int labelEvery = every;
    while (pps / fps_ * labelEvery < 34) labelEvery *= 2;
    int totalFrames = (int)std::round(an->duration * fps_);
    int firstFrame = std::max(0, (int)std::floor(sheetScroll_ * fps_ / every) * every);
    dl->PushClipRect(ImVec2(keysX0 - 6, origin.y), ImVec2(origin.x + avail.x, origin.y + avail.y), true);
    for (int f = firstFrame; f <= totalFrames; f += every) {
        float x = timeToX(f / fps_);
        if (x > keysX1 + 10) break;
        bool label = f % labelEvery == 0;
        dl->AddLine(ImVec2(x, origin.y + rulerH * (label ? 0.45f : 0.72f)), ImVec2(x, origin.y + rulerH), dimCol);
        if (label) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d", f);
            dl->AddText(ImVec2(x + 3, origin.y + 1), dimCol, buf);
        }
    }
    for (const KeyMark& m : rowMarks(rows[0])) {  // orange diamonds: frames that have keys
        float x = timeToX(m.time);
        float r = 3.5f, y = origin.y + rulerH - 5;
        ImVec2 d[4] = {{x, y - r}, {x + r, y}, {x, y + r}, {x - r, y}};
        dl->AddConvexPolyFilled(d, 4, theme::kAnimEnd);
    }
    dl->PopClipRect();

    // ---- rows (scrolling child) ----
    ImGuiWindowFlags childFlags = io.KeyCtrl ? ImGuiWindowFlags_NoScrollWithMouse : 0;
    ImGui::BeginChild("##rows", ImVec2(0, 0), ImGuiChildFlags_None, childFlags);
    ImVec2 rowsOrigin = ImGui::GetCursorScreenPos();
    float rowsH = std::max(rowH * rows.size(), ImGui::GetContentRegionAvail().y);
    ImGui::InvisibleButton("##sheet", ImVec2(avail.x, rowsH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();
    ImVec2 mouse = io.MousePos;
    int hoverRow = (int)std::floor((mouse.y - rowsOrigin.y) / rowH);
    bool inKeys = mouse.x >= keysX0 - 6;
    if (hovered && io.KeyCtrl && io.MouseWheel != 0) zoomAt(mouse.x, io.MouseWheel);

    float hitTime = -1;
    if (hovered && hoverRow >= 0 && hoverRow < (int)rows.size() && inKeys)
        for (const KeyMark& m : rowMarks(rows[hoverRow]))
            if (std::fabs(timeToX(m.time) - mouse.x) < 5) hitTime = m.time;

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hoverRow >= 0 && hoverRow < (int)rows.size()) {
        const SheetRow& r = rows[hoverRow];
        playing_ = false;
        if (!inKeys) {
            // Name column: collapse toggle / select the item / pick the channel's tool.
            if (r.kind == SheetRow::Bone) {
                if (mouse.x < rowsOrigin.x + rowH) {
                    if (collapsedBones_.count(r.index)) collapsedBones_.erase(r.index);
                    else collapsedBones_.insert(r.index);
                } else {
                    selectBone(r.index);
                }
            } else if (r.kind == SheetRow::Property) {
                selectBone(r.index);
                setTool(r.channel == Channel::Rotate ? Tool::Rotate : r.channel == Channel::Translate ? Tool::Translate : Tool::Scale);
            } else if (r.kind == SheetRow::Slot) {
                selectSlot(r.index);
            } else {
                selectAnimation(anim_);
            }
        } else if (hitTime >= 0) {
            sheetDrag_ = SheetDrag{};
            sheetDrag_.movingKey = true;
            sheetDrag_.row = r;
            sheetDrag_.from = hitTime;
            setTime(hitTime);
        } else {
            sheetDrag_ = SheetDrag{};
            sheetDrag_.scrubbing = true;
        }
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (hitTime >= 0) {
            sheetMenuRow_ = rows[hoverRow];
            sheetMenuTime_ = hitTime;
            ImGui::OpenPopup("keymenu");
        } else {
            sheetDrag_ = SheetDrag{};
            sheetDrag_.panning = true;
        }
    }

    // Drags (scrubbing may start on the ruler, so this is outside the hover checks).
    if (sheetDrag_.panning) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            if (sheetPps_ <= 0) sheetPps_ = pps;
            sheetScroll_ = std::max(0.0f, sheetScroll_ - io.MouseDelta.x / pps);
        } else {
            sheetDrag_.panning = false;
        }
    } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (sheetDrag_.scrubbing) {
            setTime(xToTime(mouse.x, !io.KeyShift));
        } else if (sheetDrag_.movingKey) {
            float t = xToTime(mouse.x, !io.KeyShift);
            if (std::fabs(t - sheetDrag_.from) > kKeyEpsilon) {
                if (!sheetDrag_.checkpointed) {
                    checkpoint();
                    sheetDrag_.checkpointed = true;
                }
                const SheetRow& r = sheetDrag_.row;
                if (r.kind == SheetRow::Overview) moveKeys(*an, sheetDrag_.from, t);
                else if (r.kind == SheetRow::Bone) moveRowKeys(*an, false, r.index, sheetDrag_.from, t);
                else moveChannelKey(*an, r.index, r.channel, sheetDrag_.from, t);
                sheetDrag_.from = t;
                setTime(t);
            }
        }
    } else {
        sheetDrag_.scrubbing = sheetDrag_.movingKey = false;
    }

    if (ImGui::BeginPopup("keymenu")) {
        const SheetRow& r = sheetMenuRow_;
        auto forBones = [&](auto&& fn) {
            if (r.kind == SheetRow::Bone || r.kind == SheetRow::Property) fn(r.index);
            if (r.kind == SheetRow::Overview) {
                std::vector<int> all;
                for (auto& [b, tl] : an->bones) all.push_back(b);
                for (int b : all) fn(b);
            }
        };
        auto channels = [&]() -> std::vector<Channel> {
            if (r.kind == SheetRow::Property) return {r.channel};
            return {Channel::Rotate, Channel::Translate, Channel::Scale};
        };
        if (ImGui::MenuItem("删除关键帧")) {
            checkpoint();
            forBones([&](int b) {
                for (Channel ch : channels()) removeKey(*an, b, ch, sheetMenuTime_);
            });
            if (r.kind == SheetRow::Slot) removeKey(*an, r.index, Channel::Attachment, sheetMenuTime_);
            if (r.kind == SheetRow::Overview) {
                std::vector<int> slots;
                for (auto& [s, tl] : an->slots) slots.push_back(s);
                for (int s : slots) removeKey(*an, s, Channel::Attachment, sheetMenuTime_);
            }
        }
        if (r.kind != SheetRow::Slot) {
            if (ImGui::MenuItem("曲线: 线性")) {
                checkpoint();
                forBones([&](int b) {
                    for (Channel ch : channels()) setKeyCurve(*an, b, ch, sheetMenuTime_, Curve::Linear);
                });
            }
            if (ImGui::MenuItem("曲线: 阶梯")) {
                checkpoint();
                forBones([&](int b) {
                    for (Channel ch : channels()) setKeyCurve(*an, b, ch, sheetMenuTime_, Curve::Stepped);
                });
            }
        }
        ImGui::EndPopup();
    }

    // ---- draw rows ----
    ImDrawList* rdl = ImGui::GetWindowDrawList();
    float rowsW = avail.x;
    for (size_t i = 0; i < rows.size(); ++i) {
        const SheetRow& r = rows[i];
        float y = rowsOrigin.y + rowH * i;
        ImVec2 a(rowsOrigin.x, y), b(rowsOrigin.x + rowsW, y + rowH);
        ImU32 bg = r.kind == SheetRow::Overview ? IM_COL32(64, 104, 104, 255)
                   : r.kind == SheetRow::Bone   ? IM_COL32(78, 120, 118, 255)
                                                : IM_COL32(56, 56, 59, 255);
        bool selected = (r.kind == SheetRow::Bone && sel_.kind == Selection::Bone && sel_.bone == r.index) ||
                        (r.kind == SheetRow::Slot && sel_.slot == r.index);
        rdl->AddRectFilled(a, b, bg);
        if (selected) rdl->AddRectFilled(a, ImVec2(a.x + 3, b.y), theme::kSelect);
        rdl->AddLine(ImVec2(a.x, b.y - 1), ImVec2(b.x, b.y - 1), IM_COL32(30, 30, 32, 255));

        // Name column.
        float tx = a.x + 6, ty = y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
        float ih = ImGui::GetTextLineHeight();
        std::string label;
        if (r.kind == SheetRow::Overview) {
            label = an->name;
            rdl->AddText(ImVec2(tx, ty), textCol, label.c_str());
        } else if (r.kind == SheetRow::Bone) {
            bool collapsed = collapsedBones_.count(r.index) > 0;
            rdl->AddText(ImVec2(tx, ty), textCol, collapsed ? "+" : "-");
            drawIcon(rdl, Icon::Bone, ImVec2(tx + ih * 1.4f, ty + ih * 0.5f), ih * 0.8f, iconColor(Icon::Bone));
            rdl->AddText(ImVec2(tx + ih * 2.2f, ty), textCol, sk_.bones[r.index].name.c_str());
        } else {
            float indent = r.kind == SheetRow::Property ? ih * 1.2f : 0;
            Icon ic = r.kind == SheetRow::Slot ? Icon::Slot : channelIcon(r.channel);
            drawIcon(rdl, ic, ImVec2(tx + indent + ih * 0.5f, ty + ih * 0.5f), ih * 0.75f, iconColor(ic));
            label = r.kind == SheetRow::Slot ? sk_.slots[r.index].name + " 附件" : channelName(r.channel);
            rdl->AddText(ImVec2(tx + indent + ih * 1.3f, ty), textCol, label.c_str());
        }

        // Keys.
        rdl->PushClipRect(ImVec2(keysX0 - 6, y), ImVec2(b.x, b.y), true);
        std::vector<KeyMark> marks = rowMarks(r);
        if (r.kind == SheetRow::Property) {
            // Interpolation lines between keys: solid = linear, dashed = stepped, none = equal values.
            auto it = an->bones.find(r.index);
            ImU32 lc = (maskColor(channelBit(r.channel)) & 0x00FFFFFF) | 0xA0000000;
            auto draw = [&](auto& keys) {
                for (size_t k = 0; k + 1 < keys.size(); ++k) {
                    bool same;
                    if constexpr (std::is_same_v<std::decay_t<decltype(keys[k].value)>, float>)
                        same = std::fabs(keys[k].value - keys[k + 1].value) < 1e-4f;
                    else
                        same = length(keys[k].value - keys[k + 1].value) < 1e-4f;
                    if (same) continue;
                    ImVec2 p0(timeToX(keys[k].time), y + rowH * 0.5f), p1(timeToX(keys[k + 1].time), y + rowH * 0.5f);
                    if (keys[k].curve == Curve::Stepped) dashedLine(rdl, p0, p1, lc);
                    else rdl->AddLine(p0, p1, lc, 1.5f);
                }
            };
            if (it != an->bones.end()) {
                if (r.channel == Channel::Rotate) draw(it->second.rotate);
                if (r.channel == Channel::Translate) draw(it->second.translate);
                if (r.channel == Channel::Scale) draw(it->second.scale);
            }
        }
        for (const KeyMark& m : marks) {
            float x = timeToX(m.time);
            bool atTime = std::fabs(m.time - time_) < kKeyEpsilon;
            ImVec2 k0(x - 2.5f, y + rowH * 0.2f), k1(x + 2.5f, y + rowH * 0.8f);
            rdl->AddRectFilled(k0, k1, maskColor(m.mask));
            rdl->AddRect(k0 - ImVec2(0.5f, 0.5f), k1 + ImVec2(0.5f, 0.5f), atTime ? IM_COL32(255, 255, 255, 255) : IM_COL32(20, 20, 20, 220));
        }
        rdl->PopClipRect();
    }

    // End of animation and playhead across the rows.
    rdl->PushClipRect(ImVec2(keysX0 - 6, rowsOrigin.y), ImVec2(rowsOrigin.x + rowsW, rowsOrigin.y + rowsH), true);
    float endX = timeToX(an->duration);
    rdl->AddLine(ImVec2(endX, rowsOrigin.y), ImVec2(endX, rowsOrigin.y + rowsH), theme::kAnimEnd, 1.5f);
    float px = timeToX(time_);
    rdl->AddLine(ImVec2(px, rowsOrigin.y), ImVec2(px, rowsOrigin.y + rowsH), theme::kPlayhead, 1.5f);
    rdl->PopClipRect();
    ImGui::EndChild();

    // Playhead marker on the ruler.
    dl->PushClipRect(ImVec2(keysX0 - 8, origin.y), ImVec2(origin.x + avail.x, origin.y + rulerH), true);
    dl->AddLine(ImVec2(endX, origin.y), ImVec2(endX, origin.y + rulerH), theme::kAnimEnd, 1.5f);
    dl->AddLine(ImVec2(px, origin.y), ImVec2(px, origin.y + rulerH), theme::kPlayhead, 1.5f);
    dl->AddTriangleFilled(ImVec2(px - 6, origin.y), ImVec2(px + 6, origin.y), ImVec2(px, origin.y + 8), theme::kPlayhead);
    dl->PopClipRect();
    ImGui::End();
}
