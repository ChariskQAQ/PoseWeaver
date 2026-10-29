#include "editor/editor.h"
#include "editor/i18n.h"

#include <algorithm>
#include <filesystem>
#include <sstream>

using namespace rig;

namespace {
const char* issueMessage(const Diagnostic& d) {
    if (d.target == IssueTarget::File && !d.message.empty()) return d.message.c_str();
    if (d.code == "MISSING_IMAGE") return tr("找不到图片文件，请重新定位资源。");
    if (d.code == "MISSING_AUDIO") return tr("找不到音频文件，请检查事件中的音频路径。");
    if (d.code == "IMAGE_LOAD_FAILED") return tr("图片无法解码，请替换资源后重新加载。");
    if (d.code == "INVALID_REFERENCE") return tr("对象引用无效，请检查骨骼、插槽或附件。");
    if (d.code == "INVALID_BLEND_MODE") return tr("插槽混合模式无效，请重新选择。");
    if (d.code == "INVALID_RENDER_EFFECTS") return tr("光源或像素设置无效，请检查数值与色板。");
    if (d.code == "GPU_EFFECTS_FALLBACK") return tr("GPU 预览失败，当前使用 CPU；切换选项可重试，详情见问题面板。");
    if (d.code == "UNDO_BUDGET") return tr("撤销历史达到预算，已移除最远的记录；超出预算的单步无法撤销。");
    if (d.code == "NON_FINITE") return tr("数值无效，请检查位置、变换和关键帧。");
    if (d.code == "BONE_CYCLE") return tr("骨骼层级存在循环，请调整父级。");
    if (d.code == "MESH_TOPOLOGY") return tr("网格顶点或三角形数据不完整。");
    if (d.code == "INVALID_WEIGHT") return tr("顶点权重或绑定骨骼无效。");
    if (d.code == "WEIGHT_SUM") return tr("顶点权重总和不为 1，请重新设置权重。");
    if (d.code == "DUPLICATE_WEIGHT") return tr("顶点重复绑定同一骨骼，请重新绑定。");
    if (d.code == "INVALID_KEY") return tr("动画时间、数值或曲线无效。");
    if (d.code == "DEFORM_TOPOLOGY") return tr("变形关键帧与附件的顶点数量不一致。");
    if (d.code == "DRAW_ORDER") return tr("绘制顺序没有完整包含所有插槽。");
    if (d.code == "OPEN_FAILED") return tr("打开失败，当前工程已保留。");
    if (d.code == "SAVE_FAILED") return tr("保存失败，原文件已保留。请检查位置、权限或磁盘空间。");
    if (d.code == "RECOVERY_FAILED") return tr("自动恢复副本无法读写，请检查恢复目录和工程数据。");
    if (d.code == "RECOVERY_CLEANUP_FAILED") return tr("恢复副本未能清理，请检查目录权限。");
    if (d.code == "SETTINGS_FAILED") return tr("用户设置保存失败，请检查目录权限。");
    if (d.code == "SESSION_FAILED") return tr("上次会话保存失败，请检查目录权限。");
    if (d.code == "EXPORT_FAILED") return tr("导出已停止，请先解决图片或数据问题。");
    if (d.code == "DEFORM_KEYS_REMOVED") return tr("顶点数量变化，已移除不匹配的变形关键帧；可撤销恢复。");
    return d.message.c_str();
}

std::string issueObject(const Skeleton& sk, const Diagnostic& d) {
    auto valid = [](int i, size_t n) { return i >= 0 && (size_t)i < n; };
    switch (d.target) {
    case IssueTarget::Bone: if (valid(d.index, sk.bones.size())) return sk.bones[d.index].name; break;
    case IssueTarget::Slot: if (valid(d.index, sk.slots.size())) return sk.slots[d.index].name; break;
    case IssueTarget::Attachment:
        if (valid(d.index, sk.slots.size()) && valid(d.sub, sk.slots[d.index].attachments.size()))
            return sk.slots[d.index].attachments[d.sub].name;
        break;
    case IssueTarget::Skin: if (valid(d.index, sk.skins.size())) return sk.skins[d.index].name; break;
    case IssueTarget::Animation: if (valid(d.index, sk.animations.size())) return sk.animations[d.index].name; break;
    case IssueTarget::Constraint: if (valid(d.index, sk.constraints.size())) return sk.constraints[d.index].name; break;
    case IssueTarget::Image:
        if (valid(d.index, sk.images.size())) return std::filesystem::u8path(sk.images[d.index].path).filename().u8string();
        break;
    case IssueTarget::Event: if (valid(d.index, sk.events.size())) return sk.events[d.index].name; break;
    case IssueTarget::File: return d.path.empty() ? tr("文件操作") : std::filesystem::u8path(d.path).filename().u8string();
    default: break;
    }
    return tr("工程");
}

std::string issueKey(const Skeleton& sk, const Diagnostic& d) {
    std::ostringstream key;
    key << d.code << ':' << (int)d.target << ':' << d.index << ':' << d.sub << ':' << d.vertex << ':'
        << d.animation << ':' << d.time << ':' << d.path << ':' << issueObject(sk, d);
    return key.str();
}
} // namespace

