// Mesh topology: outline tracing, constrained Delaunay triangulation and vertex add/remove.
#include "core/mesh_tools.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <type_traits>

namespace rig {

// ---------------------------------------------------------------- geometry helpers

static float signedArea(const std::vector<Vec2>& pts, const std::vector<int>& poly) {
    float a = 0;
    for (size_t i = 0; i < poly.size(); ++i) a += cross(pts[poly[i]], pts[poly[(i + 1) % poly.size()]]);
    return a * 0.5f;
}

bool pointInPolygon(Vec2 p, const std::vector<Vec2>& polygon) {
    bool inside = false;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const Vec2 &a = polygon[i], &b = polygon[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
    }
    return inside;
}

std::vector<int> convexHull(const std::vector<Vec2>& points) {
    std::vector<int> idx(points.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = (int)i;
    std::sort(idx.begin(), idx.end(), [&](int a, int b) {
        return points[a].x < points[b].x || (points[a].x == points[b].x && points[a].y < points[b].y);
    });
    if (idx.size() < 3) return idx;
    std::vector<int> hull(2 * idx.size());
    size_t k = 0;
    auto turn = [&](int o, int a, int b) { return cross(points[a] - points[o], points[b] - points[o]); };
    for (int i : idx) {  // lower
        while (k >= 2 && turn(hull[k - 2], hull[k - 1], i) <= 0) --k;
        hull[k++] = i;
    }
    for (size_t t = idx.size() - 1, lower = k + 1; t-- > 0;) {  // upper
        int i = idx[t];
        while (k >= lower && turn(hull[k - 2], hull[k - 1], i) <= 0) --k;
        hull[k++] = i;
    }
    hull.resize(k - 1);
    return hull;
}

// ---------------------------------------------------------------- triangulation

static bool inCircumcircle(Vec2 a, Vec2 b, Vec2 c, Vec2 p) {
    // a, b, c counter-clockwise.
    float ax = a.x - p.x, ay = a.y - p.y, bx = b.x - p.x, by = b.y - p.y, cx = c.x - p.x, cy = c.y - p.y;
    double det = (double)(ax * ax + ay * ay) * (bx * cy - cx * by) - (double)(bx * bx + by * by) * (ax * cy - cx * ay) +
                 (double)(cx * cx + cy * cy) * (ax * by - bx * ay);
    return det > 1e-9;
}

static bool earClip(const std::vector<Vec2>& pts, std::vector<int> poly, std::vector<std::array<int, 3>>& tris) {
    auto isConvex = [&](int a, int b, int c) { return cross(pts[b] - pts[a], pts[c] - pts[b]) > 1e-9f; };
    auto contains = [&](int a, int b, int c, int p) {
        Vec2 P = pts[p];
        float d1 = cross(pts[b] - pts[a], P - pts[a]), d2 = cross(pts[c] - pts[b], P - pts[b]), d3 = cross(pts[a] - pts[c], P - pts[c]);
        return d1 >= -1e-9f && d2 >= -1e-9f && d3 >= -1e-9f;
    };
    size_t guard = poly.size() * poly.size() + 10;
    while (poly.size() > 3 && guard-- > 0) {
        bool clipped = false;
        for (size_t i = 0; i < poly.size(); ++i) {
            int a = poly[(i + poly.size() - 1) % poly.size()], b = poly[i], c = poly[(i + 1) % poly.size()];
            if (!isConvex(a, b, c)) continue;
            bool empty = true;
            for (int p : poly)
                if (p != a && p != b && p != c && contains(a, b, c, p)) {
                    empty = false;
                    break;
                }
            if (!empty) continue;
            tris.push_back({a, b, c});
            poly.erase(poly.begin() + i);
            clipped = true;
            break;
        }
        if (!clipped) return false;  // self-intersecting or degenerate outline
    }
    if (poly.size() == 3) tris.push_back({poly[0], poly[1], poly[2]});
    return true;
}

static bool segmentsCross(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
    float d1 = cross(b - a, c - a), d2 = cross(b - a, d - a), d3 = cross(d - c, a - c), d4 = cross(d - c, b - c);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0)) && d1 != 0 && d2 != 0 && d3 != 0 && d4 != 0;
}

