#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "benchmark_fixture.h"
#include "core/diagnostics.h"
#include "editor/editor.h"
#include <imgui_impl_opengl3.h>
#include <json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numeric>
#include <stdexcept>
#include <thread>

using Clock = std::chrono::steady_clock;
using nlohmann::json;
namespace fs = std::filesystem;

struct EditorTestAccess {
    static void prepare(Editor& editor, rig::Skeleton skeleton, int width, int height) {
        editor.sk_ = std::move(skeleton);
        editor.pose_ = rig::makeAnimatedPose(editor.sk_, &editor.sk_.animations[0], 0.25f);
        editor.sel_.clear();
        editor.canvasOrigin_ = ImVec2(0, 0);
        editor.canvasSize_ = ImVec2((float)width, (float)height);
        editor.zoom_ = std::min(width, height) * 0.85f / 256;
        editor.viewOffset_ = ImVec2(-128 * editor.zoom_, 128 * editor.zoom_);
        if (editor.textures_.empty()) {
            editor.textures_.resize(1);
            Texture& texture = editor.textures_[0];
            texture.width = texture.height = 1;
            texture.pixels = std::make_shared<const std::vector<unsigned char>>(std::initializer_list<unsigned char>{255,255,255,255});
            glGenTextures(1, &texture.id);
            glBindTexture(GL_TEXTURE_2D, texture.id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texture.rgba().data());
        }
    }
    static void draw(Editor& editor) { editor.drawSlots(ImGui::GetForegroundDrawList()); }
};

struct Context {
    GLFWwindow* window = nullptr;
    bool imgui = false, backend = false;
    void open() {
        if (!glfwInit()) throw std::runtime_error("GLFW initialization failed");
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        window = glfwCreateWindow(64, 64, "Poseweaver GPU measurement", nullptr, nullptr);
        if (!window) throw std::runtime_error("OpenGL 3.3 context unavailable");
        glfwMakeContextCurrent(window);
        glfwSwapInterval(0);
        if (!gladLoadGL(glfwGetProcAddress) || !GLAD_GL_VERSION_3_3)
            throw std::runtime_error("OpenGL 3.3 functions unavailable");
        ImGui::CreateContext();
        imgui = true;
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DeltaTime = 1.0f / 60;
        io.DisplayFramebufferScale = ImVec2(1, 1);
        backend = ImGui_ImplOpenGL3_Init("#version 330");
        if (!backend) throw std::runtime_error("ImGui OpenGL renderer initialization failed");
    }
    ~Context() {
        if (backend) ImGui_ImplOpenGL3_Shutdown();
        if (imgui) ImGui::DestroyContext();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
    }
};

struct Target {
    GLuint framebuffer = 0, texture = 0;
    void create(int width, int height) {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("Offscreen framebuffer is incomplete");
    }
    ~Target() {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &texture);
    }
};

struct Queries {
    std::vector<GLuint> ids;
    explicit Queries(int count) : ids(count) { glGenQueries(count, ids.data()); }
    ~Queries() { glDeleteQueries((GLsizei)ids.size(), ids.data()); }
};

struct ProfileDirectory {
    fs::path parent = fs::temp_directory_path();
    fs::path path = parent / ("gpu-bench-" + std::to_string(Clock::now().time_since_epoch().count()));
    ProfileDirectory() { if (!fs::create_directory(path)) throw std::runtime_error("Cannot create isolated settings directory"); }
    ~ProfileDirectory() {
        std::error_code error;
        if (path.parent_path() == parent) fs::remove_all(path, error);
    }
};

struct Case {
    const char* name;
    int bones, grid, width, height;
    bool clipping, mixed;
};