void Editor::reportIssue(const std::string& code, const std::string& detail, Severity severity, const std::string& path) {
    Diagnostic issue{severity, code, {}, detail};
    issue.path = path;
    if (!path.empty()) issue.target = IssueTarget::File;
    reportIssue(std::move(issue));
}

void Editor::reportIssue(Diagnostic issue) {
    const bool first = std::none_of(sessionIssues_.begin(), sessionIssues_.end(), [&](const Diagnostic& d) { return d.code == issue.code; });
    dismissIssue(issue.code);
    selectedIssueKey_ = issueKey(sk_, issue);
    sessionIssues_.push_back(std::move(issue));
    issuesDirty_ = true;
    issuesRefreshIn_ = 0;
    if (first) openIssues();
}

bool Editor::failOperation(const std::string& code, const std::string& message, const std::string& path,
                           IssueTarget target, int index, int sub) {
    Diagnostic issue{Severity::Error, code, message, {}, target, index, sub};
    issue.path = path;
    setStatus(message);
    reportIssue(std::move(issue));
    openIssues();
    return false;
}

void Editor::dismissIssue(const std::string& code) {
    sessionIssues_.erase(std::remove_if(sessionIssues_.begin(), sessionIssues_.end(),
                                      [&](const Diagnostic& d) { return d.code == code; }), sessionIssues_.end());
    issuesDirty_ = true;
    issuesRefreshIn_ = 0;
}

void Editor::refreshIssues() {
    const std::string directory = projectPath_.empty() ? std::string() : std::filesystem::u8path(projectPath_).parent_path().u8string();
    issues_ = diagnose(sk_, directory);
    for (int i = 0; i < (int)textures_.size() && i < (int)sk_.images.size(); ++i) {
        std::error_code ec;
        if (textures_[i].rgba().empty() && std::filesystem::is_regular_file(std::filesystem::u8path(sk_.images[i].path), ec))
            issues_.push_back({Severity::Warning, "IMAGE_LOAD_FAILED", {}, {}, IssueTarget::Image, i, -1, -1, sk_.images[i].path});
    }
    issues_.insert(issues_.end(), sessionIssues_.begin(), sessionIssues_.end());
    std::stable_sort(issues_.begin(), issues_.end(), [](const Diagnostic& a, const Diagnostic& b) {
        return a.severity == Severity::Error && b.severity != Severity::Error;
    });
    issuesDirty_ = false;
    issuesRefreshIn_ = 0.5f;
    if (std::none_of(issues_.begin(), issues_.end(), [&](const Diagnostic& d) { return issueKey(sk_, d) == selectedIssueKey_; }))
        selectedIssueKey_ = issues_.empty() ? std::string() : issueKey(sk_, issues_.front());
}

bool Editor::checkExportResources() {
    auto blocked = [&](Diagnostic d) {
        d.detail = std::string(issueMessage(d)) + (d.detail.empty() ? std::string() : "\n" + d.detail);
        d.code = "EXPORT_FAILED";
        d.message.clear();
        d.severity = Severity::Error;
        reportIssue(std::move(d));
        openIssues();
        setStatus(tr("导出已停止，请先解决图片或数据问题。"));
        return false;
    };
    const std::string directory = projectPath_.empty() ? std::string() : std::filesystem::u8path(projectPath_).parent_path().u8string();
    for (const auto& d : diagnose(sk_, directory))
        if (d.severity == Severity::Error || d.code == "MISSING_IMAGE") return blocked(d);
    for (int i=0;i<(int)textures_.size() && i<(int)sk_.images.size();++i) {
        std::error_code error;
        if (textures_[i].rgba().empty() && std::filesystem::is_regular_file(std::filesystem::u8path(sk_.images[i].path),error))
            return blocked({Severity::Warning,"IMAGE_LOAD_FAILED",{},{},IssueTarget::Image,i,-1,-1,sk_.images[i].path});
    }
    dismissIssue("EXPORT_FAILED");
    return true;
}

