#include "core/render_effects.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace rig {
PixelEffects::PixelEffects() {
    const uint32_t colors[] = {0x171b2b, 0x34394f, 0x626878, 0xb1bac5, 0xf0eee7, 0x642c41, 0xad4554, 0xef8260,
        0xf7c66b, 0x745c42, 0x3d6653, 0x68a66b, 0xa8d07a, 0x294d72, 0x538bab, 0x8ecbd0};
    for (auto c : colors) palette.push_back({((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f});
}
bool operator==(const RenderEffects& a, const RenderEffects& b) {
    const auto& x = a.light; const auto& y = b.light;
    const auto& p = a.pixel; const auto& q = b.pixel;
    return x.enabled == y.enabled && x.position.x == y.position.x && x.position.y == y.position.y && x.color == y.color &&
        x.radius == y.radius && x.intensity == y.intensity && x.ambient == y.ambient &&
        p.enabled == q.enabled && p.quantize == q.quantize && p.dither == q.dither && p.outline == q.outline &&
        p.size == q.size && p.outlineColor == q.outlineColor && p.palette == q.palette;
}
bool effectsActive(const RenderEffects& e) { return e.light.enabled || e.pixel.enabled; }
bool validRenderEffects(const RenderEffects& e, std::string* error) {
    auto unitColor = [](const std::array<float, 3>& c) {
        return std::all_of(c.begin(), c.end(), [](float v) { return std::isfinite(v) && v >= 0 && v <= 1; });
    };
    const auto& l = e.light; const auto& p = e.pixel;
    bool ok = std::isfinite(l.position.x) && std::isfinite(l.position.y) && std::abs(l.position.x) <= 1000000 && std::abs(l.position.y) <= 1000000 &&
        std::isfinite(l.radius) && l.radius >= 1 && l.radius <= 100000 && std::isfinite(l.intensity) && l.intensity >= 0 && l.intensity <= 8 &&
        std::isfinite(l.ambient) && l.ambient >= 0 && l.ambient <= 2 && unitColor(l.color) &&
        p.size >= 1 && p.size <= 32 && unitColor(p.outlineColor) && !p.palette.empty() && p.palette.size() <= 256 &&
        std::all_of(p.palette.begin(), p.palette.end(), unitColor);
    if (!ok && error) *error = "Invalid light / pixel settings (finite values, 1-256 RGB palette colors required)";
    return ok;
}
int effectsPadding(const RenderEffects& e, float scale) {
    return e.pixel.enabled ? (int)std::ceil(e.pixel.size * scale * (e.pixel.outline ? 2 : 1)) : 0;
}
uint64_t effectsBufferBytes(const RenderEffects& e, uint64_t pixels) {
    return effectsActive(e) ? pixels * (8 + (e.pixel.enabled && e.pixel.outline ? 1 : 0)) : 0;
}

void applyRenderEffects(std::vector<unsigned char>& rgba, int w, int h, const Affine& toPixel, const RenderEffects& e) {
    if (!effectsActive(e) || !validRenderEffects(e) || w <= 0 || h <= 0 || rgba.size() != size_t(w) * h * 4 || std::abs(toPixel.det()) < 1e-12f) return;
    const auto source = rgba;
    const auto inverse = toPixel.inverse();
    const auto& p = e.pixel;
    struct Match { size_t first = 0, second = 0; float ratio = 0; };
    std::unordered_map<uint32_t, Match> cache;
    const int bayer[] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        Vec2 world = inverse.apply({x + 0.5f, y + 0.5f});
        double gx = 0, gy = 0;
        int sx = x, sy = y;
        if (p.enabled) {
            gx = std::floor(double(world.x) / p.size); gy = std::floor(double(world.y) / p.size);
            world = {(float)((gx + 0.5) * p.size), (float)((gy + 0.5) * p.size)};
            Vec2 sample = toPixel.apply(world);
            sx = (int)std::clamp(std::floor(sample.x), 0.0f, (float)(w - 1)); sy = (int)std::clamp(std::floor(sample.y), 0.0f, (float)(h - 1));
        }
        const size_t out = (size_t(y) * w + x) * 4, in = (size_t(sy) * w + sx) * 4;
        unsigned char alpha = source[in + 3];
        if (p.enabled) alpha = alpha >= 128 ? 255 : 0;
        rgba[out + 3] = alpha;
        if (!alpha) { rgba[out] = rgba[out + 1] = rgba[out + 2] = 0; continue; }
        std::array<float, 3> rgb;
        float light = 0;
        if (e.light.enabled) {
            light = std::max(0.0f, 1 - length(world - e.light.position) / e.light.radius);
            light = light * light * e.light.intensity;
        }
        for (int c = 0; c < 3; ++c) {
            float value = source[in + c] / 255.0f;
            if (e.light.enabled) value *= e.light.ambient + light * e.light.color[c];
            rgb[c] = std::clamp(value, 0.0f, 1.0f);
        }
        if (p.enabled && p.quantize) {
            uint32_t key = 0;
            for (int c = 0; c < 3; ++c) { auto value = (uint32_t)std::lround(rgb[c] * 255); key |= value << (c * 8); rgb[c] = value / 255.0f; }
            auto it = cache.find(key);
            Match m;
            if (it != cache.end()) m = it->second;
            else {
                float first = 1e9f, second = 1e9f;
                for (size_t i = 0; i < p.palette.size(); ++i) {
                    float d = 0;
                    for (int c = 0; c < 3; ++c) { float v = rgb[c] - p.palette[i][c]; d += v * v; }
                    if (d < first) { second = first; m.second = m.first; first = d; m.first = i; }
                    else if (d < second) { second = d; m.second = i; }
                }
                float projection = 0, norm = 0;
                for (int c = 0; c < 3; ++c) {
                    float v = p.palette[m.second][c] - p.palette[m.first][c];
                    projection += (rgb[c] - p.palette[m.first][c]) * v; norm += v * v;
                }
                m.ratio = norm > 0 ? std::clamp(projection / norm, 0.0f, 1.0f) : 0;
                if (cache.size() < 65536) cache.emplace(key, m);
            }
            const int bx = ((int)std::fmod(gx, 4) + 4) % 4, by = ((int)std::fmod(gy, 4) + 4) % 4;
            rgb = p.palette[p.dither && m.ratio > (bayer[by * 4 + bx] + 0.5f) / 16 ? m.second : m.first];
        }
        for (int c = 0; c < 3; ++c) rgba[out + c] = (unsigned char)std::lround(rgb[c] * 255);
    }
    if (p.enabled && p.outline) {
        auto outline = p.outlineColor;
        if (p.quantize) {
            float best = 1e9f;
            for (const auto& color : p.palette) {
                float distance = 0;
                for (int c = 0; c < 3; ++c) { float d = color[c] - p.outlineColor[c]; distance += d * d; }
                if (distance < best) { best = distance; outline = color; }
            }
        }
        std::vector<unsigned char> alpha(size_t(w) * h);
        for (size_t i = 0; i < alpha.size(); ++i) alpha[i] = rgba[i * 4 + 3];
        Vec2 dx = toPixel.applyVector({(float)p.size, 0}), dy = toPixel.applyVector({0, (float)p.size});
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            size_t i = size_t(y) * w + x;
            if (alpha[i]) continue;
            bool edge = false;
            for (int yy = -1; yy <= 1; ++yy) for (int xx = -1; xx <= 1; ++xx) {
                int sx = (int)std::lround(x + dx.x * xx + dy.x * yy), sy = (int)std::lround(y + dx.y * xx + dy.y * yy);
                if (sx >= 0 && sy >= 0 && sx < w && sy < h && alpha[size_t(sy) * w + sx]) edge = true;
            }
            if (edge) { for (int c = 0; c < 3; ++c) rgba[i * 4 + c] = (unsigned char)std::lround(outline[c] * 255); rgba[i * 4 + 3] = 255; }
        }
    }
}
} // namespace rig
