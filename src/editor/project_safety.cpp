#include "editor/editor.h"
#include "editor/i18n.h"
#include "core/file_io.h"
#include "core/serialize.h"
#include "platform/file_dialog.h"

#include <json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace rig;

std::string Editor::recoveryPath() const { return configFile("autosave.rig.json"); }

bool Editor::saveRecovery() {
    if (!dirty_ || !recoveryPending_) return true;
    // An unhandled recovery file belongs to the previous session.
    if (recoveryAvailable_ && !recoveryOwned_) return false;
    std::string error;
    if (!validateSkeleton(sk_, &error)) {
        reportIssue("RECOVERY_FAILED", error, Severity::Error, recoveryPath());
        return false;
    }
    try {
        nlohmann::json document = nlohmann::json::parse(toJson(sk_));
        document["_recoverySource"] = projectPath_;
        if (!writeFileAtomic(recoveryPath(), document.dump(2), &error)) {
            reportIssue("RECOVERY_FAILED", error, Severity::Error, recoveryPath());
            return false;
        }
    } catch (const std::exception& e) {
        reportIssue("RECOVERY_FAILED", e.what(), Severity::Error, recoveryPath());
        return false;
    }
    recoveryPending_ = false;
    recoveryOwned_ = true;
    dismissIssue("RECOVERY_FAILED");
    setStatus(tr("已自动保存恢复副本"));
    return true;
}

void Editor::updateRecovery(float dt, bool editing) {
    if (closeConfirmed_ || !dirty_ || !recoveryPending_) return;
    recoveryElapsed_ += dt;
    if (recoveryElapsed_ < 30 || editing || recoveryAvailable_) return;
    recoveryElapsed_ = 0;
    saveRecovery();
}

void Editor::clearRecovery() {
    if (!recoveryOwned_) return;
    std::error_code error;
    fs::remove(fs::u8path(recoveryPath()), error);
    if (error) {
        reportIssue("RECOVERY_CLEANUP_FAILED", error.message(), Severity::Warning, recoveryPath());
        return;
    }
    recoveryOwned_ = recoveryAvailable_ = false;
    recoveryElapsed_ = 0;
    dismissIssue("RECOVERY_CLEANUP_FAILED");
}

bool Editor::restoreRecovery() {
    Skeleton recovered;
    std::string error;
    if (!loadFile(recoveryPath(), recovered, &error)) {
        recoveryError_ = error;
        reportIssue("RECOVERY_FAILED", error, Severity::Error, recoveryPath());
        return false;
    }
    return requestDocumentChange([this, recovered = std::move(recovered)]() mutable {
        resetProject();
        sk_ = std::move(recovered);
        // Recovery is a copy. Its first explicit save asks for a destination.
        projectPath_.clear();
        reloadTextures();
        selectBone(sk_.bones.empty() ? -1 : 0);
        anim_ = sk_.animations.empty() ? -1 : 0;
        if (!sk_.skins.empty()) activeSkins_ = {0};
        markUnsaved();
        recoveryOwned_ = true;
        recoveryAvailable_ = false;
        showRecoveryDialog_ = splashRequest_ = false;
        recoveryError_.clear();
        frameAllIn_ = 5;
        issuesDirty_ = true;
        setStatus(tr("已恢复自动保存的工程，请另存为正式工程"));
        return true;
    }, true);
}

void Editor::drawRecoveryDialog() {
    if (showRecoveryDialog_) {
        ImGui::OpenPopup(tr("发现自动恢复副本###Recovery"));
        showRecoveryDialog_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(tr("发现自动恢复副本###Recovery"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted(tr("上次编辑留下了尚未正式保存的恢复副本。"));
    ImGui::TextUnformatted(tr("恢复后可检查内容，再另存为正式工程。"));
    if (!recoveryError_.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 480 * uiScale_);
        ImGui::TextColored(ImVec4(1, 0.55f, 0.5f, 1), "%s", tr("恢复失败，副本已保留。可保留副本退出，稍后排查。"));
        ImGui::TextWrapped("%s", recoveryError_.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();
    if (ImGui::Button(tr("恢复工程"))) {
        if (restoreRecovery() || pendingDocumentChange_) ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("丢弃恢复副本"))) {
        recoveryOwned_ = true;
        clearRecovery();
        if (!recoveryOwned_) ImGui::CloseCurrentPopup();
        else recoveryError_ = tr("无法删除恢复副本，请检查目录权限。");
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("保留副本并退出"))) closeConfirmed_ = true;
    ImGui::EndPopup();
}

void Editor::newFromImageDialog() {
    const std::string path = openFileDialog(tr("图片 (png/jpg/bmp/tga)"), "*.png;*.jpg;*.jpeg;*.bmp;*.tga");
    if (path.empty()) return;
    Texture probe;
    std::string error;
    if (!loadTexture(path, probe, &error)) { failOperation("IMAGE_IMPORT_FAILED", tr("无法读取图片: ") + error, path); return; }
    releaseTexture(probe);
    requestDocumentChange([this, path] { resetProject(); return importImage(path); });
}

void Editor::newFromPsd() {
    requestDocumentChange([this] { resetProject(); io_.open = IoState::Psd; return true; });
}

void Editor::restoreSession() {
    requestDocumentChange([this] {
        if (!openProjectNow(sessionPath())) return false;
        projectPath_.clear();
        markUnsaved();
        setStatus(tr("已恢复上次的会话"));
        return true;
    });
}
