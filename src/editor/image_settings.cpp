#include "editor/editor.h"
#include "editor/i18n.h"
#include "core/file_io.h"
#include <json.hpp>
#include <fstream>
#include <filesystem>
#include <algorithm>
using nlohmann::json;
namespace fs = std::filesystem;

void Editor::loadImageExportSettings() {
    io_.imageAnim = anim_;
    if (projectPath_.empty()) return;
    try {
        std::ifstream file(fs::u8path(configFile("image-exports.json")));
        if (!file) return;
        auto root = json::parse(file);
        if (!root.is_object()) throw std::runtime_error("Invalid image export settings file");
        if (!root.contains(projectPath_)) return;
        const auto& j = root.at(projectPath_);
        if (j.at("version") != 1) throw std::runtime_error("Unsupported image export settings");
        auto integer = [&](const char* name,int minimum,int maximum) {
            const auto& value = j.at(name);
            if (!value.is_number_integer() || value < minimum || value > maximum) throw std::runtime_error("Invalid integer export setting");
            return value.get<int>();
        };
        ImageExportOptions o;
        o.imageType = integer("type",0,7); o.imageFps = j.at("fps").get<float>(); o.imageScale = j.at("scale").get<float>();
        o.imageSupersample = integer("supersample",1,4); o.imagePadding = integer("padding",0,128); o.jpgQuality = integer("quality",1,100);
        o.imageBackground = j.at("background").get<bool>(); o.videoAudio = j.at("audio").get<bool>(); o.cleanOldFrames = j.at("cleanOldFrames").get<bool>();
        if (!j.at("color").is_array() || j.at("color").size() != 3) throw std::runtime_error("Invalid background color");
        auto color = j.at("color").get<std::array<float,3>>(); std::copy(color.begin(),color.end(),o.imageBgColor);
        o.ffmpegPath = j.at("ffmpeg").get<std::string>();
        auto path = j.at("path").get<std::string>();
        if (!validImageExportOptions(o) || path.size() > 32768 || o.ffmpegPath.size() > 32768 || path.find('\0') != std::string::npos || o.ffmpegPath.find('\0') != std::string::npos) throw std::runtime_error("Invalid image export settings");
        if (!j.at("animation").is_null()) {
            const auto name = j.at("animation").get<std::string>();
            for (int i = 0; i < (int)sk_.animations.size(); ++i) if (sk_.animations[i].name == name) o.imageAnim = i;
            if (o.imageAnim < 0) reportIssue("IMAGE_SETTINGS_FAILED",tr("导出动作已不存在，已改为 Setup 姿态。"),rig::Severity::Warning);
        }
        static_cast<ImageExportOptions&>(io_) = std::move(o);
        imageOutputPath_ = path.empty() ? path : imageExportDestination(path,projectPath_);
    } catch (const std::exception& error) {
        imageSettingsReadOnly_ = true;
        reportIssue("IMAGE_SETTINGS_FAILED",error.what(),rig::Severity::Warning,configFile("image-exports.json"));
    }
}

void Editor::saveImageExportSettings() {
    imageSettingsDirty_ = false;
    if (projectPath_.empty()) return;
    const auto path = configFile("image-exports.json");
    try {
        if (imageSettingsReadOnly_) throw std::runtime_error("Invalid image export settings preserved; repair or rename the settings file and reopen the project");
        json root = json::object();
        std::ifstream file(fs::u8path(path));
        if (file) { root = json::parse(file); if (!root.is_object()) throw std::runtime_error("Invalid image export settings file; original preserved"); }
        file.close();
        const auto o = imageOptions();
        if (!validImageExportOptions(o)) throw std::runtime_error("Invalid image export options");
        json animation = nullptr;
        if (o.imageAnim >= 0 && o.imageAnim < (int)sk_.animations.size()) animation = sk_.animations[o.imageAnim].name;
        json entry = {{"version",1},{"type",o.imageType},{"fps",o.imageFps},{"scale",o.imageScale},{"supersample",o.imageSupersample},
            {"padding",o.imagePadding},{"quality",o.jpgQuality},{"background",o.imageBackground},{"color",o.imageBgColor},{"audio",o.videoAudio},
            {"cleanOldFrames",o.cleanOldFrames},{"ffmpeg",o.ffmpegPath},{"animation",animation},
            {"path",imageOutputPath_.empty() ? std::string() : imageExportDestination(imageOutputPath_,projectPath_)}};
        if (root.contains(projectPath_) && root.at(projectPath_)==entry) { dismissIssue("IMAGE_SETTINGS_FAILED"); return; }
        root[projectPath_]=std::move(entry);
        std::string error;
        if (!rig::writeFileAtomic(path,root.dump(2),&error)) throw std::runtime_error(error);
        dismissIssue("IMAGE_SETTINGS_FAILED");
    } catch (const std::exception& error) { reportIssue("IMAGE_SETTINGS_FAILED",error.what(),rig::Severity::Warning,path); }
}