bool Editor::canFocusIssue(const Diagnostic& d) const {
    auto valid = [](int i, size_t n) { return i >= 0 && (size_t)i < n; };
    if (valid(d.animation, sk_.animations.size())) return true;
    switch (d.target) {
    case IssueTarget::Skeleton: return true;
    case IssueTarget::Bone: return valid(d.index, sk_.bones.size());
    case IssueTarget::Slot: return valid(d.index, sk_.slots.size());
    case IssueTarget::Attachment: return valid(d.index, sk_.slots.size()) && valid(d.sub, sk_.slots[d.index].attachments.size());
    case IssueTarget::Skin: return valid(d.index, sk_.skins.size());
    case IssueTarget::Animation: return valid(d.index, sk_.animations.size());
    case IssueTarget::Constraint: return valid(d.index, sk_.constraints.size());
    case IssueTarget::Image: return valid(d.index, sk_.images.size());
    case IssueTarget::Event: return valid(d.index, sk_.events.size());
    default: return false;
    }
}

bool Editor::focusIssue(const Diagnostic& d) {
    if (!canFocusIssue(d)) return false;
    auto valid = [](int i, size_t n) { return i >= 0 && (size_t)i < n; };
    playing_ = false;
    if (valid(d.animation, sk_.animations.size())) {
        selectAnimation(d.animation);
        if (std::isfinite(d.time) && d.time >= 0) setTime(d.time);
    }
    switch (d.target) {
    case IssueTarget::Bone: if (valid(d.index, sk_.bones.size())) selectBone(d.index); break;
    case IssueTarget::Slot: if (valid(d.index, sk_.slots.size())) selectSlot(d.index); break;
    case IssueTarget::Attachment:
        if (!valid(d.index, sk_.slots.size()) || !valid(d.sub, sk_.slots[d.index].attachments.size())) break;
        selectAttachment(d.index, d.sub);
        if (const auto* a = selectedAttachment()) if (d.vertex >= 0 && (size_t)d.vertex < a->vertexCount()) sel_.vertices = {d.vertex};
        break;
    case IssueTarget::Skin:
        if (!valid(d.index, sk_.skins.size())) break;
        selectSkin(d.index);
        if (valid(d.index, sk_.skins.size()) && valid(d.sub, sk_.skins[d.index].attachments.size())) {
            const auto& entry = sk_.skins[d.index].attachments[d.sub];
            if (valid(entry.slot, sk_.slots.size())) {
                activeSkins_.erase(std::remove(activeSkins_.begin(), activeSkins_.end(), d.index), activeSkins_.end());
                activeSkins_.push_back(d.index);
                const auto& attachments = sk_.slots[entry.slot].attachments;
                for (int a = 0; a < (int)attachments.size(); ++a)
                    if (attachments[a].skinPlaceholder && attachments[a].name == entry.placeholder) {
                        selectAttachment(entry.slot, a);
                        if (d.vertex >= 0 && (size_t)d.vertex < entry.attachment.vertexCount()) sel_.vertices = {d.vertex};
                        break;
                    }
            }
        }
        break;
    case IssueTarget::Animation: if (valid(d.index, sk_.animations.size())) selectAnimation(d.index); break;
    case IssueTarget::Constraint: if (valid(d.index, sk_.constraints.size())) selectConstraint(d.index); break;
    case IssueTarget::Event: if (valid(d.index, sk_.events.size())) selectEvent(d.index); break;
    case IssueTarget::Image:
        if (valid(d.index, sk_.images.size())) { sel_.clear(); sel_.kind = Selection::Image; sel_.index = d.index; }
        break;
    default: sel_.clear(); sel_.kind = Selection::Skeleton; break;
    }
    if (d.vertex >= 0) {
        if (d.animation < 0) setMode(Mode::Setup);
        setTool(Tool::MeshEdit);
    }
    treeFilter_.clear();
    treeShowBones_ = treeShowSlots_ = treeShowAttachments_ = true;
    scrollTreeToSelection_ = true;
    return true;
}

