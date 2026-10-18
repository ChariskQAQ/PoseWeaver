// Start-up splash (banner, new / recent / open / restore), the shortcuts window, and the persistent
// user settings behind them (recent files, splash on/off) plus the "restore last session" file.
#include "editor/editor.h"

#include "core/serialize.h"
#include "editor/icons.h"
#include "editor/theme.h"
#include "platform/file_dialog.h"

#include <json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace rig;

namespace {
constexpr const char* kAppVersion = "0.7.0";
constexpr size_t kMaxRecent = 10;

fs::path configDir() {
    fs::path dir;
#ifdef _WIN32
    wchar_t appData[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (n > 0 && n < MAX_PATH) dir = fs::path(appData) / "RigEditor";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) dir = fs::u8path(xdg) / "RigEditor";
    else if (const char* home = std::getenv("HOME")) dir = fs::u8path(home) / ".config" / "RigEditor";
#endif
    if (dir.empty()) dir = fs::current_path() / ".rigeditor";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

fs::path exeDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return fs::path(buf).parent_path();
#endif
    return fs::current_path();
}

// --- small line glyphs for the splash rows ---
void glyphDocument(ImDrawList* dl, ImVec2 c, float s, ImU32 col, bool plus) {
    float w = s * 0.62f, h = s * 0.8f, f = s * 0.22f;
    ImVec2 tl = c - ImVec2(w, h) * 0.5f;
    ImVec2 pts[] = {tl, tl + ImVec2(w - f, 0), tl + ImVec2(w, f), tl + ImVec2(w, h), tl + ImVec2(0, h)};
    dl->AddPolyline(pts, 5, col, ImDrawFlags_Closed, 1.3f);
    dl->AddLine(tl + ImVec2(w - f, 0), tl + ImVec2(w - f, f), col, 1.2f);
    dl->AddLine(tl + ImVec2(w - f, f), tl + ImVec2(w, f), col, 1.2f);
    if (plus) {
        ImVec2 p = tl + ImVec2(w, h) - ImVec2(s * 0.05f, s * 0.05f);
        float r = s * 0.24f;
        dl->AddCircleFilled(p, r, IM_COL32(30, 30, 32, 255));
        dl->AddLine(p - ImVec2(r * 0.6f, 0), p + ImVec2(r * 0.6f, 0), col, 1.5f);
        dl->AddLine(p - ImVec2(0, r * 0.6f), p + ImVec2(0, r * 0.6f), col, 1.5f);
    }
}

void glyphFolder(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    float w = s * 0.8f, h = s * 0.6f;
    ImVec2 tl = c - ImVec2(w, h) * 0.5f;
    dl->AddRectFilled(tl, tl + ImVec2(w * 0.42f, h * 0.3f), col, s * 0.06f);
    dl->AddRectFilled(tl + ImVec2(0, h * 0.18f), tl + ImVec2(w, h), col, s * 0.06f);
}

void glyphClock(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    float r = s * 0.38f;
    dl->PathArcTo(c, r, 3.14159f * 0.15f, 3.14159f * 1.85f, 20);
    dl->PathStroke(col, 0, 1.4f);
    ImVec2 end = c + ImVec2(std::cos(3.14159f * 0.15f), std::sin(3.14159f * 0.15f)) * r;
    dl->AddTriangleFilled(end + ImVec2(-s * 0.16f, -s * 0.02f), end + ImVec2(s * 0.08f, -s * 0.02f), end + ImVec2(-s * 0.02f, s * 0.14f), col);
    dl->AddLine(c, c + ImVec2(0, -r * 0.6f), col, 1.4f);
    dl->AddLine(c, c + ImVec2(r * 0.45f, r * 0.2f), col, 1.4f);
}

void glyphKeyboard(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    float w = s * 0.86f, h = s * 0.56f;
    ImVec2 tl = c - ImVec2(w, h) * 0.5f;
    dl->AddRect(tl, tl + ImVec2(w, h), col, s * 0.08f, 0, 1.3f);
    for (int r = 0; r < 2; ++r)
        for (int k = 0; k < 4; ++k)
            dl->AddRectFilled(tl + ImVec2(w * (0.12f + k * 0.21f), h * (0.2f + r * 0.28f)),
                              tl + ImVec2(w * (0.24f + k * 0.21f), h * (0.36f + r * 0.28f)), col);
    dl->AddLine(tl + ImVec2(w * 0.25f, h * 0.78f), tl + ImVec2(w * 0.75f, h * 0.78f), col, 1.3f);
}

enum class Glyph { NewDoc, Doc, Folder, Clock, Keyboard, Image, Psd, Sample };

// One clickable row: glyph + label, full column width.
bool splashRow(const char* id, Glyph g, const char* label, bool enabled = true, const char* tooltip = nullptr) {
    ImGui::PushID(id);
    const float h = ImGui::GetTextLineHeight() + 6;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::Selectable("##row", false, 0, ImVec2(ImGui::GetContentRegionAvail().x, h));
    ImGui::EndDisabled();
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", tooltip);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 col = enabled ? IM_COL32(215, 215, 220, 255) : IM_COL32(110, 110, 115, 255);
    ImVec2 c = p + ImVec2(h * 0.5f, h * 0.5f);
    float s = h * 0.82f;
    switch (g) {
    case Glyph::NewDoc: glyphDocument(dl, c, s, col, true); break;
    case Glyph::Doc: glyphDocument(dl, c, s, col, false); break;
    case Glyph::Folder: glyphFolder(dl, c, s, col); break;
    case Glyph::Clock: glyphClock(dl, c, s, col); break;
    case Glyph::Keyboard: glyphKeyboard(dl, c, s, col); break;
    case Glyph::Image: drawIcon(dl, Icon::Image, c, s, col); break;
    case Glyph::Psd: drawIcon(dl, Icon::DrawOrder, c, s, col); break;
    case Glyph::Sample: drawIcon(dl, Icon::Animation, c, s, col); break;
    }
    dl->AddText(p + ImVec2(h + 6, (h - ImGui::GetTextLineHeight()) * 0.5f), col, label);
    ImGui::PopID();
    return clicked;
}

// A Spine-style bone (kite shape) from `a` to `b`.
void drawBannerBone(ImDrawList* dl, ImVec2 a, ImVec2 b, float width, ImU32 fill, ImU32 outline) {
    ImVec2 d = b - a;
    float len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len < 1) return;
    ImVec2 n(-d.y / len * width, d.x / len * width);
    ImVec2 m = a + d * 0.18f;
    ImVec2 pts[] = {a, m + n, b, m - n};
    dl->AddConvexPolyFilled(pts, 4, fill);
    dl->AddPolyline(pts, 4, outline, ImDrawFlags_Closed, 1.2f);
}

