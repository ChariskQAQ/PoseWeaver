#include "editor/editor.h"
#include "editor/i18n.h"
#include "core/file_io.h"
#include "core/raster.h"
#include <json.hpp>
#include <stb_image_write.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <set>
#include <stdexcept>

using namespace rig;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {
struct ExportTexture { int width, height; std::shared_ptr<const std::vector<unsigned char>> pixels; };
struct Cancelled {};

std::string exportDestination(const std::string& path, const std::string& project) {
    fs::path output = fs::u8path(path);
    if (output.is_relative() && !project.empty()) output = fs::u8path(project).parent_path() / output;
    return fs::absolute(output).lexically_normal().u8string();
}

EngineExportResult bakeEngineFrames(const Skeleton& sk, const std::vector<ExportTexture>& images,
        const EngineExportOptions& options, const std::vector<int>& skins, bool physicsOn,
        bool estimate, EngineExportProgress& progress) {
    EngineExportResult result;
    fs::path folder;
    std::vector<fs::path> owned;
    auto checkCancelled = [&] { if (progress.gate == EngineExportProgress::Cancelled) throw Cancelled{}; };
    auto rollback = [&] {
        std::error_code ec;
        for (const auto& file : owned) fs::remove(file, ec);
        if (!folder.empty()) fs::remove(folder, ec);
    };
    try {
        checkCancelled();
        std::string error;
        if (!validateSkeleton(sk, &error)) throw std::runtime_error(error);
        const double fps = options.fps, scale = options.scale, ppu = options.pixelsPerUnit;
        if (!std::isfinite(fps) || fps < 1 || fps > 120 || !std::isfinite(scale) || scale < 0.05 || scale > 4 ||
            !std::isfinite(ppu) || ppu <= 0 || ppu > 10000) throw std::runtime_error("Invalid FPS, scale or pixels per unit");
        const int padding = std::max(options.padding, effectsPadding(sk.effects, options.scale));
        const int ss = sk.effects.pixel.enabled ? 1 : options.supersample;
        if (options.padding < 0 || options.padding > 128 || options.supersample < 1 || options.supersample > 4) throw std::runtime_error("Invalid padding / supersampling");
        fs::path destination;
        if (!estimate) {
            destination = fs::absolute(fs::u8path(options.path)).lexically_normal();
            if (destination.extension() != ".pwframes") throw std::runtime_error("Expected .pwframes output file");
        }
        json clips = json::array();
        std::set<std::string> names;
        int total = 0;
        for (size_t a = 0; a < std::max(size_t(1), sk.animations.size()); ++a) {
            const Animation* animation = sk.animations.empty() ? nullptr : &sk.animations[a];
            const std::string name = animation ? animation->name : "setup";
            const auto choice = std::find_if(options.clips.begin(), options.clips.end(), [&](const EngineClipOption& item) { return item.name == name; });
            if (choice != options.clips.end() && !choice->selected) continue;
            const double duration = animation ? animation->duration : 1.0 / fps;
            if (name.empty() || !names.insert(name).second || !std::isfinite(duration) || duration <= 0)
                throw std::runtime_error("Animations need unique nonempty names and positive durations");
            double count = std::max(1.0, std::ceil(duration * fps - 1e-6));
            if (count > 10000 - total || clips.size() >= 256) throw std::runtime_error("Export limit: 256 animations / 10000 frames");
            total += (int)count;
            clips.push_back({{"name", name}, {"duration", duration}, {"loop", choice == options.clips.end() || choice->loop},
                {"frames", json::array()}, {"count", (int)count}, {"animationIndex", a}});
        }
        if (clips.empty()) throw std::runtime_error("Select at least one animation");
        progress.total = total * (estimate ? 1 : 2);
        auto sampleFrames = [&](auto action) {
            for (size_t a = 0; a < clips.size(); ++a) {
                const Animation* animation = sk.animations.empty() ? nullptr : &sk.animations[clips[a]["animationIndex"].get<size_t>()];
                PhysicsState physics;
                for (int f = 0; f < clips[a]["count"].get<int>(); ++f) {
                    checkCancelled();
                    Pose pose = makeAnimatedPose(sk, animation, (float)(f / fps), skins);
                    if (physicsOn) applyPhysics(sk, pose, physics, f == 0 ? 0.0f : (float)(1 / fps));
                    action(a, f, pose);
                    ++progress.completed;
                }
            }
        };
        // One canvas including world origin for every animation keeps engine anchors stable.
        Vec2 lo{0, 0}, hi{0, 0};
        bool visible = false;
        sampleFrames([&](size_t, int, const Pose& pose) {
            Vec2 a, b;
            if (poseBounds(sk, pose, a, b)) {
                visible = true;
                lo = {std::min(lo.x, a.x), std::min(lo.y, a.y)};
                hi = {std::max(hi.x, b.x), std::max(hi.y, b.y)};
            }
        });
        const double width = std::max(1.0, std::ceil((hi.x - lo.x) * scale) + 2 * padding);
        const double height = std::max(1.0, std::ceil((hi.y - lo.y) * scale) + 2 * padding);
        if (!visible || !std::isfinite(width) || !std::isfinite(height) || width > 4096 || height > 4096 ||
            width * height * ss * ss > 16777216) throw std::runtime_error("No visible content or frame too large; reduce scale / supersampling");
        int w = (int)width, h = (int)height, maxCols = 4096 / w, capacity = maxCols * (4096 / h);
        auto& budget = result.budget;
        budget.animations = (int)clips.size(); budget.frames = total; budget.width = w; budget.height = h;
        uint64_t largestPage = 0;
        for (int remaining = total; remaining > 0; remaining -= std::min(capacity, remaining)) {
            int count = std::min(capacity, remaining), cols = std::min(maxCols, count);
            uint64_t bytes = uint64_t(cols * w) * (((count + cols - 1) / cols) * h) * 4;
            budget.atlasBytes += bytes; largestPage = std::max(largestPage, bytes); ++budget.pages;
        }
        budget.rasterBytes = largestPage + uint64_t(w) * h * 4 * (ss * ss + 1);
        budget.rasterBytes += effectsBufferBytes(sk.effects, uint64_t(w) * h * ss * ss);
        checkCancelled();
        if (estimate) return result;
        for (int n = 1; n <= 10000; ++n) {
            fs::path candidate = destination.parent_path() / fs::u8path(destination.stem().u8string() + "_frames" + (n == 1 ? "" : "_" + std::to_string(n)));
            std::error_code ec;
            if (fs::create_directory(candidate, ec)) { folder = candidate; break; }
            if (ec && ec != std::errc::file_exists) throw std::runtime_error(ec.message());
        }
        if (folder.empty()) throw std::runtime_error("Cannot create a new resource directory");
        std::vector<RasterTexture> textures;
        for (const auto& texture : images) textures.push_back({texture.width, texture.height,texture.pixels ? texture.pixels->data() : nullptr});
        json pages = json::array();
        std::vector<unsigned char> sheet;
        int index = 0, page = -1, columns = 0, pageWidth = 0, pageHeight = 0, pageCount = 0;
        Affine toPixel{(float)(scale * ss), 0, 0, (float)(-scale * ss),
            (float)((padding - lo.x * scale) * ss), (float)((padding + hi.y * scale) * ss)};
        sampleFrames([&](size_t a, int f, const Pose& pose) {
            progress.stage = EngineExportProgress::Render;
            int cell = index % capacity;
            if (cell == 0) {
                ++page;
                pageCount = std::min(capacity, total - index);
                columns = std::min(maxCols, pageCount);
                pageWidth = columns * w;
                pageHeight = ((pageCount + columns - 1) / columns) * h;
                sheet.assign(size_t(pageWidth) * pageHeight * 4, 0);
            }
            std::vector<unsigned char> high(size_t(w * ss) * (h * ss) * 4, 0);
            renderPose(sk, pose, textures, toPixel, w * ss, h * ss, high);
            checkCancelled();
            auto pixels = downsample(high, w * ss, h * ss, ss);
            int x = (cell % columns) * w, y = (cell / columns) * h;
            for (int row = 0; row < h; ++row)
                std::copy_n(pixels.data() + size_t(row) * w * 4, size_t(w) * 4, sheet.data() + (size_t(y + row) * pageWidth + x) * 4);
            double seconds = std::min(1 / fps, clips[a]["duration"].get<double>() - f / fps);
            if (seconds <= 0) throw std::runtime_error("Invalid frame duration");
            clips[a]["frames"].push_back({{"page", page}, {"x", x}, {"y", y}, {"width", w}, {"height", h}, {"duration", seconds}});
            ++index;
            if (cell + 1 == pageCount) {
                progress.stage = EngineExportProgress::Write;
                checkCancelled();
                const std::string name = "page_" + std::to_string(page) + ".png";
                owned.push_back(folder / name);
                if (!stbi_write_png(owned.back().u8string().c_str(), pageWidth, pageHeight, 4, sheet.data(), pageWidth * 4))
                    throw std::runtime_error("Cannot write sprite sheet");
                pages.push_back({{"file", folder.filename().generic_u8string() + "/" + name}, {"width", pageWidth}, {"height", pageHeight}});
            }
        });
        for (auto& clip : clips) { clip.erase("count"); clip.erase("animationIndex"); }
        json document{{"format", "poseweaver.frames"}, {"version", 1}, {"filter", sk.effects.pixel.enabled ? "nearest" : "linear"}, {"fps", fps}, {"pixelsPerUnit", ppu},
            {"frameWidth", w}, {"frameHeight", h}, {"originX", padding - lo.x * scale}, {"originY", padding + hi.y * scale},
            {"pages", pages}, {"animations", clips}};
        // Commit manifest last; an unsuccessful re-export cannot break the previous resource set.
        const std::string manifest = document.dump(2);
        if (manifest.size() > 4 * 1024 * 1024) throw std::runtime_error("Manifest exceeds 4 MiB; reduce animation names or frame count");
        int expected = EngineExportProgress::Running;
        if (!progress.gate.compare_exchange_strong(expected, EngineExportProgress::Committing)) throw Cancelled{};
        progress.stage = EngineExportProgress::Commit;
        if (!writeFileAtomic(destination.u8string(), manifest, &error)) throw std::runtime_error(error);
    } catch (const Cancelled&) {
        rollback();
        result.cancelled = true;
    } catch (const std::exception& error) {
        rollback();
        result.error = error.what();
    }
    return result;
}
} // namespace

