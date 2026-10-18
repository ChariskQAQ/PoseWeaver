#include "core/clipping.h"
#include "core/mesh_tools.h"

namespace rig {

ClipTriangles clipTrianglesOf(const std::vector<Vec2>& polygon) {
    ClipTriangles out;
    if (polygon.size() < 3) return out;
    std::vector<int> tris;
    if (!triangulate(polygon, (int)polygon.size(), tris)) {
        // Self-intersecting outline: clip with its convex hull instead.
        std::vector<int> hull = convexHull(polygon);
        std::vector<Vec2> pts;
        for (int i : hull) pts.push_back(polygon[i]);
        if (!triangulate(pts, (int)pts.size(), tris)) return out;
        for (size_t t = 0; t + 2 < tris.size(); t += 3) out.push_back({pts[tris[t]], pts[tris[t + 1]], pts[tris[t + 2]]});
        return out;
    }
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        std::array<Vec2, 3> tri = {polygon[tris[t]], polygon[tris[t + 1]], polygon[tris[t + 2]]};
        if (cross(tri[1] - tri[0], tri[2] - tri[0]) < 0) std::swap(tri[1], tri[2]);
        out.push_back(tri);
    }
    return out;
}

namespace {
struct ClipVertex {
    Vec2 pos, uv;
};

// Sutherland-Hodgman against one counter-clockwise convex triangle.
std::vector<ClipVertex> clipConvex(std::vector<ClipVertex> poly, const std::array<Vec2, 3>& tri) {
    for (int e = 0; e < 3 && !poly.empty(); ++e) {
        Vec2 a = tri[e], b = tri[(e + 1) % 3];
        auto side = [&](Vec2 p) { return cross(b - a, p - a); };  // >= 0 inside
        std::vector<ClipVertex> out;
        for (size_t i = 0; i < poly.size(); ++i) {
            const ClipVertex& cur = poly[i];
            const ClipVertex& nxt = poly[(i + 1) % poly.size()];
            float sc = side(cur.pos), sn = side(nxt.pos);
            if (sc >= 0) out.push_back(cur);
            if ((sc >= 0) != (sn >= 0)) {
                float t = sc / (sc - sn);
                out.push_back({lerp(cur.pos, nxt.pos, t), lerp(cur.uv, nxt.uv, t)});
            }
        }
        poly = std::move(out);
    }
    return poly;
}
} // namespace

void clipGeometry(const Geometry& in, const ClipTriangles& clip, Geometry& out) {
    out.positions.clear();
    out.uvs.clear();
    out.triangles.clear();
    bool hasUv = in.uvs.size() == in.positions.size();
    for (size_t t = 0; t + 2 < in.triangles.size(); t += 3) {
        std::vector<ClipVertex> tri;
        for (int k = 0; k < 3; ++k) {
            int i = in.triangles[t + k];
            tri.push_back({in.positions[i], hasUv ? in.uvs[i] : Vec2{}});
        }
        if (cross(tri[1].pos - tri[0].pos, tri[2].pos - tri[0].pos) < 0) std::swap(tri[1], tri[2]);
        for (const auto& c : clip) {
            std::vector<ClipVertex> piece = clipConvex(tri, c);
            if (piece.size() < 3) continue;
            int base = (int)out.positions.size();
            for (auto& v : piece) {
                out.positions.push_back(v.pos);
                out.uvs.push_back(v.uv);
            }
            for (int k = 1; k + 1 < (int)piece.size(); ++k) out.triangles.insert(out.triangles.end(), {base, base + k, base + k + 1});
        }
    }
}

} // namespace rig
