// Dopesheet: overview row → bone / slot rows → property rows, plus draw order and events.
#include "editor/editor.h"
#include "editor/i18n.h"

#include "core/audio_mix.h"
#include "editor/channels.h"
#include "editor/icons.h"
#include "editor/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <type_traits>

using namespace rig;

namespace {
constexpr Channel kBoneChannels[] = {Channel::Rotate, Channel::Translate, Channel::Scale, Channel::Shear};
constexpr Channel kSlotChannels[] = {Channel::Attachment, Channel::Color, Channel::Deform};

struct KeyMark {
    float time;
    unsigned mask;
};

void addMark(std::vector<KeyMark>& out, float t, Channel ch) {
    for (auto& m : out)
        if (std::fabs(m.time - t) < kKeyEpsilon) {
            m.mask |= channelBit(ch);
            return;
        }
    out.push_back({t, channelBit(ch)});
}

template <typename K>
void addKeys(std::vector<KeyMark>& out, const std::vector<K>& keys, Channel ch) {
    for (auto& k : keys) addMark(out, k.time, ch);
}

void addBone(std::vector<KeyMark>& out, const BoneTimeline& tl, Channel only = Channel::Event) {
    bool all = only == Channel::Event;  // Event = "no filter" here
    if (all || only == Channel::Rotate) addKeys(out, tl.rotate, Channel::Rotate);
    if (all || only == Channel::Translate) addKeys(out, tl.translate, Channel::Translate);
    if (all || only == Channel::Scale) addKeys(out, tl.scale, Channel::Scale);
    if (all || only == Channel::Shear) addKeys(out, tl.shear, Channel::Shear);
}

void addSlot(std::vector<KeyMark>& out, const SlotTimeline& tl, Channel only = Channel::Event, int sub = -1) {
    bool all = only == Channel::Event;
    if (all || only == Channel::Attachment) addKeys(out, tl.attachment, Channel::Attachment);
    if (all || only == Channel::Color) addKeys(out, tl.color, Channel::Color);
    if (all || only == Channel::Deform)
        for (auto& [a, keys] : tl.deform)
            if (sub < 0 || a == sub) addKeys(out, keys, Channel::Deform);
}

ImU32 maskColor(unsigned mask) {
    for (int c = 0; c <= (int)Channel::Constraint; ++c)
        if (mask == channelBit((Channel)c)) return channelColor((Channel)c);
    return theme::kMixed;
}

bool channelHasCurve(Channel ch) {
    return isBoneChannel(ch) || ch == Channel::Color || ch == Channel::Deform || ch == Channel::Constraint;
}

void dashedLine(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col) {
    float len = b.x - a.x;
    for (float x = 0; x < len; x += 6) dl->AddLine(ImVec2(a.x + x, a.y), ImVec2(a.x + std::min(x + 3, len), a.y), col, 1.5f);
}

bool sameValue(float a, float b) { return std::fabs(a - b) < 1e-4f; }
bool sameValue(Vec2 a, Vec2 b) { return length(a - b) < 1e-4f; }
bool sameValue(const std::vector<Vec2>& a, const std::vector<Vec2>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (length(a[i] - b[i]) > 1e-4f) return false;
    return true;
}
bool sameValue(const ConstraintValues& a, const ConstraintValues& b) {
    for (int i = 0; i < 6; ++i)
        if (std::fabs(a[i] - b[i]) > 1e-4f) return false;
    return true;
}
bool sameValue(const Color& a, const Color& b) {
    for (int i = 0; i < 4; ++i)
        if (std::fabs(a[i] - b[i]) > 1e-4f) return false;
    return true;
}
} // namespace