void Editor::syncEngineClips() {
    std::vector<EngineClipOption> clips;
    for (size_t a = 0; a < std::max(size_t(1), sk_.animations.size()); ++a) {
        std::string name = sk_.animations.empty() ? "setup" : sk_.animations[a].name;
        auto found = std::find_if(engineOptions_.clips.begin(), engineOptions_.clips.end(), [&](const EngineClipOption& item) { return item.name == name; });
        clips.push_back(found == engineOptions_.clips.end() ? EngineClipOption{name} : *found);
    }
    engineOptions_.clips = std::move(clips);
}

bool Editor::finishEngineExport(const EngineExportResult& result, bool estimate, const std::string& path) {
    engineNoticeError_ = !result.error.empty();
    if (result.cancelled) {
        engineNotice_ = tr("已取消；上一次导出资源保持完整。");
        setStatus(engineNotice_);
        return false;
    }
    if (!result.error.empty()) {
        engineBudget_ = {};
        engineNotice_ = std::string(tr("引擎资源导出失败: ")) + result.error;
        return failOperation("ENGINE_EXPORT_FAILED", engineNotice_, path);
    }
    engineBudget_ = result.budget;
    dismissIssue("ENGINE_EXPORT_FAILED");
    engineNotice_ = estimate ? tr("资源预算已更新。") : tr("已导出 Unity / Godot 帧动画包 ") + fs::u8path(path).filename().u8string();
    setStatus(engineNotice_);
    return true;
}