bool triangulate(const std::vector<Vec2>& pts, int hullLength, std::vector<int>& out) {
    if (hullLength < 3 || hullLength > (int)pts.size()) return false;
    // Reject self-intersecting outlines (non-adjacent edges crossing).
    for (int i = 0; i < hullLength; ++i)
        for (int j = i + 2; j < hullLength; ++j) {
            if (i == 0 && j == hullLength - 1) continue;
            if (segmentsCross(pts[i], pts[(i + 1) % hullLength], pts[j], pts[(j + 1) % hullLength])) return false;
        }
    std::vector<int> poly(hullLength);
    for (int i = 0; i < hullLength; ++i) poly[i] = i;
    if (signedArea(pts, poly) < 0) std::reverse(poly.begin(), poly.end());
    std::vector<std::array<int, 3>> tris;
    if (!earClip(pts, poly, tris)) return false;

    std::set<std::pair<int, int>> constrained;
    for (int i = 0; i < hullLength; ++i) {
        int a = i, b = (i + 1) % hullLength;
        constrained.insert({std::min(a, b), std::max(a, b)});
    }
    auto ccw = [&](std::array<int, 3>& t) {
        if (cross(pts[t[1]] - pts[t[0]], pts[t[2]] - pts[t[0]]) < 0) std::swap(t[1], t[2]);
    };

    // Insert interior points by splitting the triangle (or edge) that contains them.
    std::vector<Vec2> hullPts;
    for (int i : poly) hullPts.push_back(pts[i]);
    for (int p = hullLength; p < (int)pts.size(); ++p) {
        if (!pointInPolygon(pts[p], hullPts)) continue;
        for (size_t t = 0; t < tris.size(); ++t) {
            auto [a, b, c] = tris[t];
            Vec2 P = pts[p];
            float d = cross(pts[b] - pts[a], pts[c] - pts[a]);
            if (std::fabs(d) < 1e-12f) continue;
            float wb = cross(P - pts[a], pts[c] - pts[a]) / d, wc = cross(pts[b] - pts[a], P - pts[a]) / d, wa = 1 - wb - wc;
            const float eps = -1e-6f, onEdge = 1e-5f;
            if (wa < eps || wb < eps || wc < eps) continue;
            // On an edge: split this triangle and its neighbour across that edge in two each.
            int u = -1, v = -1, x = -1;
            if (std::fabs(wa) < onEdge) u = b, v = c, x = a;
            else if (std::fabs(wb) < onEdge) u = c, v = a, x = b;
            else if (std::fabs(wc) < onEdge) u = a, v = b, x = c;
            if (u < 0) {
                tris[t] = {a, b, p};
                tris.push_back({b, c, p});
                tris.push_back({c, a, p});
                break;
            }
            tris[t] = {u, p, x};
            tris.push_back({p, v, x});
            for (size_t o = 0; o < tris.size(); ++o) {
                if (o == t || o == tris.size() - 1) continue;
                auto& n = tris[o];
                bool hasU = n[0] == u || n[1] == u || n[2] == u, hasV = n[0] == v || n[1] == v || n[2] == v;
                if (!hasU || !hasV) continue;
                int y = n[0] + n[1] + n[2] - u - v;
                n = {u, p, y};
                tris.push_back({p, v, y});
                break;
            }
            break;
        }
    }
    for (auto& t : tris) ccw(t);

    // Lawson flips until every non-constrained edge is locally Delaunay.
    for (int pass = 0; pass < 200; ++pass) {
        std::map<std::pair<int, int>, std::vector<int>> edgeTris;
        for (size_t t = 0; t < tris.size(); ++t)
            for (int e = 0; e < 3; ++e) {
                int a = tris[t][e], b = tris[t][(e + 1) % 3];
                edgeTris[{std::min(a, b), std::max(a, b)}].push_back((int)t);
            }
        bool flipped = false;
        for (auto& [edge, ts] : edgeTris) {
            if (ts.size() != 2 || constrained.count(edge)) continue;
            auto& t0 = tris[ts[0]];
            auto& t1 = tris[ts[1]];
            int a = edge.first, b = edge.second;
            int c = t0[0] + t0[1] + t0[2] - a - b, d = t1[0] + t1[1] + t1[2] - a - b;
            // Triangle t0 = (a, b, c) in some ccw rotation; check d against its circumcircle.
            std::array<int, 3> tc = t0;
            if (!inCircumcircle(pts[tc[0]], pts[tc[1]], pts[tc[2]], pts[d])) continue;
            // Flip only if the quad a-c-b-d is convex (c and d on opposite sides of a-b, a and b of c-d).
            float s1 = cross(pts[d] - pts[c], pts[a] - pts[c]), s2 = cross(pts[d] - pts[c], pts[b] - pts[c]);
            if (s1 * s2 >= 0) continue;
            t0 = {c, d, a};
            t1 = {d, c, b};
            ccw(t0);
            ccw(t1);
            flipped = true;
            break;  // edge map is stale now
        }
        if (!flipped) break;
    }
    out.clear();
    for (auto& t : tris) {
        if (std::fabs(cross(pts[t[1]] - pts[t[0]], pts[t[2]] - pts[t[0]])) < 1e-9f) continue;  // drop slivers
        out.insert(out.end(), {t[0], t[1], t[2]});
    }
    return !out.empty();
}

