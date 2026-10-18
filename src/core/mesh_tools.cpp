#include "core/mesh_tools.h"

#include <algorithm>
#include <map>

namespace rig {

static bool cellOpaque(const unsigned char* rgba, int w, int h, int x0, int y0, int x1, int y1,
                       unsigned char threshold) {
    x0 = std::max(0, x0 - 1);
    y0 = std::max(0, y0 - 1);
    x1 = std::min(w, x1 + 1);
    y1 = std::min(h, y1 + 1);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            if (rgba[(size_t(y) * w + x) * 4 + 3] > threshold) return true;
    return false;
}

bool convertRegionToMesh(Attachment& att, const Image& img, const unsigned char* rgba, int cols,
                         int rows, unsigned char alphaThreshold) {
    if (img.width <= 0 || img.height <= 0) return false;
    cols = std::max(1, cols);
    rows = std::max(1, rows);
    const float w = (float)img.width, h = (float)img.height;
    const Affine m = att.transform.matrix();

    // Grid vertex (i, j) -> index; -1 until used by a kept cell.
    std::vector<int> index((cols + 1) * (rows + 1), -1);
    std::vector<Vec2> uvs, verts;
    std::vector<int> tris;
    auto vertex = [&](int i, int j) {
        int& idx = index[j * (cols + 1) + i];
        if (idx < 0) {
            float u = (float)i / cols, v = (float)j / rows;
            idx = (int)uvs.size();
            uvs.push_back({u, v});
            verts.push_back(m.apply({u * w - w / 2, h / 2 - v * h}));
        }
        return idx;
    };
    for (int j = 0; j < rows; ++j) {
        for (int i = 0; i < cols; ++i) {
            if (rgba) {
                int x0 = (int)std::floor(w * i / cols), x1 = (int)std::ceil(w * (i + 1) / cols);
                int y0 = (int)std::floor(h * j / rows), y1 = (int)std::ceil(h * (j + 1) / rows);
                if (!cellOpaque(rgba, img.width, img.height, x0, y0, x1, y1, alphaThreshold)) continue;
            }
            int a = vertex(i, j), b = vertex(i + 1, j), c = vertex(i + 1, j + 1), d = vertex(i, j + 1);
            tris.insert(tris.end(), {a, b, c, a, c, d});
        }
    }
    if (tris.empty()) return false;
    att.type = AttachmentType::Mesh;
    att.uvs = std::move(uvs);
    att.vertices = std::move(verts);
    att.triangles = std::move(tris);
    att.weights.clear();
    att.transform = Transform{};
    return true;
}

void bindMesh(const Skeleton& sk, const Slot& slot, Attachment& att, const std::vector<int>& bones,
              int maxInfluences) {
    if (!att.hasVertices() || bones.empty()) return;
    Pose setup = makeSetupPose(sk);
    std::vector<Vec2> world = computeVertices(sk, setup, slot, att);
    std::vector<std::vector<VertexWeight>> weights(world.size());
    for (size_t v = 0; v < world.size(); ++v) {
        std::vector<VertexWeight> list;
        for (int b : bones) {
            Vec2 a = setup.world[b].origin();
            Vec2 tip = boneTip(sk.bones[b], setup.world[b]);
            float d = distToSegment(world[v], a, tip);
            float f = 1.0f / (1.0f + d * 0.05f);
            VertexWeight w;
            w.bone = b;
            w.weight = f * f * f * f;
            list.push_back(w);
        }
        std::sort(list.begin(), list.end(),
                  [](const VertexWeight& x, const VertexWeight& y) { return x.weight > y.weight; });
        if ((int)list.size() > maxInfluences) list.resize(maxInfluences);
        float total = 0;
        for (auto& w : list) total += w.weight;
        for (auto& w : list) {
            w.weight /= total;
            w.local = setup.world[w.bone].inverse().apply(world[v]);
        }
        list.erase(std::remove_if(list.begin() + 1, list.end(),
                                  [](const VertexWeight& w) { return w.weight < 0.01f; }),
                   list.end());
        total = 0;
        for (auto& w : list) total += w.weight;
        for (auto& w : list) w.weight /= total;
        weights[v] = std::move(list);
    }
    att.weights = std::move(weights);
    att.vertices.clear();
}

void unbindMesh(const Skeleton& sk, const Slot& slot, Attachment& att) {
    if (!att.isWeighted()) return;
    Pose setup = makeSetupPose(sk);
    std::vector<Vec2> world = computeVertices(sk, setup, slot, att);
    Affine inv = setup.world[slot.bone].inverse();
    att.vertices.resize(world.size());
    for (size_t v = 0; v < world.size(); ++v) att.vertices[v] = inv.apply(world[v]);
    att.weights.clear();
}

float vertexWeight(const Attachment& att, int vertex, int bone) {
    if (!att.isWeighted() || vertex < 0 || vertex >= (int)att.weights.size()) return 0;
    for (auto& w : att.weights[vertex])
        if (w.bone == bone) return w.weight;
    return 0;
}

static bool isLocked(const std::vector<int>& locked, int bone) {
    return std::find(locked.begin(), locked.end(), bone) != locked.end();
}

// Adds `bone` to the vertex's influence list (weight 0) if missing; returns its position.
static std::vector<VertexWeight>::iterator influence(const Skeleton& sk, const Slot& slot, Attachment& att, int vertex,
                                                     int bone) {
    auto& list = att.weights[vertex];
    auto it = std::find_if(list.begin(), list.end(), [&](const VertexWeight& w) { return w.bone == bone; });
    if (it != list.end()) return it;
    Pose setup = makeSetupPose(sk);
    Vec2 world = computeVertices(sk, setup, slot, att)[vertex];
    VertexWeight w;
    w.bone = bone;
    w.local = setup.world[bone].inverse().apply(world);
    w.weight = 0;
    list.push_back(w);
    return list.end() - 1;
}

void setWeight(const Skeleton& sk, const Slot& slot, Attachment& att, int vertex, int bone, float value,
               const std::vector<int>& locked) {
    if (!att.isWeighted() || vertex < 0 || vertex >= (int)att.weights.size() || isLocked(locked, bone)) return;
    auto& list = att.weights[vertex];
    if (value > 0) influence(sk, slot, att, vertex, bone);
    float lockedSum = 0, others = 0;
    for (auto& w : list) {
        if (w.bone == bone) continue;
        if (isLocked(locked, w.bone)) lockedSum += w.weight;
        else others += w.weight;
    }
    float available = std::max(0.0f, 1.0f - lockedSum);
    float target = std::clamp(value, 0.0f, available);
    if (others <= 1e-6f) target = available;  // nothing to take weight from / give weight to
    float scale = others > 1e-6f ? (available - target) / others : 0.0f;
    for (auto& w : list) {
        if (w.bone == bone) w.weight = target;
        else if (!isLocked(locked, w.bone)) w.weight *= scale;
    }
    list.erase(std::remove_if(list.begin(), list.end(), [](const VertexWeight& w) { return w.weight < 1e-4f; }), list.end());
    if (list.empty()) {  // never leave a vertex without influence
        auto it = influence(sk, slot, att, vertex, bone);
        it->weight = 1;
    }
}

void paintWeight(const Skeleton& sk, const Slot& slot, Attachment& att, int vertex, int bone, float delta,
                 const std::vector<int>& locked) {
    if (!att.isWeighted() || vertex < 0 || vertex >= (int)att.weights.size()) return;
    float current = vertexWeight(att, vertex, bone);
    if (current <= 0 && delta <= 0) return;
    setWeight(sk, slot, att, vertex, bone, current + delta, locked);
}

void smoothWeights(const Skeleton& sk, const Slot& slot, Attachment& att, const std::vector<int>& vertices, float amount,
                   const std::vector<int>& locked) {
    if (!att.isWeighted()) return;
    int n = (int)att.weights.size();
    std::vector<std::vector<int>> neighbours(n);
    auto link = [&](int a, int b) {
        if (std::find(neighbours[a].begin(), neighbours[a].end(), b) == neighbours[a].end()) neighbours[a].push_back(b);
    };
    for (size_t t = 0; t + 2 < att.triangles.size(); t += 3)
        for (int e = 0; e < 3; ++e) {
            int a = att.triangles[t + e], b = att.triangles[t + (e + 1) % 3];
            link(a, b);
            link(b, a);
        }
    if (att.isPolygon())
        for (int v = 0; v < n; ++v) {
            link(v, (v + 1) % n);
            link((v + 1) % n, v);
        }
    std::vector<int> targets = vertices;
    if (targets.empty())
        for (int v = 0; v < n; ++v) targets.push_back(v);
    Pose setup = makeSetupPose(sk);
    std::vector<Vec2> world = computeVertices(sk, setup, slot, att);
    auto old = att.weights;
    amount = std::clamp(amount, 0.0f, 1.0f);
    for (int v : targets) {
        if (v < 0 || v >= n || neighbours[v].empty()) continue;
        std::map<int, float> avg;
        for (int nb : neighbours[v])
            for (auto& w : old[nb]) avg[w.bone] += w.weight / neighbours[v].size();
        std::map<int, float> mixed;
        for (auto& w : old[v]) mixed[w.bone] += (1 - amount) * w.weight;
        for (auto& [b, w] : avg) mixed[b] += amount * w;
        // Locked bones keep their weight; the rest is rescaled into what is left.
        float lockedSum = 0, free = 0;
        for (auto& w : old[v])
            if (isLocked(locked, w.bone)) lockedSum += w.weight;
        for (auto& [b, w] : mixed)
            if (isLocked(locked, b)) w = 0;
            else free += w;
        std::vector<VertexWeight> list;
        for (auto& w : old[v])
            if (isLocked(locked, w.bone)) list.push_back(w);
        for (auto& [b, w] : mixed) {
            if (isLocked(locked, b) || free <= 0) continue;
            float value = w / free * (1 - lockedSum);
            if (value < 1e-4f) continue;
            VertexWeight vw;
            vw.bone = b;
            vw.weight = value;
            vw.local = setup.world[b].inverse().apply(world[v]);
            list.push_back(vw);
        }
        if (!list.empty()) att.weights[v] = std::move(list);
    }
}

int pruneWeights(Attachment& att, float threshold, int maxInfluences) {
    int removed = 0;
    for (auto& list : att.weights) {
        std::sort(list.begin(), list.end(), [](const VertexWeight& a, const VertexWeight& b) { return a.weight > b.weight; });
        size_t keep = list.size();
        while (keep > 1 && (list[keep - 1].weight < threshold || (int)keep > maxInfluences)) --keep;
        removed += (int)(list.size() - keep);
        list.resize(keep);
        float total = 0;
        for (auto& w : list) total += w.weight;
        for (auto& w : list) w.weight = total > 0 ? w.weight / total : 1.0f / list.size();
    }
    return removed;
}

std::vector<int> meshBones(const Attachment& att) {
    std::vector<int> out;
    for (auto& list : att.weights)
        for (auto& w : list)
            if (std::find(out.begin(), out.end(), w.bone) == out.end()) out.push_back(w.bone);
    std::sort(out.begin(), out.end());
    return out;
}

void setVertexSetupWorld(const Skeleton&, const Pose& setup, const Slot& slot, Attachment& att, int vertex,
                         Vec2 world) {
    if (!att.hasVertices() || vertex < 0 || vertex >= (int)att.vertexCount()) return;
    if (att.isWeighted()) {
        for (auto& w : att.weights[vertex]) w.local = setup.world[w.bone].inverse().apply(world);
    } else {
        att.vertices[vertex] = setup.world[slot.bone].inverse().apply(world);
    }
}

} // namespace rig