void Editor::drawIssues() {
    issuesRefreshIn_ -= ImGui::GetIO().DeltaTime;
    if ((issuesDirty_ || showIssues_) && issuesRefreshIn_ <= 0) refreshIssues();
    if (!showIssues_) return;
    ImGui::SetNextWindowSize(ImVec2(660 * uiScale_, 300 * uiScale_), ImGuiCond_FirstUseEver);
    if (focusIssues_ > 0) { ImGui::SetNextWindowFocus(); --focusIssues_; }
    if (!ImGui::Begin(tr("问题###Issues"), &showIssues_)) { ImGui::End(); return; }
    const int errors = (int)std::count_if(issues_.begin(), issues_.end(), [](const Diagnostic& d) { return d.severity == Severity::Error; });
    ImGui::Text(tr("%d 个错误，%d 个警告"), errors, (int)issues_.size() - errors);
    if (ImGui::Button(tr("批量重定位资源..."))) showResourceRelink_ = true;
    if (ImGui::Button(tr("重新加载并检查"))) { reloadTextures(); refreshIssues(); }
    ImGui::SameLine();
    ImGui::BeginDisabled(sessionIssues_.empty());
    if (ImGui::Button(tr("清除操作记录"))) { sessionIssues_.clear(); refreshIssues(); }
    ImGui::EndDisabled();
    if (ImGui::GetContentRegionAvail().x > 120 * uiScale_) ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * uiScale_);
    const char* filters[] = {tr("全部"), tr("仅错误"), tr("仅警告")};
    ImGui::Combo("##issueFilter", &issueFilter_, filters, 3);
    ImGui::TextWrapped("%s", tr("点击问题定位对象；下方可查看和复制完整详情。"));
    std::vector<int> visible;
    const Diagnostic* selected = nullptr;
    for (int i = 0; i < (int)issues_.size(); ++i) {
        if (issueFilter_ == 0 || (issueFilter_ == 1) == (issues_[i].severity == Severity::Error)) {
            visible.push_back(i);
            if (issueKey(sk_, issues_[i]) == selectedIssueKey_) selected = &issues_[i];
        }
    }
    if (visible.empty()) {
        ImGui::Spacing();
        ImGui::TextUnformatted(issues_.empty() ? tr("检查完成，未发现问题。") : tr("此筛选下没有问题。"));
    } else {
        const float available = ImGui::GetContentRegionAvail().y;
        const float detailHeight = selected ? std::min(150 * uiScale_, available * 0.5f) : 0;
        if (ImGui::BeginTable("##issueList", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                              ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                                              ImVec2(0, std::max(1.0f, available - detailHeight)))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(tr("级别"), ImGuiTableColumnFlags_WidthFixed, 65 * uiScale_);
            ImGui::TableSetupColumn(tr("对象"), ImGuiTableColumnFlags_WidthStretch, 0.25f);
            ImGui::TableSetupColumn(tr("问题"), ImGuiTableColumnFlags_WidthStretch, 0.75f);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin((int)visible.size());
            while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto& d = issues_[visible[row]];
                ImGui::PushID(visible[row]);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, d.severity == Severity::Error ? ImVec4(1, 0.55f, 0.5f, 1) : ImVec4(1, 0.8f, 0.35f, 1));
                if (ImGui::Selectable(d.severity == Severity::Error ? tr("错误") : tr("警告"), selected == &d,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    selectedIssueKey_ = issueKey(sk_, d);
                    selected = &d;
                    focusIssue(d);
                }
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35);
                    ImGui::TextUnformatted(issueMessage(d));
                    if (!d.path.empty()) ImGui::TextUnformatted(d.path.c_str());
                    if (!d.detail.empty()) ImGui::TextUnformatted(d.detail.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(issueObject(sk_, d).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(issueMessage(d));
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (selected) {
            const auto& d = *selected;
            ImGui::BeginChild("##issueDetails", ImVec2(0, 0), ImGuiChildFlags_Borders);
            ImGui::BeginDisabled(!canFocusIssue(d));
            if (ImGui::SmallButton(tr("定位对象"))) focusIssue(d);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("复制详情"))) {
                std::ostringstream text;
                text << d.code << " | " << issueObject(sk_, d) << "\n" << issueMessage(d);
                if (!d.path.empty()) text << "\n" << d.path;
                if (!d.detail.empty()) text << "\n" << d.detail;
                if (d.animation >= 0 && d.animation < (int)sk_.animations.size()) text << "\n" << sk_.animations[d.animation].name;
                if (d.time >= 0) text << "\n" << tr("时间（秒）") << ": " << d.time;
                if (d.vertex >= 0) text << "\n" << tr("顶点") << ": " << d.vertex;
                ImGui::SetClipboardText(text.str().c_str());
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", d.code.c_str());
            ImGui::TextWrapped("%s", issueMessage(d));
            if (d.animation >= 0 && d.animation < (int)sk_.animations.size())
                ImGui::TextWrapped(tr("动画：%s"), sk_.animations[d.animation].name.c_str());
            if (d.time >= 0) ImGui::Text(tr("时间：%.3f 秒（第 %.0f 帧）"), d.time, d.time * fps_);
            if (d.vertex >= 0) ImGui::Text(tr("顶点 %d"), d.vertex);
            if (!d.path.empty()) ImGui::TextWrapped("%s", d.path.c_str());
            if (!d.detail.empty()) ImGui::TextWrapped("%s", d.detail.c_str());
            ImGui::EndChild();
        }
    }
    ImGui::End();
}
