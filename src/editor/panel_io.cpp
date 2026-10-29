// Import / export: merge projects, PSD import, runtime data, texture atlas, images and video.
#include "editor/editor.h"
#include "editor/i18n.h"

#include "core/atlas.h"
#include "core/merge.h"
#include "core/psd.h"
#include "core/raster.h"
#include "core/serialize.h"
#include "platform/file_dialog.h"

#include <stb_image_write.h>

#include <imgui_stdlib.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace rig;

namespace {
bool writePng(const std::string& path, int w, int h, const std::vector<unsigned char>& rgba) {
    return stbi_write_png(path.c_str(), w, h, 4, rgba.data(), w * 4) != 0;
}

// Image types of the export dialog (IoState::imageType).
bool isSequence(int t) { return t == 0 || t == 5; }
bool isVideo(int t) { return t == 3 || t == 4 || t == 6 || t == 7; }
bool isOpaque(int t) { return t == 3 || t == 5 || t == 6; }  // JPG, MP4 (yuv420p) and AVI (MJPEG) have no alpha

bool readBytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

bool writeBytes(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(fs::u8path(path), std::ios::binary);
    f.write((const char*)data.data(), (std::streamsize)data.size());
    return (bool)f;
}

std::string safeFileName(std::string name) {
    for (char& c : name)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    return name.empty() ? "layer" : name;
}

} // namespace

// ---------------------------------------------------------------- import

bool Editor::importMergePath(const std::string& path) {
    Skeleton src;
    std::string err;
    if (!loadFile(path, src, &err)) {
        return failOperation("IMPORT_FAILED", tr("导入失败: ") + err, path);
    }
    // Image paths in the imported file are relative to it.
    fs::path dir = fs::absolute(fs::u8path(path)).parent_path();
    for (auto& im : src.images) {
        fs::path p = fs::u8path(im.path);
        // Same normal form as importImage, so shared images are recognised by path.
        im.path = fs::absolute(p.is_absolute() ? p : dir / p).lexically_normal().u8string();
    }
    for (auto& event : src.events) if (!event.audioPath.empty()) {
        fs::path audio = fs::u8path(event.audioPath);
        event.audioPath = fs::absolute(audio.is_absolute() ? audio : dir / audio).lexically_normal().u8string();
    }
    checkpoint();
    std::string report = mergeSkeleton(sk_, src);
    reloadTextures();
    if (anim_ < 0 && !sk_.animations.empty()) anim_ = 0;
    updatePose();
    frameAllIn_ = 3;
    dismissIssue("IMPORT_FAILED");
    setStatus(report);
    return true;
}

