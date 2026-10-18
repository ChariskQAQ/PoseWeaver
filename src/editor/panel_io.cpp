// Import / export: merge projects, PSD import, runtime data, texture atlas, images and video.
#include "editor/editor.h"

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

// Pipe to an external process (binary, UTF-8 command line).
FILE* openPipe(const std::string& cmd) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, w.data(), n);
    return _wpopen(w.c_str(), L"wb");
#else
    return popen(cmd.c_str(), "w");
#endif
}

int closePipe(FILE* f) {
#ifdef _WIN32
    return _pclose(f);
#else
    return pclose(f);
#endif
}

bool ffmpegAvailable(const std::string& exe) {
#ifdef _WIN32
    std::string cmd = "\"" + exe + "\" -version >NUL 2>&1";
#else
    std::string cmd = "\"" + exe + "\" -version >/dev/null 2>&1";
#endif
    return std::system(cmd.c_str()) == 0;
}
} // namespace

// ---------------------------------------------------------------- import

bool Editor::importMergePath(const std::string& path) {
    Skeleton src;
    std::string err;
    if (!loadFile(path, src, &err)) {
        setStatus("导入失败: " + err);
        return false;
    }
    // Image paths in the imported file are relative to it.
    fs::path dir = fs::absolute(fs::u8path(path)).parent_path();
    for (auto& im : src.images) {
        fs::path p = fs::u8path(im.path);
        // Same normal form as importImage, so shared images are recognised by path.
        im.path = fs::absolute(p.is_absolute() ? p : dir / p).lexically_normal().u8string();
    }
    checkpoint();
    std::string report = mergeSkeleton(sk_, src);
    reloadTextures();
    if (anim_ < 0 && !sk_.animations.empty()) anim_ = 0;
    updatePose();
    frameAllIn_ = 3;
    setStatus(report);
    return true;
}

bool Editor::importPsdPath(const std::string& path, bool includeHidden) {
    std::vector<uint8_t> bytes;
    PsdDocument doc;
    std::string err;
    if (!readBytes(path, bytes) || !parsePsd(bytes, doc, &err)) {
        setStatus("PSD 读取失败: " + (err.empty() ? std::string("无法打开文件") : err));
        return false;
    }
    if (doc.layers.empty()) {
        setStatus("PSD 里没有可导入的图层");
        return false;
    }
    fs::path psd = fs::u8path(path);
    fs::path outDir = psd.parent_path() / fs::u8path(psd.stem().u8string() + "_images");
    std::error_code ec;
    fs::create_directories(outDir, ec);
    checkpoint();
    if (sk_.bones.empty()) sk_.addBone("root", -1, {}, 0);
    int root = 0;
    Pose setup = makeSetupPose(sk_);
    int imported = 0;
    std::vector<std::string> used;
    for (const PsdLayer& layer : doc.layers) {
        if (!layer.visible && !includeHidden) continue;
        std::string base = safeFileName(layer.name);
        std::string file = base;
        for (int n = 2; std::find(used.begin(), used.end(), file) != used.end(); ++n) file = base + "_" + std::to_string(n);
        used.push_back(file);
        fs::path png = outDir / fs::u8path(file + ".png");
        if (!writePng(png.u8string(), layer.width, layer.height, layer.rgba)) continue;
        Texture tex;
        if (!loadTexture(png.u8string(), tex, &err)) continue;
        sk_.images.push_back({fs::absolute(png).lexically_normal().u8string(), tex.width, tex.height});
        textures_.push_back(std::move(tex));
        Slot slot;
        slot.name = sk_.uniqueSlotName(layer.name.empty() ? "layer" : layer.name);
        slot.bone = root;
        slot.color[3] = layer.opacity;
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
    setStatus("已从 PSD 导入 " + std::to_string(imported) + " 个图层，图片保存在 " + outDir.filename().u8string());
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
        setStatus("导出失败: " + err);
        return false;
    }
    std::error_code ec;
    auto size = fs::file_size(fs::u8path(path), ec);
    setStatus("已导出 " + fs::u8path(path).filename().u8string() + "（" + std::to_string(size / 1024 + 1) + " KB）");
    return true;
}

