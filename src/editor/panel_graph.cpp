// Graph view: value-over-time curves of the selected bone (or slot colour), Bezier handles,
// zoom and pan.
#include "editor/editor.h"
#include "editor/i18n.h"

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
};
constexpr CurveDef kCurves[] = {
    {Channel::Rotate, 0}, {Channel::Translate, 0}, {Channel::Translate, 1},
    {Channel::Scale, 0}, {Channel::Scale, 1}, {Channel::Shear, 0},
    {Channel::Shear, 1}, {Channel::Color, 0}, {Channel::Color, 1},
    {Channel::Color, 2}, {Channel::Color, 3},
};
constexpr int kCurveCount = (int)(sizeof(kCurves) / sizeof(kCurves[0]));

// Uniform access to the keys of one curve.
struct Access {
    std::vector<FloatKey>* f = nullptr;
    std::vector<Vec2Key>* v = nullptr;
    std::vector<ColorKey>* c = nullptr;
    std::vector<size_t> indices;
    int comp = 0;
    float fallback = 0;

    bool present() const { return f || v || c; }
    size_t size() const { return f ? f->size() : indices.size(); }
    size_t key(size_t i) const { return f ? i : indices[i]; }
    float time(size_t i) const { return f ? (*f)[i].time : v ? (*v)[key(i)].time : (*c)[key(i)].time; }
    float value(size_t i) const {
        if (f) return (*f)[i].value;
        if (v) return comp ? (*v)[key(i)].value.y : (*v)[key(i)].value.x;
        return (*c)[key(i)].value[comp];
    }
    void setValue(size_t i, float x) {
        if (f) (*f)[i].value = x;
        else if (v) (comp ? (*v)[key(i)].value.y : (*v)[key(i)].value.x) = x;
        else (*c)[key(i)].value[comp] = std::clamp(x, 0.0f, 1.0f);
    }
    const CurveShape& curve(size_t i) const { return f ? (*f)[i].curve : v ? componentCurve((*v)[key(i)],comp) : componentCurve((*c)[key(i)],comp); }
    CurveShape& editCurve(size_t i) { return f ? (*f)[i].curve : v ? editComponentCurve((*v)[key(i)],comp) : editComponentCurve((*c)[key(i)],comp); }
    float sampleAt(float t) const {
        if (f) return sample(*f, t, fallback);
        if (v) {
            Vec2 s = sample(*v, t, Vec2{fallback,fallback});
            return comp ? s.y : s.x;
        }
        Color base; base[comp] = fallback;
        return sample(*c, t, base)[comp];
    }
    int indexAt(float t) const {
        for (size_t i = 0; i < size(); ++i)
            if (std::fabs(time(i) - t) < kKeyEpsilon) return (int)i;
        return -1;
    }
};

// The timelines the graph shows: a bone's or a slot's (colour).
struct Source {
    BoneTimeline* bone = nullptr;
    SlotTimeline* slot = nullptr;
    Color setupColor;
};

Access access(const Source& src, const CurveDef& c) {
    Access a;
    a.comp = c.comp;
    a.fallback = c.channel == Channel::Scale || c.channel == Channel::Color ? 1.0f : 0.0f;
    if (c.channel == Channel::Color) {
        a.fallback = src.setupColor[c.comp];
        if (src.slot) a.c = &src.slot->color;
        if (a.c) for (size_t i = 0; i < a.c->size(); ++i) if ((*a.c)[i].componentMask & (1u << a.comp)) a.indices.push_back(i);
        return a;
    }
    if (!src.bone) return a;
    BoneTimeline& tl = *src.bone;
    switch (c.channel) {
    case Channel::Rotate: a.f = &tl.rotate; break;
    case Channel::Translate: a.v = &tl.translate; break;
    case Channel::Scale: a.v = &tl.scale; break;
    default: a.v = &tl.shear; break;
    }
    if (a.v) for (size_t i = 0; i < a.v->size(); ++i) if ((*a.v)[i].componentMask & (1u << a.comp)) a.indices.push_back(i);
    return a;
}