bool Editor::importPsdPath(const std::string& path, bool includeHidden) {
    std::vector<uint8_t> bytes;
    PsdDocument doc;
    std::string err;
    if (!readBytes(path, bytes) || !parsePsd(bytes, doc, &err)) {
        return failOperation("PSD_IMPORT_FAILED", tr("PSD 读取失败: ") + (err.empty() ? std::string(tr("无法打开文件")) : err), path);
    }
    if (doc.layers.empty()) {
        return failOperation("PSD_IMPORT_FAILED", tr("PSD 里没有可导入的图层"), path);
    }
    fs::path psd = fs::u8path(path);
    fs::path outDir = psd.parent_path() / fs::u8path(psd.stem().u8string() + "_images");
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) return failOperation("PSD_IMPORT_FAILED", tr("PSD 读取失败: ") + ec.message(), outDir.u8string());
    dismissIssue("PSD_IMPORT_FAILED");
    dismissIssue("PSD_BLEND_UNSUPPORTED");
    checkpoint();
    if (sk_.bones.empty()) sk_.addBone("root", -1, {}, 0);
    int root = 0;
    Pose setup = makeSetupPose(sk_);
    int imported = 0;
    std::string unsupportedBlends;
    std::vector<std::string> used;
    for (const PsdLayer& layer : doc.layers) {
        if (!layer.visible && !includeHidden) continue;
        std::string base = safeFileName(layer.name);
        std::string file = base;
        for (int n = 2; std::find(used.begin(), used.end(), file) != used.end(); ++n) file = base + "_" + std::to_string(n);
        used.push_back(file);
        fs::path png = outDir / fs::u8path(file + ".png");
        if (!writePng(png.u8string(), layer.width, layer.height, layer.rgba)) {
            failOperation("PSD_IMPORT_FAILED", tr("写入图层图片失败，部分图层可能已导入。"), png.u8string());
            continue;
        }
        Texture tex;
        if (!loadTexture(png.u8string(), tex, &err)) {
            failOperation("PSD_IMPORT_FAILED", tr("无法读取图片: ") + err, png.u8string());
            continue;
        }
        sk_.images.push_back({fs::absolute(png).lexically_normal().u8string(), tex.width, tex.height});
        textures_.push_back(std::move(tex));
        Slot slot;
        slot.name = sk_.uniqueSlotName(layer.name.empty() ? "layer" : layer.name);
        slot.bone = root;
        slot.color[3] = layer.opacity;
        slot.blend = layer.blend;
        if (layer.blend == BlendMode::Normal && layer.blendKey != "norm")
            unsupportedBlends += (unsupportedBlends.empty() ? "" : "\n") + layer.name + " (" + layer.blendKey + ")";
        Attachment att;
        att.name = layer.name.empty() ? file : layer.name;
        att.image = (int)sk_.images.size() - 1;
        // Layer centre in world space (document centred on the origin, y up).
        float cx = layer.left + layer.width * 0.5f - doc.width * 0.5f;
        float cy = doc.height * 0.5f - (layer.top + layer.height * 0.5f);
        att.transform = decompose(setup.world[root].inverse() * Affine::fromTRS(cx, cy, 0, 1, 1));
        slot.attachments.push_back(att);
        slot.activeAttachment = layer.visible ? 0 : -1;
        sk_.slots.push_back(slot);
        ++imported;
    }
    updatePose();
    frameAllIn_ = 3;
    if (!unsupportedBlends.empty()) {
        Diagnostic warning{Severity::Warning, "PSD_BLEND_UNSUPPORTED",
            tr("部分 PSD 图层使用了不支持的混合模式，已按正常模式导入。"), unsupportedBlends, IssueTarget::File};
        warning.path = path;
        reportIssue(std::move(warning));
    }
    if (std::any_of(sessionIssues_.begin(), sessionIssues_.end(), [](const Diagnostic& d) { return d.code == "PSD_IMPORT_FAILED"; })) return false;
    setStatus(tr("已从 PSD 导入 ") + std::to_string(imported) + tr(" 个图层，图片保存在 ") + outDir.filename().u8string());
    return imported > 0;
}

// ---------------------------------------------------------------- export: data & atlas

bool Editor::exportDataPath(const std::string& path) {
    std::string err;
    Skeleton out = sk_;
    fs::path dir = fs::u8path(path).parent_path();
    for (auto& im : out.images) {
        std::error_code ec;
        fs::path rel = fs::relative(fs::u8path(im.path), dir, ec);
        if (!ec && !rel.empty() && !rel.has_root_name()) im.path = rel.generic_u8string();
    }
    for (auto& e : out.events) {
        if (e.audioPath.empty()) continue;
        std::error_code ec;
        fs::path rel = fs::relative(fs::u8path(e.audioPath), dir, ec);
        if (!ec && !rel.empty() && !rel.has_root_name()) e.audioPath = rel.generic_u8string();
    }
    ExportOptions opt;
    opt.binary = io_.dataBinary;
    opt.decimals = io_.dataDecimals;
    opt.indent = io_.dataPretty ? 2 : -1;
    if (!exportFile(path, out, opt, &err)) {
        return failOperation("DATA_EXPORT_FAILED", tr("导出失败: ") + err, path);
    }
    dismissIssue("DATA_EXPORT_FAILED");
    std::error_code ec;
    auto size = fs::file_size(fs::u8path(path), ec);
    setStatus(tr("已导出 ") + fs::u8path(path).filename().u8string() + tr("（") + std::to_string(size / 1024 + 1) + tr(" KB）"));
    return true;
}

