#include "editor/image_export.h"
#include "editor/export_transaction.h"
#include "editor/sequence_manifest.h"
#include "core/raster.h"
#include "core/constraints.h"
#include "platform/audio.h"
#include "platform/encoder_pipe.h"
#include <stb_image_write.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
namespace fs = std::filesystem;
using namespace rig;
namespace {
bool isSequence(int t) { return t == 0 || t == 5; }
bool isVideo(int t) { return t == 3 || t == 4 || t == 6 || t == 7; }
bool isOpaque(int t) { return t == 3 || t == 5 || t == 6; }
bool writePng(const std::string& path,int w,int h,const std::vector<unsigned char>& rgba) { return stbi_write_png(path.c_str(),w,h,4,rgba.data(),w*4) != 0; }
bool writeBytes(const std::string& path,const std::vector<uint8_t>& data) { std::ofstream f(fs::u8path(path),std::ios::binary); f.write((const char*)data.data(),(std::streamsize)data.size()); return bool(f); }
struct Cancelled {};
}
ImageExportResult bakeImages(const Skeleton& sk_,const std::vector<ImageExportTexture>& images,
    const ImageExportOptions& options,const std::vector<int>& activeSkins_,bool physicsOn_,const std::string& path,EngineExportProgress& progress,bool estimate,const std::string& recoveryDirectory) {
    ImageExportResult result;
    try {
        if (!validImageExportOptions(options)) throw std::runtime_error("Invalid image export options");
        auto checkCancelled = [&] { if (progress.gate == EngineExportProgress::Cancelled) throw Cancelled{}; };
        auto fail = [](const char*,const std::string& error,const std::string& file) -> bool { throw std::runtime_error(error + ": " + file); };
        checkCancelled();
        const auto destination = fs::absolute(fs::u8path(path)).lexically_normal();
        if (destination.filename().empty() || destination.filename() == "." || destination.filename() == "..") throw std::runtime_error("Invalid destination");
        std::unique_ptr<ExportTransaction> staging;
        if (!estimate) staging = std::make_unique<ExportTransaction>(destination.parent_path(),fs::u8path(recoveryDirectory));
        const char* extensions[] = {".png",".png",".gif",".mp4",".webm",".jpg",".avi",".mov"};
        fs::path stagedOutput = (estimate ? fs::path() : staging->directory) / (std::string("output") + extensions[options.imageType]);
        auto render = [&]() -> bool {

    const Animation* an = options.imageAnim >= 0 && options.imageAnim < (int)sk_.animations.size() ? &sk_.animations[options.imageAnim] : nullptr;
    std::vector<RasterTexture> tex;
    for (const auto& t : images) {
        const auto& rgba = t.pixels ? *t.pixels : t.rgba;
        tex.push_back({t.width,t.height,rgba.empty() ? nullptr : rgba.data()});
    }
    const float fps = std::max(1.0f, options.imageFps);
    if (an && (!std::isfinite(an->duration) || an->duration < 0 || an->duration * fps > 9999)) throw std::runtime_error("Animation exceeds 10000 frames");
    const int frames = an ? std::max(1, (int)std::round(an->duration * fps)) + (isSequence(options.imageType) ? 1 : 0) : 1;
    // Poses for every frame (physics simulated in order), and the union of their bounds.

    progress.total = frames * 2;
    PhysicsState phys;
    Vec2 lo{1e9f, 1e9f}, hi{-1e9f, -1e9f};
    for (int f = 0; f < frames; ++f) {
        checkCancelled();
        Pose p = makeAnimatedPose(sk_, an, f / fps, activeSkins_);
        if (physicsOn_) applyPhysics(sk_, p, phys, f == 0 ? 0.0f : 1 / fps);
        Vec2 a, b;
        if (poseBounds(sk_, p, a, b)) {
            lo = {std::min(lo.x, a.x), std::min(lo.y, a.y)};
            hi = {std::max(hi.x, b.x), std::max(hi.y, b.y)};
        }
        ++progress.completed;
    }
    if (lo.x > hi.x) {
        return fail("IMAGE_EXPORT_FAILED", "No visible attachments to render", path);
    }
    const float scale = std::max(0.05f, options.imageScale);
    const int pad = std::max(std::max(0, options.imagePadding), effectsPadding(sk_.effects, scale));
    double width = std::ceil(double(hi.x - lo.x) * scale) + 2 * pad, height = std::ceil(double(hi.y - lo.y) * scale) + 2 * pad;
    if (!std::isfinite(width) || !std::isfinite(height) || width < 1 || height < 1 || width > 16384 || height > 16384) throw std::runtime_error("Frame exceeds 16384 pixels per side");
    int w = (int)width, h = (int)height;
    if (isVideo(options.imageType)) {  // video codecs want even sizes
        w += w & 1;
        h += h & 1;
    }
    const int ss = sk_.effects.pixel.enabled ? 1 : std::clamp(options.imageSupersample, 1, 4);
    uint64_t frameBytes = uint64_t(w) * h * 4, highPixels = uint64_t(w) * h * ss * ss;
    uint64_t working = highPixels * 4 + frameBytes + effectsBufferBytes(sk_.effects,highPixels);
    if (options.imageType == 2) working += frameBytes * frames * 2;
    if (working > 512ull * 1024 * 1024) throw std::runtime_error("Export buffers exceed 512 MiB; reduce scale, antialiasing or duration");
    if (options.imageType == 1) {
        int cols = (int)std::ceil(std::sqrt((float)frames)), rows = (frames+cols-1)/cols;
        if (w*cols > 16384 || h*rows > 16384) throw std::runtime_error("Sprite sheet exceeds size limit");
        working += uint64_t(w)*cols*h*rows*4;
    }
    if (working > 512ull*1024*1024) throw std::runtime_error("Export buffers exceed 512 MiB");
    result.width = w; result.height = h; result.frames = frames; result.bufferBytes = working;
    if (estimate) return true;
    phys = {};
    auto poseAt = [&](int f) {
        checkCancelled(); progress.stage = EngineExportProgress::Render; ++progress.completed;
        Pose p = makeAnimatedPose(sk_,an,f / fps,activeSkins_);
        if (physicsOn_) applyPhysics(sk_,p,phys,f == 0 ? 0.0f : 1 / fps);
        return p;
    };
    Affine toPx{scale * ss, 0, 0, -scale * ss, (pad - lo.x * scale) * ss, (pad + hi.y * scale) * ss};
    bool opaque = isOpaque(options.imageType);
    auto renderFrame = [&](const Pose& p) {
        std::vector<unsigned char> img(size_t(w * ss) * (h * ss) * 4, 0);
        if (options.imageBackground || opaque)
            for (size_t i = 0; i < img.size(); i += 4) {
                img[i] = (unsigned char)(options.imageBgColor[0] * 255);
                img[i + 1] = (unsigned char)(options.imageBgColor[1] * 255);
                img[i + 2] = (unsigned char)(options.imageBgColor[2] * 255);
                img[i + 3] = 255;
            }
        renderPose(sk_, p, tex, toPx, w * ss, h * ss, img);
        auto pixels = downsample(img, w * ss, h * ss, ss);
        progress.stage = EngineExportProgress::Write;
        return pixels;
    };

    fs::path out = stagedOutput;
    std::string stem = out.stem().u8string();
    switch (options.imageType) {
    case 0: {  // PNG sequence
        int written = 0;
        for (int f = 0; f < frames; ++f) {
            char num[16];
            std::snprintf(num, sizeof(num), "_%04d.png", f);
            const std::string file = (out.parent_path() / fs::u8path(stem + num)).u8string();
            if (!writePng(file, w, h, renderFrame(poseAt(f)))) return fail("IMAGE_EXPORT_FAILED", "Could not write PNG", file);
            ++written;
        }
        return written == frames;
    }
    case 5: {  // JPG sequence
        int written = 0;
        for (int f = 0; f < frames; ++f) {
            char num[16];
            std::snprintf(num, sizeof(num), "_%04d.jpg", f);
            std::vector<unsigned char> img = renderFrame(poseAt(f));
            const std::string file = (out.parent_path() / fs::u8path(stem + num)).u8string();
            if (!stbi_write_jpg(file.c_str(), w, h, 4, img.data(), std::clamp(options.jpgQuality, 1, 100)))
                return fail("IMAGE_EXPORT_FAILED", "Could not write JPG", file);
            ++written;
        }
        return written == frames;
    }
    case 1: {  // sprite sheet
        int cols = (int)std::ceil(std::sqrt((float)frames)), rows = (frames + cols - 1) / cols;
        std::vector<unsigned char> sheet(size_t(w * cols) * (h * rows) * 4, 0);
        for (int f = 0; f < frames; ++f) {
            std::vector<unsigned char> img = renderFrame(poseAt(f));
            int ox = (f % cols) * w, oy = (f / cols) * h;
            for (int y = 0; y < h; ++y)
                std::copy(img.begin() + size_t(y) * w * 4, img.begin() + size_t(y + 1) * w * 4,
                          sheet.begin() + (size_t(oy + y) * w * cols + ox) * 4);
        }
        bool ok = writePng(out.u8string(), w * cols, h * rows, sheet);
        if (!ok) return fail("IMAGE_EXPORT_FAILED", "Failed to write the sprite sheet", path);
        return ok;
    }
    case 2: {  // GIF
        std::vector<std::vector<unsigned char>> imgs;
        for (int f = 0; f < frames; ++f) imgs.push_back(renderFrame(poseAt(f)));
        bool ok = writeBytes(out.u8string(), encodeGif(imgs, w, h, std::max(2, (int)std::round(100 / fps)), true));
        if (!ok) return fail("IMAGE_EXPORT_FAILED", "Failed to write the GIF", path);
        return ok;
    }
    default: {  // video through ffmpeg

        char sizeArg[32], fpsArg[32];
        std::snprintf(sizeArg, sizeof(sizeArg), "%dx%d", w, h);
        std::snprintf(fpsArg, sizeof(fpsArg), "%g", fps);
        const char* codecs[] = {"-c:v libx264 -pix_fmt yuv420p -crf 18", "-c:v libvpx-vp9 -pix_fmt yuva420p -b:v 0 -crf 30",
                                "-c:v mjpeg -pix_fmt yuvj444p -q:v 2", "-c:v prores_ks -profile:v 4444 -pix_fmt yuva444p10le"};
        const char* audioCodecs[] = {" -c:a aac -b:a 192k", " -c:a libopus -b:a 128k", " -c:a pcm_s16le", " -c:a pcm_s16le"};
        const int vi = options.imageType == 3 ? 0 : options.imageType == 4 ? 1 : options.imageType == 6 ? 2 : 3;
        std::string codec = codecs[vi];
        // Audio events mixed into one track (written next to the video, removed afterwards).
        std::string audioInput, audioCodec;
        fs::path wav = out.parent_path() / fs::u8path(stem + "_audio.wav");
        AudioPlayer decoder(false);
        if (options.videoAudio && an) {
            if (double(frames) / fps * 48000 * 8 > 128ull * 1024 * 1024) throw std::runtime_error("Audio mix exceeds 128 MiB");
            std::vector<AudioPlacement> placements;
            for (const AudioEventKey& k : audioEventKeys(sk_, *an)) {
                const EventDef& def = sk_.events[k.event];
                if (const AudioSamples* s = decoder.samples(def.audioPath)) placements.push_back({s, k.time, def.volume, def.balance});
            }
            if (!placements.empty()) {
                std::vector<float> mix = mixAudio(placements, frames / fps, 48000);
                std::vector<uint8_t> bytes = encodeWav(mix, 48000);
                std::ofstream wf(wav, std::ios::binary);
                wf.write((const char*)bytes.data(), (std::streamsize)bytes.size());
                wf.close();
                if (!wf) return fail("VIDEO_EXPORT_FAILED", "Could not write the video audio track", wav.u8string());
                if (wf) {
                    audioInput = " -i \"" + wav.u8string() + "\" -map 0:v -map 1:a";
                    audioCodec = audioCodecs[vi];
                }
            }
        }
        std::string cmd = "\"" + options.ffmpegPath + "\" -y -loglevel error -f rawvideo -pix_fmt rgba -s " + sizeArg + " -r " + fpsArg +
                          " -i -" + audioInput + " " + codec + audioCodec + " \"" + out.u8string() + "\"";
        EncoderPipe pipe(cmd,progress,staging->directory / "encoder.log");
        bool written = true;
        for (int f = 0; f < frames; ++f) {
            std::vector<unsigned char> img = renderFrame(poseAt(f));
            if (!pipe.write(img)) { written = false; break; }
        }
        int code = pipe.finish();
        std::error_code rmEc;
        if (!audioInput.empty()) fs::remove(wav, rmEc);
        if (!written || code != 0) {
            std::ifstream log(staging->directory / "encoder.log",std::ios::binary);
            char detail[4096] = {}; log.read(detail,sizeof(detail)-1);
            return fail("VIDEO_EXPORT_FAILED", "ffmpeg encoding failed (exit code " + std::to_string(code) + "): " + detail, path);
        }
        return code == 0;
    }
    }

        };
        if (!render()) throw std::runtime_error("Image export failed");
        if (estimate) return result;
        checkCancelled();
        if (isSequence(options.imageType)) stageSequenceManifest(*staging,destination,extensions[options.imageType],options.cleanOldFrames,result,checkCancelled);
        for (const auto& file : fs::directory_iterator(staging->directory)) {
            const auto name = file.path().filename().u8string();
            if (file.path() == stagedOutput) staging->add(file.path(),destination);
            else if (isSequence(options.imageType) && name.rfind("output_",0) == 0 && file.path().extension() == extensions[options.imageType])
                staging->add(file.path(),destination.parent_path() / fs::u8path(destination.stem().u8string() + name.substr(6)));
        }
        int expected = EngineExportProgress::Running;
        if (!progress.gate.compare_exchange_strong(expected,EngineExportProgress::Committing)) throw Cancelled{};
        progress.stage = EngineExportProgress::Commit;
        staging->commit();
    } catch (const Cancelled&) { result.cancelled = true; }
    catch (const std::exception& error) { if (progress.gate == EngineExportProgress::Cancelled) result.cancelled = true; else result.error = error.what(); }
    return result;
}

