#include "editor/editor.h"
#include "benchmark_fixture.h"
#include "core/serialize.h"
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif
#include <GLFW/glfw3.h>
#include <stb_image_write.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <numeric>
#include <thread>
#include <cmath>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
struct EditorTestAccess {
    static void animate(Editor& e) { e.setMode(Mode::Animate); e.playing_ = true; }
    static void checkpoint(Editor& e) { e.checkpoint(); e.sk_.bones[0].name += "x"; }
    static void checkpointLocal(Editor& e) { e.selectBone(0); e.renameSelection(e.sk_.bones[0].name+"x"); }
    static void checkpointBone(Editor& e) { e.checkpointBone(0); e.sk_.bones[0].setup.x += 1; }
    static void undo(Editor& e) { e.undo(); }
    static void start(Editor& e,const std::string& path) {
        e.io_.imageAnim = 0; e.io_.imageSupersample = 1;
        if (!e.startImageExport(path)) throw std::runtime_error(e.status());
    }
    static void stop(Editor& e) { e.resetImageExport(); }
    static void describe(Editor& e,bool project) {
        size_t bytes=0; for (const auto& texture : e.textures_) bytes+=texture.rgba().size();
        std::fprintf(stderr,"GPU=%s; renderer=%s; fixture=%s; bones=%zu; slots=%zu; textures=%zu; source_RGBA_MiB=%.1f; hidden UI CPU build, GPU submission/presentation excluded\n",
            glGetString(GL_VENDOR),glGetString(GL_RENDERER),project ? "project" : "synthetic",e.sk_.bones.size(),e.sk_.slots.size(),e.textures_.size(),double(bytes)/1048576);
    }
    static void beginDrag(Editor& e) { e.setMode(Mode::Setup); e.checkpointBone(e.sk_.bones.size()>1 ? 1 : 0); }
    static void drag(Editor& e) {
        if (e.sk_.bones.empty()) throw std::runtime_error("No bone to measure");
        int bone=e.sk_.bones.size()>1 ? 1 : 0; auto t=e.sk_.bones[bone].setup; t.rotation+=0.1f;
        e.transformBone(bone,t,true,false,false,false);
    }
};
template<class F> void measure(const char* fixture,const char* stage,int samples,F action) {
    std::vector<double> times;
    for (int i = 0; i < samples; ++i) {
        const auto start = Clock::now(); action();
        const auto elapsed=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
        if (!std::isfinite(elapsed) || elapsed<0) throw std::runtime_error("Invalid benchmark timing");
        times.push_back(elapsed);
    }
    std::sort(times.begin(),times.end());
    std::printf("%s,%s,%d,%.3f,%.3f,%.3f\n",fixture,stage,samples,
        std::accumulate(times.begin(),times.end(),0.0)/times.size(),times[(times.size()-1)/2],times[(times.size()*95+99)/100-1]);
}
int main(int argc,char** argv) {
    const bool smoke = argc == 2 && std::string(argv[1]) == "--smoke";
    const bool project = argc == 3 && std::string(argv[1]) == "--project";
    if (argc > 1 && !smoke && !project) { std::fprintf(stderr,"usage: rigeditor_bench [--smoke | --project path]\n"); return 1; }
    if (!glfwInit()) return 77;
    glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
    auto* window = glfwCreateWindow(1280,800,"Editor benchmark",nullptr,nullptr);
    if (!window) { glfwTerminate(); return 77; }
    glfwMakeContextCurrent(window); ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = {1280,800}; io.DeltaTime = 1.0f/60;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    unsigned char* pixels; int width,height; io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    const auto parent = fs::current_path();
    const auto root = parent/("editor-benchmark-"+std::to_string(Clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    int code = 0;
    try {
        auto path = root/"stress.rig.json";
        const int count = smoke ? 32 : 512, side = smoke ? 64 : 2048, images = smoke ? 1 : 8;
        if (project) {
#ifdef _WIN32
            int nativeCount=0; auto** native=CommandLineToArgvW(GetCommandLineW(),&nativeCount);
            if (!native || nativeCount!=3) throw std::runtime_error("Invalid project argument");
            path=fs::absolute(fs::path(native[2])); LocalFree(native);
#else
            path=fs::absolute(fs::u8path(argv[2]));
#endif
        }
        else {
            auto sk = rig::bench::fixture(count,5,false); sk.images.clear();
            std::vector<unsigned char> rgba(size_t(side)*side*4,255);
            for (int i = 0; i < images; ++i) {
                auto image = root/("texture"+std::to_string(i)+".png");
                if (!stbi_write_png(image.u8string().c_str(),side,side,4,rgba.data(),side*4)) throw std::runtime_error("Cannot create benchmark image");
                sk.images.push_back({image.u8string(),side,side});
            }
            for (int i = 0; i < count; ++i) sk.slots[i].attachments[0].image = i%images;
            std::string error; if (!rig::saveFile(path.u8string(),sk,&error)) throw std::runtime_error(error);
        }
        std::puts("fixture,stage,samples,mean_ms,p50_ms,p95_ms");
        Editor editor((root/"profile").u8string());
        const auto name = project ? "user_project" : smoke ? "smoke" : "512_mesh_128MiB";
        measure(name,"open",smoke ? 1 : 5,[&] { if (!editor.openProject(path.u8string())) throw std::runtime_error(editor.status()); });
        EditorTestAccess::describe(editor,project);
        EditorTestAccess::animate(editor);
        for (int i = 0; i < 5; ++i) { ImGui::NewFrame(); editor.frame(1.0f/60); ImGui::Render(); }
        measure(name,"play_ui_cpu",smoke ? 3 : 60,[&] { ImGui::NewFrame(); editor.frame(1.0f/60); ImGui::Render(); });
        EditorTestAccess::beginDrag(editor);
        measure(name,"drag_update",smoke ? 3 : 60,[&] { EditorTestAccess::drag(editor); });
        EditorTestAccess::undo(editor);
        measure(name,"checkpoint",smoke ? 1 : 5,[&] { EditorTestAccess::checkpoint(editor); });
        measure(name,"undo",smoke ? 1 : 5,[&] { EditorTestAccess::undo(editor); });
        measure(name,"checkpoint_name_local",smoke ? 1 : 5,[&] { EditorTestAccess::checkpointLocal(editor); });
        measure(name,"undo_name_local",smoke ? 1 : 5,[&] { EditorTestAccess::undo(editor); });
        measure(name,"checkpoint_bone_local",smoke ? 1 : 5,[&] { EditorTestAccess::checkpointBone(editor); });
        measure(name,"undo_bone_local",smoke ? 1 : 5,[&] { EditorTestAccess::undo(editor); });
        // Measure only the UI call; cancellation and worker drain happen outside the timed region.
        for (int i = 0; i < (smoke ? 1 : 5); ++i) {
            measure(name,"export_start",1,[&] { EditorTestAccess::start(editor,(root/"output.png").u8string()); });
            EditorTestAccess::stop(editor);
        }
    } catch (const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); code = 1; }
    ImGui::DestroyContext(); glfwDestroyWindow(window); glfwTerminate();
    if (root.parent_path() == parent) fs::remove_all(root);
    return code;
}
