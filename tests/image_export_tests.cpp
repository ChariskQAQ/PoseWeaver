#include "editor/image_export.h"
#include "editor/export_transaction.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <cstdio>
#include <limits>
#include <json.hpp>
namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n",__LINE__,#x); ++failures; } } while (false)
static void put(const fs::path& file,const std::string& text) { std::ofstream(file,std::ios::binary) << text; }
static std::string get(const fs::path& file) { std::ifstream in(file,std::ios::binary); return std::string(std::istreambuf_iterator<char>(in),{}); }
int main() {
    auto parent = fs::current_path(), root = parent / ("image-export-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())); fs::create_directory(root);
    rig::Skeleton sk; sk.addBone("root",-1,{},0); sk.images.push_back({"pixel",16,16});
    rig::Slot slot; slot.activeAttachment = 0; slot.attachments.push_back({}); slot.attachments[0].image = 0; sk.slots.push_back(slot);
    sk.animations.push_back({}); sk.animations[0].name = "wave"; sk.animations[0].duration = 0.2f;
    std::vector<ImageExportTexture> textures{{16,16,std::vector<unsigned char>(16*16*4,255)}};
    ImageExportOptions options; options.imageAnim = 0; options.imageSupersample = 1;
    for (int type : {0,1,2,5}) {
        options.imageType = type; EngineExportProgress p;
        auto result = bakeImages(sk,textures,options,{},false,(root / ("result"+std::to_string(type))).u8string(),p);
        CHECK(result.error.empty() && !result.cancelled && p.gate == EngineExportProgress::Committing);
    }
    CHECK(fs::is_regular_file(root / "result0_0000.png") && fs::is_regular_file(root / "result5_0000.jpg"));
    {
        ImageExportOptions o; o.imageAnim = 0; o.imageSupersample = 1;
        EngineExportProgress budgetProgress;
        const auto budget = bakeImages(sk,{},o,{},false,(root / "estimate-only.png").u8string(),budgetProgress,true);
        CHECK(budget.error.empty() && budget.width > 0 && budget.frames == 7 && budget.bufferBytes > 0);
        CHECK(!fs::exists(root / "estimate-only.png.pwexport.json"));
        auto run = [&](const char* name) { EngineExportProgress p; return bakeImages(sk,textures,o,{},false,(root / name).u8string(),p); };
        CHECK(run("tracked.png").error.empty());
        put(root / "tracked_0005.png","user edited"); put(root / "tracked_0099.png","untracked user file");
        sk.animations[0].duration = 0.05f;
        auto shorter = run("tracked.png");
        CHECK(shorter.error.empty() && shorter.preservedFrames == 1);
        CHECK(!fs::exists(root / "tracked_0004.png") && get(root / "tracked_0005.png") == "user edited" && get(root / "tracked_0099.png") == "untracked user file");
        sk.animations[0].duration = 0.2f; CHECK(run("optional.png").error.empty());
        o.cleanOldFrames = false; sk.animations[0].duration = 0.05f; CHECK(run("optional.png").error.empty());
        CHECK(fs::is_regular_file(root / "optional_0006.png"));
        o.cleanOldFrames = true; CHECK(run("optional.png").error.empty()); CHECK(!fs::exists(root / "optional_0006.png"));
        auto manifest = nlohmann::json::parse(get(root / "optional.png.pwexport.json"));
        manifest["files"][0]["file"] = "../outside.txt";
        put(root / "optional.png.pwexport.json",manifest.dump());
        const auto before = get(root / "optional_0000.png");
        CHECK(!run("optional.png").error.empty() && get(root / "optional_0000.png") == before);
        sk.animations[0].duration = 0.2f;
    }
    {
        options.imageType = 0; EngineExportProgress p; p.cancel();
        auto r = bakeImages(sk,textures,options,{},false,(root / "cancel.png").u8string(),p); CHECK(r.cancelled && r.error.empty());
    }
    {
        sk.animations[0].duration = 50; options.imageFps = 120; options.imageScale = 8;
        EngineExportProgress p; put(root / "cancel_0000.png","previous");
        auto work = std::async(std::launch::async,[&] { return bakeImages(sk,textures,options,{},false,(root / "cancel.png").u8string(),p); });
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while (p.completed <= p.total/2 && work.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(p.cancel()); const auto r = work.get(); CHECK(r.cancelled && get(root / "cancel_0000.png") == "previous");
    }
    {
        sk.animations[0].duration = 0.2f; options.imageScale = std::numeric_limits<float>::quiet_NaN(); EngineExportProgress p;
        CHECK(!bakeImages(sk,textures,options,{},false,(root / "invalid.png").u8string(),p).error.empty());
        options.imageScale = 8; options.imageType = 2; options.imageFps = 120; sk.animations[0].duration = 50;
        EngineExportProgress large; CHECK(!bakeImages(sk,textures,options,{},false,(root / "large.gif").u8string(),large).error.empty());
    }
#ifdef _WIN32
    {
        options.imageType = 3; options.videoAudio = false; options.ffmpegPath = ENCODER_BLOCK_FIXTURE;
        EngineExportProgress p; put(root / "blocked.mp4","previous video");
        auto work = std::async(std::launch::async,[&] { return bakeImages(sk,textures,options,{},false,(root / "blocked.mp4").u8string(),p); });
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while (p.completed <= p.total/2 && work.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(p.cancel()); CHECK(work.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        const auto result = work.get();
        if (!result.cancelled) std::printf("Blocked encoder error: %s\n",result.error.c_str());
        CHECK(result.cancelled && get(root / "blocked.mp4") == "previous video");
    }
#endif
    {
        put(root / "first.png","old"); fs::create_directory(root / "second.png");
        ExportTransaction transaction(root); put(transaction.directory / "first.png","new"); put(transaction.directory / "second.png","new");
        transaction.add(transaction.directory / "first.png",root / "first.png");
        put(root / "removed.png","old tail"); transaction.remove(root / "removed.png");
        transaction.add(transaction.directory / "second.png",root / "second.png");
        bool failed = false; try { transaction.commit(); } catch (...) { failed = true; }
        CHECK(failed && get(root / "first.png") == "old" && get(root / "removed.png") == "old tail" && fs::is_directory(root / "second.png"));
    }
    for (const auto& file : fs::directory_iterator(root)) CHECK(file.path().filename().u8string().rfind(".poseweaver-export-",0) != 0);
    if (root.parent_path() == parent) fs::remove_all(root);
    std::printf("image export: %d failure(s)\n",failures); return failures ? 1 : 0;
}
