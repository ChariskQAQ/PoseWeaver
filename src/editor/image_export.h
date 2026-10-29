#pragma once
#include "core/model.h"
#include "editor/engine_export.h"

struct ImageExportOptions {
    int imageType = 0, jpgQuality = 90, imageAnim = -1, imageSupersample = 2, imagePadding = 8;
    float imageScale = 1, imageFps = 30;
    bool imageBackground = false, videoAudio = true, cleanOldFrames = true;
    float imageBgColor[3] = {0.2f,0.2f,0.22f};
    std::string ffmpegPath = "ffmpeg";
};
struct ImageExportTexture {
    int width, height;
    std::vector<unsigned char> rgba;
    std::shared_ptr<const std::vector<unsigned char>> pixels;
};
bool operator==(const ImageExportOptions& a,const ImageExportOptions& b);
bool validImageExportOptions(const ImageExportOptions& options);
std::string imageExportDestination(const std::string& path,const std::string& project);
struct ImageExportResult {
    bool cancelled = false; std::string error;
    int width = 0, height = 0, frames = 0, preservedFrames = 0;
    uint64_t bufferBytes = 0;
};
struct ImageExportJob {
    std::shared_ptr<EngineExportProgress> progress = std::make_shared<EngineExportProgress>();
    std::future<ImageExportResult> result;
    std::string path;
    bool video = false;
    bool estimate = false;
    ImageExportOptions options;
    uint64_t history = 0;
    std::vector<int> skins;
    bool physics = false;
};
ImageExportResult bakeImages(const rig::Skeleton& skeleton, const std::vector<ImageExportTexture>& images,
    const ImageExportOptions& options, const std::vector<int>& skins, bool physicsOn,
    const std::string& path, EngineExportProgress& progress, bool estimate = false,const std::string& recoveryDirectory = {});