static double elapsed(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

static void report(const Case& test, int drawCalls, int callbacks, const char* metric, std::vector<double> times) {
    if (times.empty() || std::any_of(times.begin(), times.end(), [](double ms) { return !std::isfinite(ms) || ms < 0; }))
        throw std::runtime_error("Invalid timing samples");
    std::sort(times.begin(), times.end());
    double mean = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
    std::printf("%s,%d,%d,%s,%d,%d,%d,%d,%d,%d,%zu,%s,%.6f,%.6f,%.6f,%.6f,%.6f\n", test.name,
        test.width, test.height, test.mixed ? "mixed" : "normal", test.bones + 1, test.bones + (test.clipping ? 1 : 0),
        test.bones * test.grid * test.grid, test.bones * (test.grid - 1) * (test.grid - 1) * 2, drawCalls, callbacks,
        times.size(), metric, mean, times[(times.size() - 1) / 2], times[(size_t)std::ceil(times.size() * 0.95) - 1], times.front(), times.back());
}

static void run(const Case& test, Editor& editor, int samples) {
    rig::Skeleton skeleton = rig::bench::fixture(test.bones, test.grid, test.clipping);
    if (!test.mixed) for (auto& slot : skeleton.slots) slot.blend = rig::BlendMode::Normal;
    std::string error;
    if (!rig::validateSkeleton(skeleton, &error)) throw std::runtime_error(error);
    EditorTestAccess::prepare(editor, std::move(skeleton), test.width, test.height);
    Target target;
    target.create(test.width, test.height);
    ImGui::GetIO().DisplaySize = ImVec2((float)test.width, (float)test.height);
    auto build = [&] {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        EditorTestAccess::draw(editor);
        ImGui::Render();
    };
    auto clear = [&] {
        glViewport(0, 0, test.width, test.height);
        glClearColor(0.1f, 0.1f, 0.11f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    };
    for (int i = 0; i < 10; ++i) { build(); clear(); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); }
    glFinish(); // Drain warm-up once; no forced wait inside measured frames.
    Queries queries(samples);
    std::vector<double> cpuBuild, cpuSubmit, gpu;
    for (int i = 0; i < samples; ++i) {
        auto start = Clock::now();
        build();
        cpuBuild.push_back(elapsed(start));
        clear();
        glBeginQuery(GL_TIME_ELAPSED, queries.ids[i]);
        start = Clock::now();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        cpuSubmit.push_back(elapsed(start));
        glEndQuery(GL_TIME_ELAPSED);
        glFlush(); // Submit the end marker before next frame's CPU clipping can starve the GPU queue.
    }
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    for (GLuint query : queries.ids) {
        GLint available = GL_FALSE;
        while (!available) {
            glGetQueryObjectiv(query, GL_QUERY_RESULT_AVAILABLE, &available);
            if (available) break;
            if (Clock::now() >= deadline) throw std::runtime_error("GPU query timed out; results are incomplete");
            glfwPollEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        GLuint64 nanoseconds = 0;
        glGetQueryObjectui64v(query, GL_QUERY_RESULT, &nanoseconds);
        gpu.push_back(double(nanoseconds) / 1e6);
    }
    if (std::none_of(gpu.begin(), gpu.end(), [](double value) { return value > 0; }))
        throw std::runtime_error("GPU timer returned only zero samples");
    std::vector<unsigned char> pixels(size_t(test.width) * test.height * 4);
    glReadPixels(0, 0, test.width, test.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    size_t painted = 0;
    for (size_t p = 0; p < pixels.size(); p += 4) if (pixels[p] > 32) ++painted;
    if (painted == 0 || glGetError() != GL_NO_ERROR) throw std::runtime_error("GPU rendering validation failed");
    int drawCalls = 0, callbacks = 0;
    for (const ImDrawList* list : ImGui::GetDrawData()->CmdLists)
        for (const ImDrawCmd& command : list->CmdBuffer) {
            if (command.UserCallback) ++callbacks;
            else if (command.ElemCount) ++drawCalls;
        }
    if (!drawCalls || (test.mixed && !callbacks)) throw std::runtime_error("Expected draw commands are missing");
    report(test, drawCalls, callbacks, "cpu_draw_list", cpuBuild);
    report(test, drawCalls, callbacks, "cpu_submit", cpuSubmit);
    report(test, drawCalls, callbacks, "gpu_draw", gpu);
}

int main(int argc, char** argv) {
    const bool smoke = argc == 2 && std::string(argv[1]) == "--smoke";
    if (argc > 1 && !smoke) { std::fprintf(stderr, "usage: riggpu_bench [--smoke]\n"); return 1; }
    const int samples = smoke ? 3 : 120;
    json metadata{{"benchmark_version", 1}, {"status", "error"}, {"samples", samples}, {"warmup", 10}, {"timer", "GL_TIME_ELAPSED"},
        {"query_readback", "deferred until GL_QUERY_RESULT_AVAILABLE"}, {"target", "offscreen RGBA8, single sample"},
        {"flush_each_frame", true}, {"cpu_submit_scope", "RenderDrawData only; clear, query calls, flush and readback excluded"},
        {"swap_buffers", false}, {"pose", "fixed at 0.25 seconds"}, {"texture", "synthetic 1x1 opaque white"}};
    int result = 1;
    try {
        Context context;
        context.open();
        metadata["vendor"] = (const char*)glGetString(GL_VENDOR);
        metadata["renderer"] = (const char*)glGetString(GL_RENDERER);
        metadata["opengl_version"] = (const char*)glGetString(GL_VERSION);
        GLint bits = 0;
        glGetQueryiv(GL_TIME_ELAPSED, GL_QUERY_COUNTER_BITS, &bits);
        metadata["timer_bits"] = bits;
        if (bits <= 0) { metadata["status"] = "unsupported"; result = 77; }
        else {
            ProfileDirectory profile;
            Editor editor(profile.path.u8string());
            std::puts("fixture,width,height,blend,bones,slots,mesh_vertices,mesh_triangles,draw_calls,callbacks,samples,metric,mean_ms,p50_ms,p95_ms,min_ms,max_ms");
            for (const Case& test : {
                Case{"small", 32, 3, 1280, 720, false, true},
                Case{"medium", 128, 5, 1280, 720, false, true},
                Case{"large_clipped", 512, 5, 1280, 720, true, true},
                Case{"large_clipped", 512, 5, 1280, 720, true, false},
                Case{"large_clipped", 512, 5, 1920, 1080, true, true},
                Case{"large_clipped", 512, 5, 1920, 1080, true, false}}) run(test, editor, samples);
            metadata["status"] = "ok";
            result = 0;
        }
    } catch (const std::exception& error) { metadata["error"] = error.what(); }
    std::fprintf(stderr, "%s\n", metadata.dump(2).c_str());
    return result;
}
