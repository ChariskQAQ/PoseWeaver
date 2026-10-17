#include "editor/theme.h"

#include <filesystem>

namespace theme {

static ImFont* g_large = nullptr;

ImFont* largeFont() { return g_large ? g_large : ImGui::GetFont(); }

void loadFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    static ImVector<ImWchar> ranges;
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesChineseFull());
    static const ImWchar extra[] = {0x2000, 0x206F, 0x2190, 0x21FF, 0x25A0, 0x25FF, 0xFF00, 0xFFEF, 0};
    builder.AddRanges(extra);
    builder.BuildRanges(&ranges);

    const char* candidates[] = {"C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf",
                                "/System/Library/Fonts/PingFang.ttc",
                                "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"};
    const char* found = nullptr;
    for (const char* path : candidates)
        if (std::filesystem::exists(path)) {
            found = path;
            break;
        }
    ImFontConfig cfg;
    cfg.OversampleH = 1;
    cfg.PixelSnapH = true;
    if (found) {
        io.Fonts->AddFontFromFileTTF(found, 16.0f * scale, &cfg, ranges.Data);
        g_large = io.Fonts->AddFontFromFileTTF(found, 30.0f * scale, &cfg, io.Fonts->GetGlyphRangesDefault());
    } else {
        cfg.SizePixels = 16.0f * scale;
        io.Fonts->AddFontDefault(&cfg);  // no CJK font found: Chinese labels will render as '?'
        cfg.SizePixels = 30.0f * scale;
        g_large = io.Fonts->AddFontDefault(&cfg);
    }
}

void apply(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 3;
    s.ChildRounding = 4;
    s.FrameRounding = 3;
    s.PopupRounding = 3;
    s.GrabRounding = 3;
    s.TabRounding = 3;
    s.WindowBorderSize = 1;
    s.FrameBorderSize = 0;
    s.WindowPadding = ImVec2(6, 6);
    s.FramePadding = ImVec2(6, 3);
    s.ItemSpacing = ImVec2(6, 4);
    s.ItemInnerSpacing = ImVec2(5, 4);
    s.IndentSpacing = 16;
    s.ScrollbarSize = 12;
    s.TabBarBorderSize = 1;
    s.DockingSeparatorSize = 3;

    auto rgb = [](int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); };
    ImVec4* c = s.Colors;
    const ImVec4 accent = rgb(41, 141, 217), accentHi = rgb(66, 160, 232), accentLo = rgb(30, 112, 176);
    c[ImGuiCol_Text] = rgb(222, 222, 222);
    c[ImGuiCol_TextDisabled] = rgb(130, 130, 134);
    c[ImGuiCol_WindowBg] = rgb(60, 60, 63);
    c[ImGuiCol_ChildBg] = rgb(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = rgb(52, 52, 55, 0.98f);
    c[ImGuiCol_Border] = rgb(36, 36, 38);
    c[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = rgb(44, 44, 47);
    c[ImGuiCol_FrameBgHovered] = rgb(54, 54, 58);
    c[ImGuiCol_FrameBgActive] = rgb(40, 40, 43);
    c[ImGuiCol_TitleBg] = rgb(42, 42, 45);
    c[ImGuiCol_TitleBgActive] = rgb(42, 42, 45);
    c[ImGuiCol_TitleBgCollapsed] = rgb(42, 42, 45);
    c[ImGuiCol_MenuBarBg] = rgb(42, 42, 45);
    c[ImGuiCol_ScrollbarBg] = rgb(48, 48, 51);
    c[ImGuiCol_ScrollbarGrab] = rgb(92, 92, 96);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(110, 110, 115);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(125, 125, 130);
    c[ImGuiCol_CheckMark] = accentHi;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accentHi;
    c[ImGuiCol_Button] = rgb(84, 84, 88);
    c[ImGuiCol_ButtonHovered] = rgb(100, 100, 105);
    c[ImGuiCol_ButtonActive] = accentLo;
    c[ImGuiCol_Header] = rgb(41, 141, 217, 0.80f);
    c[ImGuiCol_HeaderHovered] = rgb(80, 80, 85);
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Separator] = rgb(40, 40, 42);
    c[ImGuiCol_SeparatorHovered] = accentHi;
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = rgb(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = accentHi;
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_Tab] = rgb(48, 48, 51);
    c[ImGuiCol_TabHovered] = rgb(80, 80, 85);
    c[ImGuiCol_TabSelected] = rgb(60, 60, 63);
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = rgb(48, 48, 51);
    c[ImGuiCol_TabDimmedSelected] = rgb(60, 60, 63);
    c[ImGuiCol_TabDimmedSelectedOverline] = rgb(0, 0, 0, 0);
    c[ImGuiCol_DockingPreview] = rgb(255, 150, 40, 0.6f);  // orange drop rectangle
    c[ImGuiCol_DockingEmptyBg] = rgb(36, 36, 38);
    c[ImGuiCol_TableHeaderBg] = rgb(50, 50, 53);
    c[ImGuiCol_TableBorderStrong] = rgb(40, 40, 42);
    c[ImGuiCol_TableBorderLight] = rgb(52, 52, 55);
    c[ImGuiCol_TableRowBg] = rgb(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.02f);
    c[ImGuiCol_TextSelectedBg] = rgb(41, 141, 217, 0.45f);
    c[ImGuiCol_DragDropTarget] = rgb(255, 150, 40);
    c[ImGuiCol_NavCursor] = accentHi;
    s.ScaleAllSizes(scale);
}

} // namespace theme
