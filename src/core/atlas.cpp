#include "core/atlas.h"

#include <algorithm>
#include <sstream>

namespace rig {

namespace {
struct Rect {
    int x, y, w, h;
};

bool contains(const Rect& a, const Rect& b) { return b.x >= a.x && b.y >= a.y && b.x + b.w <= a.x + a.w && b.y + b.h <= a.y + a.h; }

// MaxRects bin with best-short-side-fit placement.
class MaxRects {
public:
    MaxRects(int w, int h) : free_{{0, 0, w, h}} {}

    bool insert(int w, int h, bool allowRotate, Rect& placed, bool& rotated) {
        int bestShort = 1 << 30, bestLong = 1 << 30;
        bool found = false;
        for (const Rect& f : free_) {
            for (int r = 0; r < (allowRotate ? 2 : 1); ++r) {
                int pw = r ? h : w, ph = r ? w : h;
                if (pw > f.w || ph > f.h) continue;
                int dx = f.w - pw, dy = f.h - ph;
                int s = std::min(dx, dy), l = std::max(dx, dy);
                if (s < bestShort || (s == bestShort && l < bestLong)) {
                    bestShort = s;
                    bestLong = l;
                    placed = {f.x, f.y, pw, ph};
                    rotated = r == 1;
                    found = true;
                }
            }
        }
        if (!found) return false;
        split(placed);
        return true;
    }

private:
    std::vector<Rect> free_;