// ---------------------------------------------------------------- outline tracing

static void simplify(const std::vector<Vec2>& pts, int i0, int i1, float tol, std::vector<bool>& keep) {
    if (i1 <= i0 + 1) return;
    float best = -1;
    int idx = -1;
    for (int i = i0 + 1; i < i1; ++i) {
        float d = distToSegment(pts[i], pts[i0], pts[i1]);
        if (d > best) {
            best = d;
            idx = i;
        }
    }
    if (best > tol) {
        keep[idx] = true;
        simplify(pts, i0, idx, tol, keep);
        simplify(pts, idx, i1, tol, keep);
    }
}

std::vector<Vec2> traceAlphaOutline(const unsigned char* rgba, int w, int h, unsigned char threshold, float tolerance,
                                    float padding) {
    if (!rgba || w <= 0 || h <= 0) return {};
    const int cell = std::max(1, (int)std::ceil(std::max(w, h) / 128.0f));
    const int pad = std::max(0, (int)std::round(padding / cell));
    const int border = pad + 1;
    const int cw = (w + cell - 1) / cell, ch = (h + cell - 1) / cell;
    const int gw = cw + 2 * border, gh = ch + 2 * border;
    std::vector<unsigned char> grid(size_t(gw) * gh, 0);
    auto at = [&](int x, int y) -> unsigned char& { return grid[size_t(y) * gw + x]; };
    for (int cy = 0; cy < ch; ++cy)
        for (int cx = 0; cx < cw; ++cx) {
            bool opaque = false;
            for (int y = cy * cell; y < std::min(h, (cy + 1) * cell) && !opaque; ++y)
                for (int x = cx * cell; x < std::min(w, (cx + 1) * cell); ++x)
                    if (rgba[(size_t(y) * w + x) * 4 + 3] > threshold) {
                        opaque = true;
                        break;
                    }
            at(cx + border, cy + border) = opaque;
        }
    if (pad > 0) {  // square dilation
        std::vector<unsigned char> src = grid;
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x) {
                if (!src[size_t(y) * gw + x]) continue;
                for (int dy = -pad; dy <= pad; ++dy)
                    for (int dx = -pad; dx <= pad; ++dx) {
                        int nx = x + dx, ny = y + dy;
                        if (nx > 0 && ny > 0 && nx < gw - 1 && ny < gh - 1) at(nx, ny) = 1;
                    }
            }
    }
    // Keep the largest 8-connected component.
    std::vector<int> label(grid.size(), 0);
    int best = 0, bestSize = 0, next = 0;
    for (int y = 0; y < gh; ++y)
        for (int x = 0; x < gw; ++x) {
            if (!at(x, y) || label[size_t(y) * gw + x]) continue;
            ++next;
            int size = 0;
            std::vector<std::pair<int, int>> stack{{x, y}};
            label[size_t(y) * gw + x] = next;
            while (!stack.empty()) {
                auto [px, py] = stack.back();
                stack.pop_back();
                ++size;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        int nx = px + dx, ny = py + dy;
                        if (nx < 0 || ny < 0 || nx >= gw || ny >= gh) continue;
                        size_t k = size_t(ny) * gw + nx;
                        if (grid[k] && !label[k]) {
                            label[k] = next;
                            stack.push_back({nx, ny});
                        }
                    }
            }
            if (size > bestSize) {
                bestSize = size;
                best = next;
            }
        }
    if (!best) return {};
    auto filled = [&](int x, int y) { return x >= 0 && y >= 0 && x < gw && y < gh && label[size_t(y) * gw + x] == best; };

    // Moore-neighbour contour tracing (clockwise in grid space).
    static const int dx[8] = {-1, -1, 0, 1, 1, 1, 0, -1};  // W, NW, N, NE, E, SE, S, SW
    static const int dy[8] = {0, -1, -1, -1, 0, 1, 1, 1};
    int sx = -1, sy = -1;
    for (int y = 0; y < gh && sx < 0; ++y)
        for (int x = 0; x < gw; ++x)
            if (filled(x, y)) {
                sx = x;
                sy = y;
                break;
            }
    std::vector<std::pair<int, int>> contour{{sx, sy}};
    int px = sx, py = sy, back = 0;  // the cell west of the start is empty
    for (int guard = 0; guard < gw * gh * 4; ++guard) {
        int found = -1;
        for (int k = 1; k <= 8; ++k) {
            int d = (back + k) % 8;
            if (filled(px + dx[d], py + dy[d])) {
                found = d;
                break;
            }
        }
        if (found < 0) break;  // isolated cell
        int prevDir = (found + 7) % 8;  // the empty neighbour checked just before
        int bx = px + dx[prevDir], by = py + dy[prevDir];
        px += dx[found];
        py += dy[found];
        // Direction of the backtrack cell as seen from the new current cell.
        for (int d = 0; d < 8; ++d)
            if (px + dx[d] == bx && py + dy[d] == by) back = d;
        if (px == sx && py == sy) break;
        contour.push_back({px, py});
    }
    std::vector<Vec2> pts;
    for (auto [x, y] : contour) {
        float fx = std::clamp((x - border + 0.5f) * cell, 0.0f, (float)w);
        float fy = std::clamp((y - border + 0.5f) * cell, 0.0f, (float)h);
        pts.push_back({fx, fy});
    }
    if (pts.size() < 3) {
        // A single cell: use its square.
        Vec2 c = pts.empty() ? Vec2{w * 0.5f, h * 0.5f} : pts[0];
        float r = cell * 0.5f + padding;
        return {{c.x - r, c.y - r}, {c.x + r, c.y - r}, {c.x + r, c.y + r}, {c.x - r, c.y + r}};
    }
    // Douglas-Peucker on the closed loop, split at the point farthest from the first one.
    int far = 0;
    float farD = -1;
    for (size_t i = 1; i < pts.size(); ++i) {
        float d = length(pts[i] - pts[0]);
        if (d > farD) {
            farD = d;
            far = (int)i;
        }
    }
    std::vector<Vec2> loop = pts;
    loop.push_back(pts[0]);
    std::vector<bool> keep(loop.size(), false);
    keep[0] = keep[far] = keep[loop.size() - 1] = true;
    float tol = std::max(tolerance, cell * 0.75f);
    simplify(loop, 0, far, tol, keep);
    simplify(loop, far, (int)loop.size() - 1, tol, keep);
    std::vector<Vec2> out;
    for (size_t i = 0; i + 1 < loop.size(); ++i)
        if (keep[i]) out.push_back(loop[i]);
    if (out.size() < 3) return {};
    return out;
}