bool Editor::exportEnginePath(const std::string& path) {
    if (engineJob_ || imageJob_ || exportRecoveryTask_.valid() || !checkExportResources()) return false;
    try {
        EngineExportOptions options = engineOptions_;
        options.path = path;
        std::vector<ExportTexture> textures;
        for (const auto& image : textures_) textures.push_back({image.width, image.height, image.pixels});
        EngineExportProgress progress;
        auto result = bakeEngineFrames(sk_, textures, options, activeSkins_, physicsOn_, false, progress);
        if (!finishEngineExport(result, false, path)) return false;
        engineOptions_.path = fs::absolute(fs::u8path(path)).lexically_normal().u8string();
        saveEngineExportSettings();
        return true;
    } catch (const std::exception& error) {
        EngineExportResult result; result.error = error.what();
        return finishEngineExport(result, false, path);
    }
}

bool Editor::startEngineExport(bool estimate) {
    if (engineJob_ || imageJob_ || exportRecoveryTask_.valid()) { setStatus(tr("已有导出任务正在运行，请先完成或取消。")); return false; }
    if (!checkExportResources()) { engineNotice_ = status_; engineNoticeError_ = true; return false; }
    try {
        if (!estimate && !engineOptions_.path.empty()) engineOptions_.path = exportDestination(engineOptions_.path, projectPath_);
        auto job = std::make_unique<EngineExportJob>();
        job->estimate = estimate; job->path = engineOptions_.path;
        std::vector<ExportTexture> images;
        if (!estimate) for (const auto& image : textures_) images.push_back({image.width, image.height, image.pixels});
        auto progress = job->progress;
        job->result = std::async(std::launch::async,
            [sk = sk_, options = engineOptions_, skins = activeSkins_, physicsOn = physicsOn_,
             images = std::move(images), estimate, progress]() {
                return bakeEngineFrames(sk, images, options, skins, physicsOn, estimate, *progress);
            });
        engineJob_ = std::move(job);
        engineBudget_ = {}; engineNotice_.clear(); engineNoticeError_ = false;
        saveEngineExportSettings();
        return true;
    } catch (const std::exception& error) {
        EngineExportResult result; result.error = error.what();
        return finishEngineExport(result, estimate, engineOptions_.path);
    }
}

