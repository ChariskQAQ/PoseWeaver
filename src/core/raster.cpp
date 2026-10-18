#include "core/raster.h"
#include "core/clipping.h"

#include <algorithm>
#include <cmath>

namespace rig {

bool poseBounds(const Skeleton& sk, const Pose& pose, Vec2& lo, Vec2& hi) {
    bool any = false;
    for (int s : pose.drawOrder) {
        const Attachment* att = poseAttachment(sk, pose, s);
        if (!att || att->isPolygon()) continue;
        int a = s < (int)pose.slotAttachment.size() ? pose.slotAttachment[s] : -1;
        for (Vec2 p : computeVertices(sk, pose, sk.slots[s], *att, findDeform(pose, s, a))) {
            if (!any) lo = hi = p;
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
            any = true;
        }
    }
    return any;
}

namespace {
struct Sampler {
    const RasterTexture& tex;
    void sample(float u, float v, float out[4]) const {
        float x = u * tex.width - 0.5f, y = v * tex.height - 0.5f;
        int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
        float fx = x - x0, fy = y - y0;
        auto px = [&](int ix, int iy) {
            ix = std::clamp(ix, 0, tex.width - 1);
            iy = std::clamp(iy, 0, tex.height - 1);
            return tex.rgba + (size_t(iy) * tex.width + ix) * 4;
        };
        const unsigned char *a = px(x0, y0), *b = px(x0 + 1, y0), *c = px(x0, y0 + 1), *d = px(x0 + 1, y0 + 1);
        // Interpolate premultiplied colour so transparent texels do not bleed their RGB.
        float acc[4] = {0, 0, 0, 0};
        const float w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
        const unsigned char* p[4] = {a, b, c, d};
        for (int k = 0; k < 4; ++k) {
            float alpha = p[k][3] / 255.0f * w[k];
            acc[0] += p[k][0] / 255.0f * alpha;
            acc[1] += p[k][1] / 255.0f * alpha;
            acc[2] += p[k][2] / 255.0f * alpha;
            acc[3] += alpha;
        }
        out[3] = acc[3];
        for (int k = 0; k < 3; ++k) out[k] = acc[3] > 1e-6f ? acc[k] / acc[3] : 0.0f;
    }
};

void blend(unsigned char* dst, const float src[4]) {
    float sa = src[3];
    if (sa <= 0) return;
    float da = dst[3] / 255.0f;
    float oa = sa + da * (1 - sa);
    for (int k = 0; k < 3; ++k) {
        float d = dst[k] / 255.0f;
        float o = oa > 1e-6f ? (src[k] * sa + d * da * (1 - sa)) / oa : 0.0f;
        dst[k] = (unsigned char)std::clamp((int)std::lround(o * 255), 0, 255);
    }
    dst[3] = (unsigned char)std::clamp((int)std::lround(oa * 255), 0, 255);
}

void drawTriangles(const Geometry& g, const RasterTexture& tex, const Color& tint, const Affine& toPx, int w, int h,
                   std::vector<unsigned char>& rgba) {
    Sampler sampler{tex};
    std::vector<Vec2> px(g.positions.size());
    for (size_t i = 0; i < px.size(); ++i) px[i] = toPx.apply(g.positions[i]);
    for (size_t t = 0; t + 2 < g.triangles.size(); t += 3) {
        int i0 = g.triangles[t], i1 = g.triangles[t + 1], i2 = g.triangles[t + 2];
        Vec2 a = px[i0], b = px[i1], c = px[i2];
        float area = cross(b - a, c - a);
        if (std::fabs(area) < 1e-9f) continue;
        int x0 = std::max(0, (int)std::floor(std::min({a.x, b.x, c.x}))), x1 = std::min(w - 1, (int)std::ceil(std::max({a.x, b.x, c.x})));
        int y0 = std::max(0, (int)std::floor(std::min({a.y, b.y, c.y}))), y1 = std::min(h - 1, (int)std::ceil(std::max({a.y, b.y, c.y})));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                Vec2 p{x + 0.5f, y + 0.5f};
                float w0 = cross(b - p, c - p) / area, w1 = cross(c - p, a - p) / area, w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                Vec2 uv = g.uvs[i0] * w0 + g.uvs[i1] * w1 + g.uvs[i2] * w2;
                float col[4];
                sampler.sample(uv.x, uv.y, col);
                for (int k = 0; k < 4; ++k) col[k] *= tint[k];
                blend(&rgba[(size_t(y) * w + x) * 4], col);
            }
    }
}
} // namespace