bool Editor::exportAtlasPath(const std::string& path) {
    if (!checkExportResources()) return false;
    std::vector<AtlasInput> inputs;
    for (size_t i = 0; i < sk_.images.size(); ++i) {
        if (i >= textures_.size() || textures_[i].rgba().empty()) continue;
        inputs.push_back({fs::u8path(sk_.images[i].path).stem().u8string(), textures_[i].width, textures_[i].height,
                          textures_[i].rgba().data()});
    }
    if (inputs.empty()) {
        return failOperation("ATLAS_EXPORT_FAILED", tr("没有已加载的图片可打包"), path);
    }
    AtlasSettings st;
    st.maxWidth = st.maxHeight = io_.atlasMaxSize;
    st.padding = io_.atlasPadding;
    st.trim = io_.atlasTrim;
    st.allowRotate = io_.atlasRotate;
    st.powerOfTwo = io_.atlasPow2;
    st.scale = io_.atlasScale;
    st.autoScale = io_.atlasAutoScale;
    Atlas atlas;
    std::string err;
    if (!packAtlas(inputs, st, atlas, &err)) {
        return failOperation("ATLAS_EXPORT_FAILED", tr("打包失败: ") + err, path);
    }
    fs::path base = fs::u8path(path);
    std::string stem = base.stem().u8string();
    std::vector<std::string> pageFiles;
    for (size_t p = 0; p < atlas.pages.size(); ++p) {
        std::string name = stem + (p == 0 ? "" : "_" + std::to_string(p + 1)) + ".png";
        pageFiles.push_back(name);
        if (!writePng((base.parent_path() / fs::u8path(name)).u8string(), atlas.pages[p].width, atlas.pages[p].height, atlas.pages[p].rgba)) {
            return failOperation("ATLAS_EXPORT_FAILED", tr("写入图集页失败"), (base.parent_path() / fs::u8path(name)).u8string());
        }
    }
    std::ofstream f(base, std::ios::binary);
    f << atlasText(atlas, pageFiles);
    f.close();
    if (!f) return failOperation("ATLAS_EXPORT_FAILED", tr("写入图集描述失败"), path);
    dismissIssue("ATLAS_EXPORT_FAILED");
    int used = 0, total = 0;
    for (auto& r : atlas.regions) used += r.width * r.height;
    for (auto& p : atlas.pages) total += p.width * p.height;
    setStatus(tr("已打包 ") + std::to_string(inputs.size()) + tr(" 张图片到 ") + std::to_string(atlas.pages.size()) + tr(" 页，利用率 ") +
              std::to_string(total ? used * 100 / total : 0) + "%" +
              (atlas.scale != 1.0f ? tr("，缩放 ") + std::to_string((int)std::lround(atlas.scale * 100)) + "%" : std::string()));
    return true;
}

// ---------------------------------------------------------------- export: images / video

ImageExportOptions Editor::imageOptions() const { return io_; }

bool Editor::finishImageExport(const ImageExportResult& result,const std::string& path,bool video) {
    if (result.cancelled) { setStatus(tr("已取消；上一次导出资源保持完整。")); return false; }
    const char* code = video ? "VIDEO_EXPORT_FAILED" : "IMAGE_EXPORT_FAILED";
    if (!result.error.empty()) {
        failOperation(code,std::string(tr("图片或视频导出失败: "))+result.error,path);
        refreshExportRecoveries(); return false;
    }
    dismissIssue(code);
    if (result.preservedFrames) reportIssue("SEQUENCE_MODIFIED",tr("部分旧帧已被修改或不是普通文件，已保留，请手动检查输出目录。"),Severity::Warning,path);
    else dismissIssue("SEQUENCE_MODIFIED");
    setStatus(std::string(tr("已导出图片或视频 ")) + fs::u8path(path).filename().u8string()); return true;
}

