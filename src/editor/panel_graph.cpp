// Graph view: value-over-time curves of the selected bone, with Bezier handle editing.
#include "editor/editor.h"

#include "editor/channels.h"
#include "editor/icons.h"
#include "editor/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace rig;

namespace {
// One drawable curve: a bone channel and, for 2D channels, the component.
struct CurveDef {
    Channel channel;
    int comp;  // 0 = x, 1 = y (ignored for rotate)
    const char* name;
};
constexpr CurveDef kCurves[] = {
    {Channel::Rotate, 0, "旋转"},  {Channel::Translate, 0, "移动 X"}, {Channel::Translate, 1, "移动 Y"},
    {Channel::Scale, 0, "缩放 X"}, {Channel::Scale, 1, "缩放 Y"},     {Channel::Shear, 0, "斜切 X"},
    {Channel::Shear, 1, "斜切 Y"},
};
constexpr int kCurveCount = (int)(sizeof(kCurves) / sizeof(kCurves[0]));

// Uniform access to the keys of one curve.
struct Access {
    std::vector<FloatKey>* f = nullptr;
    std::vector<Vec2Key>* v = nullptr;
    int comp = 0;

    size_t size() const { return f ? f->size() : v ? v->size() : 0; }
    float time(size_t i) const { return f ? (*f)[i].time : (*v)[i].time; }
    float value(size_t i) const { return f ? (*f)[i].value : (comp ? (*v)[i].value.y : (*v)[i].value.x); }
    void setValue(size_t i, float x) {
        if (f) (*f)[i].value = x;
        else if (comp) (*v)[i].value.y = x;
        else (*v)[i].value.x = x;
    }
    CurveShape& curve(size_t i) { return f ? (*f)[i].curve : (*v)[i].curve; }
    float sampleAt(float t) const {
        if (f) return sample(*f, t, 0.0f);
        Vec2 s = sample(*v, t, Vec2{});
        return comp ? s.y : s.x;
    }
    int indexAt(float t) const {
        for (size_t i = 0; i < size(); ++i)
            if (std::fabs(time(i) - t) < kKeyEpsilon) return (int)i;
        return -1;
    }
};

Access access(BoneTimeline& tl, const CurveDef& c) {
    Access a;
    a.comp = c.comp;
    switch (c.channel) {
    case Channel::Rotate: a.f = &tl.rotate; break;
    case Channel::Translate: a.v = &tl.translate; break;
    case Channel::Scale: a.v = &tl.scale; break;
    default: a.v = &tl.shear; break;
    }
    return a;
}

ImU32 curveColor(int id) {
    const CurveDef& c = kCurves[id];
    ImU32 base = channelColor(c.channel);
    if (c.comp == 1) {  // Y component: lighter shade of the channel colour
        ImVec4 v = ImGui::ColorConvertU32ToFloat4(base);
        v.x = v.x * 0.55f + 0.45f;
        v.y = v.y * 0.55f + 0.45f;
        v.z = v.z * 0.55f + 0.45f;
        return ImGui::ColorConvertFloat4ToU32(v);
    }
    return base;
}
} // namespace

