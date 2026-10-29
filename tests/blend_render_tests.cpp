#include "core/raster.h"
#include "editor/editor.h"
#include "platform/texture.h"
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <filesystem>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (false)

struct EditorTestAccess {
    static Texture& texture(Editor& editor) { editor.textures_.resize(1); return editor.textures_[0]; }
    static void draw(Editor& editor, const rig::Skeleton& skeleton, ImDrawList* list) {
        editor.sk_ = skeleton;
        editor.pose_ = rig::makeSetupPose(skeleton);
        editor.sel_.clear();
        editor.canvasOrigin_ = ImVec2(0, 0);
        editor.canvasSize_ = ImVec2(64, 64);
        editor.viewOffset_ = ImVec2(0, 0);
        editor.zoom_ = 1;
        editor.drawSlots(list);
    }
};

int main() {
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(64, 64, "Blend regression", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().DisplaySize = ImVec2(64, 64);
    ImGui::GetIO().DeltaTime = 1.0f / 60;
    CHECK(ImGui_ImplOpenGL3_Init("#version 330"));
    rig::Skeleton sk;
    sk.addBone("root", -1, {}, 0);
    sk.images.push_back({"pixel", 16, 16});
    rig::Slot slot;
    slot.activeAttachment = 0;
    slot.attachments.push_back({});
    slot.attachments[0].image = 0;
    sk.slots.push_back(slot);
    namespace fs = std::filesystem;
    const fs::path parent = fs::current_path();
    const fs::path profile = parent / ("blend-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!fs::create_directory(profile)) return 1;
    for (int textureAlpha : {255, 128, 0}) {
        Editor editor(profile.u8string());
        Texture& texture = EditorTestAccess::texture(editor);
        texture.width = texture.height = 1;
        texture.pixels = std::make_shared<const std::vector<unsigned char>>(std::initializer_list<unsigned char>{204,102,51,(unsigned char)textureAlpha});
        glGenTextures(1, &texture.id);
        glBindTexture(GL_TEXTURE_2D, texture.id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texture.rgba().data());
        for (float tintAlpha : {1.0f, 0.5f, 0.0f}) for (int m = 0; m < 4; ++m) {
            const auto mode = (rig::BlendMode)m;
            sk.slots[0].blend = mode;
            sk.slots[0].color = rig::Color(1, 0.5f, 0.75f, tintAlpha);
            std::vector<unsigned char> expected(64 * 64 * 4);
            for (size_t i = 0; i < expected.size(); i += 4) {
                expected[i] = 51; expected[i + 1] = 102; expected[i + 2] = 153; expected[i + 3] = 255;
            }
            rig::renderPose(sk, rig::makeSetupPose(sk), {{1, 1, texture.rgba().data()}},
                            rig::Affine{1, 0, 0, -1, 32, 32}, 64, 64, expected);
            ImGui_ImplOpenGL3_NewFrame();
            ImGui::NewFrame();
            ImDrawList* list = ImGui::GetForegroundDrawList();
            EditorTestAccess::draw(editor, sk, list);
            list->AddRectFilled(ImVec2(1, 1), ImVec2(8, 8), IM_COL32(0, 0, 255, 128));
            ImGui::Render();
            glViewport(0, 0, 64, 64);
            glClearColor(0.2f, 0.4f, 0.6f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            unsigned char actual[4];
            glReadPixels(31, 31, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, actual);
            for (int c = 0; c < 3; ++c) {
                int reference = expected[(32 * 64 + 31) * 4 + c];
                if (std::abs(actual[c] - reference) > 2) {
                    std::printf("mode=%d texture alpha=%d tint alpha=%g channel=%d GPU=%d CPU=%d\n",
                                m, textureAlpha, tintAlpha, c, actual[c], reference);
                    ++failures;
                }
            }
            glReadPixels(3, 60, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, actual);
            CHECK(std::abs(actual[0] - 25) <= 1 && std::abs(actual[1] - 51) <= 1 && std::abs(actual[2] - 204) <= 1);
            CHECK(glGetError() == GL_NO_ERROR);
        }
        CHECK(texture.premultipliedId != 0);
        const auto cached = texture.premultipliedId;
        CHECK(texture.premultiplied() == cached);
        releaseTexture(texture);
        CHECK(!glIsTexture(cached));
    }
    if (profile.parent_path() == parent) fs::remove_all(profile);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    std::printf("blend rendering: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
