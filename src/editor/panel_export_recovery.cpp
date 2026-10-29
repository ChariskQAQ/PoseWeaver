#include "editor/editor.h"
#include "editor/i18n.h"
#include <filesystem>
#include <algorithm>
void Editor::refreshExportRecoveries() {
    if (exportRecoveryTask_.valid()) return;
    try {
        exportRecoveries_ = ExportTransaction::pending(std::filesystem::u8path(configFile("export-recovery")));
        if (!exportRecoveries_.empty()) showExportRecovery_ = true;
    } catch (const std::exception& error) { reportIssue("EXPORT_RECOVERY_FAILED",error.what(),rig::Severity::Warning,configFile("export-recovery")); }
}
bool Editor::startExportRecovery(int index) {
    if (exportRecoveryTask_.valid() || imageJob_ || engineJob_ || index<0 || index>=(int)exportRecoveries_.size()) return false;
    try {
        auto recovery=exportRecoveries_[index];
        exportRecoveryEntry_=recovery.entry;
        exportRecoveryTask_=std::async(std::launch::async,[recovery] {
            std::string error; ExportTransaction::recover(recovery,error); return error;
        });
        return true;
    } catch (const std::exception& error) { return failOperation("EXPORT_RECOVERY_FAILED",error.what()); }
}
void Editor::pollExportRecovery() {
    if (!exportRecoveryTask_.valid() || exportRecoveryTask_.wait_for(std::chrono::seconds(0))!=std::future_status::ready) return;
    std::string error;
    try { error=exportRecoveryTask_.get(); } catch (const std::exception& e) { error=e.what(); }
    if (error.empty()) {
        exportRecoveries_.erase(std::remove_if(exportRecoveries_.begin(),exportRecoveries_.end(),[&](const ExportRecovery& r) { return r.entry==exportRecoveryEntry_; }),exportRecoveries_.end());
        dismissIssue("EXPORT_RECOVERY_FAILED"); setStatus(tr("导出恢复处理完成。"));
    } else {
        for (auto& r : exportRecoveries_) if (r.entry==exportRecoveryEntry_) r.error=error;
        failOperation("EXPORT_RECOVERY_FAILED",error,exportRecoveryEntry_);
    }
}
void Editor::drawExportRecovery() {
    if (!showExportRecovery_) return;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize()*36,0),ImGuiCond_Appearing);
    const auto size=ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSizeConstraints(ImVec2(0,0),ImVec2(size.x-24,size.y-24));
    if (ImGui::Begin(tr("导出中断恢复###ExportRecovery"),&showExportRecovery_,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(tr("发现上次未处理的导出记录。恢复前会检查文件；输出被修改时会保留现场并停止。"));
        if (exportRecoveryTask_.valid()) ImGui::TextWrapped(tr("正在检查并恢复输出，请稍候..."));
        if (exportRecoveries_.empty()) ImGui::TextUnformatted(tr("没有待恢复的导出。"));
        for (int i=0;i<(int)exportRecoveries_.size();++i) {
            auto& recovery=exportRecoveries_[i]; ImGui::PushID(i); ImGui::Separator();
            ImGui::TextWrapped("%s",recovery.directory.empty() ? recovery.entry.c_str() : recovery.directory.c_str());
            ImGui::TextWrapped(recovery.committed ? tr("导出已经完成，仅需清理备份。") : tr("恢复会撤回这次导出，找回上一次完整输出。"));
            if (!recovery.error.empty()) ImGui::TextWrapped("%s",recovery.error.c_str());
            ImGui::BeginDisabled(bool(imageJob_) || bool(engineJob_) || exportRecoveryTask_.valid());
            if (ImGui::Button(recovery.committed ? tr("清理已完成导出的备份") : tr("恢复上次完整输出"))) {
                startExportRecovery(i);
            }
            ImGui::EndDisabled(); ImGui::PopID();
        }
        if (ImGui::Button(tr("重新检查"))) refreshExportRecoveries();
        ImGui::SameLine(); if (ImGui::Button(tr("稍后处理"))) showExportRecovery_=false;
    }
    ImGui::End();
}