void Editor::drawGraph() {
    bool visible = ImGui::Begin("Graph###Graph");
    if (focusGraph_ > 0) {
        ImGui::SetWindowFocus();
        --focusGraph_;
    }
    if (!visible) {
        ImGui::End();
        return;
    }
    Animation* an = currentAnimation();
    int bone = sel_.bone >= 0 && sel_.kind != Selection::None ? sel_.bone : -1;
    auto boneIt = an && bone >= 0 ? an->bones.find(bone) : std::map<int, BoneTimeline>::iterator{};
    if (!an || bone < 0 || boneIt == an->bones.end()) {
        ImGui::TextDisabled(!an ? "没有动画。" : bone < 0 ? "选择一根骨骼，查看它的关键帧曲线。" : "所选骨骼在当前动画里没有关键帧。");
        ImGui::End();
        return;
    }
    BoneTimeline& tl = boneIt->second;
    ImGuiIO& io = ImGui::GetIO();
    const float fh = ImGui::GetFrameHeight();

    if (graphCurve_ >= 0 && (graphKey_ < 0 || graphKey_ >= (int)access(tl, kCurves[graphCurve_]).size())) graphCurve_ = graphKey_ = -1;

    // ---- toolbar: curve presets for the selected key ----
    {
        bool hasSel = graphCurve_ >= 0;
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", sk_.bones[bone].name.c_str());
        ImGui::SameLine(0, 12);
        ImGui::BeginDisabled(!hasSel);
        auto preset = [&](const char* label, const CurveShape& c) {
            if (ImGui::Button(label) && hasSel) {
                checkpoint();
                access(tl, kCurves[graphCurve_]).curve(graphKey_) = c;
            }
            ImGui::SameLine(0, 3);
        };
        preset("线性", CurveShape::linear());
        preset("阶梯", CurveShape::stepped());
        preset("缓入缓出", CurveShape::bezier(0.42f, 0, 0.58f, 1));
        preset("缓入", CurveShape::bezier(0.42f, 0, 1, 1));
        preset("缓出", CurveShape::bezier(0, 0, 0.58f, 1));
        ImGui::EndDisabled();
        ImGui::SameLine(0, 12);
        if (hasSel) {
            Access a = access(tl, kCurves[graphCurve_]);
            ImGui::TextDisabled("%s  第 %d 帧  值 %.2f", kCurves[graphCurve_].name, (int)std::round(a.time(graphKey_) * fps_),
                                a.value(graphKey_));
            if (kCurves[graphCurve_].channel != Channel::Rotate) {
                ImGui::SameLine();
                ImGui::TextDisabled("(X/Y 共用同一条曲线形状)");
            }
        } else {
            ImGui::TextDisabled("点击曲线上的关键帧，拖动手柄调整缓动");
        }
    }

    // ---- curve list ----
    float listW = ImGui::GetFontSize() * 7;
    ImGui::BeginChild("##curves", ImVec2(listW, 0), ImGuiChildFlags_Borders);
    for (int id = 0; id < kCurveCount; ++id) {
        Access a = access(tl, kCurves[id]);
        if (a.size() == 0) continue;
        ImGui::PushID(id);
        bool shown = !(graphHidden_ & (1u << id));
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(p + ImVec2(2, h * 0.3f), p + ImVec2(h - 2, h * 0.7f), curveColor(id));
        ImGui::Dummy(ImVec2(h, h));
        ImGui::SameLine();
        if (ImGui::Checkbox(kCurves[id].name, &shown)) graphHidden_ ^= (1u << id);
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ---- graph area ----
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = std::max(size.x, 100.0f);
    size.y = std::max(size.y, 80.0f);
    ImGui::InvisibleButton("##graph", size, ImGuiButtonFlags_MouseButtonLeft);
    bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rulerH = fh;
    const float labelW = ImGui::GetFontSize() * 3.2f;
    ImVec2 plotLo(origin.x + labelW, origin.y + rulerH + 6), plotHi(origin.x + size.x - 10, origin.y + size.y - 8);
    float dur = std::max(an->duration, 1.0f / fps_);

    // Vertical range: fit every visible curve (sampled), its keys and the selected key's handles.
    float vmin = 1e9f, vmax = -1e9f;
    auto grow = [&](float v) {
        vmin = std::min(vmin, v);
        vmax = std::max(vmax, v);
    };
    for (int id = 0; id < kCurveCount; ++id) {
        if (graphHidden_ & (1u << id)) continue;
        Access a = access(tl, kCurves[id]);
        if (a.size() == 0) continue;
        for (int i = 0; i <= 64; ++i) grow(a.sampleAt(dur * i / 64));
        for (size_t k = 0; k < a.size(); ++k) grow(a.value(k));
    }
    if (vmin > vmax) {
        vmin = -1;
        vmax = 1;
    }
    if (vmax - vmin < 1e-3f) {
        vmin -= 1;
        vmax += 1;
    }
    float pad = (vmax - vmin) * 0.12f;
    vmin -= pad;
    vmax += pad;

    auto tx = [&](float t) { return plotLo.x + t / dur * (plotHi.x - plotLo.x); };
    auto vy = [&](float v) { return plotHi.y - (v - vmin) / (vmax - vmin) * (plotHi.y - plotLo.y); };
    auto xt = [&](float x) { return (x - plotLo.x) / (plotHi.x - plotLo.x) * dur; };
    auto yv = [&](float y) { return vmin + (plotHi.y - y) / (plotHi.y - plotLo.y) * (vmax - vmin); };

    dl->AddRectFilled(origin, origin + size, IM_COL32(46, 46, 49, 255));
    dl->AddRectFilled(ImVec2(origin.x, origin.y), ImVec2(origin.x + size.x, origin.y + rulerH), IM_COL32(40, 40, 43, 255));
    ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);

    // Grid: frames on X, "nice" steps on Y.
    int every = 1;
    float ppf = (plotHi.x - plotLo.x) / dur / fps_;
    while (ppf * every < 34) every *= 2;
    for (int f = 0; f <= (int)std::round(dur * fps_); f += every) {
        float x = tx(f / fps_);
        dl->AddLine(ImVec2(x, plotLo.y), ImVec2(x, plotHi.y), IM_COL32(255, 255, 255, 14));
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", f);
        dl->AddText(ImVec2(x + 3, origin.y + 2), dim, buf);
    }
    float range = vmax - vmin;
    float step = std::pow(10.0f, std::floor(std::log10(range / 4)));
    if (range / step > 10) step *= 2.5f;
    if (range / step > 10) step *= 2;
    for (float v = std::ceil(vmin / step) * step; v <= vmax; v += step) {
        float y = vy(v);
        dl->AddLine(ImVec2(plotLo.x, y), ImVec2(plotHi.x, y), std::fabs(v) < step * 0.01f ? IM_COL32(255, 255, 255, 50) : IM_COL32(255, 255, 255, 14));
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%.4g", v);
        dl->AddText(ImVec2(origin.x + 4, y - ImGui::GetTextLineHeight() * 0.5f), dim, buf);
    }

    // ---- hit testing ----
    ImVec2 mouse = io.MousePos;
    auto handlePos = [&](Access& a, int k, int which) -> ImVec2 {
        float t0 = a.time(k), t1 = a.time(k + 1), v0 = a.value(k), v1 = a.value(k + 1);
        const CurveShape& c = a.curve(k);
        float hx = which == 1 ? c.cx1 : c.cx2, hy = which == 1 ? c.cy1 : c.cy2;
        if (c.type != Curve::Bezier) hx = hy = which == 1 ? 1 / 3.0f : 2 / 3.0f;
        float dv = std::fabs(v1 - v0) > 1e-5f ? v1 - v0 : (vmax - vmin) * 0.25f;
        return ImVec2(tx(t0 + hx * (t1 - t0)), vy(v0 + hy * dv));
    };
    bool selHasNext = false;
    if (graphCurve_ >= 0) {
        Access a = access(tl, kCurves[graphCurve_]);
        selHasNext = graphKey_ + 1 < (int)a.size() && a.curve(graphKey_).type != Curve::Stepped;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        graphDrag_ = GraphDrag{};
        if (selHasNext) {
            Access a = access(tl, kCurves[graphCurve_]);
            for (int which = 1; which <= 2 && graphDrag_.kind == GraphDrag::None; ++which) {
                ImVec2 h = handlePos(a, graphKey_, which);
                if (std::hypot(mouse.x - h.x, mouse.y - h.y) < 7) graphDrag_.kind = which == 1 ? GraphDrag::Handle1 : GraphDrag::Handle2;
            }
        }
        if (graphDrag_.kind == GraphDrag::None) {
            float best = 8;
            for (int id = 0; id < kCurveCount; ++id) {
                if (graphHidden_ & (1u << id)) continue;
                Access a = access(tl, kCurves[id]);
                for (size_t k = 0; k < a.size(); ++k) {
                    float d = std::hypot(mouse.x - tx(a.time(k)), mouse.y - vy(a.value(k)));
                    if (d < best) {
                        best = d;
                        graphCurve_ = id;
                        graphKey_ = (int)k;
                        graphDrag_.kind = GraphDrag::Key;
                    }
                }
            }
            if (graphDrag_.kind == GraphDrag::Key) {
                setTime(access(tl, kCurves[graphCurve_]).time(graphKey_));
            } else {
                graphCurve_ = graphKey_ = -1;
                graphDrag_.kind = GraphDrag::Scrub;
            }
        }
    }
    if (graphDrag_.kind != GraphDrag::None && ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2)) {
        auto ensure = [&] {
            if (!graphDrag_.checkpointed) {
                checkpoint();
                graphDrag_.checkpointed = true;
            }
        };
        if (graphDrag_.kind == GraphDrag::Scrub) {
            float t = std::clamp(xt(mouse.x), 0.0f, an->duration);
            setTime(io.KeyShift ? t : snapTime(t));
        } else if (graphDrag_.kind == GraphDrag::Key && graphCurve_ >= 0) {
            ensure();
            Access a = access(tl, kCurves[graphCurve_]);
            a.setValue(graphKey_, yv(mouse.y));
            float from = a.time(graphKey_);
            float to = std::clamp(io.KeyShift ? xt(mouse.x) : snapTime(xt(mouse.x)), 0.0f, an->duration);
            if (std::fabs(to - from) > kKeyEpsilon && a.indexAt(to) < 0) {
                moveChannelKey(*an, bone, kCurves[graphCurve_].channel, from, to);
                graphKey_ = access(tl, kCurves[graphCurve_]).indexAt(to);
            }
            setTime(access(tl, kCurves[graphCurve_]).time(graphKey_));
        } else if (graphCurve_ >= 0 && selHasNext) {
            ensure();
            Access a = access(tl, kCurves[graphCurve_]);
            float t0 = a.time(graphKey_), t1 = a.time(graphKey_ + 1), v0 = a.value(graphKey_), v1 = a.value(graphKey_ + 1);
            float dv = std::fabs(v1 - v0) > 1e-5f ? v1 - v0 : (vmax - vmin) * 0.25f;
            CurveShape& c = a.curve(graphKey_);
            if (c.type != Curve::Bezier) c = CurveShape::bezier(1 / 3.0f, 1 / 3.0f, 2 / 3.0f, 2 / 3.0f);
            float hx = std::clamp((xt(mouse.x) - t0) / (t1 - t0), 0.0f, 1.0f);
            float hy = (yv(mouse.y) - v0) / dv;
            if (graphDrag_.kind == GraphDrag::Handle1) {
                c.cx1 = hx;
                c.cy1 = hy;
            } else {
                c.cx2 = hx;
                c.cy2 = hy;
            }
        }
        updatePose();
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) graphDrag_ = GraphDrag{};

    // ---- curves ----
    dl->PushClipRect(plotLo - ImVec2(6, 6), plotHi + ImVec2(6, 6), true);
    for (int id = 0; id < kCurveCount; ++id) {
        if (graphHidden_ & (1u << id)) continue;
        Access a = access(tl, kCurves[id]);
        if (a.size() == 0) continue;
        ImU32 col = curveColor(id);
        std::vector<ImVec2> pts;
        int n = std::max(32, (int)((plotHi.x - plotLo.x) / 3));
        for (int i = 0; i <= n; ++i) {
            float t = dur * i / n;
            pts.push_back(ImVec2(tx(t), vy(a.sampleAt(t))));
        }
        dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, id == graphCurve_ ? 2.5f : 1.6f);
        for (size_t k = 0; k < a.size(); ++k) {
            ImVec2 p(tx(a.time(k)), vy(a.value(k)));
            bool sel = id == graphCurve_ && (int)k == graphKey_;
            dl->AddCircleFilled(p, sel ? 5.5f : 4.0f, sel ? IM_COL32(255, 255, 255, 255) : col);
            dl->AddCircle(p, sel ? 5.5f : 4.0f, IM_COL32(20, 20, 20, 220));
        }
    }
    // Handles of the selected key's outgoing segment.
    if (graphCurve_ >= 0 && selHasNext) {
        Access a = access(tl, kCurves[graphCurve_]);
        ImVec2 k0(tx(a.time(graphKey_)), vy(a.value(graphKey_)));
        ImVec2 k1(tx(a.time(graphKey_ + 1)), vy(a.value(graphKey_ + 1)));
        ImVec2 h1 = handlePos(a, graphKey_, 1), h2 = handlePos(a, graphKey_, 2);
        bool bez = a.curve(graphKey_).type == Curve::Bezier;
        ImU32 hc = bez ? IM_COL32(255, 210, 90, 255) : IM_COL32(255, 210, 90, 120);
        dl->AddLine(k0, h1, hc, 1.2f);
        dl->AddLine(k1, h2, hc, 1.2f);
        for (ImVec2 h : {h1, h2}) {
            dl->AddRectFilled(h - ImVec2(4, 4), h + ImVec2(4, 4), hc);
            dl->AddRect(h - ImVec2(4, 4), h + ImVec2(4, 4), IM_COL32(20, 20, 20, 220));
        }
    }
    dl->PopClipRect();

    // Playhead.
    float px = tx(time_);
    dl->AddLine(ImVec2(px, origin.y), ImVec2(px, plotHi.y), theme::kPlayhead, 1.5f);
    dl->AddTriangleFilled(ImVec2(px - 6, origin.y), ImVec2(px + 6, origin.y), ImVec2(px, origin.y + 8), theme::kPlayhead);
    if (hovered && graphDrag_.kind == GraphDrag::None)
        ImGui::SetItemTooltip("左键：选关键帧 / 拖动改时间和数值（Shift 不吸附）；拖黄色手柄调整缓动");
    ImGui::End();
}