void Editor::drawDopesheet() {
    if (!ImGui::Begin("Dopesheet###Dopesheet")) {
        sheetActive_ = false;
        ImGui::End();
        return;
    }
    sheetActive_ = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) || ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    ImGuiIO& io = ImGui::GetIO();
    const float fh = ImGui::GetFrameHeight();

    // ---- toolbar ----
    {
        Animation* an = currentAnimation();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
        if (ImGui::BeginCombo("##anim", an ? an->name.c_str() : tr("(无动画)"))) {
            for (int i = 0; i < (int)sk_.animations.size(); ++i) {
                ImGui::PushID(i);
                if (ImGui::Selectable(sk_.animations[i].name.c_str(), i == anim_)) selectAnimation(i);
                ImGui::PopID();
            }
            ImGui::Separator();
            if (ImGui::Selectable(tr("+ 新建动画"))) selectAnimation(createAnimation());
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, 12);

        std::vector<float> times = an ? keyTimes(*an) : std::vector<float>{};
        float dur = an ? an->duration : 0.0f;
        if (iconOnlyButton("first", Icon::First, false, tr("第一帧"))) setTime(0);
        ImGui::SameLine(0, 2);
        if (iconOnlyButton("prev", Icon::PrevKey, false, tr("上一个关键帧"))) {
            float t = 0;
            for (float k : times)
                if (k < time_ - kKeyEpsilon) t = k;
            setTime(t);
        }
        ImGui::SameLine(0, 2);
        bool back = playing_ && playDir_ < 0, fwd = playing_ && playDir_ > 0;
        if (iconOnlyButton("pb", back ? Icon::Pause : Icon::PlayBack, back, tr("倒放"))) {
            playing_ = !back;
            playDir_ = -1;
            if (playing_ && time_ <= 0) time_ = dur;
        }
        ImGui::SameLine(0, 2);
        if (iconOnlyButton("pf", fwd ? Icon::Pause : Icon::Play, fwd, tr("播放 / 暂停 (空格)"))) {
            playing_ = !fwd;
            playDir_ = 1;
            if (playing_ && time_ >= dur) time_ = 0;
        }
        ImGui::SameLine(0, 2);
        if (iconOnlyButton("next", Icon::NextKey, false, tr("下一个关键帧"))) {
            for (float k : times)
                if (k > time_ + kKeyEpsilon) {
                    setTime(k);
                    break;
                }
        }
        ImGui::SameLine(0, 2);
        if (iconOnlyButton("last", Icon::Last, false, tr("最后一帧"))) setTime(dur);
        ImGui::SameLine(0, 6);
        if (iconOnlyButton("loop", Icon::Loop, loop_, tr("循环播放"))) loop_ = !loop_;
        ImGui::SameLine(0, 12);

        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(tr("当前"));
        ImGui::SameLine();
        int frame = (int)std::round(time_ * fps_);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3.5f);
        if (ImGui::DragInt("##frame", &frame, 0.2f, 0, (int)std::round(dur * fps_))) setTime(frame / fps_);
        ImGui::SameLine();
        ImGui::TextDisabled(tr("结束"));
        ImGui::SameLine();
        int endFrame = (int)std::round(dur * fps_);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3.5f);
        ImGui::BeginDisabled(!an);
        bool changed = ImGui::DragInt("##end", &endFrame, 0.2f, 1, 100000);
        if (ImGui::IsItemActivated()) checkpoint();
        if (changed && an) an->duration = std::max(1, endFrame) / fps_;
        ImGui::EndDisabled();
        ImGui::SameLine(0, 12);
        ImGui::TextDisabled(tr("速度"));
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
        if (iconOnlyButton("lock", Icon::Lock, sheetLocked_, tr("锁定：不随选择改变显示的骨骼"))) {
            sheetLocked_ = !sheetLocked_;
            sheetLockedBones_.clear();
            if (sheetLocked_) {
                if (sel_.bone >= 0 && sel_.kind != Selection::None) sheetLockedBones_.push_back(sel_.bone);
                else if (an)
                    for (auto& [b, tl] : an->bones) sheetLockedBones_.push_back(b);
            }
        }
        ImGui::SameLine(0, 12);
        if (iconOnlyButton("audio", Icon::Audio, audioOn_, audio_->deviceAvailable() ? tr("播放音频事件") : tr("没有可用的音频输出设备"))) {
            audioOn_ = !audioOn_;
            if (!audioOn_) audio_->stopAll();
        }
        ImGui::SameLine(0, 2);
        if (iconOnlyButton("ghost", Icon::Ghost, ghostOn_, tr("洋葱皮（右键设置）"))) ghostOn_ = !ghostOn_;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("ghostcfg");
        if (ImGui::BeginPopup("ghostcfg")) {
            ImGui::TextDisabled(tr("洋葱皮：前后帧半透明叠加"));
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            ImGui::SliderInt(tr("之前"), &ghostBefore_, 0, 8);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            ImGui::SliderInt(tr("之后"), &ghostAfter_, 0, 8);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            ImGui::SliderInt(tr("间隔（帧）"), &ghostStep_, 1, 30);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            ImGui::SliderFloat(tr("不透明度"), &ghostAlpha_, 0.05f, 1.0f, "%.2f");
            ImGui::EndPopup();
        }
        ImGui::SameLine(0, 2);
        if (iconOnlyButton("zfit", Icon::ZoomFit, sheetPps_ == 0, tr("时间轴适配宽度"))) {
            sheetPps_ = 0;
            sheetScroll_ = 0;
        }
    }

    Animation* an = currentAnimation();
    if (!an) {
        ImGui::TextDisabled(tr("没有动画。在树的“动画”节点上双击，或用左上角下拉框新建。"));
        ImGui::End();
        return;
    }

    // ---- rows ----
    std::vector<int> bones;
    bool showAll = false;
    if (sheetLocked_) {
        for (int b : sheetLockedBones_)
            if (b < (int)sk_.bones.size()) bones.push_back(b);
    } else if (sel_.bone >= 0 && (sel_.kind == Selection::Bone || sel_.kind == Selection::Slot || sel_.kind == Selection::Attachment)) {
        bones.push_back(sel_.bone);
    } else {
        showAll = true;
        for (auto& [b, tl] : an->bones) bones.push_back(b);
    }
    std::vector<SheetRow> rows;
    rows.push_back({SheetRow::Overview, -1});
    for (int b : bones) {
        rows.push_back({SheetRow::Bone, b});
        if (collapsedBones_.count(b)) continue;
        auto it = an->bones.find(b);
        if (it == an->bones.end()) continue;
        const BoneTimeline& tl = it->second;
        if (!tl.rotate.empty()) rows.push_back({SheetRow::Property, b, Channel::Rotate});
        if (!tl.translate.empty()) rows.push_back({SheetRow::Property, b, Channel::Translate});
        if (!tl.scale.empty()) rows.push_back({SheetRow::Property, b, Channel::Scale});
        if (!tl.shear.empty()) rows.push_back({SheetRow::Property, b, Channel::Shear});
    }
    for (auto& [s, tl] : an->slots) {
        if (s >= (int)sk_.slots.size() || tl.empty()) continue;
        bool shown = showAll || sel_.slot == s || std::find(bones.begin(), bones.end(), sk_.slots[s].bone) != bones.end();
        if (!shown) continue;
        rows.push_back({SheetRow::Slot, s});
        if (!tl.attachment.empty()) rows.push_back({SheetRow::Property, s, Channel::Attachment});
        if (!tl.color.empty()) rows.push_back({SheetRow::Property, s, Channel::Color});
        for (auto& [a, keys] : tl.deform)
            if (!keys.empty()) rows.push_back({SheetRow::Property, s, Channel::Deform, a});
    }
    if (!an->drawOrder.empty()) rows.push_back({SheetRow::Property, -1, Channel::DrawOrder});
    if (!an->events.empty()) rows.push_back({SheetRow::Property, -1, Channel::Event});
    {
        std::vector<int> audioEvents;
        for (const AudioEventKey& k : audioEventKeys(sk_, *an))
            if (std::find(audioEvents.begin(), audioEvents.end(), k.event) == audioEvents.end()) audioEvents.push_back(k.event);
        std::sort(audioEvents.begin(), audioEvents.end());
        for (int e : audioEvents) rows.push_back({SheetRow::Audio, e, Channel::Event});
    }
    for (auto& [ci, keys] : an->constraints)
        if (!keys.empty() && ci < (int)sk_.constraints.size()) rows.push_back({SheetRow::Property, ci, Channel::Constraint});

    auto rowMarks = [&](const SheetRow& r) {
        std::vector<KeyMark> m;
        switch (r.kind) {
        case SheetRow::Overview:
            for (auto& [b, tl] : an->bones) addBone(m, tl);
            for (auto& [s, tl] : an->slots) addSlot(m, tl);
            addKeys(m, an->drawOrder, Channel::DrawOrder);
            addKeys(m, an->events, Channel::Event);
            for (auto& [ci, keys] : an->constraints) addKeys(m, keys, Channel::Constraint);
            break;
        case SheetRow::Bone: {
            auto it = an->bones.find(r.index);
            if (it != an->bones.end()) addBone(m, it->second);
            break;
        }
        case SheetRow::Slot: {
            auto it = an->slots.find(r.index);
            if (it != an->slots.end()) addSlot(m, it->second);
            break;
        }
        case SheetRow::Audio:
            for (const EventKey& k : an->events)
                if (k.event == r.index) addMark(m, k.time, Channel::Event);
            break;
        case SheetRow::Property:
            if (r.channel == Channel::Constraint) {
                auto it = an->constraints.find(r.index);
                if (it != an->constraints.end()) addKeys(m, it->second, Channel::Constraint);
            } else if (r.channel == Channel::DrawOrder) {
                addKeys(m, an->drawOrder, Channel::DrawOrder);
            } else if (r.channel == Channel::Event) {
                addKeys(m, an->events, Channel::Event);
            } else if (isSlotChannel(r.channel)) {
                auto it = an->slots.find(r.index);
                if (it != an->slots.end()) addSlot(m, it->second, r.channel, r.sub);
            } else {
                auto it = an->bones.find(r.index);
                if (it != an->bones.end()) addBone(m, it->second, r.channel);
            }
            break;
        }
        std::sort(m.begin(), m.end(), [](const KeyMark& a, const KeyMark& b) { return a.time < b.time; });
        return m;
    };
    // Bone / slot / channel triples a row's keys belong to (for delete and curve commands).
    struct Target {
        int index;
        Channel channel;
        int sub;
    };
    auto rowChannels = [&](const SheetRow& r) {
        std::vector<Target> out;
        switch (r.kind) {
        case SheetRow::Overview:
            for (auto& [b, tl] : an->bones)
                for (Channel ch : kBoneChannels) out.push_back({b, ch, -1});
            for (auto& [s, tl] : an->slots)
                for (Channel ch : kSlotChannels) out.push_back({s, ch, -1});
            out.push_back({-1, Channel::DrawOrder, -1});
            out.push_back({-1, Channel::Event, -1});
            for (auto& [ci, keys] : an->constraints) out.push_back({ci, Channel::Constraint, -1});
            break;
        case SheetRow::Bone:
            for (Channel ch : kBoneChannels) out.push_back({r.index, ch, -1});
            break;
        case SheetRow::Slot:
            for (Channel ch : kSlotChannels) out.push_back({r.index, ch, -1});
            break;
        case SheetRow::Property:
            out.push_back({r.index, r.channel, r.sub});
            break;
        case SheetRow::Audio:
            out.push_back({r.index, Channel::Event, -1});
            break;
        }
        return out;
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
    // Scale handles at both ends of the selected keys' time span.
    float selMin = 1e9f, selMax = -1e9f;
    if (sheetSelAnim_ == anim_)
        for (auto& k : sheetSel_) {
            selMin = std::min(selMin, k.time);
            selMax = std::max(selMax, k.time);
        }
    bool canScale = selMax - selMin > kKeyEpsilon;
    int handleHit = 0;  // -1 left, 1 right
    if (canScale && rulerHovered) {
        if (std::fabs(io.MousePos.x - timeToX(selMin)) < 7) handleHit = -1;
        else if (std::fabs(io.MousePos.x - timeToX(selMax)) < 7) handleHit = 1;
        if (handleHit) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (ImGui::IsItemActivated() && handleHit) {
        sheetDrag_ = SheetDrag{};
        sheetDrag_.scaling = true;
        sheetDrag_.pivot = handleHit > 0 ? selMin : selMax;
        sheetDrag_.origEdge = handleHit > 0 ? selMax : selMin;
        for (auto& k : sheetSel_) sheetDrag_.origTimes.push_back(k.time);
        playing_ = false;
    } else if (ImGui::IsItemActivated() && io.MousePos.x >= keysX0 - 4) {
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
    if (canScale) {
        ImU32 hc = IM_COL32(255, 220, 60, 230);
        float x0 = timeToX(selMin), x1 = timeToX(selMax), yb = origin.y + rulerH;
        dl->AddLine(ImVec2(x0, yb - 2), ImVec2(x1, yb - 2), (hc & 0x00FFFFFF) | 0x80000000, 2.0f);
        for (float x : {x0, x1}) dl->AddRectFilled(ImVec2(x - 3, origin.y + rulerH * 0.35f), ImVec2(x + 3, yb), hc, 1.5f);
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

    // ---- key selection helpers ----
    if (sheetSelAnim_ != anim_) {
        sheetSel_.clear();
        sheetSelAnim_ = anim_;
    }
    auto sameRow = [](const SheetRow& a, const SheetRow& b) {
        return a.kind == b.kind && a.index == b.index && (a.kind != SheetRow::Property || (a.channel == b.channel && a.sub == b.sub));
    };
    auto findSel = [&](const SheetRow& r, float t) {
        for (size_t i = 0; i < sheetSel_.size(); ++i)
            if (sameRow(sheetSel_[i].row, r) && std::fabs(sheetSel_[i].time - t) < kKeyEpsilon) return (int)i;
        return -1;
    };
    auto moveOne = [&](const SheetRow& r, float from, float to) {
        if (r.kind == SheetRow::Overview) moveKeys(*an, from, to);
        else if (r.kind == SheetRow::Bone) moveRowKeys(*an, false, r.index, from, to);
        else if (r.kind == SheetRow::Slot) moveRowKeys(*an, true, r.index, from, to);
        else if (r.kind == SheetRow::Audio) moveChannelKey(*an, r.index, Channel::Event, from, to);
        else moveChannelKey(*an, r.index, r.channel, from, to, r.sub);
    };
    auto deleteKeysAt = [&](const SheetRow& r, float t) {
        for (auto& tg : rowChannels(r))
            while (removeKey(*an, tg.index, tg.channel, t, tg.sub)) {
                if (tg.channel != Channel::Event) break;  // several events can share a time
            }
    };
    auto selectAllVisible = [&]() {
        sheetSel_.clear();
        for (const SheetRow& r : rows)
            for (const KeyMark& m : rowMarks(r)) sheetSel_.push_back({r, m.time});
    };

    // ---- scaling the selection's time span (ruler handles) ----
    if (sheetDrag_.scaling) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float edge = xToTime(mouse.x, !io.KeyShift);
            float span = sheetDrag_.origEdge - sheetDrag_.pivot;
            float factor = std::fabs(span) > kKeyEpsilon ? (edge - sheetDrag_.pivot) / span : 1.0f;
            factor = std::max(factor, 0.0f);
            std::vector<float> targets;
            bool changed = false;
            for (size_t i = 0; i < sheetSel_.size() && i < sheetDrag_.origTimes.size(); ++i) {
                float t = sheetDrag_.pivot + (sheetDrag_.origTimes[i] - sheetDrag_.pivot) * factor;
                t = std::clamp(io.KeyShift ? t : snapTime(t), 0.0f, an->duration);
                targets.push_back(t);
                changed |= std::fabs(t - sheetSel_[i].time) > kKeyEpsilon;
            }
            if (changed) {
                if (!sheetDrag_.checkpointed) {
                    checkpoint();
                    sheetDrag_.checkpointed = true;
                }
                // Two phases through unique parking times so selected keys never overwrite each other.
                float park = an->duration + 1000;
                for (size_t i = 0; i < targets.size(); ++i) moveOne(sheetSel_[i].row, sheetSel_[i].time, park + i);
                for (size_t i = 0; i < targets.size(); ++i) {
                    moveOne(sheetSel_[i].row, park + i, targets[i]);
                    sheetSel_[i].time = targets[i];
                }
            }
        } else {
            sheetDrag_.scaling = false;
        }
    }

    // ---- mouse ----
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
            } else if (r.kind == SheetRow::Property && isBoneChannel(r.channel)) {
                selectBone(r.index);
                setTool(r.channel == Channel::Rotate ? Tool::Rotate : r.channel == Channel::Translate ? Tool::Translate
                        : r.channel == Channel::Scale ? Tool::Scale : Tool::Shear);
            } else if (r.kind == SheetRow::Property && r.channel == Channel::Constraint) {
                selectConstraint(r.index);
            } else if (r.kind == SheetRow::Property && r.channel == Channel::Deform) {
                selectAttachment(r.index, r.sub);
                setTool(Tool::MeshEdit);
            } else if (r.kind == SheetRow::Slot || (r.kind == SheetRow::Property && isSlotChannel(r.channel))) {
                selectSlot(r.index);
            } else if (r.kind == SheetRow::Overview) {
                selectAnimation(anim_);
            } else if (r.kind == SheetRow::Audio) {
                selectEvent(r.index);
            }
        } else if (hitTime >= 0) {
            int idx = findSel(r, hitTime);
            if (io.KeyCtrl) {
                if (idx >= 0) sheetSel_.erase(sheetSel_.begin() + idx);
                else sheetSel_.push_back({r, hitTime});
            } else if (idx < 0) {
                sheetSel_ = {{r, hitTime}};
            }
            sheetDrag_ = SheetDrag{};
            sheetDrag_.movingKey = !io.KeyCtrl;
            sheetDrag_.row = r;
            sheetDrag_.from = hitTime;
            setTime(hitTime);
        } else {
            sheetDrag_ = SheetDrag{};
            sheetDrag_.pressedEmpty = true;
            sheetDrag_.start = mouse;
        }
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (hitTime >= 0) {
            sheetMenuRow_ = rows[hoverRow];
            sheetMenuTime_ = hitTime;
            if (findSel(sheetMenuRow_, hitTime) < 0) sheetSel_ = {{sheetMenuRow_, hitTime}};
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
        } else if (sheetDrag_.pressedEmpty) {
            if (std::fabs(mouse.x - sheetDrag_.start.x) + std::fabs(mouse.y - sheetDrag_.start.y) > 4) sheetDrag_.boxSelecting = true;
        } else if (sheetDrag_.movingKey) {
            // Every selected key moves by the same amount.
            float t = xToTime(mouse.x, !io.KeyShift);
            float delta = t - sheetDrag_.from;
            float minT = 1e9f, maxT = -1e9f;
            for (auto& k : sheetSel_) {
                minT = std::min(minT, k.time);
                maxT = std::max(maxT, k.time);
            }
            delta = std::clamp(delta, -minT, an->duration - maxT);
            if (std::fabs(delta) > kKeyEpsilon && !sheetSel_.empty()) {
                if (!sheetDrag_.checkpointed) {
                    checkpoint();
                    sheetDrag_.checkpointed = true;
                }
                // Move in an order that never runs a key over another selected one.
                std::vector<size_t> order(sheetSel_.size());
                for (size_t i = 0; i < order.size(); ++i) order[i] = i;
                std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                    return delta > 0 ? sheetSel_[a].time > sheetSel_[b].time : sheetSel_[a].time < sheetSel_[b].time;
                });
                for (size_t i : order) {
                    moveOne(sheetSel_[i].row, sheetSel_[i].time, sheetSel_[i].time + delta);
                    sheetSel_[i].time += delta;
                }
                sheetDrag_.from += delta;
                setTime(sheetDrag_.from);
            }
        }
    } else {
        if (sheetDrag_.pressedEmpty) {
            if (sheetDrag_.boxSelecting) {
                // Box select: every key mark inside the rectangle (Ctrl adds to the selection).
                if (!io.KeyCtrl) sheetSel_.clear();
                ImVec2 lo(std::min(mouse.x, sheetDrag_.start.x), std::min(mouse.y, sheetDrag_.start.y));
                ImVec2 hi(std::max(mouse.x, sheetDrag_.start.x), std::max(mouse.y, sheetDrag_.start.y));
                for (size_t i = 0; i < rows.size(); ++i) {
                    float cy = rowsOrigin.y + rowH * (i + 0.5f);
                    if (cy < lo.y || cy > hi.y) continue;
                    for (const KeyMark& m : rowMarks(rows[i])) {
                        float x = timeToX(m.time);
                        if (x >= lo.x && x <= hi.x && findSel(rows[i], m.time) < 0) sheetSel_.push_back({rows[i], m.time});
                    }
                }
            } else {
                if (!io.KeyCtrl) sheetSel_.clear();
                setTime(xToTime(sheetDrag_.start.x, !io.KeyShift));
            }
        }
        sheetDrag_.scrubbing = sheetDrag_.movingKey = sheetDrag_.pressedEmpty = sheetDrag_.boxSelecting = false;
    }

    // ---- keyboard (when the dopesheet has the mouse or focus) ----
    if (sheetActive_ && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !sheetSel_.empty()) {
            checkpoint();
            for (auto& k : sheetSel_) deleteKeysAt(k.row, k.time);
            sheetSel_.clear();
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) selectAllVisible();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false) && !sheetSel_.empty()) {
            float t0 = 1e9f, t1 = -1e9f;
            for (auto& k : sheetSel_) {
                t0 = std::min(t0, k.time);
                t1 = std::max(t1, k.time);
            }
            sheetClip_ = Animation{};
            for (auto& k : sheetSel_)
                for (auto& tg : rowChannels(k.row)) copyKey(*an, sheetClip_, tg.index, tg.channel, tg.sub, k.time, k.time - t0);
            sheetClipLength_ = t1 - t0;
            hasSheetClip_ = true;
            setStatus(tr("已复制 ") + std::to_string(sheetSel_.size()) + tr(" 个关键帧"));
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false) && hasSheetClip_) {
            checkpoint();
            mergeAnimation(*an, sheetClip_, time_);
            an->duration = std::max(an->duration, time_ + sheetClipLength_);
            setStatus(tr("已粘贴到第 ") + std::to_string((int)std::round(time_ * fps_)) + tr(" 帧"));
        }
    }

    if (ImGui::BeginPopup("keymenu")) {
        ImGui::TextDisabled(tr("%d 个关键帧"), (int)sheetSel_.size());
        if (ImGui::MenuItem(tr("删除"), "Del")) {
            checkpoint();
            for (auto& k : sheetSel_) deleteKeysAt(k.row, k.time);
            sheetSel_.clear();
        }
        if (ImGui::MenuItem(tr("复制"), "Ctrl+C")) {
            float t0 = 1e9f, t1 = -1e9f;
            for (auto& k : sheetSel_) {
                t0 = std::min(t0, k.time);
                t1 = std::max(t1, k.time);
            }
            sheetClip_ = Animation{};
            for (auto& k : sheetSel_)
                for (auto& tg : rowChannels(k.row)) copyKey(*an, sheetClip_, tg.index, tg.channel, tg.sub, k.time, k.time - t0);
            sheetClipLength_ = t1 - t0;
            hasSheetClip_ = true;
        }
        if (ImGui::MenuItem(tr("粘贴到当前帧"), "Ctrl+V", false, hasSheetClip_)) {
            checkpoint();
            mergeAnimation(*an, sheetClip_, time_);
            an->duration = std::max(an->duration, time_ + sheetClipLength_);
        }
        bool curves = false;
        for (auto& k : sheetSel_)
            for (auto& tg : rowChannels(k.row)) curves |= channelHasCurve(tg.channel);
        if (curves) {
            ImGui::Separator();
            auto setCurve = [&](const CurveShape& c) {
                checkpoint();
                for (auto& k : sheetSel_)
                    for (auto& tg : rowChannels(k.row))
                        if (channelHasCurve(tg.channel)) setKeyCurve(*an, tg.index, tg.channel, k.time, c, tg.sub);
            };
            if (ImGui::MenuItem(tr("曲线: 线性"))) setCurve(CurveShape::linear());
            if (ImGui::MenuItem(tr("曲线: 阶梯"))) setCurve(CurveShape::stepped());
            if (ImGui::MenuItem(tr("曲线: 缓入缓出"))) setCurve(CurveShape::bezier(0.42f, 0, 0.58f, 1));
            if (ImGui::MenuItem(tr("曲线: 缓入"))) setCurve(CurveShape::bezier(0.42f, 0, 1, 1));
            if (ImGui::MenuItem(tr("曲线: 缓出"))) setCurve(CurveShape::bezier(0, 0, 0.58f, 1));
            ImGui::TextDisabled(tr("在 Graph 视图里可以拖动手柄自定义"));
        }
        ImGui::EndPopup();
    }

    // ---- draw rows ----
    ImDrawList* rdl = ImGui::GetWindowDrawList();
    float rowsW = avail.x;
    float ih = ImGui::GetTextLineHeight();
    for (size_t i = 0; i < rows.size(); ++i) {
        const SheetRow& r = rows[i];
        float y = rowsOrigin.y + rowH * i;
        ImVec2 a(rowsOrigin.x, y), b(rowsOrigin.x + rowsW, y + rowH);
        ImU32 bg = r.kind == SheetRow::Audio  ? IM_COL32(46, 42, 58, 255)
                 : r.kind == SheetRow::Overview ? IM_COL32(64, 104, 104, 255)
                   : r.kind == SheetRow::Bone   ? IM_COL32(78, 120, 118, 255)
                   : r.kind == SheetRow::Slot   ? IM_COL32(88, 100, 128, 255)
                                                : IM_COL32(56, 56, 59, 255);
        bool selected = (r.kind == SheetRow::Bone && sel_.kind == Selection::Bone && sel_.bone == r.index) ||
                        (r.kind == SheetRow::Slot && sel_.slot == r.index);
        rdl->AddRectFilled(a, b, bg);
        if (selected) rdl->AddRectFilled(a, ImVec2(a.x + 3, b.y), theme::kSelect);
        rdl->AddLine(ImVec2(a.x, b.y - 1), ImVec2(b.x, b.y - 1), IM_COL32(30, 30, 32, 255));

        // Name column.
        float tx = a.x + 6, ty = y + (rowH - ih) * 0.5f;
        if (r.kind == SheetRow::Overview) {
            rdl->AddText(ImVec2(tx, ty), textCol, an->name.c_str());
        } else if (r.kind == SheetRow::Bone) {
            bool collapsed = collapsedBones_.count(r.index) > 0;
            rdl->AddText(ImVec2(tx, ty), textCol, collapsed ? "+" : "-");
            drawIcon(rdl, Icon::Bone, ImVec2(tx + ih * 1.4f, ty + ih * 0.5f), ih * 0.8f, iconColor(Icon::Bone));
            rdl->AddText(ImVec2(tx + ih * 2.2f, ty), textCol, sk_.bones[r.index].name.c_str());
        } else if (r.kind == SheetRow::Audio) {
            drawIcon(rdl, Icon::Audio, ImVec2(tx + ih * 0.5f, ty + ih * 0.5f), ih * 0.75f, theme::kEvent);
            rdl->AddText(ImVec2(tx + ih * 1.3f, ty), textCol, sk_.events[r.index].name.c_str());
        } else if (r.kind == SheetRow::Slot) {
            drawIcon(rdl, Icon::Slot, ImVec2(tx + ih * 1.4f, ty + ih * 0.5f), ih * 0.75f, iconColor(Icon::Slot));
            rdl->AddText(ImVec2(tx + ih * 2.2f, ty), textCol, sk_.slots[r.index].name.c_str());
        } else {
            float indent = r.index >= 0 && r.channel != Channel::Constraint ? ih * 1.2f : 0;
            Icon ic = channelIcon(r.channel);
            drawIcon(rdl, ic, ImVec2(tx + indent + ih * 0.5f, ty + ih * 0.5f), ih * 0.75f, channelColor(r.channel));
            std::string name = channelName(r.channel);
            if (r.channel == Channel::Deform && r.index >= 0 && r.sub >= 0 && r.sub < (int)sk_.slots[r.index].attachments.size())
                name += " " + sk_.slots[r.index].attachments[r.sub].name;
            if (r.channel == Channel::Constraint && r.index < (int)sk_.constraints.size()) name += " " + sk_.constraints[r.index].name;
            rdl->AddText(ImVec2(tx + indent + ih * 1.3f, ty), textCol, name.c_str());
        }

        // Interpolation lines: solid = linear, curved = Bezier, dashed = stepped, none = equal values.
        rdl->PushClipRect(ImVec2(keysX0 - 6, y), ImVec2(b.x, b.y), true);
        if (r.kind == SheetRow::Property && channelHasCurve(r.channel)) {
            ImU32 lc = (channelColor(r.channel) & 0x00FFFFFF) | 0xA0000000;
            float mid = y + rowH * 0.5f;
            auto draw = [&](auto& keys) {
                using K = typename std::decay_t<decltype(keys)>::value_type;
                if constexpr (std::is_same_v<K,Vec2Key> || std::is_same_v<K,ColorKey>) {
                    constexpr unsigned mask = std::is_same_v<K,Vec2Key> ? 3 : 15;
                    // Independent components have no single channel interpolation line; Graph shows each one.
                    if (std::any_of(keys.begin(),keys.end(),[mask](const K& key) { return key.separatedCurves || key.componentMask != mask; })) return;
                }
                for (size_t k = 0; k + 1 < keys.size(); ++k) {
                    if (sameValue(keys[k].value, keys[k + 1].value)) continue;
                    float x0 = timeToX(keys[k].time), x1 = timeToX(keys[k + 1].time);
                    const CurveShape& c = keys[k].curve;
                    if (c.type == Curve::Stepped) {
                        dashedLine(rdl, ImVec2(x0, mid), ImVec2(x1, mid), lc);
                    } else if (c.type == Curve::Bezier) {
                        ImVec2 pts[17];
                        for (int j = 0; j <= 16; ++j) {
                            float f = j / 16.0f;
                            pts[j] = ImVec2(x0 + (x1 - x0) * f, mid - (applyCurve(c, f) - f) * rowH * 0.6f);
                        }
                        rdl->AddPolyline(pts, 17, lc, 0, 1.5f);
                    } else {
                        rdl->AddLine(ImVec2(x0, mid), ImVec2(x1, mid), lc, 1.5f);
                    }
                }
            };
            if (r.channel == Channel::Constraint) {
                auto it = an->constraints.find(r.index);
                if (it != an->constraints.end()) draw(it->second);
            } else if (r.channel == Channel::Color || r.channel == Channel::Deform) {
                auto it = an->slots.find(r.index);
                if (it != an->slots.end()) {
                    if (r.channel == Channel::Color) draw(it->second.color);
                    else if (it->second.deform.count(r.sub)) draw(it->second.deform.at(r.sub));
                }
            } else {
                auto it = an->bones.find(r.index);
                if (it != an->bones.end()) {
                    if (r.channel == Channel::Rotate) draw(it->second.rotate);
                    if (r.channel == Channel::Translate) draw(it->second.translate);
                    if (r.channel == Channel::Scale) draw(it->second.scale);
                    if (r.channel == Channel::Shear) draw(it->second.shear);
                }
            }
        }
        if (r.kind == SheetRow::Audio) {
            const std::string& file = sk_.events[r.index].audioPath;
            const std::vector<float>* peaks = audio_->peaks(file);
            float mid = y + rowH * 0.5f, amp = rowH * 0.42f;
            for (const EventKey& k : an->events) {
                if (k.event != r.index) continue;
                float x0 = timeToX(k.time);
                if (!peaks || peaks->empty()) {
                    rdl->AddText(ImVec2(x0 + 6, ty), IM_COL32(240, 110, 110, 255), tr("无法读取音频"));
                    continue;
                }
                float x1 = timeToX(k.time + peaks->size() / 100.0f);
                rdl->AddRectFilled(ImVec2(x0, y + 2), ImVec2(x1, b.y - 2), IM_COL32(170, 110, 235, 40), 2);
                // One vertical bar per pixel column, using the loudest peak it covers.
                float perPixel = (float)peaks->size() / std::max(1.0f, x1 - x0);
                for (float x = std::max(x0, keysX0 - 6); x < std::min(x1, b.x); x += 1) {
                    size_t p0 = (size_t)((x - x0) * perPixel), p1 = std::min(peaks->size(), (size_t)((x + 1 - x0) * perPixel) + 1);
                    float p = 0;
                    for (size_t q = p0; q < p1; ++q) p = std::max(p, (*peaks)[q]);
                    rdl->AddLine(ImVec2(x, mid - p * amp), ImVec2(x, mid + p * amp), IM_COL32(200, 160, 255, 210));
                }
            }
        }
        // Keys.
        for (const KeyMark& m : rowMarks(r)) {
            float x = timeToX(m.time);
            bool atTime = std::fabs(m.time - time_) < kKeyEpsilon;
            bool picked = findSel(r, m.time) >= 0;
            ImVec2 k0(x - 2.5f, y + rowH * 0.2f), k1(x + 2.5f, y + rowH * 0.8f);
            if (picked) rdl->AddRectFilled(k0 - ImVec2(2.5f, 2.5f), k1 + ImVec2(2.5f, 2.5f), IM_COL32(255, 220, 60, 255));
            rdl->AddRectFilled(k0, k1, maskColor(m.mask));
            rdl->AddRect(k0 - ImVec2(0.5f, 0.5f), k1 + ImVec2(0.5f, 0.5f), atTime ? IM_COL32(255, 255, 255, 255) : IM_COL32(20, 20, 20, 220));
        }
        // Event names next to their keys.
        if (r.kind == SheetRow::Property && r.channel == Channel::Event) {
            for (auto& k : an->events)
                if (k.event >= 0 && k.event < (int)sk_.events.size())
                    rdl->AddText(ImVec2(timeToX(k.time) + 5, ty), dimCol, sk_.events[k.event].name.c_str());
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
    if (sheetDrag_.boxSelecting) {
        rdl->AddRectFilled(sheetDrag_.start, mouse, IM_COL32(255, 220, 60, 30));
        rdl->AddRect(sheetDrag_.start, mouse, IM_COL32(255, 220, 60, 200));
    }
    ImGui::EndChild();

    // Playhead marker on the ruler.
    dl->PushClipRect(ImVec2(keysX0 - 8, origin.y), ImVec2(origin.x + avail.x, origin.y + rulerH), true);
    dl->AddLine(ImVec2(endX, origin.y), ImVec2(endX, origin.y + rulerH), theme::kAnimEnd, 1.5f);
    dl->AddLine(ImVec2(px, origin.y), ImVec2(px, origin.y + rulerH), theme::kPlayhead, 1.5f);
    dl->AddTriangleFilled(ImVec2(px - 6, origin.y), ImVec2(px + 6, origin.y), ImVec2(px, origin.y + 8), theme::kPlayhead);
    dl->PopClipRect();
    ImGui::End();
}
