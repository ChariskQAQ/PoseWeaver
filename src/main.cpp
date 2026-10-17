#include "editor/editor.h"

#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#include <stb_image_write.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

static Editor* g_editor = nullptr;

static void onDrop(GLFWwindow*, int count, const char** paths) {
    // GLFW hands over UTF-8 paths.
    std::vector<std::string> files(paths, paths + count);
    if (g_editor) g_editor->onFilesDropped(files);
}

// Saves the current framebuffer as a PNG (used for automated visual checks).
static bool saveScreenshot(GLFWwindow* window, const std::string& path) {
    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    std::vector<unsigned char> px(size_t(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    stbi_flip_vertically_on_write(1);
    return stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4) != 0;
}

static void onError(int code, const char* desc) { std::fprintf(stderr, "GLFW error %d: %s\n", code, desc); }

static void loadFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    const float size = 17.0f * scale;
    static ImVector<ImWchar> ranges;
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesChineseFull());
    static const ImWchar extra[] = {0x2000, 0x206F, 0x2190, 0x21FF, 0x25A0, 0x25FF, 0xFF00, 0xFFEF, 0};
    builder.AddRanges(extra);
    builder.BuildRanges(&ranges);

    const char* candidates[] = {"C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf",
                                "/System/Library/Fonts/PingFang.ttc",
                                "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"};
    for (const char* path : candidates) {
        if (!std::filesystem::exists(path)) continue;
        ImFontConfig cfg;
        cfg.OversampleH = 1;
        cfg.PixelSnapH = true;
        if (io.Fonts->AddFontFromFileTTF(path, size, &cfg, ranges.Data)) return;
    }
    ImFontConfig cfg;
    cfg.SizePixels = size;
    io.Fonts->AddFontDefault(&cfg);  // no CJK font found: Chinese labels will render as '?'
}

static void applyStyle(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 4;
    s.FrameRounding = 3;
    s.GrabRounding = 3;
    s.TabRounding = 3;
    s.FramePadding = ImVec2(6, 4);
    s.ItemSpacing = ImVec2(7, 5);
    s.Colors[ImGuiCol_WindowBg] = ImVec4(0.13f, 0.14f, 0.16f, 1);
    s.Colors[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.12f, 0.14f, 1);
    s.Colors[ImGuiCol_Header] = ImVec4(0.85f, 0.55f, 0.15f, 0.35f);
    s.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.85f, 0.55f, 0.15f, 0.55f);
    s.Colors[ImGuiCol_HeaderActive] = ImVec4(0.85f, 0.55f, 0.15f, 0.75f);
    s.ScaleAllSizes(scale);
}

struct Options {
    std::string open;        // project or image to open at startup
    std::string demoImage;   // --demo <png>
    float demoTime = 0.5f;   // --time <seconds>
    bool demoWeights = false;  // --weights
    std::string screenshot;  // --screenshot <png>: save after a few frames and exit
};

static Options parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--demo") o.demoImage = next();
        else if (a == "--time") o.demoTime = std::strtof(next().c_str(), nullptr);
        else if (a == "--weights") o.demoWeights = true;
        else if (a == "--screenshot") o.screenshot = next();
        else o.open = a;
    }
    return o;
}

int main(int argc, char** argv) {
    Options opt = parseArgs(argc, argv);
    glfwSetErrorCallback(onError);
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(1600, 900, "RigEditor", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    float xs = 1, ys = 1;
    glfwGetWindowContentScale(window, &xs, &ys);
    float scale = std::max(1.0f, xs);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    applyStyle(scale);
    loadFonts(scale);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    {
        Editor editor;
        editor.setUiScale(scale);
        g_editor = &editor;
        glfwSetDropCallback(window, onDrop);
        if (!opt.demoImage.empty()) editor.buildDemo(opt.demoImage, opt.demoTime, opt.demoWeights);
        else if (!opt.open.empty()) editor.onFilesDropped({opt.open});
        int frameCount = 0;

        double last = glfwGetTime();
        while (true) {
            glfwPollEvents();
            if (glfwWindowShouldClose(window)) {
                if (editor.requestClose()) break;
                glfwSetWindowShouldClose(window, GLFW_FALSE);
            }
            if (editor.shouldQuit()) break;
            if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
                glfwWaitEventsTimeout(0.1);
                last = glfwGetTime();
                continue;
            }
            double now = glfwGetTime();
            float dt = (float)std::min(now - last, 0.1);
            last = now;

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            editor.frame(dt);
            ImGui::Render();

            int w, h;
            glfwGetFramebufferSize(window, &w, &h);
            glViewport(0, 0, w, h);
            glClearColor(0.1f, 0.1f, 0.11f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if (!opt.screenshot.empty() && ++frameCount == 6) {
                bool ok = saveScreenshot(window, opt.screenshot);
                std::printf("%s %s\n", ok ? "saved" : "failed to save", opt.screenshot.c_str());
                break;
            }
            glfwSwapBuffers(window);
        }
        g_editor = nullptr;
    }  // Editor releases its textures while the GL context is still alive.

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
