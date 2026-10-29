// Mix preview: plays animations on several tracks with crossfades, the way a runtime AnimationState does.
#include "editor/editor.h"
#include "editor/i18n.h"

#include <algorithm>
#include <cstdio>

using namespace rig;

namespace {
// Animation combo; -1 = none. Returns true when changed.
bool animationCombo(const char* label, const Skeleton& sk, int& anim, const char* noneText) {
    const char* name = anim >= 0 && anim < (int)sk.animations.size() ? sk.animations[anim].name.c_str() : noneText;
    bool changed = false;
    if (ImGui::BeginCombo(label, name)) {
        if (ImGui::Selectable(noneText, anim < 0)) {
            anim = -1;
            changed = true;
        }
        for (int i = 0; i < (int)sk.animations.size(); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(sk.animations[i].name.c_str(), i == anim)) {
                anim = i;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}
} // namespace

void Editor::drawMixPreview() {
    bool visible = ImGui::Begin(tr("混合预览###Preview"));
    if (focusPreview_ > 0) {
        ImGui::SetWindowFocus();
        --focusPreview_;
    }
    if (!visible) {
        ImGui::End();
        return;
    }
    const float fs = ImGui::GetFontSize();
    // Header: preview on/off, pause, default mix.
    if (ImGui::Checkbox(tr("在视口中预览"), &mixActive_) && mixActive_) playing_ = false;
    ImGui::SameLine();
    if (ImGui::Button(mixPlaying_ ? tr("暂停") : tr("播放"))) mixPlaying_ = !mixPlaying_;
    ImGui::SameLine();
    if (ImGui::Button(tr("全部清空"))) {
        mixState_.clearTracks();
        mixActive_ = false;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fs * 6);
    ImGui::DragFloat(tr("默认混合"), &mixState_.defaultMix, 0.01f, 0.0f, 5.0f, tr("%.2f 秒"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fs * 6);
    ImGui::DragFloat(tr("队列延迟"), &mixQueueDelay_, 0.01f, -5.0f, 5.0f, tr("%.2f 秒"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(tr("加入队列的动画在上一个动画开始后多少秒开始。\n<= 0：相对上一个动画结束（减去混合时长）。"));

    // One row per track.
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##tracks", 6, flags)) {
        ImGui::TableSetupColumn(tr("轨道"));
        ImGui::TableSetupColumn(tr("动画"), ImGuiTableColumnFlags_WidthFixed, fs * 9);
        ImGui::TableSetupColumn(tr("循环"));
        ImGui::TableSetupColumn(tr("权重"), ImGuiTableColumnFlags_WidthFixed, fs * 6);
        ImGui::TableSetupColumn(tr("操作"));
        ImGui::TableSetupColumn(tr("状态"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int t = 0; t < kMixTracks; ++t) {
            MixTrackUi& ui = mixUi_[t];
            ImGui::PushID(t);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d", t);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            animationCombo("##anim", sk_, ui.anim, tr("(无)"));
            ImGui::TableNextColumn();
            ImGui::Checkbox("##loop", &ui.loop);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##alpha", &ui.alpha, 0.0f, 1.0f, "%.2f")) mixState_.setTrackAlpha(t, ui.alpha);
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(ui.anim < 0);
            if (ImGui::SmallButton(tr("播放"))) {
                mixState_.setTrackAlpha(t, ui.alpha);
                mixState_.setAnimation(t, ui.anim, ui.loop);
                mixActive_ = true;
                playing_ = false;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("加入队列"))) {
                mixState_.setTrackAlpha(t, ui.alpha);
                mixState_.addAnimation(sk_, t, ui.anim, ui.loop, mixQueueDelay_);
                mixActive_ = true;
                playing_ = false;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!mixState_.current(t));
            if (ImGui::SmallButton(tr("淡出"))) mixState_.setEmptyAnimation(t, mixState_.defaultMix);
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            if (const TrackEntry* e = mixState_.current(t)) {
                const char* name = e->animation >= 0 && e->animation < (int)sk_.animations.size()
                                       ? sk_.animations[e->animation].name.c_str()
                                       : tr("(淡出中)");
                char buf[160];
                std::snprintf(buf, sizeof(buf), tr("%s  %.2f 秒"), name, e->animationTime(sk_));
                ImGui::TextUnformatted(buf);
                if (mixState_.mixingCount(t) > 1 || e->mixFraction() < 1) {
                    ImGui::SameLine();
                    std::snprintf(buf, sizeof(buf), tr("混合 %d%%"), (int)(e->mixFraction() * 100));
                    ImGui::ProgressBar(e->mixFraction(), ImVec2(fs * 6, 0), buf);
                }
                if (int q = mixState_.queuedCount(t)) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(tr("队列 %d"), q);
                }
            } else {
                ImGui::TextDisabled(mixState_.queuedCount(t) ? tr("等待队列") : tr("空"));
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // Mix durations for specific animation pairs.
    if (ImGui::CollapsingHeader(tr("按动画对设置混合时长"))) {
        ImGui::SetNextItemWidth(fs * 8);
        animationCombo(tr("从"), sk_, mixPairFrom_, tr("(空轨道)"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fs * 8);
        animationCombo(tr("到"), sk_, mixPairTo_, tr("(无)"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fs * 5);
        ImGui::DragFloat("##dur", &mixPairDuration_, 0.01f, 0.0f, 5.0f, tr("%.2f 秒"));
        ImGui::SameLine();
        ImGui::BeginDisabled(mixPairTo_ < 0);
        if (ImGui::Button(tr("设置"))) mixState_.setMix(mixPairFrom_, mixPairTo_, mixPairDuration_);
        ImGui::EndDisabled();
        std::pair<int, int> remove{-2, -2};
        for (auto& [pair, dur] : mixState_.mixes()) {
            auto nameOf = [&](int a, const char* none) {
                return a >= 0 && a < (int)sk_.animations.size() ? sk_.animations[a].name.c_str() : none;
            };
            ImGui::PushID(pair.first * 1000 + pair.second);
            ImGui::BulletText(tr("%s → %s：%.2f 秒"), nameOf(pair.first, tr("(空轨道)")), nameOf(pair.second, "?"), dur);
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("删除"))) remove = pair;
            ImGui::PopID();
        }
        if (remove.first != -2) mixState_.clearMix(remove.first, remove.second);
    }
    if (mixActive_)
        ImGui::TextDisabled(tr("视口显示混合结果；在摄影表中播放或编辑关键帧前请先关闭预览。"));
    ImGui::End();
}