// ---------------------------------------------------------------- image mapping

std::vector<Vec2> setupLocalVertices(const Skeleton& sk, const Slot& slot, const Attachment& att) {
    Pose setup = makeSetupPose(sk);
    std::vector<Vec2> world = computeVertices(sk, setup, slot, att);
    Affine inv = setup.world[slot.bone].inverse();
    for (auto& p : world) p = inv.apply(p);
    return world;
}

Affine imageToLocal(const Skeleton& sk, const Slot& slot, const Attachment& att, const Image& img) {
    const float w = (float)img.width, h = (float)img.height;
    // pixel -> image-centred, y-up coordinates
    Affine centre{1, 0, 0, -1, -w / 2, h / 2};
    if (att.type == AttachmentType::Region) return att.transform.matrix() * centre;
    // Least-squares fit of local = M * pixel over all vertices.
    std::vector<Vec2> local = setupLocalVertices(sk, slot, att);
    size_t n = std::min(local.size(), att.uvs.size());
    double sxx = 0, sxy = 0, sx = 0, syy = 0, sy = 0, sn = (double)n;
    double bx[3] = {0, 0, 0}, by[3] = {0, 0, 0};
    for (size_t i = 0; i < n; ++i) {
        double x = att.uvs[i].x * w, y = att.uvs[i].y * h;
        sxx += x * x;
        sxy += x * y;
        sx += x;
        syy += y * y;
        sy += y;
        double r[3] = {x, y, 1};
        for (int k = 0; k < 3; ++k) {
            bx[k] += r[k] * local[i].x;
            by[k] += r[k] * local[i].y;
        }
    }
    double m[3][3] = {{sxx, sxy, sx}, {sxy, syy, sy}, {sx, sy, sn}};
    double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                 m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (n < 3 || std::fabs(det) < 1e-6) return centre;
    auto solve = [&](const double b[3], double out[3]) {  // Cramer's rule
        for (int c = 0; c < 3; ++c) {
            double t[3][3];
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k) t[r][k] = k == c ? b[r] : m[r][k];
            out[c] = (t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) - t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0]) +
                      t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0])) /
                     det;
        }
    };
    double rx[3], ry[3];
    solve(bx, rx);
    solve(by, ry);
    return {(float)rx[0], (float)rx[1], (float)ry[0], (float)ry[1], (float)rx[2], (float)ry[2]};
}