void Editor::pollEngineExport() {
    if (engineSettingsDirty_ && (engineSettingsDelay_ -= ImGui::GetIO().DeltaTime) <= 0 && !ImGui::IsAnyItemActive()) saveEngineExportSettings();
    if (!engineJob_ || engineJob_->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    EngineExportResult result;
    try { result = engineJob_->result.get(); }
    catch (const std::exception& error) { result.error = error.what(); }
    if (engineJob_->progress->gate == EngineExportProgress::Cancelled) { result.cancelled = true; result.error.clear(); }
    finishEngineExport(result, engineJob_->estimate, engineJob_->path);
    engineJob_.reset();
}

void Editor::resetEngineExport() {
    if (engineJob_) {
        engineJob_->progress->cancel();
        engineJob_->result.wait();
        engineJob_.reset();
    }
    if (engineSettingsDirty_) saveEngineExportSettings();
    engineOptions_ = {}; engineBudget_ = {}; engineNotice_.clear(); engineNoticeError_ = false;
    engineSettingsDirty_ = false; engineSettingsDelay_ = 0;
}

void Editor::loadEngineExportSettings() {
    engineOptions_ = {};
    try {
        std::ifstream file(fs::u8path(configFile("engine-exports.json")));
        if (file) {
            auto root = json::parse(file);
            auto entry = root.find(projectPath_);
            if (entry != root.end()) {
                const auto& j = *entry;
                EngineExportOptions options;
                options.fps = j.at("fps").get<float>(); options.scale = j.at("scale").get<float>();
                options.pixelsPerUnit = j.at("pixelsPerUnit").get<float>();
                options.supersample = j.at("supersample").get<int>(); options.padding = j.at("padding").get<int>();
                options.path = j.at("path").get<std::string>();
                if (!std::isfinite(options.fps) || options.fps < 1 || options.fps > 120 || !std::isfinite(options.scale) || options.scale < 0.05 || options.scale > 4 ||
                    !std::isfinite(options.pixelsPerUnit) || options.pixelsPerUnit <= 0 || options.pixelsPerUnit > 10000 ||
                    options.supersample < 1 || options.supersample > 4 || options.padding < 0 || options.padding > 128 ||
                    !j.at("clips").is_array() || j.at("clips").size() > 10000) throw std::runtime_error("Invalid export settings");
                for (const auto& clip : j.at("clips")) options.clips.push_back({clip.at("name").get<std::string>(), clip.at("selected").get<bool>(), clip.at("loop").get<bool>()});
                engineOptions_ = std::move(options);
            }
        }
    } catch (const std::exception& error) {
        reportIssue("ENGINE_SETTINGS_FAILED", error.what(), Severity::Warning, configFile("engine-exports.json"));
    }
    syncEngineClips();
}

void Editor::saveEngineExportSettings() {
    engineSettingsDirty_ = false;
    if (projectPath_.empty()) return;
    const std::string path = configFile("engine-exports.json");
    try {
        json root = json::object();
        std::ifstream file(fs::u8path(path));
        if (file) { root = json::parse(file); if (!root.is_object()) throw std::runtime_error("Invalid export settings file"); }
        file.close();
        const auto& options = engineOptions_;
        json clips = json::array();
        for (const auto& clip : options.clips) clips.push_back({{"name", clip.name}, {"selected", clip.selected}, {"loop", clip.loop}});
        json entry = {{"fps", options.fps}, {"scale", options.scale}, {"pixelsPerUnit", options.pixelsPerUnit},
            {"supersample", options.supersample}, {"padding", options.padding},
            {"path", options.path.empty() ? std::string() : exportDestination(options.path, projectPath_)}, {"clips", clips}};
        if (root.contains(projectPath_) && root.at(projectPath_)==entry) { dismissIssue("ENGINE_SETTINGS_FAILED"); return; }
        root[projectPath_]=std::move(entry);
        std::string error;
        if (!writeFileAtomic(path, root.dump(2), &error)) throw std::runtime_error(error);
        dismissIssue("ENGINE_SETTINGS_FAILED");
    } catch (const std::exception& error) {
        reportIssue("ENGINE_SETTINGS_FAILED", error.what(), Severity::Warning, path);
    }
}
