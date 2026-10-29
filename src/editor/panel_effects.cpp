#include "editor/editor.h"
#include "editor/i18n.h"
#include "core/raster.h"
#include <algorithm>
#include <cmath>

using namespace rig;

void Editor::setEffectsGpu(bool enabled) {
    settings_.effectsGpu = enabled;
    effectsCacheValid_ = effectsUsingGpu_ = false;
    effectsGpuError_.clear();
    effectsGpu_.release();
    dismissIssue("GPU_EFFECTS_FALLBACK");
    saveSettings();
}

bool Editor::setRenderEffects(const RenderEffects& effects, bool continuous) {
    std::string error;
    if (!validRenderEffects(effects, &error)) return failOperation("RENDER_EFFECTS_FAILED", error, {}, IssueTarget::Skeleton);
    if (effects == sk_.effects) return false;
    if (!continuous || !effectsEditing_) checkpoint();
    sk_.effects = effects; effectsEditing_ = continuous; effectsCacheValid_ = false;
    engineBudget_ = {};
    dismissIssue("RENDER_EFFECTS_FAILED");
    return true;
}

void Editor::drawRenderEffects() {
    if (!showEffects_) return;
    const float font = ImGui::GetFontSize();
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + font, viewport->WorkPos.y + font), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(font * 27, std::min(font * 39, viewport->WorkSize.y - font * 2)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(tr("光源与像素###RenderEffects"), &showEffects_)) { ImGui::End(); return; }
    if (!ImGui::IsAnyItemActive()) effectsEditing_ = false;
    RenderEffects value = sk_.effects;
    bool changed = false;
    ImGui::TextWrapped(tr("作用于整个角色；不修改源图片，随工程保存并烘焙到导出图片。"));
    int backend = settings_.effectsGpu ? 1 : 0;
    const char* backends[] = {tr("CPU 预览"), tr("GPU 预览")};
    if (ImGui::Combo(tr("效果预览"), &backend, backends, 2)) setEffectsGpu(backend == 1);
    ImGui::TextWrapped(tr("仅切换预览的效果处理；导出始终使用 CPU。"));
    if (!effectsGpuError_.empty()) ImGui::TextWrapped(tr("GPU 预览失败，当前使用 CPU；切换选项可重试，详情见问题面板。"));
    if (ImGui::Button(tr("恢复默认效果"))) { value = {}; changed = true; }
    changed |= ImGui::Checkbox(tr("启用 2D 点光源"), &value.light.enabled);
    ImGui::BeginDisabled(!value.light.enabled);
    float xy[] = {value.light.position.x, value.light.position.y};
    if (ImGui::DragFloat2(tr("光源位置"), xy, 1, -1000000, 1000000, "%.1f", ImGuiSliderFlags_AlwaysClamp)) { value.light.position = {xy[0], xy[1]}; changed = true; }
    changed |= ImGui::ColorEdit3(tr("光源颜色"), value.light.color.data());
    changed |= ImGui::DragFloat(tr("光照半径"), &value.light.radius, 1, 1, 100000, "%.1f", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::SliderFloat(tr("光源强度"), &value.light.intensity, 0, 8, "%.2f");
    changed |= ImGui::SliderFloat(tr("环境亮度"), &value.light.ambient, 0, 2, "%.2f");
    ImGui::EndDisabled();
    ImGui::TextWrapped(tr("平面光照按距离衰减，不产生法线凹凸或投影。"));
    ImGui::Separator();
    changed |= ImGui::Checkbox(tr("启用像素效果"), &value.pixel.enabled);
    ImGui::BeginDisabled(!value.pixel.enabled);
    changed |= ImGui::SliderInt(tr("像素网格"), &value.pixel.size, 1, 32, "%d");
    changed |= ImGui::Checkbox(tr("调色板限色"), &value.pixel.quantize);
    ImGui::BeginDisabled(!value.pixel.quantize);
    changed |= ImGui::Checkbox(tr("有序抖动"), &value.pixel.dither);
    ImGui::Text(tr("色板（%d 色）"), (int)value.pixel.palette.size());
    for (size_t i = 0; i < value.pixel.palette.size(); ++i) {
        if (i % 12) ImGui::SameLine(0, 3);
        ImGui::PushID((int)i);
        const auto& c = value.pixel.palette[i];
        if (ImGui::ColorButton("##swatch", ImVec4(c[0], c[1], c[2], 1), 0, ImVec2(font, font))) effectsColor_ = (int)i;
        ImGui::PopID();
    }
    effectsColor_ = std::clamp(effectsColor_, 0, (int)value.pixel.palette.size() - 1);
    changed |= ImGui::ColorEdit3(tr("色板颜色"), value.pixel.palette[effectsColor_].data());
    ImGui::BeginDisabled(value.pixel.palette.size() >= 256);
    if (ImGui::Button(tr("添加颜色"))) { value.pixel.palette.push_back({1,1,1}); effectsColor_ = (int)value.pixel.palette.size() - 1; changed = true; }
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(value.pixel.palette.size() <= 1);
    if (ImGui::Button(tr("删除颜色"))) { value.pixel.palette.erase(value.pixel.palette.begin() + effectsColor_); changed = true; }
    ImGui::EndDisabled();
    if (ImGui::Button(tr("默认色板"))) { value.pixel.palette = PixelEffects{}.palette; changed = true; }
    ImGui::SameLine();
    if (ImGui::Button(tr("四级灰度"))) { value.pixel.palette = {{{0,0,0}},{{0.333333f,0.333333f,0.333333f}},{{0.666667f,0.666667f,0.666667f}},{{1,1,1}}}; changed = true; }
    ImGui::EndDisabled();
    changed |= ImGui::Checkbox(tr("像素描边"), &value.pixel.outline);
    ImGui::BeginDisabled(!value.pixel.outline);
    changed |= ImGui::ColorEdit3(tr("描边颜色"), value.pixel.outlineColor.data());
    ImGui::EndDisabled(); ImGui::EndDisabled();
    ImGui::TextWrapped(tr("像素模式使用最近邻采样和硬边透明，不使用超采样；抖动固定在世界网格上。"));
    ImGui::Separator();
    if (changed) setRenderEffects(value, ImGui::IsAnyItemActive());
    ImGui::TextWrapped(tr("预览最长边为 2048 像素；导出按目标尺寸渲染。"));
    ImGui::End();
}

