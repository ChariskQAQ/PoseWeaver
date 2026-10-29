#include "editor/editor.h"
#include "editor/i18n.h"
#include "platform/file_dialog.h"
#include <imgui_stdlib.h>
#include <algorithm>
#include <cmath>

void Editor::drawEngineExportDialog() {
    const float font = ImGui::GetFontSize();
    const auto viewport = ImGui::GetMainViewport();
    const float width = std::min(font * 36, viewport->WorkSize.x - 24);
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, viewport->WorkSize.y - 24));
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(tr("Unity / Godot 帧动画"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    auto& options = engineOptions_;
    bool changed = false, budgetChanged = false;
    ImGui::TextWrapped(tr("选择要导出的动作及当前皮肤，安装引擎插件后导入 .pwframes。"));
    if (engineJob_) {
        const auto& progress = *engineJob_->progress;
        const char* stages[] = {tr("正在分析动作与画布..."), tr("正在烘焙动画帧..."), tr("正在写入图集..."), tr("正在提交资源，请稍候...")};
        ImGui::TextUnformatted(stages[progress.stage.load()]);
        ImGui::ProgressBar(std::min(0.99f, float(progress.completed.load()) / std::max(1, progress.total.load())), ImVec2(-1, 0));
        const int gate = progress.gate.load();
        ImGui::BeginDisabled(gate != EngineExportProgress::Running);
        if (ImGui::Button(tr("取消任务"))) engineJob_->progress->cancel();
        ImGui::EndDisabled();
        if (gate == EngineExportProgress::Cancelled) ImGui::TextWrapped(tr("正在取消，等待当前帧或图集写入完成..."));
        ImGui::Separator();
    }
    ImGui::BeginDisabled(engineJob_ != nullptr);
    if (ImGui::Button(tr("全选"))) { for (auto& clip : options.clips) clip.selected = true; budgetChanged = true; }
    ImGui::SameLine();
    if (ImGui::Button(tr("全不选"))) { for (auto& clip : options.clips) clip.selected = false; budgetChanged = true; }
    ImGui::BeginChild("engine-clips", ImVec2(0, font * std::min(7.0f, float(options.clips.size()) * 1.7f + 2.1f)), ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("clips", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(tr("动画"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("循环播放"), ImGuiTableColumnFlags_WidthFixed, font * 7);
        ImGui::TableHeadersRow();
        for (size_t a = 0; a < options.clips.size(); ++a) {
            auto& clip = options.clips[a];
            ImGui::PushID((int)a);
            ImGui::TableNextColumn();
            budgetChanged |= ImGui::Checkbox("##selected", &clip.selected);
            ImGui::SameLine(); ImGui::TextUnformatted(clip.name.c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!clip.selected);
            changed |= ImGui::Checkbox("##loop", &clip.loop);
            ImGui::EndDisabled(); ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::PushItemWidth(font * 9);
    budgetChanged |= ImGui::DragFloat(tr("帧率"), &options.fps, 1, 1, 120, "%.0f FPS", ImGuiSliderFlags_AlwaysClamp);
    budgetChanged |= ImGui::DragFloat(tr("导出比例"), &options.scale, 0.05f, 0.05f, 4, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::DragFloat(tr("Unity 每单位像素"), &options.pixelsPerUnit, 1, 1, 10000, "%.0f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::BeginDisabled(sk_.effects.pixel.enabled);
    budgetChanged |= ImGui::SliderInt(tr("超采样"), &options.supersample, 1, 4, "%dx");
    ImGui::EndDisabled();
    if (sk_.effects.pixel.enabled) ImGui::TextWrapped(tr("像素模式已固定为 1x 采样。"));
    budgetChanged |= ImGui::SliderInt(tr("留边"), &options.padding, 0, 128, "%d px");
    ImGui::PopItemWidth();
    ImGui::TextUnformatted(tr("输出路径"));
    ImGui::SetNextItemWidth(-font * 6);
    changed |= ImGui::InputText("##engine-path", &options.path);
    ImGui::SameLine();
    if (ImGui::Button(tr("浏览..."))) {
        auto path = saveFileDialog("Poseweaver frames (*.pwframes)", "*.pwframes", "pwframes");
        if (!path.empty()) { options.path = path; changed = true; }
    }
    ImGui::EndDisabled();
    if (budgetChanged) engineBudget_ = {};
    if (changed || budgetChanged) {
        engineNotice_.clear();
        engineSettingsDirty_ = true; engineSettingsDelay_ = 0.5f;
    }
    ImGui::TextWrapped(projectPath_.empty() ? tr("保存工程后会按工程记住导出配置。") : tr("配置按工程自动记住，不影响工程内容和撤销记录。"));
    ImGui::Separator();
    const bool selected = std::any_of(options.clips.begin(), options.clips.end(), [](const EngineClipOption& clip) { return clip.selected; });
    if (!selected) ImGui::TextWrapped(tr("请至少选择一个动作。"));
    if (engineBudget_.frames > 0) {
        const auto& b = engineBudget_;
        ImGui::Text(tr("%d 个动作 / %d 帧 / 每帧 %d × %d / %d 页图集"), b.animations, b.frames, b.width, b.height, b.pages);
        ImGui::Text(tr("未压缩图集 %.2f MiB；绘制缓冲约 %.2f MiB"), b.atlasBytes / 1048576.0, b.rasterBytes / 1048576.0);
        ImGui::TextWrapped(tr("预算不含图片副本、颜色查找缓存、PNG 编码和引擎开销；压缩后文件大小以实际导出为准。"));
    } else if (!engineJob_) ImGui::TextWrapped(tr("计算资源预算可查看帧尺寸、图集页数及内存占用。"));
    if (!engineJob_) {
        if (!engineNotice_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, engineNoticeError_ ? ImVec4(1, 0.55f, 0.5f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Text));
            ImGui::TextWrapped("%s", engineNotice_.c_str()); ImGui::PopStyleColor();
        }
        ImGui::BeginDisabled(!selected);
        if (ImGui::Button(tr("计算资源预算"))) startEngineExport(true);
        ImGui::SameLine();
        ImGui::BeginDisabled(options.path.empty());
        if (ImGui::Button(tr("导出"))) startEngineExport(false);
        ImGui::EndDisabled(); ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("关闭"))) { saveEngineExportSettings(); ImGui::CloseCurrentPopup(); }
    }
    ImGui::Spacing();
    ImGui::TextWrapped(tr("骨骼、换装和事件音频不随帧动画导出；混合效果烘焙在图片中。"));
    ImGui::EndPopup();
}