bool buildMeshFromOutline(const Skeleton& sk, const Slot& slot, Attachment& att, const Image& img,
                          const std::vector<Vec2>& outlinePx, float spacing) {
    if (outlinePx.size() < 3 || img.width <= 0 || img.height <= 0) return false;
    Affine map = imageToLocal(sk, slot, att, img);
    // Subdivide long outline edges so the hull is about as dense as the interior grid.
    std::vector<Vec2> px;
    for (size_t i = 0; i < outlinePx.size(); ++i) {
        Vec2 p0 = outlinePx[i], p1 = outlinePx[(i + 1) % outlinePx.size()];
        px.push_back(p0);
        int extra = spacing > 0 ? (int)(length(p1 - p0) / spacing) : 0;
        for (int k = 1; k <= extra; ++k) px.push_back(lerp(p0, p1, (float)k / (extra + 1)));
    }
    int hull = (int)px.size();
    if (spacing > 0) {
        for (float y = spacing * 0.5f; y < img.height; y += spacing)
            for (float x = spacing * 0.5f; x < img.width; x += spacing) {
                Vec2 p{x, y};
                if (!pointInPolygon(p, outlinePx)) continue;
                bool nearEdge = false;
                for (size_t i = 0; i < outlinePx.size() && !nearEdge; ++i)
                    nearEdge = distToSegment(p, outlinePx[i], outlinePx[(i + 1) % outlinePx.size()]) < spacing * 0.45f;
                if (!nearEdge) px.push_back(p);
            }
    }
    std::vector<int> tris;
    if (!triangulate(px, hull, tris)) {
        // Self-intersecting outline: fall back to its convex hull.
        std::vector<Vec2> outline(px.begin(), px.begin() + hull);
        std::vector<int> ch = convexHull(outline);
        std::vector<Vec2> fixed;
        for (int i : ch) fixed.push_back(outline[i]);
        fixed.insert(fixed.end(), px.begin() + hull, px.end());
        hull = (int)ch.size();
        px = fixed;
        if (!triangulate(px, hull, tris)) return false;
    }
    att.type = AttachmentType::Mesh;
    att.uvs.clear();
    att.vertices.clear();
    att.weights.clear();
    for (Vec2 p : px) {
        att.uvs.push_back({p.x / img.width, p.y / img.height});
        att.vertices.push_back(map.apply(p));
    }
    att.triangles = tris;
    att.hullLength = hull;
    att.transform = Transform{};
    return true;
}

// ---------------------------------------------------------------- topology edits

