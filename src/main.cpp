#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#endif

#include "editor/editor.h"
#include "editor/theme.h"

#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

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

struct Options {
    std::string open;        // project or image to open at startup
    std::string demoImage;   // --demo <png>
    float demoTime = 0.5f;   // --time <seconds>
    std::string scenario;      // --weights | --mesh | --clip
    bool demoGraph = false;    // --graph
    bool splash = false;       // --splash: show the start-up page even with a demo / file
    bool issues = false;       // --issues: open the diagnostics panel
    std::string lang;          // --lang zh|en (this run only)
    std::string screenshot;  // --screenshot <png>: save after a few frames and exit
    std::vector<std::pair<std::string, std::string>> runs;  // --run <type> <path>: import/export, then exit
};

// Command-line arguments as UTF-8 (on Windows argv is in the ANSI code page, which loses non-ASCII paths).
static std::vector<std::string> utf8Args(int argc, char** argv) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    std::vector<std::string> out;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 0; w && i < n; ++i) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(len > 0 ? len - 1 : 0, ' ');
        if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
        out.push_back(std::move(s));
    }
    if (w) LocalFree(w);
    return out;
#else
    return std::vector<std::string>(argv, argv + argc);
#endif
}

// The exe is a GUI program; when started from a console with no redirected output, print there.
static void attachParentConsole() {
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if ((out == nullptr || out == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
#endif
}

static Options parseArgs(const std::vector<std::string>& args) {
    Options o;
    const int argc = (int)args.size();
    for (int i = 1; i < argc; ++i) {
        std::string a = args[i];
        auto next = [&]() { return i + 1 < argc ? args[++i] : std::string(); };
        if (a == "--demo") o.demoImage = next();
        else if (a == "--time") o.demoTime = std::strtof(next().c_str(), nullptr);
        else if (a == "--weights" || a == "--mesh" || a == "--clip" || a == "--ik" || a == "--path" || a == "--ghost" || a == "--bake" || a == "--skin" || a == "--graphcolor" || a == "--mix") o.scenario = a.substr(2);
        else if (a == "--graph") o.demoGraph = true;
        else if (a == "--splash") o.splash = true;
        else if (a == "--issues") o.issues = true;
        else if (a == "--lang") o.lang = next();
        else if (a == "--screenshot") o.screenshot = next();
        else if (a == "--run") {
            std::string type = next();
            o.runs.push_back({type, next()});
        }
        else o.open = a;
    }
    return o;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    // Establish the UI apartment before the audio engine initializes COM.
    struct UiApartment {
        HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ~UiApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    } apartment;
#endif
    std::vector<std::string> args = utf8Args(argc, argv);
    if (args.size() > 1) attachParentConsole();
    Options opt = parseArgs(args);
    glfwSetErrorCallback(onError);
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
    if (!opt.runs.empty() && opt.screenshot.empty()) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Poseweaver", nullptr, nullptr);
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
    theme::apply(scale);
    theme::loadFonts(scale);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    {
        Editor editor;
        editor.setUiScale(scale);
        if (!opt.lang.empty()) editor.setLanguage(opt.lang, false);
        g_editor = &editor;
        glfwSetDropCallback(window, onDrop);
        if (!opt.demoImage.empty()) editor.buildDemo(opt.demoImage, opt.demoTime, opt.scenario);
        if (opt.demoGraph) editor.focusGraph();
        if (opt.issues) editor.openIssues();
        if (opt.demoImage.empty() && !opt.open.empty()) editor.onFilesDropped({opt.open});
        if (!opt.runs.empty()) {
            int failed = 0;
            for (auto& [type, path] : opt.runs) {
                bool ok = editor.runExport(type, path);
                failed += !ok;
                std::printf("%s %s: %s\n", ok ? "ok" : "FAILED", type.c_str(), editor.status().c_str());
            }
            if (opt.screenshot.empty()) {
                g_editor = nullptr;
                ImGui_ImplOpenGL3_Shutdown();
                ImGui_ImplGlfw_Shutdown();
                ImGui::DestroyContext();
                glfwDestroyWindow(window);
                glfwTerminate();
                return failed ? 1 : 0;
            }
        }
        // Start-up page when nothing was given on the command line (or when asked for).
        if (opt.splash || (opt.demoImage.empty() && opt.open.empty() && opt.runs.empty() && editor.splashEnabled()))
            editor.openSplash();
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
        if (opt.screenshot.empty()) editor.saveSession();  // "restore last session" on the splash
        g_editor = nullptr;
    }  // Editor releases its textures while the GL context is still alive.

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