bool Editor::exportImagesPath(const std::string& path) {
    if (imageJob_ || engineJob_ || exportRecoveryTask_.valid() || !checkExportResources()) return false;
    std::vector<ImageExportTexture> images;
    for (const auto& texture : textures_) images.push_back({texture.width,texture.height,{},texture.pixels});
    EngineExportProgress progress;
    return finishImageExport(bakeImages(sk_,images,imageOptions(),activeSkins_,physicsOn_,path,progress,false,configFile("export-recovery")),path,isVideo(io_.imageType));
}

bool Editor::startImageExport(const std::string& path,bool estimate) {
    if (imageJob_ || engineJob_ || exportRecoveryTask_.valid()) { setStatus(tr("已有导出任务正在运行，请先完成或取消。")); return false; }
    if (!checkExportResources()) return false;
    try {
        auto job = std::make_unique<ImageExportJob>(); job->estimate = estimate;
        job->path = imageExportDestination(path.empty() ? "export.png" : path,projectPath_); job->video = isVideo(io_.imageType);
        job->options = imageOptions(); job->history = historyPosition_; job->skins = activeSkins_; job->physics = physicsOn_;
        std::vector<ImageExportTexture> images;
        if (!estimate) for (const auto& texture : textures_) images.push_back({texture.width,texture.height,{},texture.pixels});
        auto progress = job->progress;
        job->result = std::async(std::launch::async,[sk = sk_,images = std::move(images),options = imageOptions(),skins = activeSkins_,physicsOn = physicsOn_,path = job->path,progress,estimate,recovery = configFile("export-recovery")] {
            return bakeImages(sk,images,options,skins,physicsOn,path,*progress,estimate,recovery);
        });
        imageJob_ = std::move(job);
        if (!estimate) imageOutputPath_ = imageJob_->path;
        saveImageExportSettings(); return true;
    } catch (const std::exception& error) { ImageExportResult result; result.error = error.what(); return finishImageExport(result,path,isVideo(io_.imageType)); }
}
void Editor::pollImageExport() {
    if (imageSettingsDirty_ && (imageSettingsDelay_ -= ImGui::GetIO().DeltaTime) <= 0 && !ImGui::IsAnyItemActive()) saveImageExportSettings();
    if (!imageJob_ || imageJob_->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    ImageExportResult result;
    try { result = imageJob_->result.get(); } catch (const std::exception& error) { result.error = error.what(); }
    if (imageJob_->progress->gate == EngineExportProgress::Cancelled) { result.cancelled = true; result.error.clear(); }
    if (imageJob_->estimate && result.error.empty() && !result.cancelled) {
        imageBudget_ = result; imageBudgetOptions_ = imageJob_->options; imageBudgetHistory_ = imageJob_->history;
        imageBudgetSkins_ = imageJob_->skins; imageBudgetPhysics_ = imageJob_->physics;
        setStatus(tr("普通导出预算已更新。"));
    } else finishImageExport(result,imageJob_->path,imageJob_->video);
    imageJob_.reset();
}
void Editor::resetImageExport() {
    if (imageJob_) { imageJob_->progress->cancel(); imageJob_->result.wait(); imageJob_.reset(); }
}
void Editor::drawImageExportProgress() {
    if (!imageJob_ || imageJob_->estimate) return;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize()*28,0),ImGuiCond_Appearing);
    if (ImGui::Begin(tr("图片与视频导出###ImageExportProgress"),nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto& p = *imageJob_->progress;
        const char* stages[] = {tr("正在分析动作与画布..."),tr("正在烘焙动画帧..."),tr("正在编码并写入文件..."),tr("正在提交资源，请稍候...")};
        ImGui::TextUnformatted(stages[p.stage.load()]);
        ImGui::ProgressBar(std::min(0.99f,float(p.completed.load())/std::max(1,p.total.load())),ImVec2(-1,0));
        ImGui::BeginDisabled(p.gate != EngineExportProgress::Running);
        if (ImGui::Button(tr("取消任务"))) imageJob_->progress->cancel();
        ImGui::EndDisabled();
        if (p.gate == EngineExportProgress::Cancelled) ImGui::TextWrapped(tr("正在取消，等待当前帧或编码步骤结束..."));
        ImGui::TextWrapped(tr("可继续编辑；任务使用启动时的工程快照。关闭或切换工程会取消任务。"));
    }
    ImGui::End();
}