// Applies a vertex permutation / removal (newIndex[old] = new or -1) to every per-vertex array.
static void remapVertices(Attachment& att, const std::vector<int>& newIndex, int newCount) {
    auto remap = [&](auto& arr) {
        if (arr.empty()) return;
        std::decay_t<decltype(arr)> out(newCount);
        for (size_t i = 0; i < arr.size() && i < newIndex.size(); ++i)
            if (newIndex[i] >= 0) out[newIndex[i]] = arr[i];
        arr = std::move(out);
    };
    remap(att.uvs);
    remap(att.vertices);
    remap(att.weights);
    std::vector<int> tris;
    for (size_t t = 0; t + 2 < att.triangles.size(); t += 3) {
        int a = newIndex[att.triangles[t]], b = newIndex[att.triangles[t + 1]], c = newIndex[att.triangles[t + 2]];
        if (a >= 0 && b >= 0 && c >= 0) tris.insert(tris.end(), {a, b, c});
    }
    att.triangles = std::move(tris);
}

bool ensureHull(Attachment& att) {
    if (att.isPolygon()) {
        att.hullLength = (int)att.vertexCount();
        return true;
    }
    if (att.hullLength >= 3) return true;
    std::map<std::pair<int, int>, int> count;
    for (size_t t = 0; t + 2 < att.triangles.size(); t += 3)
        for (int e = 0; e < 3; ++e) {
            int a = att.triangles[t + e], b = att.triangles[t + (e + 1) % 3];
            count[{std::min(a, b), std::max(a, b)}]++;
        }
    std::map<int, std::vector<int>> adj;
    int boundaryEdges = 0;
    for (auto& [e, c] : count)
        if (c == 1) {
            adj[e.first].push_back(e.second);
            adj[e.second].push_back(e.first);
            ++boundaryEdges;
        }
    if (boundaryEdges < 3) return false;
    for (auto& [v, n] : adj)
        if (n.size() != 2) return false;
    std::vector<int> loop{adj.begin()->first};
    int prev = -1, cur = loop[0];
    while (true) {
        int nxt = adj[cur][0] == prev ? adj[cur][1] : adj[cur][0];
        if (nxt == loop[0]) break;
        loop.push_back(nxt);
        prev = cur;
        cur = nxt;
        if ((int)loop.size() > boundaryEdges) return false;
    }
    if ((int)loop.size() != boundaryEdges) return false;  // several loops (holes or islands)
    int n = (int)att.vertexCount();
    std::vector<int> newIndex(n, -1);
    int next = 0;
    for (int v : loop) newIndex[v] = next++;
    for (int v = 0; v < n; ++v)
        if (newIndex[v] < 0) newIndex[v] = next++;
    remapVertices(att, newIndex, n);
    att.hullLength = (int)loop.size();
    return true;
}

bool retriangulate(const Skeleton& sk, const Slot& slot, Attachment& att) {
    if (att.type != AttachmentType::Mesh || att.hullLength < 3) return false;
    std::vector<int> tris;
    if (!triangulate(setupLocalVertices(sk, slot, att), att.hullLength, tris)) return false;
    att.triangles = std::move(tris);
    return true;
}