ImU32 lerpColor(ImU32 a, ImU32 b, float t) {
    ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

// Animated banner: a dusk gradient with swaying bone chains, like a rig in motion.
void drawBanner(ImDrawList* dl, ImVec2 p0, ImVec2 size, float t) {
    ImVec2 p1 = p0 + size;
    dl->AddRectFilledMultiColor(p0, p1, IM_COL32(16, 44, 70, 255), IM_COL32(52, 30, 78, 255), IM_COL32(26, 18, 40, 255),
                                IM_COL32(10, 30, 40, 255));
    dl->PushClipRect(p0, p1, true);
    // Viewport-like grid and soft light.
    const float cell = size.y / 8;
    for (float x = p0.x + std::fmod(t * 6, cell); x < p1.x; x += cell) dl->AddLine({x, p0.y}, {x, p1.y}, IM_COL32(255, 255, 255, 10));
    for (float y = p0.y; y < p1.y; y += cell) dl->AddLine({p0.x, y}, {p1.x, y}, IM_COL32(255, 255, 255, 10));
    for (int i = 0; i < 6; ++i)
        dl->AddCircleFilled(p0 + ImVec2(size.x * 0.68f, size.y * 0.2f), size.y * (0.12f + i * 0.08f), IM_COL32(120, 170, 255, 9));
    // Bubbles rising.
    for (int i = 0; i < 18; ++i) {
        float seed = i * 12.9898f;
        float fx = std::fmod(std::abs(std::sin(seed) * 43758.5453f), 1.0f);
        float speed = 0.04f + 0.05f * std::fmod(fx * 7.0f, 1.0f);
        float fy = 1.0f - std::fmod(t * speed + fx * 3.1f, 1.0f);
        ImVec2 c = p0 + ImVec2(size.x * (0.3f + 0.7f * fx) + std::sin(t + i) * 4, size.y * fy);
        dl->AddCircle(c, 1.5f + 2.5f * std::fmod(fx * 13.0f, 1.0f), IM_COL32(170, 220, 255, 60), 0, 1.0f);
    }
    // Swaying chains (back ones dimmer), each bone a little shorter than its parent.
    struct Chain {
        float x, scale, phase;
        ImU32 base, tip;
        int alpha;
    } chains[] = {
        {0.95f, 0.5f, 2.4f, IM_COL32(60, 90, 170, 255), IM_COL32(80, 170, 210, 255), 80},
        {0.40f, 0.48f, 3.1f, IM_COL32(60, 100, 160, 255), IM_COL32(90, 190, 200, 255), 80},
        {0.86f, 0.72f, 1.7f, IM_COL32(70, 120, 200, 255), IM_COL32(90, 210, 220, 255), 140},
        {0.53f, 0.78f, 0.9f, IM_COL32(110, 80, 200, 255), IM_COL32(220, 120, 200, 255), 150},
        {0.70f, 1.0f, 0.0f, theme::kAccent, theme::kSelect, 255},
    };
    for (const Chain& ch : chains) {
        ImVec2 joint = p0 + ImVec2(size.x * ch.x, size.y * 1.02f);
        float angle = -90.0f, len = size.y * 0.24f * ch.scale;
        const int bones = 7;
        for (int i = 0; i < bones; ++i) {
            angle += std::sin(t * 1.5f - i * 0.55f + ch.phase) * (6.0f + i * 2.2f) + (i == 0 ? -8.0f : 4.0f);
            float r = angle * 3.14159265f / 180.0f;
            ImVec2 tip = joint + ImVec2(std::cos(r), std::sin(r)) * len;
            ImU32 fill = lerpColor(ch.base, ch.tip, (float)i / (bones - 1));
            fill = (fill & 0x00FFFFFF) | (ImU32(ch.alpha) << 24);
            drawBannerBone(dl, joint, tip, len * 0.2f, fill, IM_COL32(10, 12, 20, ch.alpha));
            dl->AddCircleFilled(joint, len * 0.08f, IM_COL32(235, 240, 255, ch.alpha));
            joint = tip;
            len *= 0.86f;
        }
        dl->AddCircleFilled(joint, len * 0.12f, IM_COL32(255, 255, 255, ch.alpha));
    }
    dl->PopClipRect();
}
} // namespace