bool Editor::exportAtlasPath(const std::string& path) {
    std::vector<AtlasInput> inputs;
    for (size_t i = 0; i < sk_.images.size(); ++i) {
        if (i >= textures_.size() || textures_[i].rgba.empty()) continue;
        inputs.push_back({fs::u8path(sk_.images[i].path).stem().u8string(), textures_[i].width, textures_[i].height,
                          textures_[i].rgba.data()});
    }
    if (inputs.empty()) {
        setStatus("没有已加载的图片可打包");
        return false;
    }
    AtlasSettings st;
    st.maxWidth = st.maxHeight = io_.atlasMaxSize;
    st.padding = io_.atlasPadding;
    st.trim = io_.atlasTrim;
    st.allowRotate = io_.atlasRotate;
    st.powerOfTwo = io_.atlasPow2;
    Atlas atlas;
    std::string err;
    if (!packAtlas(inputs, st, atlas, &err)) {
        setStatus("打包失败: " + err);
        return false;
    }
    fs::path base = fs::u8path(path);
    std::string stem = base.stem().u8string();
    std::vector<std::string> pageFiles;
    for (size_t p = 0; p < atlas.pages.size(); ++p) {
        std::string name = stem + (p == 0 ? "" : "_" + std::to_string(p + 1)) + ".png";
        pageFiles.push_back(name);
        if (!writePng((base.parent_path() / fs::u8path(name)).u8string(), atlas.pages[p].width, atlas.pages[p].height, atlas.pages[p].rgba)) {
            setStatus("写入图集页失败");
            return false;
        }
    }
    std::ofstream f(base, std::ios::binary);
    f << atlasText(atlas, pageFiles);
    int used = 0, total = 0;
    for (auto& r : atlas.regions) used += r.width * r.height;
    for (auto& p : atlas.pages) total += p.width * p.height;
    setStatus("已打包 " + std::to_string(inputs.size()) + " 张图片到 " + std::to_string(atlas.pages.size()) + " 页，利用率 " +
              std::to_string(total ? used * 100 / total : 0) + "%");
    return true;
}

// ---------------------------------------------------------------- export: images / video