// ---------------------------------------------------------------- dialogs

void Editor::drawIoDialogs() {
    if (io_.open != IoState::None) {
        if (io_.open == IoState::Engine) { syncEngineClips(); engineBudget_ = {}; engineNotice_.clear(); }
        const char* titles[] = {"", tr("导出数据"), tr("纹理打包"), tr("导出图片 / 动画"), tr("导入 PSD"), tr("Unity / Godot 帧动画")};
        ImGui::OpenPopup(titles[(int)io_.open]);
        io_.openPopup = io_.open;
        io_.open = IoState::None;
    }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    auto begin = [&](const char* title) {
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        const auto size = ImGui::GetMainViewport()->WorkSize;
        ImGui::SetNextWindowSizeConstraints(ImVec2(0,0),ImVec2(size.x-24,size.y-24));
        return ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    };
    auto buttons = [&](const char* ok) {
        ImGui::Separator();
        bool go = ImGui::Button(ok, ImVec2(ImGui::GetFontSize() * 6, 0));
        ImGui::SameLine();
        if (ImGui::Button(tr("取消"), ImVec2(ImGui::GetFontSize() * 6, 0))) ImGui::CloseCurrentPopup();
        return go;
    };

    pollEngineExport();
    pollImageExport();
    drawImageExportProgress();
    drawEngineExportDialog();
    if (begin(tr("导出数据"))) {
        int fmt = io_.dataBinary ? 1 : 0;
        ImGui::RadioButton("JSON", &fmt, 0);
        ImGui::SameLine();
        ImGui::RadioButton(tr("二进制（.rigb，MessagePack）"), &fmt, 1);
        io_.dataBinary = fmt == 1;
        ImGui::BeginDisabled(io_.dataBinary);
        ImGui::Checkbox(tr("格式化（缩进）"), &io_.dataPretty);
        ImGui::EndDisabled();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::SliderInt(tr("小数位"), &io_.dataDecimals, -1, 6, io_.dataDecimals < 0 ? tr("完整精度") : tr("%d 位"));
        ImGui::TextDisabled(tr("图片路径会改为相对于导出文件。"));
        if (buttons(tr("导出..."))) {
            std::string path = io_.dataBinary ? saveFileDialog(tr("二进制骨架 (*.rigb)"), "*.rigb", "rigb") : saveFileDialog("JSON (*.json)", "*.json", "json");
            if (!path.empty()) exportDataPath(path);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (begin(tr("纹理打包"))) {
        const char* sizes[] = {"256", "512", "1024", "2048", "4096"};
        int sizeIdx = 0;
        while (sizeIdx < 4 && (256 << sizeIdx) < io_.atlasMaxSize) ++sizeIdx;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        if (ImGui::Combo(tr("最大页尺寸"), &sizeIdx, sizes, 5)) io_.atlasMaxSize = 256 << sizeIdx;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::SliderInt(tr("间距"), &io_.atlasPadding, 0, 8, "%d px");
        ImGui::Checkbox(tr("裁掉透明边"), &io_.atlasTrim);
        ImGui::Checkbox(tr("允许旋转"), &io_.atlasRotate);
        ImGui::Checkbox(tr("页尺寸为 2 的幂"), &io_.atlasPow2);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::DragFloat(tr("缩放"), &io_.atlasScale, 0.01f, 0.05f, 4.0f, "%.2f");
        ImGui::Checkbox(tr("自动缩放（缩小到一页放得下）"), &io_.atlasAutoScale);
        ImGui::TextDisabled(tr("输出 .atlas 描述文件和同名的页 PNG。"));
        if (buttons(tr("打包..."))) {
            std::string path = saveFileDialog(tr("图集 (*.atlas)"), "*.atlas", "atlas");
            if (!path.empty()) exportAtlasPath(path);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (begin(tr("导出图片 / 动画"))) {
        auto previousOptions = imageOptions();
        ImGui::BeginDisabled(imageJob_ != nullptr);
        const char* types[] = {tr("PNG 序列"),          tr("精灵表 PNG"),         "GIF",
                               tr("视频 MP4 (H.264)"),  tr("视频 WebM (VP9，透明)"), tr("JPG 序列"),
                               tr("视频 AVI (MJPEG)"),  tr("视频 QuickTime MOV (ProRes 4444，透明)")};
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        ImGui::Combo(tr("类型"), &io_.imageType, types, IM_ARRAYSIZE(types));
        if (previousOptions.imageType != io_.imageType && !imageOutputPath_.empty()) {
            const char* extensions[] = {".png",".png",".gif",".mp4",".webm",".jpg",".avi",".mov"};
            auto path = fs::u8path(imageOutputPath_); path.replace_extension(extensions[io_.imageType]); imageOutputPath_ = path.u8string();
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        const char* animName = io_.imageAnim >= 0 && io_.imageAnim < (int)sk_.animations.size() ? sk_.animations[io_.imageAnim].name.c_str() : tr("(Setup 姿态)");
        if (ImGui::BeginCombo(tr("动画"), animName)) {
            if (ImGui::Selectable(tr("(Setup 姿态)"), io_.imageAnim < 0)) io_.imageAnim = -1;
            for (int i = 0; i < (int)sk_.animations.size(); ++i) {
                ImGui::PushID(i);
                if (ImGui::Selectable(sk_.animations[i].name.c_str(), i == io_.imageAnim)) io_.imageAnim = i;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::DragFloat(tr("缩放"), &io_.imageScale, 0.01f, 0.05f, 8.0f, "%.2f");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::DragFloat(tr("帧率"), &io_.imageFps, 1, 1, 120, "%.0f FPS");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::BeginDisabled(sk_.effects.pixel.enabled);
        ImGui::SliderInt(tr("抗锯齿"), &io_.imageSupersample, 1, 4, "%dx");
        ImGui::EndDisabled();
        if (sk_.effects.pixel.enabled) ImGui::TextWrapped(tr("像素模式已固定为 1x 采样。"));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::SliderInt(tr("留边"), &io_.imagePadding, 0, 64, "%d px");
        ImGui::Checkbox(tr("背景色"), &io_.imageBackground);
        ImGui::SameLine();
        ImGui::ColorEdit3("##bg", io_.imageBgColor, ImGuiColorEditFlags_NoInputs);
        if (io_.imageType == 5) {
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            ImGui::SliderInt(tr("JPG 质量"), &io_.jpgQuality, 1, 100);
        }
        if (isOpaque(io_.imageType)) ImGui::TextDisabled(tr("此格式不支持透明：空白处用背景色填充。"));
        if (isVideo(io_.imageType)) {
            ImGui::Checkbox(tr("包含音频事件的声音"), &io_.videoAudio);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
            ImGui::InputText("ffmpeg", &io_.ffmpegPath);
            ImGui::TextDisabled(tr("视频由本机安装的 ffmpeg 编码（不随编辑器分发）。"));
        }
        if (isSequence(io_.imageType)) ImGui::Checkbox(tr("清理清单内未修改的多余旧帧"),&io_.cleanOldFrames);
        ImGui::SetNextItemWidth(ImGui::GetFontSize()*19);
        bool pathChanged = ImGui::InputText(tr("输出路径"),&imageOutputPath_);
        if (ImGui::Button(tr("浏览..."))) {
            const char* ext[] = {"png","png","gif","mp4","webm","jpg","avi","mov"};
            const auto path = saveFileDialog(tr("导出文件"),"*.*",ext[io_.imageType]);
            if (!path.empty()) { imageOutputPath_ = path; pathChanged = true; }
        }
        if (!(previousOptions == imageOptions()) || pathChanged) { imageSettingsDirty_ = true; imageSettingsDelay_ = 0.5f; }
        if (ImGui::Button(tr("计算导出尺寸与内存"))) startImageExport(imageOutputPath_,true);
        ImGui::EndDisabled();
        if (imageJob_ && imageJob_->estimate) {
            ImGui::TextUnformatted(tr("正在分析动作与画布..."));
            if (ImGui::Button(tr("取消任务"))) imageJob_->progress->cancel();
        }
        if (imageBudget_.width && imageBudgetHistory_ == historyPosition_ && imageBudgetOptions_ == imageOptions() && imageBudgetSkins_ == activeSkins_ && imageBudgetPhysics_ == physicsOn_) {
            ImGui::Text(tr("每帧 %d x %d，%d 帧；缓冲估算 %.1f MiB"),imageBudget_.width,imageBudget_.height,imageBudget_.frames,imageBudget_.bufferBytes/1048576.0);
            ImGui::TextWrapped(tr("估算不含工程、输入资源、编码器和驱动开销。"));
        }
        ImGui::BeginDisabled(imageJob_ != nullptr || engineJob_ != nullptr);
        if (buttons(tr("导出..."))) {
            const char* names[] = {"PNG (*.png)",   "PNG (*.png)", "GIF (*.gif)", "MP4 (*.mp4)",
                                   "WebM (*.webm)", "JPG (*.jpg)", "AVI (*.avi)", "QuickTime (*.mov)"};
            const char* specs[] = {"*.png", "*.png", "*.gif", "*.mp4", "*.webm", "*.jpg", "*.avi", "*.mov"};
            const char* exts[] = {"png", "png", "gif", "mp4", "webm", "jpg", "avi", "mov"};
            std::string path = imageOutputPath_;
            if (path.empty()) path = saveFileDialog(names[io_.imageType], specs[io_.imageType], exts[io_.imageType]);
            if (!path.empty()) startImageExport(path);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    if (begin(tr("导入 PSD"))) {
        ImGui::Checkbox(tr("包含隐藏图层（导入但不显示）"), &io_.psdIncludeHidden);
        ImGui::TextDisabled(tr("每个图层存成 PNG（放在 PSD 旁的 <名字>_images 文件夹），\n并按图层顺序和位置建立插槽。"));
        if (buttons(tr("选择 PSD..."))) {
            std::string path = openFileDialog("Photoshop (*.psd)", "*.psd");
            if (!path.empty()) importPsdPath(path, io_.psdIncludeHidden);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

bool Editor::runExport(const std::string& type, const std::string& path) {
    if (type == "engines") return exportEnginePath(path);
    if (type == "skeleton") return exportRuntimePath(path);
    if (type == "json" || type == "bin") {
        io_.dataBinary = type == "bin";
        return exportDataPath(path);
    }
    if (type == "atlas") return exportAtlasPath(path);
    if (type == "merge") return importMergePath(path);
    if (type == "psd") return importPsdPath(path, true);
    const char* types[] = {"png", "sheet", "gif", "mp4", "webm", "jpg", "avi", "mov"};
    for (int i = 0; i < 8; ++i)
        if (type == types[i]) {
            io_.imageType = i;
            return exportImagesPath(path);
        }
    return failOperation("EXPORT_TYPE_FAILED", tr("未知的导出类型 ") + type, path);
}
