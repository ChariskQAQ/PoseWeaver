#pragma once
#include "core/math.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rig {
struct PointLight {
    bool enabled = false;
    Vec2 position{-80, 120};
    std::array<float, 3> color{1, 0.85f, 0.65f};
    float radius = 300, intensity = 1.2f, ambient = 0.6f;
};
struct PixelEffects {
    bool enabled = false, quantize = true, dither = false, outline = false;
    int size = 2;
    std::array<float, 3> outlineColor{0.08f, 0.09f, 0.12f};
    std::vector<std::array<float, 3>> palette;
    PixelEffects();
};
struct RenderEffects {
    PointLight light;
    PixelEffects pixel;
};
bool operator==(const RenderEffects& a, const RenderEffects& b);
bool effectsActive(const RenderEffects& effects);
bool validRenderEffects(const RenderEffects& effects, std::string* error = nullptr);
int effectsPadding(const RenderEffects& effects, float scale);
uint64_t effectsBufferBytes(const RenderEffects& effects, uint64_t pixels);
// World-anchored sampling and dithering, independent of canvas cropping or animation time.
void applyRenderEffects(std::vector<unsigned char>& rgba, int width, int height,
                        const Affine& worldToPixel, const RenderEffects& effects);
} // namespace rig