// ---------------------------------------------------------------- settings / session

void Editor::loadSettings() {
    std::ifstream f(configDir() / "settings.json");
    if (!f) return;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (!j.is_object()) return;
    settings_.showSplash = j.value("showSplash", true);
    if (j.contains("recent") && j["recent"].is_array())
        for (auto& r : j["recent"])
            if (r.is_string()) settings_.recent.push_back(r.get<std::string>());
}

void Editor::saveSettings() {
    nlohmann::json j;
    j["showSplash"] = settings_.showSplash;
    j["recent"] = settings_.recent;
    std::ofstream f(configDir() / "settings.json");
    f << j.dump(2);
}

void Editor::addRecent(const std::string& path) {
    std::error_code ec;
    std::string abs = fs::absolute(fs::u8path(path), ec).lexically_normal().u8string();
    if (ec) abs = path;
    if (fs::u8path(abs) == fs::u8path(sessionPath())) return;
    auto& r = settings_.recent;
    r.erase(std::remove(r.begin(), r.end(), abs), r.end());
    r.insert(r.begin(), abs);
    if (r.size() > kMaxRecent) r.resize(kMaxRecent);
    saveSettings();
}

std::string Editor::sessionPath() const { return (configDir() / "last_session.rig.json").u8string(); }

std::string Editor::findSample(const std::string& name) const {
    const fs::path exe = exeDir();
    const fs::path candidates[] = {fs::current_path() / "samples", exe / "samples", exe.parent_path() / "samples",
                                   exe.parent_path().parent_path() / "samples"};
    for (const fs::path& dir : candidates) {
        std::error_code ec;
        if (fs::exists(dir / fs::u8path(name), ec)) return (dir / fs::u8path(name)).lexically_normal().u8string();
    }
    return {};
}

