#include "editor/icons.h"
#include "editor/theme.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

static ImVec2 P(ImVec2 c, float s, float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); }

static void arrowHead(ImDrawList* dl, ImVec2 tip, ImVec2 dir, float len, ImU32 col) {
    ImVec2 n(-dir.y, dir.x);
    dl->AddTriangleFilled(tip, ImVec2(tip.x - dir.x * len + n.x * len * 0.6f, tip.y - dir.y * len + n.y * len * 0.6f),
                          ImVec2(tip.x - dir.x * len - n.x * len * 0.6f, tip.y - dir.y * len - n.y * len * 0.6f), col);
}

ImU32 iconColor(Icon icon) {
    switch (icon) {
    case Icon::Rotate: return theme::kRotate;
    case Icon::Translate: return theme::kTranslate;
    case Icon::Scale: return theme::kScale;
    case Icon::Shear: return theme::kShear;
    case Icon::Event: return theme::kEvent;
    case Icon::Color: return theme::kColor;
    case Icon::Graph: return IM_COL32(110, 220, 120, 255);
    case Icon::Slot: return IM_COL32(120, 200, 255, 255);
    case Icon::Region: return IM_COL32(120, 200, 140, 255);
    case Icon::Mesh: return IM_COL32(110, 200, 230, 255);
    case Icon::Animation: return IM_COL32(240, 240, 240, 255);
    case Icon::Image: return IM_COL32(230, 200, 110, 255);
    case Icon::DrawOrder: return IM_COL32(110, 150, 240, 255);
    case Icon::Pose: return IM_COL32(110, 220, 110, 255);
    case Icon::Weights: return IM_COL32(240, 110, 90, 255);
    case Icon::Create: return IM_COL32(90, 220, 220, 255);
    case Icon::MeshTool: return IM_COL32(110, 200, 230, 255);
    case Icon::Local: case Icon::Parent: case Icon::World: return IM_COL32(110, 220, 210, 255);
    case Icon::AutoKey: return theme::kKeyKeyed;
    default: return IM_COL32(225, 225, 225, 255);
    }
}