    void split(const Rect& used) {
        std::vector<Rect> next;
        for (const Rect& f : free_) {
            if (used.x >= f.x + f.w || used.x + used.w <= f.x || used.y >= f.y + f.h || used.y + used.h <= f.y) {
                next.push_back(f);
                continue;
            }
            if (used.x > f.x) next.push_back({f.x, f.y, used.x - f.x, f.h});
            if (used.x + used.w < f.x + f.w) next.push_back({used.x + used.w, f.y, f.x + f.w - used.x - used.w, f.h});
            if (used.y > f.y) next.push_back({f.x, f.y, f.w, used.y - f.y});
            if (used.y + used.h < f.y + f.h) next.push_back({f.x, used.y + used.h, f.w, f.y + f.h - used.y - used.h});
        }
        // Drop free rectangles contained in others.
        std::vector<Rect> pruned;
        for (size_t i = 0; i < next.size(); ++i) {
            bool inside = false;
            for (size_t j = 0; j < next.size() && !inside; ++j)
                if (i != j && contains(next[j], next[i]) && (!contains(next[i], next[j]) || j < i)) inside = true;
            if (!inside) pruned.push_back(next[i]);
        }
        free_ = std::move(pruned);
    }
};

int nextPow2(int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}
} // namespace

bool packAtlas(const std::vector<AtlasInput>& inputs, const AtlasSettings& st, Atlas& out, std::string* error) {
    out = Atlas{};
    const int pad = std::max(0, st.padding);
    struct Item {
        size_t index;
        Rect trim;  // inside the original image
    };
    std::vector<Item> items;
    for (size_t i = 0; i < inputs.size(); ++i) {
        const AtlasInput& in = inputs[i];
        Rect t{0, 0, std::max(1, in.width), std::max(1, in.height)};
        if (st.trim && in.rgba && in.width > 0 && in.height > 0) {
            int x0 = in.width, y0 = in.height, x1 = -1, y1 = -1;
            for (int y = 0; y < in.height; ++y)
                for (int x = 0; x < in.width; ++x)
                    if (in.rgba[(size_t(y) * in.width + x) * 4 + 3] > st.alphaThreshold) {
                        x0 = std::min(x0, x);
                        y0 = std::min(y0, y);
                        x1 = std::max(x1, x);
                        y1 = std::max(y1, y);
                    }
            t = x1 < 0 ? Rect{0, 0, 1, 1} : Rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
        }
        if (t.w + 2 * pad > st.maxWidth || t.h + 2 * pad > st.maxHeight) {
            bool fitsRotated = st.allowRotate && t.h + 2 * pad <= st.maxWidth && t.w + 2 * pad <= st.maxHeight;
            if (!fitsRotated) {
                if (error) *error = "image " + in.name + " is larger than the maximum page size";
                return false;
            }
        }
        items.push_back({i, t});
    }
    // Largest first packs best.
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return std::max(a.trim.w, a.trim.h) > std::max(b.trim.w, b.trim.h);
    });

    out.regions.resize(inputs.size());
    std::vector<MaxRects> bins;
    std::vector<int> usedW, usedH;
    for (const Item& it : items) {
        Rect placed{};
        bool rotated = false;
        int page = -1;
        for (size_t b = 0; b < bins.size() && page < 0; ++b)
            if (bins[b].insert(it.trim.w + pad, it.trim.h + pad, st.allowRotate, placed, rotated)) page = (int)b;
        if (page < 0) {
            // The page edge keeps `pad` free on the top/left; each region reserves pad on its right/bottom.
            bins.emplace_back(st.maxWidth - pad, st.maxHeight - pad);
            usedW.push_back(0);
            usedH.push_back(0);
            page = (int)bins.size() - 1;
            if (!bins.back().insert(it.trim.w + pad, it.trim.h + pad, st.allowRotate, placed, rotated)) {
                if (error) *error = "image " + inputs[it.index].name + " does not fit on a page";
                return false;
            }
        }
        AtlasRegion& r = out.regions[it.index];
        r.name = inputs[it.index].name;
        r.page = page;
        r.x = placed.x + pad;
        r.y = placed.y + pad;
        r.width = placed.w - pad;
        r.height = placed.h - pad;
        r.rotated = rotated;
        r.offsetX = it.trim.x;
        r.offsetY = it.trim.y;
        r.originalWidth = std::max(1, inputs[it.index].width);
        r.originalHeight = std::max(1, inputs[it.index].height);
        usedW[page] = std::max(usedW[page], r.x + r.width + pad);
        usedH[page] = std::max(usedH[page], r.y + r.height + pad);
    }

    out.pages.resize(bins.size());
    for (size_t p = 0; p < bins.size(); ++p) {
        AtlasPage& page = out.pages[p];
        page.width = st.powerOfTwo ? nextPow2(usedW[p]) : usedW[p];
        page.height = st.powerOfTwo ? nextPow2(usedH[p]) : usedH[p];
        page.rgba.assign(size_t(page.width) * page.height * 4, 0);
    }
    // Copy pixels (rotated regions are stored 90 degrees clockwise).
    for (size_t i = 0; i < inputs.size(); ++i) {
        const AtlasInput& in = inputs[i];
        const AtlasRegion& r = out.regions[i];
        if (!in.rgba) continue;
        AtlasPage& page = out.pages[r.page];
        int tw = r.rotated ? r.height : r.width, th = r.rotated ? r.width : r.height;  // trimmed size
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x) {
                int sx = r.offsetX + x, sy = r.offsetY + y;
                if (sx >= in.width || sy >= in.height) continue;
                int dx = r.rotated ? r.x + (th - 1 - y) : r.x + x;
                int dy = r.rotated ? r.y + x : r.y + y;
                const unsigned char* src = in.rgba + (size_t(sy) * in.width + sx) * 4;
                unsigned char* dst = page.rgba.data() + (size_t(dy) * page.width + dx) * 4;
                std::copy(src, src + 4, dst);
            }
    }
    return true;
}

std::string atlasText(const Atlas& atlas, const std::vector<std::string>& pageFiles) {
    std::ostringstream o;
    for (size_t p = 0; p < atlas.pages.size(); ++p) {
        o << (p < pageFiles.size() ? pageFiles[p] : "page" + std::to_string(p) + ".png") << "\n";
        o << "size: " << atlas.pages[p].width << "," << atlas.pages[p].height << "\n";
        o << "format: RGBA8888\nfilter: Linear,Linear\nrepeat: none\n";
        for (const AtlasRegion& r : atlas.regions) {
            if (r.page != (int)p) continue;
            o << r.name << "\n";
            o << "  rotate: " << (r.rotated ? "true" : "false") << "\n";
            o << "  xy: " << r.x << ", " << r.y << "\n";
            o << "  size: " << (r.rotated ? r.height : r.width) << ", " << (r.rotated ? r.width : r.height) << "\n";
            o << "  orig: " << r.originalWidth << ", " << r.originalHeight << "\n";
            // Offset from the bottom-left of the original image, the usual atlas convention.
            int th = r.rotated ? r.width : r.height;
            o << "  offset: " << r.offsetX << ", " << (r.originalHeight - r.offsetY - th) << "\n";
            o << "  index: -1\n";
        }
        o << "\n";
    }
    return o.str();
}

} // namespace rig