void Editor::saveSession() {
    if (sk_.bones.size() <= 1 && sk_.slots.empty() && sk_.animations.empty()) return;  // nothing worth restoring
    std::string err;
    saveFile(sessionPath(), sk_, &err);  // image paths are absolute in memory
}

// ---------------------------------------------------------------- splash

void Editor::drawSplash() {
    if (splashRequest_) {
        ImGui::OpenPopup("##splash");
        splashRequest_ = false;
    }
    const float fs = ImGui::GetFontSize();
    const float width = std::round(fs * 38);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, fs * 0.4f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(30, 30, 32, 250));
    bool open = ImGui::BeginPopup("##splash", ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (!open) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();

    // Banner with the name and version.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 bannerSize(width, std::round(width * 0.46f));
    dl->PushClipRect(p0, p0 + bannerSize, true);
    drawBanner(dl, p0, bannerSize, (float)ImGui::GetTime());
    const float pad = fs * 1.4f;
    const float titleSize = fs * 2.6f;
    ImVec2 logo = p0 + ImVec2(pad + titleSize * 0.5f, pad + titleSize * 0.55f);
    dl->AddCircleFilled(logo, titleSize * 0.55f, IM_COL32(255, 255, 255, 235));
    drawIcon(dl, Icon::Bone, logo, titleSize * 0.8f, theme::kAccent);
    dl->AddText(theme::largeFont(), titleSize, p0 + ImVec2(pad + titleSize * 1.25f, pad), IM_COL32(255, 255, 255, 255), "RigEditor");
    dl->AddText(p0 + ImVec2(pad + titleSize * 1.3f, pad + titleSize * 1.12f), IM_COL32(220, 225, 240, 200), "2D 骨骼动画编辑器");
    char version[32];
    std::snprintf(version, sizeof(version), "v%s", kAppVersion);
    ImVec2 vs = ImGui::CalcTextSize(version);
    dl->AddText(p0 + ImVec2(width - vs.x - fs, fs * 0.8f), IM_COL32(255, 255, 255, 220), version);
    dl->AddText(p0 + ImVec2(fs, bannerSize.y - fs * 1.7f), IM_COL32(255, 255, 255, 150), "C++17 · Dear ImGui · OpenGL");
    dl->PopClipRect();
    ImGui::Dummy(bannerSize);

    ImGui::Dummy(ImVec2(0, fs * 0.5f));
    ImGui::Indent(pad);
    const ImVec2 tableSize(width - 2 * pad, 0);
    bool close = false;
    if (ImGui::BeginTable("##splashTop", 2, ImGuiTableFlags_SizingStretchSame, tableSize)) {
        ImGui::TableNextColumn();
        ImGui::TextDisabled("新建");
        if (splashRow("new", Glyph::NewDoc, "空白工程")) {
            newProject();
            close = true;
        }
        if (splashRow("img", Glyph::Image, "从图片新建...")) {
            newProject();
            importImageDialog();
            close = true;
        }
        if (splashRow("psd", Glyph::Psd, "从 PSD 新建...")) {
            newProject();
            io_.open = IoState::Psd;
            close = true;
        }
        std::string sample = findSample("arm.png");
        if (splashRow("demo", Glyph::Sample, "示例：挥手的手臂", !sample.empty(), sample.empty() ? "找不到 samples/arm.png" : sample.c_str())) {
            buildDemo(sample, 0.0f, "");
            close = true;
        }

        ImGui::TableNextColumn();
        ImGui::TextDisabled("最近打开的文件");
        int shown = 0;
        std::string openPath;
        for (const std::string& path : settings_.recent) {
            std::error_code ec;
            if (shown >= 6 || !fs::exists(fs::u8path(path), ec)) continue;
            std::string name = fs::u8path(path).filename().u8string();
            if (splashRow(path.c_str(), Glyph::Doc, name.c_str(), true, path.c_str())) openPath = path;
            ++shown;
        }
        if (!shown) ImGui::TextDisabled("  （还没有）");
        if (!openPath.empty()) {
            openProject(openPath);
            close = true;
        }
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0, fs * 0.4f));
    if (ImGui::BeginTable("##splashBottom", 2, ImGuiTableFlags_SizingStretchSame, tableSize)) {
        ImGui::TableNextColumn();
        if (splashRow("open", Glyph::Folder, "打开...")) {
            close = true;
            openProjectDialog();
        }
        std::error_code ec;
        bool hasSession = fs::exists(fs::u8path(sessionPath()), ec);
        if (splashRow("restore", Glyph::Clock, "恢复上次的会话", hasSession, hasSession ? "打开上次退出时的工程状态" : "还没有上次会话的记录")) {
            if (openProject(sessionPath())) {
                projectPath_.clear();  // a recovered copy: Save asks where to keep it
                dirty_ = true;
                setStatus("已恢复上次的会话");
            }
            close = true;
        }
        ImGui::TableNextColumn();
        if (splashRow("keys", Glyph::Keyboard, "快捷键")) {
            showShortcuts_ = true;
            close = true;
        }
        if (ImGui::Checkbox("启动时显示此页", &settings_.showSplash)) saveSettings();
        ImGui::EndTable();
    }
    if (dirty_) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "当前工程有未保存的修改，新建或打开会丢弃它们。");
    ImGui::Unindent(pad);
    ImGui::Dummy(ImVec2(0, fs * 0.6f));
    if (close) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void Editor::drawShortcuts() {
    if (!showShortcuts_) return;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("快捷键###Shortcuts", &showShortcuts_, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking |
                                                                ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    static const char* rows[][2] = {
        {"Tab", "切换 Setup / Animate 模式"},
        {"C / V / X / Z", "旋转 / 移动 / 缩放 / 斜切"},
        {"B / N / G / M", "姿势 / 建骨骼 / 权重 / 网格（Animate 下为变形）"},
        {"右键拖动 / 滚轮", "平移 / 缩放视图；右键单击切回上一个工具"},
        {"F / Ctrl+F", "适配骨架 / 100% 缩放"},
        {"Esc / Del", "取消选择 / 删除所选"},
        {"空格", "播放 / 暂停（Animate）"},
        {"K", "为所有改动打关键帧"},
        {"← / →", "上一帧 / 下一帧"},
        {"Ctrl+Z / Ctrl+Y", "撤销 / 重做"},
        {"Ctrl+N / O / S", "新建 / 打开 / 保存（Ctrl+Shift+S 另存为）"},
        {"Ctrl+I", "导入图片（也可把 PNG / PSD 拖进窗口）"},
    };
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        for (auto& r : rows) {
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1.0f), "%s", r[0]);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r[1]);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
