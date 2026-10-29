#include "editor/editor.h"
#include "editor/i18n.h"
#include "platform/file_dialog.h"
#include <filesystem>
#include <algorithm>
namespace fs = std::filesystem;

bool Editor::scanResourceMatches(const std::string& directory) {
    if (resourceScan_) return false;
    try {
        auto job = std::make_unique<ResourceScanJob>();
        rig::Skeleton resources; resources.images = sk_.images; resources.events = sk_.events;
        auto* progress = &job->progress;
        job->result = std::async(std::launch::async,[resources = std::move(resources),directory,progress] {
            return rig::findMissingResources(resources,directory,progress);
        });
        resourceScan_ = std::move(job);
        resourceMatches_.clear(); resourceScanNotice_.clear(); resourceRelinkDirectory_ = directory;
        showResourceRelink_ = true; dismissIssue("RESOURCE_RELINK_FAILED"); return true;
    } catch (const std::exception& error) { return failOperation("RESOURCE_RELINK_FAILED",error.what(),directory); }
}

void Editor::pollResourceScan() {
    if (!resourceScan_ || resourceScan_->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    try {
        auto matches = resourceScan_->result.get();
        if (resourceScan_->progress.cancelled) resourceScanNotice_ = tr("已取消扫描，工程未改变。");
        else resourceMatches_ = std::move(matches);
    } catch (const std::exception& error) {
        if (resourceScan_->progress.cancelled) resourceScanNotice_ = tr("已取消扫描，工程未改变。");
        else { resourceScanNotice_ = tr("扫描失败，请检查目录或选择较小的资源目录。"); failOperation("RESOURCE_RELINK_FAILED",error.what(),resourceRelinkDirectory_); }
    }
    resourceScan_.reset();
}

void Editor::resetResourceScan() {
    if (!resourceScan_) return;
    resourceScan_->progress.cancelled = true;
    resourceScan_->result.wait(); resourceScan_.reset();
}

bool Editor::applyResourceMatches() {
    if (resourceScan_) return false;
    rig::Skeleton proposed = sk_;
    AudioPlayer decoder(false);
    int count = 0;
    for (const auto& row : resourceMatches_) {
        if (row.selected < 0) continue;
        if (row.selected >= (int)row.candidates.size() || row.index < 0 ||
            (row.audio ? row.index >= (int)proposed.events.size() : row.index >= (int)proposed.images.size()))
            return failOperation("RESOURCE_RELINK_FAILED",tr("资源列表已变化，请重新扫描。"));
        const auto& current = row.audio ? sk_.events[row.index].audioPath : sk_.images[row.index].path;
        std::error_code error;
        if (current != row.original || fs::is_regular_file(fs::u8path(current),error)) return failOperation("RESOURCE_RELINK_FAILED",tr("资源列表已变化，请重新扫描。"));
        const auto& candidate = row.candidates[row.selected];
        if (row.audio) {
            if (!decoder.samples(candidate)) return failOperation("RESOURCE_RELINK_FAILED",tr("无法读取音频: ")+candidate,candidate);
            proposed.events[row.index].audioPath = candidate;
        } else {
            Texture probe; std::string message;
            if (!loadTexture(candidate,probe,&message)) return failOperation("RESOURCE_RELINK_FAILED",tr("无法读取图片: ")+message,candidate);
            proposed.images[row.index] = {candidate,probe.width,probe.height}; releaseTexture(probe);
        }
        ++count;
    }
    if (!count) return false;
    if (!commitDocumentChange(std::move(proposed))) return false;
    audio_->stopAll(); reloadTextures(); updatePose(); refreshIssues();
    resourceMatches_.erase(std::remove_if(resourceMatches_.begin(),resourceMatches_.end(),[](const rig::ResourceMatch& row) { return row.selected >= 0; }),resourceMatches_.end());
    dismissIssue("RESOURCE_RELINK_FAILED"); setStatus(tr("已批量修复资源，可整步撤销。")); return true;
}

void Editor::drawResourceRelink() {
    if (!showResourceRelink_) return;
    const float font = ImGui::GetFontSize();
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(std::min(font*43,viewport->WorkSize.x-32),std::min(font*27,viewport->WorkSize.y-32)),ImGuiCond_FirstUseEver);
    const bool expanded = ImGui::Begin(tr("批量重定位资源###ResourceRelink"),&showResourceRelink_);
    if (!showResourceRelink_ && resourceScan_) resourceScan_->progress.cancelled = true;
    if (!expanded) { ImGui::End(); return; }
    ImGui::BeginDisabled(bool(resourceScan_));
    if (ImGui::Button(tr("选择资源目录并扫描..."))) {
        const auto folder = openFolderDialog(tr("选择包含图片与音频的目录"));
        if (!folder.empty()) scanResourceMatches(folder);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool selected = std::any_of(resourceMatches_.begin(),resourceMatches_.end(),[](const rig::ResourceMatch& row) { return row.selected >= 0; });
    ImGui::BeginDisabled(!selected || bool(resourceScan_));
    if (ImGui::Button(tr("应用已选匹配"))) applyResourceMatches();
    ImGui::EndDisabled();
    ImGui::TextWrapped(tr("递归搜索缺失图片与音频；同名候选不自动选择。应用前检查全部文件，可整步撤销。"));
    if (!resourceRelinkDirectory_.empty()) ImGui::TextWrapped("%s",resourceRelinkDirectory_.c_str());
    if (resourceScan_) {
        ImGui::Text(tr("已扫描 %llu 个条目"),(unsigned long long)resourceScan_->progress.visited.load());
        if (ImGui::Button(tr("取消扫描"))) resourceScan_->progress.cancelled = true;
        if (resourceScan_->progress.cancelled) ImGui::TextWrapped(tr("正在取消，等待当前目录读取结束..."));
    } else if (!resourceScanNotice_.empty()) ImGui::TextWrapped("%s",resourceScanNotice_.c_str());
    else if (resourceMatches_.empty()) ImGui::TextUnformatted(resourceRelinkDirectory_.empty() ? tr("选择目录开始匹配。") : tr("没有待修复的缺失资源。"));
    else if (ImGui::BeginTable("matches",3,ImGuiTableFlags_RowBg|ImGuiTableFlags_Resizable|ImGuiTableFlags_ScrollY,ImVec2(0,-1))) {
        ImGui::TableSetupColumn(tr("类型"),ImGuiTableColumnFlags_WidthFixed,font*4);
        ImGui::TableSetupColumn(tr("缺失资源")); ImGui::TableSetupColumn(tr("替换为")); ImGui::TableHeadersRow();
        ImGuiListClipper clipper; clipper.Begin((int)resourceMatches_.size());
        while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            auto& row = resourceMatches_[i]; ImGui::PushID(i); ImGui::TableNextRow(); ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.audio ? tr("音频") : tr("图片")); ImGui::TableNextColumn();
            const auto filename = fs::u8path(row.original).filename().u8string();
            ImGui::TextUnformatted(filename.c_str()); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",row.original.c_str());
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
            auto relative = [&](const std::string& path) { return fs::u8path(path).lexically_relative(fs::absolute(fs::u8path(resourceRelinkDirectory_))).generic_u8string(); };
            const std::string label = row.selected >= 0 ? relative(row.candidates[row.selected]) : row.candidates.empty() ? tr("未匹配") : tr("跳过 / 请选择候选");
            if (ImGui::BeginCombo("##candidate",label.c_str())) {
                if (ImGui::Selectable(tr("跳过"),row.selected < 0)) row.selected = -1;
                for (int c = 0; c < (int)row.candidates.size(); ++c) {
                    const auto candidate = relative(row.candidates[c]);
                    if (ImGui::Selectable(candidate.c_str(),row.selected == c)) row.selected = c;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",row.candidates[c].c_str());
                }
                ImGui::EndCombo();
            }
            if (row.selected >= 0 && ImGui::IsItemHovered()) ImGui::SetTooltip("%s",row.candidates[row.selected].c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