bool Editor::exportImagesPath(const std::string& path) {
    Animation* an = io_.imageAnim >= 0 && io_.imageAnim < (int)sk_.animations.size() ? &sk_.animations[io_.imageAnim] : nullptr;
    std::vector<RasterTexture> tex;
    for (auto& t : textures_) tex.push_back({t.width, t.height, t.rgba.empty() ? nullptr : t.rgba.data()});
    const float fps = std::max(1.0f, io_.imageFps);
    const int frames = an ? std::max(1, (int)std::round(an->duration * fps)) + (io_.imageType == 0 ? 1 : 0) : 1;
    // Poses for every frame (physics simulated in order), and the union of their bounds.
    std::vector<Pose> poses;
    PhysicsState phys;
    Vec2 lo{1e9f, 1e9f}, hi{-1e9f, -1e9f};
    for (int f = 0; f < frames; ++f) {
        Pose p = makeAnimatedPose(sk_, an, f / fps, activeSkins_);
        if (physicsOn_) applyPhysics(sk_, p, phys, f == 0 ? 0.0f : 1 / fps);
        Vec2 a, b;
        if (poseBounds(sk_, p, a, b)) {
            lo = {std::min(lo.x, a.x), std::min(lo.y, a.y)};
            hi = {std::max(hi.x, b.x), std::max(hi.y, b.y)};
        }
        poses.push_back(std::move(p));
    }
    if (lo.x > hi.x) {
        setStatus("没有可见的附件可以渲染");
        return false;
    }
    const float scale = std::max(0.05f, io_.imageScale);
    const int pad = std::max(0, io_.imagePadding);
    int w = (int)std::ceil((hi.x - lo.x) * scale) + 2 * pad, h = (int)std::ceil((hi.y - lo.y) * scale) + 2 * pad;
    if (io_.imageType >= 3) {  // video codecs want even sizes
        w += w & 1;
        h += h & 1;
    }
    const int ss = std::clamp(io_.imageSupersample, 1, 4);
    Affine toPx{scale * ss, 0, 0, -scale * ss, (pad - lo.x * scale) * ss, (pad + hi.y * scale) * ss};
    bool opaque = io_.imageType == 3;  // MP4 has no alpha
    auto renderFrame = [&](const Pose& p) {
        std::vector<unsigned char> img(size_t(w * ss) * (h * ss) * 4, 0);
        if (io_.imageBackground || opaque)
            for (size_t i = 0; i < img.size(); i += 4) {
                img[i] = (unsigned char)(io_.imageBgColor[0] * 255);
                img[i + 1] = (unsigned char)(io_.imageBgColor[1] * 255);
                img[i + 2] = (unsigned char)(io_.imageBgColor[2] * 255);
                img[i + 3] = 255;
            }
        renderPose(sk_, p, tex, toPx, w * ss, h * ss, img);
        return downsample(img, w * ss, h * ss, ss);
    };

    fs::path out = fs::u8path(path);
    std::string stem = out.stem().u8string();
    switch (io_.imageType) {
    case 0: {  // PNG sequence
        int written = 0;
        for (int f = 0; f < frames; ++f) {
            char num[16];
            std::snprintf(num, sizeof(num), "_%04d.png", f);
            written += writePng((out.parent_path() / fs::u8path(stem + num)).u8string(), w, h, renderFrame(poses[f]));
        }
        setStatus("已导出 " + std::to_string(written) + " 张 PNG（" + std::to_string(w) + "x" + std::to_string(h) + "）");
        return written == frames;
    }
    case 1: {  // sprite sheet
        int cols = (int)std::ceil(std::sqrt((float)frames)), rows = (frames + cols - 1) / cols;
        std::vector<unsigned char> sheet(size_t(w * cols) * (h * rows) * 4, 0);
        for (int f = 0; f < frames; ++f) {
            std::vector<unsigned char> img = renderFrame(poses[f]);
            int ox = (f % cols) * w, oy = (f / cols) * h;
            for (int y = 0; y < h; ++y)
                std::copy(img.begin() + size_t(y) * w * 4, img.begin() + size_t(y + 1) * w * 4,
                          sheet.begin() + (size_t(oy + y) * w * cols + ox) * 4);
        }
        bool ok = writePng(out.u8string(), w * cols, h * rows, sheet);
        setStatus(ok ? "已导出精灵表 " + std::to_string(cols) + "x" + std::to_string(rows) + "（每帧 " + std::to_string(w) + "x" +
                           std::to_string(h) + "）"
                     : "写入精灵表失败");
        return ok;
    }
    case 2: {  // GIF
        std::vector<std::vector<unsigned char>> imgs;
        for (int f = 0; f < frames; ++f) imgs.push_back(renderFrame(poses[f]));
        bool ok = writeBytes(out.u8string(), encodeGif(imgs, w, h, std::max(2, (int)std::round(100 / fps)), true));
        setStatus(ok ? "已导出 GIF（" + std::to_string(frames) + " 帧）" : "写入 GIF 失败");
        return ok;
    }
    default: {  // video through ffmpeg
        if (!ffmpegAvailable(io_.ffmpegPath)) {
            setStatus("找不到 ffmpeg：请安装 ffmpeg 或在导出设置里填写 ffmpeg.exe 的路径");
            return false;
        }
        char sizeArg[32], fpsArg[32];
        std::snprintf(sizeArg, sizeof(sizeArg), "%dx%d", w, h);
        std::snprintf(fpsArg, sizeof(fpsArg), "%g", fps);
        std::string codec = io_.imageType == 3 ? "-c:v libx264 -pix_fmt yuv420p -crf 18" : "-c:v libvpx-vp9 -pix_fmt yuva420p -b:v 0 -crf 30";
        // Audio events mixed into one track (written next to the video, removed afterwards).
        std::string audioInput, audioCodec;
        fs::path wav = out.parent_path() / fs::u8path(stem + "_audio.wav");
        if (io_.videoAudio && an) {
            std::vector<AudioPlacement> placements;
            for (const AudioEventKey& k : audioEventKeys(sk_, *an)) {
                const EventDef& def = sk_.events[k.event];
                if (const AudioSamples* s = audio_->samples(def.audioPath)) placements.push_back({s, k.time, def.volume, def.balance});
            }
            if (!placements.empty()) {
                std::vector<float> mix = mixAudio(placements, frames / fps, 48000);
                std::vector<uint8_t> bytes = encodeWav(mix, 48000);
                std::ofstream wf(wav, std::ios::binary);
                wf.write((const char*)bytes.data(), (std::streamsize)bytes.size());
                if (wf) {
                    audioInput = " -i \"" + wav.u8string() + "\" -map 0:v -map 1:a";
                    audioCodec = io_.imageType == 3 ? " -c:a aac -b:a 192k" : " -c:a libopus -b:a 128k";
                }
            }
        }
        std::string cmd = "\"" + io_.ffmpegPath + "\" -y -loglevel error -f rawvideo -pix_fmt rgba -s " + sizeArg + " -r " + fpsArg +
                          " -i -" + audioInput + " " + codec + audioCodec + " \"" + out.u8string() + "\"";
#ifdef _WIN32
        cmd = "\"" + cmd + "\"";  // cmd.exe strips the outer quotes
#endif
        FILE* pipe = openPipe(cmd);
        if (!pipe) {
            setStatus("无法启动 ffmpeg");
            return false;
        }
        for (int f = 0; f < frames; ++f) {
            std::vector<unsigned char> img = renderFrame(poses[f]);
            fwrite(img.data(), 1, img.size(), pipe);
        }
        int code = closePipe(pipe);
        std::error_code rmEc;
        if (!audioInput.empty()) fs::remove(wav, rmEc);
        setStatus(code == 0 ? "已导出视频 " + out.filename().u8string() + "（" + std::to_string(frames) + " 帧）" : "ffmpeg 编码失败（返回 " + std::to_string(code) + "）");
        return code == 0;
    }
    }
}