void renderPose(const Skeleton& sk, const Pose& pose, const std::vector<RasterTexture>& textures, const Affine& toPx,
                int width, int height, std::vector<unsigned char>& rgba) {
    rgba.resize(size_t(width) * height * 4, 0);
    Geometry g, clipped;
    ClipTriangles clip;
    int clipEnd = -2;
    for (int s : pose.drawOrder) {
        bool endsHere = clipEnd != -2 && s == clipEnd;
        const Attachment* att = poseAttachment(sk, pose, s);
        int a = s < (int)pose.slotAttachment.size() ? pose.slotAttachment[s] : -1;
        if (att) {
            if (att->type == AttachmentType::Clipping) {
                clip = clipTrianglesOf(computeVertices(sk, pose, sk.slots[s], *att, findDeform(pose, s, a)));
                clipEnd = att->endSlot;
            } else if (!att->isPolygon() && att->image >= 0 && att->image < (int)textures.size() && textures[att->image].rgba) {
                computeGeometry(sk, pose, sk.slots[s], *att, g, findDeform(pose, s, a));
                if (!clip.empty()) {
                    clipGeometry(g, clip, clipped);
                    std::swap(g, clipped);
                }
                const Color& tint = s < (int)pose.slotColor.size() ? pose.slotColor[s] : sk.slots[s].color;
                drawTriangles(g, textures[att->image], tint, toPx, width, height, rgba);
            }
        }
        if (endsHere) {
            clip.clear();
            clipEnd = -2;
        }
    }
}

std::vector<unsigned char> downsample(const std::vector<unsigned char>& src, int w, int h, int f) {
    if (f <= 1) return src;
    int ow = w / f, oh = h / f;
    std::vector<unsigned char> out(size_t(ow) * oh * 4);
    for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int sy = 0; sy < f; ++sy)
                for (int sx = 0; sx < f; ++sx) {
                    const unsigned char* p = &src[(size_t(y * f + sy) * w + (x * f + sx)) * 4];
                    float a = p[3] / 255.0f;
                    for (int k = 0; k < 3; ++k) acc[k] += p[k] * a;
                    acc[3] += a;
                }
            unsigned char* o = &out[(size_t(y) * ow + x) * 4];
            for (int k = 0; k < 3; ++k) o[k] = acc[3] > 0 ? (unsigned char)std::lround(acc[k] / acc[3]) : 0;
            o[3] = (unsigned char)std::lround(acc[3] / (f * f) * 255);
        }
    return out;
}

// ---------------------------------------------------------------- GIF