int addMeshVertex(const Skeleton& sk, const Slot& slot, Attachment& att, const Image* img, Vec2 setupLocal, int hullAfter) {
    if (!att.hasVertices()) return -1;
    if (!att.isPolygon() && att.hullLength < 3 && !ensureHull(att)) return -1;
    if (att.isPolygon()) att.hullLength = (int)att.vertexCount();
    std::vector<Vec2> local = setupLocalVertices(sk, slot, att);
    int n = (int)att.vertexCount();

    // uv: interpolate within the containing triangle, otherwise use the image mapping.
    Vec2 uv;
    if (!att.isPolygon() && img) {
        Affine toPx = imageToLocal(sk, slot, att, *img).inverse();
        Vec2 p = toPx.apply(setupLocal);
        uv = {p.x / img->width, p.y / img->height};
    }

    // Weights: blend the containing triangle's (or nearest vertex's) influences.
    std::vector<VertexWeight> weights;
    if (att.isWeighted()) {
        Pose setup = makeSetupPose(sk);
        Vec2 world = setup.world[slot.bone].apply(setupLocal);
        std::map<int, float> blend;
        bool found = false;
        for (size_t t = 0; t + 2 < att.triangles.size() && !found; t += 3) {
            int i0 = att.triangles[t], i1 = att.triangles[t + 1], i2 = att.triangles[t + 2];
            float d = cross(local[i1] - local[i0], local[i2] - local[i0]);
            if (std::fabs(d) < 1e-9f) continue;
            float b1 = cross(setupLocal - local[i0], local[i2] - local[i0]) / d;
            float b2 = cross(local[i1] - local[i0], setupLocal - local[i0]) / d;
            float b0 = 1 - b1 - b2;
            if (b0 < -1e-4f || b1 < -1e-4f || b2 < -1e-4f) continue;
            for (auto [vi, bw] : {std::pair{i0, b0}, std::pair{i1, b1}, std::pair{i2, b2}})
                for (auto& w : att.weights[vi]) blend[w.bone] += w.weight * bw;
            found = true;
        }
        if (!found) {
            int nearest = 0;
            for (int v = 1; v < n; ++v)
                if (length(local[v] - setupLocal) < length(local[nearest] - setupLocal)) nearest = v;
            for (auto& w : att.weights[nearest]) blend[w.bone] += w.weight;
        }
        float total = 0;
        for (auto& [b, w] : blend) total += std::max(0.0f, w);
        for (auto& [b, w] : blend) {
            if (w <= 1e-4f) continue;
            VertexWeight vw;
            vw.bone = b;
            vw.weight = total > 0 ? w / total : 1;
            vw.local = setup.world[b].inverse().apply(world);
            weights.push_back(vw);
        }
    }

    // New vertex index: inserted into the hull after `hullAfter`, else appended.
    int index = n;
    if (hullAfter >= 0 && hullAfter < att.hullLength) index = hullAfter + 1;
    std::vector<int> newIndex(n);
    for (int v = 0; v < n; ++v) newIndex[v] = v < index ? v : v + 1;
    remapVertices(att, newIndex, n + 1);
    if (!att.isPolygon()) att.uvs[index] = uv;
    if (att.isWeighted()) att.weights[index] = weights;
    else att.vertices[index] = setupLocal;
    if (hullAfter >= 0 || att.isPolygon()) att.hullLength++;
    if (!att.isPolygon() && !retriangulate(sk, slot, att)) return index;  // keep the vertex, old triangles
    return index;
}

bool removeMeshVertices(const Skeleton& sk, const Slot& slot, Attachment& att, std::vector<int> vertices) {
    if (!att.hasVertices()) return false;
    if (!att.isPolygon() && att.hullLength < 3 && !ensureHull(att)) return false;
    if (att.isPolygon()) att.hullLength = (int)att.vertexCount();
    int n = (int)att.vertexCount();
    std::sort(vertices.begin(), vertices.end());
    vertices.erase(std::unique(vertices.begin(), vertices.end()), vertices.end());
    int hullRemoved = 0;
    for (int v : vertices) hullRemoved += v >= 0 && v < att.hullLength;
    if (att.hullLength - hullRemoved < 3) {
        // Keep the outline valid: only drop the interior ones.
        vertices.erase(std::remove_if(vertices.begin(), vertices.end(), [&](int v) { return v < att.hullLength; }),
                       vertices.end());
        hullRemoved = 0;
    }
    if (vertices.empty()) return false;
    std::vector<int> newIndex(n);
    int next = 0;
    for (int v = 0; v < n; ++v) newIndex[v] = std::binary_search(vertices.begin(), vertices.end(), v) ? -1 : next++;
    remapVertices(att, newIndex, next);
    att.hullLength -= hullRemoved;
    if (!att.isPolygon()) retriangulate(sk, slot, att);
    return true;
}

void setVertexUV(Attachment& att, int vertex, Vec2 setupLocal, const Affine& imageToLocalMap, const Image& img) {
    if (att.type != AttachmentType::Mesh || vertex < 0 || vertex >= (int)att.uvs.size() || img.width <= 0) return;
    Vec2 p = imageToLocalMap.inverse().apply(setupLocal);
    att.uvs[vertex] = {p.x / img.width, p.y / img.height};
}

void makePolygon(Attachment& att, const std::vector<Vec2>& points) {
    att.uvs.clear();
    att.triangles.clear();
    att.weights.clear();
    att.vertices = points;
    att.hullLength = (int)points.size();
}

} // namespace rig