void drawIcon(ImDrawList* dl, Icon icon, ImVec2 c, float size, ImU32 col) {
    const float s = size * 0.5f;  // half extent: coordinates below are in [-1, 1]
    const float t = std::max(1.0f, size * 0.09f);
    switch (icon) {
    case Icon::Skeleton:
        dl->AddCircle(P(c, s, 0, -0.65f), s * 0.25f, col, 0, t);
        dl->AddLine(P(c, s, 0, -0.4f), P(c, s, 0, 0.3f), col, t);
        dl->AddLine(P(c, s, -0.6f, -0.15f), P(c, s, 0.6f, -0.15f), col, t);
        dl->AddLine(P(c, s, 0, 0.3f), P(c, s, -0.45f, 0.9f), col, t);
        dl->AddLine(P(c, s, 0, 0.3f), P(c, s, 0.45f, 0.9f), col, t);
        break;
    case Icon::Bone: {
        ImVec2 pts[4] = {P(c, s, -0.75f, 0.75f), P(c, s, -0.25f, 0.05f), P(c, s, 0.8f, -0.8f), P(c, s, -0.05f, 0.25f)};
        dl->AddConvexPolyFilled(pts, 4, col);
        dl->AddCircleFilled(P(c, s, -0.75f, 0.75f), s * 0.2f, col);
        break;
    }
    case Icon::Slot:
        dl->AddRect(P(c, s, -0.75f, -0.75f), P(c, s, 0.75f, 0.75f), col, s * 0.2f, 0, t);
        dl->AddRectFilled(P(c, s, -0.35f, -0.35f), P(c, s, 0.35f, 0.35f), col, s * 0.1f);
        break;
    case Icon::Region:
    case Icon::Image:
        dl->AddRect(P(c, s, -0.85f, -0.7f), P(c, s, 0.85f, 0.7f), col, s * 0.1f, 0, t);
        dl->AddTriangleFilled(P(c, s, -0.7f, 0.55f), P(c, s, -0.15f, -0.15f), P(c, s, 0.35f, 0.55f), col);
        dl->AddCircleFilled(P(c, s, 0.4f, -0.3f), s * 0.17f, col);
        break;
    case Icon::Mesh:
    case Icon::MeshTool: {
        ImVec2 a = P(c, s, -0.8f, 0.7f), b = P(c, s, 0.8f, 0.7f), d = P(c, s, 0, -0.8f), m = P(c, s, 0, 0.2f);
        dl->AddTriangle(a, b, d, col, t);
        dl->AddLine(a, m, col, t);
        dl->AddLine(b, m, col, t);
        dl->AddLine(d, m, col, t);
        for (ImVec2 v : {a, b, d, m}) dl->AddCircleFilled(v, s * 0.16f, col);
        break;
    }
    case Icon::Animation:
        dl->AddCircle(P(c, s, 0.25f, -0.6f), s * 0.22f, col, 0, t);
        dl->AddLine(P(c, s, 0.1f, -0.35f), P(c, s, -0.2f, 0.25f), col, t);
        dl->AddLine(P(c, s, -0.2f, 0.25f), P(c, s, -0.7f, 0.85f), col, t);
        dl->AddLine(P(c, s, -0.2f, 0.25f), P(c, s, 0.35f, 0.85f), col, t);
        dl->AddLine(P(c, s, -0.75f, -0.15f), P(c, s, 0.05f, -0.1f), col, t);
        dl->AddLine(P(c, s, 0.05f, -0.1f), P(c, s, 0.7f, 0.1f), col, t);
        break;
    case Icon::DrawOrder:
        for (int i = 0; i < 3; ++i)
            dl->AddRectFilled(P(c, s, -0.8f + i * 0.25f, -0.75f + i * 0.5f), P(c, s, 0.3f + i * 0.25f, -0.4f + i * 0.5f), col, s * 0.08f);
        break;
    case Icon::Pose:
        dl->AddCircle(c, s * 0.55f, col, 0, t);
        dl->AddLine(P(c, s, -0.95f, 0), P(c, s, -0.35f, 0), col, t);
        dl->AddLine(P(c, s, 0.35f, 0), P(c, s, 0.95f, 0), col, t);
        dl->AddLine(P(c, s, 0, -0.95f), P(c, s, 0, -0.35f), col, t);
        dl->AddLine(P(c, s, 0, 0.35f), P(c, s, 0, 0.95f), col, t);
        break;
    case Icon::Weights:
        dl->AddRectFilledMultiColor(P(c, s, -0.8f, -0.8f), P(c, s, 0.8f, 0.8f), IM_COL32(240, 70, 60, 255),
                                    IM_COL32(240, 200, 60, 255), IM_COL32(60, 200, 90, 255), IM_COL32(60, 110, 240, 255));
        break;
    case Icon::Create:
        drawIcon(dl, Icon::Bone, P(c, s, -0.15f, 0.15f), size * 0.8f, col);
        dl->AddLine(P(c, s, 0.35f, 0.65f), P(c, s, 0.95f, 0.65f), col, t);
        dl->AddLine(P(c, s, 0.65f, 0.35f), P(c, s, 0.65f, 0.95f), col, t);
        break;
    case Icon::Rotate: {
        const int n = 16;
        ImVec2 pts[n + 1];
        for (int i = 0; i <= n; ++i) {
            float a = 0.6f + 4.6f * i / n;
            pts[i] = P(c, s, std::cos(a) * 0.7f, std::sin(a) * 0.7f);
        }
        dl->AddPolyline(pts, n + 1, col, 0, t * 1.2f);
        float a = 5.2f;
        arrowHead(dl, P(c, s, std::cos(a) * 0.7f, std::sin(a) * 0.7f), ImVec2(-std::sin(a), std::cos(a)), s * 0.45f, col);
        break;
    }
    case Icon::Translate:
        dl->AddLine(P(c, s, -0.7f, 0), P(c, s, 0.7f, 0), col, t);
        dl->AddLine(P(c, s, 0, -0.7f), P(c, s, 0, 0.7f), col, t);
        arrowHead(dl, P(c, s, 0.95f, 0), ImVec2(1, 0), s * 0.35f, col);
        arrowHead(dl, P(c, s, -0.95f, 0), ImVec2(-1, 0), s * 0.35f, col);
        arrowHead(dl, P(c, s, 0, 0.95f), ImVec2(0, 1), s * 0.35f, col);
        arrowHead(dl, P(c, s, 0, -0.95f), ImVec2(0, -1), s * 0.35f, col);
        break;
    case Icon::Scale:
        dl->AddRect(P(c, s, -0.8f, -0.8f), P(c, s, 0.8f, 0.8f), col, 0, 0, t);
        dl->AddRectFilled(P(c, s, -0.8f, -0.1f), P(c, s, 0.1f, 0.8f), col);
        break;
    case Icon::Shear: {
        ImVec2 pts[4] = {P(c, s, -0.75f, 0.75f), P(c, s, -0.25f, -0.75f), P(c, s, 0.75f, -0.75f), P(c, s, 0.25f, 0.75f)};
        dl->AddPolyline(pts, 4, col, ImDrawFlags_Closed, t * 1.2f);
        break;
    }
    case Icon::Event: {
        ImVec2 pts[5] = {P(c, s, -0.8f, -0.75f), P(c, s, 0.8f, -0.75f), P(c, s, 0.8f, 0.35f), P(c, s, -0.2f, 0.35f), P(c, s, -0.8f, 0.85f)};
        dl->AddConvexPolyFilled(pts, 5, col);
        break;
    }
    case Icon::Color:
        dl->AddCircleFilled(P(c, s, -0.3f, -0.25f), s * 0.45f, IM_COL32(240, 80, 80, 230));
        dl->AddCircleFilled(P(c, s, 0.3f, -0.25f), s * 0.45f, IM_COL32(80, 220, 90, 230));
        dl->AddCircleFilled(P(c, s, 0.0f, 0.3f), s * 0.45f, IM_COL32(80, 130, 240, 230));
        break;
    case Icon::Graph: {
        const int n = 12;
        ImVec2 pts[n + 1];
        for (int i = 0; i <= n; ++i) {
            float x = -0.85f + 1.7f * i / n;
            pts[i] = P(c, s, x, 0.7f - 1.4f * (0.5f - 0.5f * std::cos(3.14159f * i / n)));
        }
        dl->AddPolyline(pts, n + 1, col, 0, t * 1.3f);
        break;
    }
    case Icon::Local:
    case Icon::Parent:
    case Icon::World: {
        float rot = icon == Icon::Local ? -0.5f : (icon == Icon::Parent ? -0.25f : 0.0f);
        ImVec2 o = P(c, s, -0.6f, 0.6f);
        ImVec2 x(std::cos(rot), std::sin(rot)), y(std::sin(rot), -std::cos(rot));
        dl->AddLine(o, ImVec2(o.x + x.x * s * 1.4f, o.y + x.y * s * 1.4f), theme::kScale, t * 1.2f);
        dl->AddLine(o, ImVec2(o.x + y.x * s * 1.4f, o.y + y.y * s * 1.4f), theme::kRotate, t * 1.2f);
        break;
    }
    case Icon::AutoKey:
    case Icon::Key: {
        ImVec2 head = P(c, s, -0.4f, -0.35f);
        dl->AddCircle(head, s * 0.38f, col, 0, t * 1.3f);
        dl->AddLine(P(c, s, -0.15f, -0.1f), P(c, s, 0.8f, 0.85f), col, t * 1.3f);
        dl->AddLine(P(c, s, 0.45f, 0.5f), P(c, s, 0.2f, 0.75f), col, t * 1.3f);
        dl->AddLine(P(c, s, 0.65f, 0.7f), P(c, s, 0.42f, 0.93f), col, t * 1.3f);
        break;
    }
    case Icon::Play:
        dl->AddTriangleFilled(P(c, s, -0.5f, -0.75f), P(c, s, 0.75f, 0), P(c, s, -0.5f, 0.75f), col);
        break;
    case Icon::PlayBack:
        dl->AddTriangleFilled(P(c, s, 0.5f, -0.75f), P(c, s, 0.5f, 0.75f), P(c, s, -0.75f, 0), col);
        break;
    case Icon::Pause:
        dl->AddRectFilled(P(c, s, -0.6f, -0.7f), P(c, s, -0.15f, 0.7f), col);
        dl->AddRectFilled(P(c, s, 0.15f, -0.7f), P(c, s, 0.6f, 0.7f), col);
        break;
    case Icon::First:
    case Icon::PrevKey:
        dl->AddRectFilled(P(c, s, -0.75f, -0.7f), P(c, s, -0.5f, 0.7f), col);
        dl->AddTriangleFilled(P(c, s, 0.7f, -0.7f), P(c, s, 0.7f, 0.7f), P(c, s, -0.4f, 0), col);
        if (icon == Icon::First) dl->AddTriangleFilled(P(c, s, 1.0f, -0.7f), P(c, s, 1.0f, 0.7f), P(c, s, 0.2f, 0), col);
        break;
    case Icon::Last:
    case Icon::NextKey:
        dl->AddRectFilled(P(c, s, 0.5f, -0.7f), P(c, s, 0.75f, 0.7f), col);
        dl->AddTriangleFilled(P(c, s, -0.7f, -0.7f), P(c, s, 0.4f, 0), P(c, s, -0.7f, 0.7f), col);
        if (icon == Icon::Last) dl->AddTriangleFilled(P(c, s, -1.0f, -0.7f), P(c, s, -0.2f, 0), P(c, s, -1.0f, 0.7f), col);
        break;
    case Icon::Loop: {
        const int n = 18;
        ImVec2 pts[n + 1];
        for (int i = 0; i <= n; ++i) {
            float a = 0.3f + 5.4f * i / n;
            pts[i] = P(c, s, std::cos(a) * 0.65f, std::sin(a) * 0.5f);
        }
        dl->AddPolyline(pts, n + 1, col, 0, t * 1.2f);
        arrowHead(dl, pts[n], ImVec2(0.3f, 1.0f), s * 0.4f, col);
        break;
    }
    case Icon::Lock:
        dl->AddRectFilled(P(c, s, -0.6f, -0.05f), P(c, s, 0.6f, 0.8f), col, s * 0.1f);
        dl->PathArcTo(P(c, s, 0, -0.1f), s * 0.38f, 3.14159f, 6.28318f, 10);
        dl->PathStroke(col, 0, t * 1.3f);
        break;
    case Icon::Zoom100:
    case Icon::Search:
        dl->AddCircle(P(c, s, -0.2f, -0.2f), s * 0.5f, col, 0, t * 1.2f);
        dl->AddLine(P(c, s, 0.15f, 0.15f), P(c, s, 0.8f, 0.8f), col, t * 1.6f);
        break;
    case Icon::ZoomFit:
        dl->AddRect(P(c, s, -0.75f, -0.75f), P(c, s, 0.75f, 0.75f), col, 0, 0, t);
        dl->AddRectFilled(P(c, s, -0.35f, -0.35f), P(c, s, 0.35f, 0.35f), col);
        break;
    }
}

