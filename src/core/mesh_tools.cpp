#include "core/mesh_tools.h"

#include <algorithm>

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
    if (att.type != AttachmentType::Mesh || bones.empty()) return;
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

void paintWeight(const Skeleton& sk, const Slot& slot, Attachment& att, int vertex, int bone, float delta) {
    if (!att.isWeighted() || vertex < 0 || vertex >= (int)att.weights.size()) return;
    auto& list = att.weights[vertex];
    auto it = std::find_if(list.begin(), list.end(), [&](const VertexWeight& w) { return w.bone == bone; });
    if (it == list.end()) {
        if (delta <= 0) return;
        Pose setup = makeSetupPose(sk);
        Vec2 world = computeVertices(sk, setup, slot, att)[vertex];
        VertexWeight w;
        w.bone = bone;
        w.local = setup.world[bone].inverse().apply(world);
        w.weight = 0;
        list.push_back(w);
        it = list.end() - 1;
    }
    float others = 0;
    for (auto& w : list)
        if (w.bone != bone) others += w.weight;
    if (others <= 1e-6f) {
        it->weight = 1;  // a lone influence must stay at 1
        return;
    }
    float target = std::clamp(it->weight + delta, 0.0f, 1.0f);
    float scale = (1 - target) / others;
    for (auto& w : list) w.weight = (w.bone == bone) ? target : w.weight * scale;
    list.erase(std::remove_if(list.begin(), list.end(), [](const VertexWeight& w) { return w.weight < 1e-4f; }),
               list.end());
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
    if (att.type != AttachmentType::Mesh || vertex < 0 || vertex >= (int)att.vertexCount()) return;
    if (att.isWeighted()) {
        for (auto& w : att.weights[vertex]) w.local = setup.world[w.bone].inverse().apply(world);
    } else {
        att.vertices[vertex] = setup.world[slot.bone].inverse().apply(world);
    }
}

} // namespace rig