namespace {
struct BitWriter {
    std::vector<uint8_t>& out;
    std::vector<uint8_t> block;
    uint32_t acc = 0;
    int bits = 0;
    void put(int code, int size) {
        acc |= (uint32_t)code << bits;
        bits += size;
        while (bits >= 8) {
            byte((uint8_t)(acc & 0xFF));
            acc >>= 8;
            bits -= 8;
        }
    }
    void byte(uint8_t b) {
        block.push_back(b);
        if (block.size() == 255) flushBlock();
    }
    void flushBlock() {
        if (block.empty()) return;
        out.push_back((uint8_t)block.size());
        out.insert(out.end(), block.begin(), block.end());
        block.clear();
    }
    void finish() {
        if (bits > 0) byte((uint8_t)(acc & 0xFF));
        acc = 0;
        bits = 0;
        flushBlock();
        out.push_back(0);  // block terminator
    }
};

void lzw(const std::vector<uint8_t>& indices, std::vector<uint8_t>& out) {
    const int minCode = 8, clear = 1 << minCode, eoi = clear + 1;
    out.push_back((uint8_t)minCode);
    BitWriter w{out};
    std::vector<int> dict(4096 * 256, -1);  // (prefix code, next byte) -> code
    int next = eoi + 1, size = minCode + 1;
    w.put(clear, size);
    int prefix = indices.empty() ? -1 : indices[0];
    for (size_t i = 1; i < indices.size(); ++i) {
        int c = indices[i];
        int& entry = dict[size_t(prefix) * 256 + c];
        if (entry >= 0) {
            prefix = entry;
            continue;
        }
        w.put(prefix, size);
        if (next < 4096) {
            entry = next++;
            if (next > (1 << size) && size < 12) ++size;
        } else {
            w.put(clear, size);
            std::fill(dict.begin(), dict.end(), -1);
            next = eoi + 1;
            size = minCode + 1;
        }
        prefix = c;
    }
    // Like most encoders: last code, a clear code (decoder tables stay in sync), then EOI.
    if (prefix >= 0) w.put(prefix, size);
    w.put(clear, size);
    w.put(eoi, minCode + 1);
    w.finish();
}
} // namespace

std::vector<uint8_t> encodeGif(const std::vector<std::vector<unsigned char>>& frames, int width, int height, int delay, bool loop) {
    std::vector<uint8_t> out;
    auto u16 = [&](int v) {
        out.push_back((uint8_t)(v & 0xFF));
        out.push_back((uint8_t)((v >> 8) & 0xFF));
    };
    const char* head = "GIF89a";
    out.insert(out.end(), head, head + 6);
    u16(width);
    u16(height);
    out.push_back(0xF7);  // global colour table, 8 bits, 256 entries
    out.push_back(252);   // background = transparent index
    out.push_back(0);
    // Palette: 6 x 7 x 6 colour cube (252 entries) + 4 unused (index 252 = transparent).
    for (int i = 0; i < 256; ++i) {
        int r = 0, g = 0, b = 0;
        if (i < 252) {
            r = (i / 42) * 51;
            g = ((i / 6) % 7) * 255 / 6;
            b = (i % 6) * 51;
        }
        out.push_back((uint8_t)r);
        out.push_back((uint8_t)g);
        out.push_back((uint8_t)b);
    }
    if (loop) {
        const uint8_t ext[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 0x03, 0x01, 0x00, 0x00, 0x00};
        out.insert(out.end(), ext, ext + sizeof(ext));
    }
    std::vector<uint8_t> idx(size_t(width) * height);
    for (const auto& f : frames) {
        // Graphic control: disposal 2 (clear to background) so transparency does not accumulate.
        const uint8_t gce[] = {0x21, 0xF9, 0x04, 0x09, (uint8_t)(delay & 0xFF), (uint8_t)((delay >> 8) & 0xFF), 252, 0x00};
        out.insert(out.end(), gce, gce + sizeof(gce));
        out.push_back(0x2C);
        u16(0);
        u16(0);
        u16(width);
        u16(height);
        out.push_back(0);
        for (size_t p = 0; p < idx.size(); ++p) {
            const unsigned char* c = &f[p * 4];
            if (c[3] < 128) {
                idx[p] = 252;
                continue;
            }
            int r = (c[0] * 5 + 127) / 255, g = (c[1] * 6 + 127) / 255, b = (c[2] * 5 + 127) / 255;
            idx[p] = (uint8_t)(r * 42 + g * 6 + b);
        }
        lzw(idx, out);
    }
    out.push_back(0x3B);
    return out;
}

} // namespace rig