void iconInline(Icon icon, ImU32 col) {
    float h = ImGui::GetTextLineHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(h, h));
    drawIcon(ImGui::GetWindowDrawList(), icon, ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), h * 0.85f, col ? col : iconColor(icon));
}

bool iconButton(const char* id, Icon icon, const char* label, bool active, float width, bool enabled) {
    ImGuiStyle& st = ImGui::GetStyle();
    float h = ImGui::GetFrameHeight();
    float labelW = label && *label ? ImGui::CalcTextSize(label, nullptr, true).x : 0;
    float w = width > 0 ? width : h + labelW + (labelW > 0 ? st.FramePadding.x * 2 : 0);
    ImGui::PushID(id);
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(theme::kAccent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(theme::kAccent));
    }
    ImGui::BeginDisabled(!enabled);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::Button("##b", ImVec2(w, h));
    ImU32 col = enabled ? iconColor(icon) : IM_COL32(120, 120, 120, 255);
    if (active) col = IM_COL32(255, 255, 255, 255);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    drawIcon(dl, icon, ImVec2(p.x + h * 0.55f, p.y + h * 0.5f), h * 0.62f, col);
    if (labelW > 0)
        dl->AddText(ImVec2(p.x + h + st.FramePadding.x * 0.2f, p.y + st.FramePadding.y),
                    ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled), label, ImGui::FindRenderedTextEnd(label));
    ImGui::EndDisabled();
    if (active) ImGui::PopStyleColor(2);
    ImGui::PopID();
    return clicked;
}