bool Editor::drawEffectsPreview(ImDrawList* list) {
    if (!validRenderEffects(sk_.effects)) return false;
    std::vector<float> key;
    key.reserve(pose_.world.size() * 6 + pose_.slotColor.size() * 5);
    for (const auto& m : pose_.world) key.insert(key.end(), {m.a,m.b,m.c,m.d,m.tx,m.ty});
    for (const auto& c : pose_.slotColor) key.insert(key.end(), {c[0],c[1],c[2],c[3]});
    for (int a : pose_.slotAttachment) key.push_back((float)a);
    for (int a : pose_.drawOrder) key.push_back((float)a);
    for (int a : pose_.boneActive) key.push_back((float)a);
    for (const auto& deform : pose_.deform) {
        key.insert(key.end(), {(float)deform.first.first, (float)deform.first.second, (float)deform.second.size()});
        for (auto v : deform.second) key.insert(key.end(), {v.x,v.y});
    }
    key.push_back((float)pose_.skins.size()); for (int a : pose_.skins) key.push_back((float)a);
    key.push_back((float)hiddenSlots_.size()); for (int a : hiddenSlots_) key.push_back((float)a);
    // Geometry can change within one drag without creating another history checkpoint.
    Geometry geometry;
    for (int s : pose_.drawOrder) {
        if (hiddenSlots_.count(s)) continue;
        const Attachment* attachment = poseAttachment(sk_,pose_,s);
        if (!attachment || (attachment->isPolygon() && attachment->type != AttachmentType::Clipping)) continue;
        key.insert(key.end(),{(float)s,(float)attachment->type,(float)attachment->image,(float)attachment->endSlot,(float)sk_.slots[s].blend});
        int a = s < (int)pose_.slotAttachment.size() ? pose_.slotAttachment[s] : -1;
        computeGeometry(sk_,pose_,sk_.slots[s],*attachment,geometry,findDeform(pose_,s,a));
        key.push_back((float)geometry.positions.size());
        for (auto p : geometry.positions) key.insert(key.end(),{p.x,p.y});
        key.push_back((float)geometry.uvs.size());
        for (auto uv : geometry.uvs) key.insert(key.end(),{uv.x,uv.y});
        key.push_back((float)geometry.triangles.size());
        for (int triangle : geometry.triangles) key.push_back((float)triangle);
    }
    if (!effectsCacheValid_ || !(effectsCached_ == sk_.effects) || key != effectsPoseKey_) {
        Pose pose = pose_;
        for (int s : hiddenSlots_) if (s >= 0 && s < (int)pose.slotAttachment.size()) pose.slotAttachment[s] = -1;
        Vec2 lo, hi;
        if (!poseBounds(sk_, pose, lo, hi)) return false;
        const float grid = sk_.effects.pixel.enabled ? (float)sk_.effects.pixel.size : 1;
        lo = {std::floor(lo.x / grid) * grid - grid * 2, std::floor(lo.y / grid) * grid - grid * 2};
        hi = {std::ceil(hi.x / grid) * grid + grid * 2, std::ceil(hi.y / grid) * grid + grid * 2};
        float scale = std::min({1.0f, 2048 / (hi.x - lo.x), 2048 / (hi.y - lo.y)});
        if (!std::isfinite(scale) || scale <= 1e-6f) return false;
        const int w = std::clamp((int)std::ceil((hi.x - lo.x) * scale), 1, 2048), h = std::clamp((int)std::ceil((hi.y - lo.y) * scale), 1, 2048);
        Affine matrix{scale,0,0,-scale,-lo.x * scale,hi.y * scale};
        std::vector<RasterTexture> textures;
        for (const auto& texture : textures_) textures.push_back({texture.width, texture.height, texture.rgba().empty() ? nullptr : texture.rgba().data()});
        std::vector<unsigned char> rgba;
        effectsUsingGpu_ = false;
        if (settings_.effectsGpu && effectsGpuError_.empty()) {
            renderPoseBase(sk_, pose, textures, matrix, w, h, rgba);
            effectsUsingGpu_ = effectsGpu_.render(rgba, w, h, matrix, sk_.effects, effectsGpuError_);
            if (effectsUsingGpu_) releaseTexture(effectsTexture_);
            else {
                reportIssue("GPU_EFFECTS_FALLBACK", effectsGpuError_, Severity::Warning);
                applyRenderEffects(rgba, w, h, matrix, sk_.effects);
            }
        } else renderPose(sk_, pose, textures, matrix, w, h, rgba);
        if (!effectsUsingGpu_ && !updateTexturePixels(effectsTexture_, w, h, std::move(rgba), sk_.effects.pixel.enabled)) return false;
        effectsLo_ = {lo.x, hi.y - h / scale}; effectsHi_ = {lo.x + w / scale, hi.y};
        effectsHistory_ = historyPosition_; effectsPoseKey_ = std::move(key); effectsCacheValid_ = true;
        effectsCached_ = sk_.effects;
    }
    const auto texture = effectsUsingGpu_ ? effectsGpu_.texture() : effectsTexture_.id;
    list->AddImage((ImTextureID)(intptr_t)texture, toScreen({effectsLo_.x, effectsHi_.y}), toScreen({effectsHi_.x, effectsLo_.y}));
    return true;
}