// ---------------------------------------------------------------- dialogs

void Editor::drawIoDialogs() {
    if (io_.open != IoState::None) {
        const char* titles[] = {"", "导出数据", "纹理打包", "导出图片 / 动画", "导入 PSD"};
        ImGui::OpenPopup(titles[(int)io_.open]);
        io_.openPopup = io_.open;
        io_.open = IoState::None;
    }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    auto begin = [&](const char* title) {
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        return ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    };
    auto buttons = [&](const char* ok) {
        ImGui::Separator();
        bool go = ImGui::Button(ok, ImVec2(ImGui::GetFontSize() * 6, 0));
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(ImGui::GetFontSize() * 6, 0))) ImGui::CloseCurrentPopup();
        return go;
    };

    if (begin("导出数据")) {
        int fmt = io_.dataBinary ? 1 : 0;
        ImGui::RadioButton("JSON", &fmt, 0);
        ImGui::SameLine();
        ImGui::RadioButton("二进制（.rigb，MessagePack）", &fmt, 1);
        io_.dataBinary = fmt == 1;
        ImGui::BeginDisabled(io_.dataBinary);
        ImGui::Checkbox("格式化（缩进）", &io_.dataPretty);
        ImGui::EndDisabled();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::SliderInt("小数位", &io_.dataDecimals, -1, 6, io_.dataDecimals < 0 ? "完整精度" : "%d 位");
        ImGui::TextDisabled("图片路径会改为相对于导出文件。");
        if (buttons("导出...")) {
            std::string path = io_.dataBinary ? saveFileDialog("二进制骨架 (*.rigb)", "*.rigb", "rigb") : saveFileDialog("JSON (*.json)", "*.json", "json");
            if (!path.empty()) exportDataPath(path);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (begin("纹理打包")) {
        const char* sizes[] = {"256", "512", "1024", "2048", "4096"};
        int sizeIdx = 0;
        while (sizeIdx < 4 && (256 << sizeIdx) < io_.atlasMaxSize) ++sizeIdx;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        if (ImGui::Combo("最大页尺寸", &sizeIdx, sizes, 5)) io_.atlasMaxSize = 256 << sizeIdx;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::SliderInt("间距", &io_.atlasPadding, 0, 8, "%d px");
        ImGui::Checkbox("裁掉透明边", &io_.atlasTrim);
        ImGui::Checkbox("允许旋转", &io_.atlasRotate);
        ImGui::Checkbox("页尺寸为 2 的幂", &io_.atlasPow2);
        ImGui::TextDisabled("输出 .atlas 描述文件和同名的页 PNG。");
        if (buttons("打包...")) {
            std::string path = saveFileDialog("图集 (*.atlas)", "*.atlas", "atlas");
            if (!path.empty()) exportAtlasPath(path);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (begin("导出图片 / 动画")) {
        const char* types[] = {"PNG 序列", "精灵表 PNG", "GIF", "视频 MP4 (H.264)", "视频 WebM (VP9，透明)"};
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        ImGui::Combo("类型", &io_.imageType, types, 5);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        const char* animName = io_.imageAnim >= 0 && io_.imageAnim < (int)sk_.animations.size() ? sk_.animations[io_.imageAnim].name.c_str() : "(Setup 姿态)";
        if (ImGui::BeginCombo("动画", animName)) {
            if (ImGui::Selectable("(Setup 姿态)", io_.imageAnim < 0)) io_.imageAnim = -1;
            for (int i = 0; i < (int)sk_.animations.size(); ++i) {
                ImGui::PushID(i);
                if (ImGui::Selectable(sk_.animations[i].name.c_str(), i == io_.imageAnim)) io_.imageAnim = i;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::DragFloat("缩放", &io_.imageScale, 0.01f, 0.05f, 8.0f, "%.2f");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::DragFloat("帧率", &io_.imageFps, 1, 1, 120, "%.0f FPS");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::SliderInt("抗锯齿", &io_.imageSupersample, 1, 4, "%dx");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::SliderInt("留边", &io_.imagePadding, 0, 64, "%d px");
        ImGui::Checkbox("背景色", &io_.imageBackground);
        ImGui::SameLine();
        ImGui::ColorEdit3("##bg", io_.imageBgColor, ImGuiColorEditFlags_NoInputs);
        if (io_.imageType >= 3) {
            ImGui::Checkbox("包含音频事件的声音", &io_.videoAudio);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
            ImGui::InputText("ffmpeg", &io_.ffmpegPath);
            ImGui::TextDisabled("视频由本机安装的 ffmpeg 编码（不随编辑器分发）。");
        }
        if (buttons("导出...")) {
            const char* names[] = {"PNG (*.png)", "PNG (*.png)", "GIF (*.gif)", "MP4 (*.mp4)", "WebM (*.webm)"};
            const char* specs[] = {"*.png", "*.png", "*.gif", "*.mp4", "*.webm"};
            const char* exts[] = {"png", "png", "gif", "mp4", "webm"};
            std::string path = saveFileDialog(names[io_.imageType], specs[io_.imageType], exts[io_.imageType]);
            if (!path.empty()) exportImagesPath(path);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (begin("导入 PSD")) {
        ImGui::Checkbox("包含隐藏图层（导入但不显示）", &io_.psdIncludeHidden);
        ImGui::TextDisabled("每个图层存成 PNG（放在 PSD 旁的 <名字>_images 文件夹），\n并按图层顺序和位置建立插槽。");
        if (buttons("选择 PSD...")) {
            std::string path = openFileDialog("Photoshop (*.psd)", "*.psd");
            if (!path.empty()) importPsdPath(path, io_.psdIncludeHidden);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

bool Editor::runExport(const std::string& type, const std::string& path) {
    if (type == "json" || type == "bin") {
        io_.dataBinary = type == "bin";
        return exportDataPath(path);
    }
    if (type == "atlas") return exportAtlasPath(path);
    if (type == "merge") return importMergePath(path);
    if (type == "psd") return importPsdPath(path, true);
    const char* types[] = {"png", "sheet", "gif", "mp4", "webm"};
    for (int i = 0; i < 5; ++i)
        if (type == types[i]) {
            io_.imageType = i;
            io_.imageAnim = anim_;
            return exportImagesPath(path);
        }
    setStatus("未知的导出类型 " + type);
    return false;
}