bool iconOnlyButton(const char* id, Icon icon, bool active, const char* tooltip, ImU32 col) {
    float h = ImGui::GetFrameHeight();
    ImGui::PushID(id);
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(theme::kAccent));
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::Button("##b", ImVec2(h * 1.15f, h));
    if (active) ImGui::PopStyleColor();
    drawIcon(ImGui::GetWindowDrawList(), icon, ImVec2(p.x + h * 0.575f, p.y + h * 0.5f), h * 0.55f,
             col ? col : (active ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 220, 255)));
    if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return clicked;
}

bool keyButton(const char* id, ImU32 stateColor, const char* tooltip) {
    float h = ImGui::GetFrameHeight();
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##k", ImVec2(h, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered()) dl->AddRectFilled(p, ImVec2(p.x + h, p.y + h), IM_COL32(255, 255, 255, 25), 3);
    drawIcon(dl, Icon::Key, ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), h * 0.62f, stateColor);
    if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return clicked;
}

bool dotToggle(const char* id, bool on, const char* tooltip) {
    float h = ImGui::GetTextLineHeight();
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##d", ImVec2(h, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(p.x + h * 0.5f, p.y + h * 0.5f);
    bool hovered = ImGui::IsItemHovered();
    if (on) dl->AddCircleFilled(c, h * 0.17f, hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(215, 215, 215, 255));
    else dl->AddCircle(c, h * 0.15f, hovered ? IM_COL32(200, 200, 200, 255) : IM_COL32(110, 110, 110, 255));
    if (tooltip && hovered) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return clicked;
}