bool operator==(const ImageExportOptions& a,const ImageExportOptions& b) {
    return a.imageType==b.imageType && a.jpgQuality==b.jpgQuality && a.imageAnim==b.imageAnim && a.imageSupersample==b.imageSupersample && a.imagePadding==b.imagePadding &&
        a.imageScale==b.imageScale && a.imageFps==b.imageFps && a.imageBackground==b.imageBackground && a.videoAudio==b.videoAudio && a.cleanOldFrames==b.cleanOldFrames &&
        std::equal(std::begin(a.imageBgColor),std::end(a.imageBgColor),std::begin(b.imageBgColor)) && a.ffmpegPath==b.ffmpegPath;
}

bool validImageExportOptions(const ImageExportOptions& o) {
    return o.imageType >= 0 && o.imageType <= 7 && o.jpgQuality >= 1 && o.jpgQuality <= 100 &&
        std::isfinite(o.imageScale) && o.imageScale >= 0.05f && o.imageScale <= 8 && std::isfinite(o.imageFps) && o.imageFps >= 1 && o.imageFps <= 120 &&
        o.imageSupersample >= 1 && o.imageSupersample <= 4 && o.imagePadding >= 0 && o.imagePadding <= 128 &&
        std::all_of(std::begin(o.imageBgColor),std::end(o.imageBgColor),[](float v) { return std::isfinite(v) && v >= 0 && v <= 1; });
}
std::string imageExportDestination(const std::string& path,const std::string& project) {
    auto output = fs::u8path(path);
    if (output.is_relative() && !project.empty()) output = fs::u8path(project).parent_path()/output;
    return fs::absolute(output).lexically_normal().u8string();
}