ImU32 curveColor(int id) {
    const CurveDef& c = kCurves[id];
    if (c.channel == Channel::Color) {
        static const ImU32 rgba[4] = {IM_COL32(240, 80, 80, 255), IM_COL32(90, 220, 100, 255), IM_COL32(90, 140, 250, 255),
                                      IM_COL32(220, 220, 220, 255)};
        return rgba[c.comp];
    }
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
    const char* names[] = {tr("旋转"),tr("移动 X"),tr("移动 Y"),tr("缩放 X"),tr("缩放 Y"),tr("斜切 X"),tr("斜切 Y"),tr("红"),tr("绿"),tr("蓝"),tr("透明度")};
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
    // A selected slot / attachment shows its colour curves, otherwise the selected bone's curves.
    bool slotMode = sel_.slot >= 0 && (sel_.kind == Selection::Slot || sel_.kind == Selection::Attachment);
    int bone = sel_.bone >= 0 && sel_.kind != Selection::None ? sel_.bone : -1;
    Source tl;
    if (an && slotMode) {
        auto it = an->slots.find(sel_.slot);
        if (it != an->slots.end() && !it->second.color.empty()) { tl.slot = &it->second; tl.setupColor = sk_.slots[sel_.slot].color; }
    } else if (an && bone >= 0) {
        auto it = an->bones.find(bone);
        if (it != an->bones.end()) tl.bone = &it->second;
    }
    if (!tl.bone && !tl.slot) {
        ImGui::TextDisabled(!an ? tr("没有动画。")
                            : slotMode ? tr("所选插槽在当前动画里没有颜色关键帧。")
                            : bone < 0 ? tr("选择一根骨骼（或插槽，看颜色曲线）。")
                                       : tr("所选骨骼在当前动画里没有关键帧。"));
        ImGui::End();
        return;
    }
    const int keyIndex = tl.slot ? sel_.slot : bone;  // timeline owner for moveChannelKey
    ImGuiIO& io = ImGui::GetIO();
    const float fh = ImGui::GetFrameHeight();

    if (graphCurve_ >= kCurveCount || (graphCurve_ >= 0 && !access(tl,kCurves[graphCurve_]).present())) graphCurve_ = graphKey_ = -1;
    if (graphCurve_ >= 0 && (graphKey_ < 0 || graphKey_ >= (int)access(tl, kCurves[graphCurve_]).size())) graphKey_ = -1;

    // ---- toolbar: curve presets for the selected key ----
    {
        bool hasSel = graphCurve_ >= 0 && graphKey_ >= 0;
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", tl.slot ? sk_.slots[sel_.slot].name.c_str() : sk_.bones[bone].name.c_str());
        ImGui::SameLine(0, 12);
        ImGui::BeginDisabled(!hasSel);
        auto preset = [&](const char* label, const CurveShape& c) {
            if (ImGui::Button(label) && hasSel) {
                checkpoint();
                access(tl, kCurves[graphCurve_]).editCurve(graphKey_) = c;
            }
            ImGui::SameLine(0, 3);
        };
        preset(tr("线性"), CurveShape::linear());
        preset(tr("阶梯"), CurveShape::stepped());
        preset(tr("缓入缓出"), CurveShape::bezier(0.42f, 0, 0.58f, 1));
        preset(tr("缓入"), CurveShape::bezier(0.42f, 0, 1, 1));
        preset(tr("缓出"), CurveShape::bezier(0, 0, 0.58f, 1));
        ImGui::EndDisabled();
        ImGui::SameLine(0, 12);
        if (hasSel) {
            Access a = access(tl, kCurves[graphCurve_]);
            ImGui::TextDisabled(tr("%s  第 %d 帧  值 %.2f"), names[graphCurve_], (int)std::round(a.time(graphKey_) * fps_),
                                a.value(graphKey_));
        } else {
            ImGui::TextDisabled(tr("点击关键帧，拖手柄调缓动；滚轮缩放时间，Ctrl+滚轮缩放数值，右键拖动平移"));
        }
        ImGui::SameLine(0, 12);
        if (ImGui::SmallButton(tr("适配"))) {
            graphPps_ = 0;
            graphScroll_ = 0;
            graphAutoY_ = true;
        }
    }
    ImGui::BeginDisabled(graphCurve_ < 0);
    if (ImGui::Button(tr("打所选分量关键帧"))) {
        const auto& curve = kCurves[graphCurve_];
        auto a = access(tl,curve); float value = a.sampleAt(time_);
        if (curve.channel == Channel::Color) value = std::clamp(value,0.0f,1.0f);
        checkpoint();
        if (curve.channel == Channel::Rotate) setRotateKey(*an,keyIndex,time_,value);
        else setComponentKey(*an,keyIndex,curve.channel,curve.comp,time_,value);
        graphKey_ = access(tl,curve).indexAt(time_); updatePose();
    }
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(graphCurve_ < 0 || graphKey_ < 0);
    if (ImGui::Button(tr("删除所选分量关键帧"))) {
        const auto& curve = kCurves[graphCurve_]; auto a = access(tl,curve);
        checkpoint();
        if (curve.channel == Channel::Rotate) removeKey(*an,keyIndex,curve.channel,a.time(graphKey_));
        else removeComponentKey(*an,keyIndex,curve.channel,curve.comp,a.time(graphKey_));
        graphKey_ = -1; updatePose(); ImGui::EndDisabled(); ImGui::End(); return;
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped(tr("分量可独立打帧、移动和调整缓动。"));

    // ---- curve list ----
    float listW = ImGui::GetFontSize() * 10;
    ImGui::BeginChild("##curves", ImVec2(listW, 0), ImGuiChildFlags_Borders);
    for (int id = 0; id < kCurveCount; ++id) {
        Access a = access(tl, kCurves[id]);
        if (!a.present()) continue;
        ImGui::PushID(id);
        bool shown = !(graphHidden_ & (1u << id));
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(p + ImVec2(2, h * 0.3f), p + ImVec2(h - 2, h * 0.7f), curveColor(id));
        ImGui::Dummy(ImVec2(h, h));
        ImGui::SameLine();
        if (ImGui::Checkbox("##show", &shown)) graphHidden_ ^= (1u << id);
        ImGui::SameLine();
        if (ImGui::Selectable(names[id],graphCurve_ == id)) {
            graphCurve_ = id; graphKey_ = a.indexAt(time_);
            if (graphKey_ < 0 && a.size()) graphKey_ = 0;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ---- graph area ----
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = std::max(size.x, 100.0f);
    size.y = std::max(size.y, 80.0f);
    ImGui::InvisibleButton("##graph", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rulerH = fh;
    const float labelW = ImGui::GetFontSize() * 3.2f;
    ImVec2 plotLo(origin.x + labelW, origin.y + rulerH + 6), plotHi(origin.x + size.x - 10, origin.y + size.y - 8);
    float dur = std::max(an->duration, 1.0f / fps_);

    // Vertical range: fit every visible curve (sampled) and its keys, unless the user zoomed.
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
    if (graphAutoY_) {
        graphVMin_ = vmin;
        graphVMax_ = vmax;
    } else {
        vmin = graphVMin_;
        vmax = graphVMax_;
    }
    const float plotW = plotHi.x - plotLo.x, plotH = plotHi.y - plotLo.y;
    float pps = graphPps_ > 0 ? graphPps_ : plotW / dur;  // pixels per second
    if (graphPps_ <= 0) graphScroll_ = 0;

    auto tx = [&](float t) { return plotLo.x + (t - graphScroll_) * pps; };
    auto vy = [&](float v) { return plotHi.y - (v - vmin) / (vmax - vmin) * plotH; };
    auto xt = [&](float x) { return graphScroll_ + (x - plotLo.x) / pps; };
    auto yv = [&](float y) { return vmin + (plotHi.y - y) / plotH * (vmax - vmin); };

    // Zoom (wheel: time, Ctrl+wheel: values) and pan (right drag) around the mouse.
    if (hovered && io.MouseWheel != 0) {
        if (io.KeyCtrl) {
            float at = yv(io.MousePos.y), k = std::pow(1.2f, -io.MouseWheel);
            graphVMin_ = at + (vmin - at) * k;
            graphVMax_ = at + (vmax - at) * k;
            graphAutoY_ = false;
        } else {
            float at = xt(io.MousePos.x);
            float n = std::clamp(pps * std::pow(1.2f, io.MouseWheel), 10.0f, 10000.0f);
            graphPps_ = n;
            graphScroll_ = at - (io.MousePos.x - plotLo.x) / n;
        }
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) graphPanning_ = true;
    if (graphPanning_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            if (graphPps_ <= 0) graphPps_ = pps;
            graphScroll_ -= io.MouseDelta.x / pps;
            float dv = io.MouseDelta.y / plotH * (vmax - vmin);
            graphVMin_ = vmin + dv;
            graphVMax_ = vmax + dv;
            graphAutoY_ = false;
        } else {
            graphPanning_ = false;
        }
    }

    dl->AddRectFilled(origin, origin + size, IM_COL32(46, 46, 49, 255));
    dl->AddRectFilled(ImVec2(origin.x, origin.y), ImVec2(origin.x + size.x, origin.y + rulerH), IM_COL32(40, 40, 43, 255));
    ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);

    // Grid: frames on X, "nice" steps on Y.
    int every = 1;
    float ppf = pps / fps_;
    while (ppf * every < 34) every *= 2;
    dl->PushClipRect(ImVec2(plotLo.x - 4, origin.y), ImVec2(origin.x + size.x, origin.y + size.y), true);
    int firstF = std::max(0, (int)std::floor(graphScroll_ * fps_ / every) * every);
    for (int f = firstF; f <= (int)std::round(dur * fps_); f += every) {
        float x = tx(f / fps_);
        if (x > plotHi.x + 4) break;
        dl->AddLine(ImVec2(x, plotLo.y), ImVec2(x, plotHi.y), IM_COL32(255, 255, 255, 14));
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", f);
        dl->AddText(ImVec2(x + 3, origin.y + 2), dim, buf);
    }
    dl->PopClipRect();
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
    if (graphCurve_ >= 0 && graphKey_ >= 0) {
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
            for (int offset = 0; offset < kCurveCount; ++offset) {
                const int id = (offset + std::max(graphCurve_,0)) % kCurveCount;
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
                const auto& curve = kCurves[graphCurve_];
                if (curve.channel == Channel::Rotate) moveChannelKey(*an,keyIndex,curve.channel,from,to);
                else moveComponentKey(*an,keyIndex,curve.channel,curve.comp,from,to);
                graphKey_ = access(tl, kCurves[graphCurve_]).indexAt(to);
            }
            setTime(access(tl, kCurves[graphCurve_]).time(graphKey_));
        } else if (graphCurve_ >= 0 && selHasNext) {
            ensure();
            Access a = access(tl, kCurves[graphCurve_]);
            float t0 = a.time(graphKey_), t1 = a.time(graphKey_ + 1), v0 = a.value(graphKey_), v1 = a.value(graphKey_ + 1);
            float dv = std::fabs(v1 - v0) > 1e-5f ? v1 - v0 : (vmax - vmin) * 0.25f;
            CurveShape& c = a.editCurve(graphKey_);
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
        int n = std::max(32, (int)(plotW / 3));
        float t0 = std::max(0.0f, xt(plotLo.x)), t1 = std::min(dur, xt(plotHi.x));
        for (int i = 0; i <= n && t1 > t0; ++i) {
            float t = t0 + (t1 - t0) * i / n;
            pts.push_back(ImVec2(tx(t), vy(a.sampleAt(t))));
        }
        if (pts.size() < 2) continue;
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
        ImGui::SetItemTooltip(tr("左键：选关键帧 / 拖动改时间和数值（Shift 不吸附）；拖黄色手柄调整缓动"));
    ImGui::End();
}
