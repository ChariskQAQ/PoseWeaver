// Minimal self-checking tests for rigcore (no framework dependency).
#include "core/atlas.h"
#include "core/audio_mix.h"
#include "core/clipping.h"
#include "core/merge.h"
#include "core/psd.h"
#include "core/raster.h"
#include "core/constraints.h"
#include "core/mesh_tools.h"
#include "core/pose.h"
#include "core/serialize.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace rig;

static int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

static bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }
static bool near(Vec2 a, Vec2 b, float eps = 1e-3f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps); }

static Skeleton makeArm() {
    Skeleton sk;
    int root = sk.addBone("root", -1, {}, 0);
    Transform upper;
    upper.rotation = 90;  // pointing up
    int arm = sk.addBone("upper", root, upper, 100);
    Transform lower;
    lower.x = 100;
    sk.addBone("lower", arm, lower, 100);
    return sk;
}

static void testWorldTransforms() {
    Skeleton sk = makeArm();
    Pose p = makeSetupPose(sk);
    CHECK(near(p.world[2].origin(), {0, 100}));
    CHECK(near(boneTip(sk.bones[2], p.world[2]), {0, 200}));
}

static void testAnimationSampling() {
    Skeleton sk = makeArm();
    Animation an;
    an.name = "wave";
    setRotateKey(an, 1, 0.0f, 0.0f);
    setRotateKey(an, 1, 1.0f, -90.0f);
    Pose p = makeAnimatedPose(sk, &an, 0.5f);
    CHECK(near(p.local[1].rotation, 45.0f));
    p = makeAnimatedPose(sk, &an, 1.0f);
    CHECK(near(p.world[2].origin(), {100, 0}));
    setKeyCurve(an, 1, Channel::Rotate, 0.0f, CurveShape::stepped());
    p = makeAnimatedPose(sk, &an, 0.9f);
    CHECK(near(p.local[1].rotation, 90.0f));
    // Replacing an existing key keeps one key.
    setRotateKey(an, 1, 1.0f, -45.0f);
    CHECK(an.bones[1].rotate.size() == 2);
}

static void testReparentKeepsWorld() {
    Skeleton sk = makeArm();
    Pose before = makeSetupPose(sk);
    CHECK(sk.reparentBone(2, 0));
    Pose after = makeSetupPose(sk);
    CHECK(near(after.world[2].origin(), before.world[2].origin()));
    CHECK(near(after.world[2].rotationDeg(), before.world[2].rotationDeg()));
    CHECK(!sk.reparentBone(0, 2));  // cycle
}

static Skeleton makeMeshRig() {
    Skeleton sk = makeArm();
    sk.images.push_back({"arm.png", 40, 200});
    Slot s;
    s.name = "arm";
    s.bone = 0;
    Attachment a;
    a.name = "arm";
    a.image = 0;
    a.transform.y = 100;  // image centre at the elbow
    s.attachments.push_back(a);
    s.activeAttachment = 0;
    sk.slots.push_back(s);
    return sk;
}

static void testMeshSkinning() {
    Skeleton sk = makeMeshRig();
    Slot& s = sk.slots[0];
    CHECK(convertRegionToMesh(s.attachments[0], sk.images[0], nullptr, 2, 4));
    Attachment& a = s.attachments[0];
    CHECK(a.vertexCount() == 15);
    CHECK(a.triangles.size() == 2 * 4 * 6);

    Pose setup = makeSetupPose(sk);
    std::vector<Vec2> before = computeVertices(sk, setup, s, a);
    bindMesh(sk, s, a, {1, 2});
    CHECK(a.isWeighted());
    std::vector<Vec2> bound = computeVertices(sk, setup, s, a);
    for (size_t i = 0; i < before.size(); ++i) CHECK(near(before[i], bound[i]));
    for (auto& list : a.weights) {
        float total = 0;
        for (auto& w : list) total += w.weight;
        CHECK(near(total, 1.0f));
    }

    // Bend the elbow: the top vertex (fully on "lower" bone) must follow the bone.
    Animation an;
    setRotateKey(an, 2, 0, -90);
    Pose bent = makeAnimatedPose(sk, &an, 0);
    std::vector<Vec2> moved = computeVertices(sk, bent, s, a);
    // uv (0.5, 0) = top centre, originally at (0, 200).
    for (size_t i = 0; i < a.uvs.size(); ++i) {
        if (near(a.uvs[i], {0.5f, 0.0f})) {
            CHECK(vertexWeight(a, (int)i, 2) > 0.9f);
            CHECK(near(moved[i], {100, 100}, 10.0f));
        }
    }

    paintWeight(sk, s, a, 0, 1, 0.3f);
    float total = 0;
    for (auto& w : a.weights[0]) total += w.weight;
    CHECK(near(total, 1.0f));

    unbindMesh(sk, s, a);
    CHECK(!a.isWeighted());
    std::vector<Vec2> unbound = computeVertices(sk, setup, s, a);
    CHECK(near(unbound[5], computeVertices(sk, setup, s, a)[5]));
}

static void testAlphaTrim() {
    Image img{"x.png", 4, 4};
    std::vector<unsigned char> px(4 * 4 * 4, 0);
    px[(0 * 4 + 0) * 4 + 3] = 255;  // only the top-left pixel is opaque
    Attachment a;
    a.image = 0;
    CHECK(convertRegionToMesh(a, img, px.data(), 4, 4));
    // The opaque pixel plus the 1px margin touches the top-left 2x2 cells.
    CHECK(a.triangles.size() == 4 * 6);
}

static void testRemoveBone() {
    Skeleton sk = makeMeshRig();
    Slot& s = sk.slots[0];
    convertRegionToMesh(s.attachments[0], sk.images[0], nullptr, 2, 4);
    bindMesh(sk, s, s.attachments[0], {1, 2});
    Animation an;
    setRotateKey(an, 2, 0, 10);
    setRotateKey(an, 1, 0, 5);
    sk.animations.push_back(an);
    Pose setup = makeSetupPose(sk);
    std::vector<Vec2> before = computeVertices(sk, setup, sk.slots[0], sk.slots[0].attachments[0]);

    sk.removeBone(2);
    CHECK(sk.bones.size() == 2);
    CHECK(sk.animations[0].bones.size() == 1 && sk.animations[0].bones.count(1));
    setup = makeSetupPose(sk);
    std::vector<Vec2> after = computeVertices(sk, setup, sk.slots[0], sk.slots[0].attachments[0]);
    for (size_t i = 0; i < before.size(); ++i) CHECK(near(before[i], after[i]));
}


static void testShearMatrix() {
    Transform t;
    t.x = 5;
    t.y = -3;
    t.rotation = 30;
    t.scaleX = 2;
    t.scaleY = -1.5f;
    t.shearY = 20;
    Transform d = decompose(t.matrix());
    CHECK(near(d.x, 5) && near(d.y, -3));
    CHECK(near(d.rotation, 30) && near(d.scaleX, 2) && near(d.scaleY, -1.5f) && near(d.shearY, 20));
    // shearX folds into rotation + shearY, the matrix must survive the round trip.
    t.shearX = 10;
    Affine a = t.matrix(), b = decompose(a).matrix();
    CHECK(near(a.a, b.a) && near(a.b, b.b) && near(a.c, b.c) && near(a.d, b.d));
    // Identity shear leaves the old TRS matrix unchanged.
    Transform plain;
    plain.rotation = 45;
    plain.scaleX = 3;
    Affine p = plain.matrix(), q = Affine::fromTRS(0, 0, 45, 3, 1);
    CHECK(near(p.a, q.a) && near(p.b, q.b) && near(p.c, q.c) && near(p.d, q.d));
}

static void testCurves() {
    CHECK(near(applyCurve(CurveShape::linear(), 0.3f), 0.3f));
    CHECK(near(applyCurve(CurveShape::stepped(), 0.99f), 0.0f));
    CHECK(near(applyCurve(CurveShape::bezier(1 / 3.0f, 1 / 3.0f, 2 / 3.0f, 2 / 3.0f), 0.7f), 0.7f));
    CurveShape ease = CurveShape::bezier(0.42f, 0, 0.58f, 1);
    CHECK(near(applyCurve(ease, 0.5f), 0.5f));
    CHECK(applyCurve(ease, 0.2f) < 0.15f);
    CHECK(applyCurve(ease, 0.8f) > 0.85f);
    CHECK(near(applyCurve(ease, 0), 0) && near(applyCurve(ease, 1), 1));

    Skeleton sk = makeArm();
    Animation an;
    setRotateKey(an, 1, 0, 0);
    setRotateKey(an, 1, 1, 100);
    setKeyCurve(an, 1, Channel::Rotate, 0, ease);
    Pose pose = makeAnimatedPose(sk, &an, 0.2f);
    CHECK(pose.local[1].rotation - 90 < 15);
    CHECK(keyCurve(an, 1, Channel::Rotate, 0)->type == Curve::Bezier);
}

static Skeleton makeThreeSlots() {
    Skeleton sk = makeArm();
    for (const char* n : {"a", "b", "c"}) {
        Slot s;
        s.name = n;
        sk.slots.push_back(s);
    }
    return sk;
}

static void testSlotColorAndDrawOrder() {
    Skeleton sk = makeThreeSlots();
    Animation an;
    setColorKey(an, 1, 0, Color(1, 0, 0, 1));
    setColorKey(an, 1, 1, Color(0, 0, 1, 0));
    setDrawOrderKey(an, 0.5f, {2, 0, 1});
    sk.animations.push_back(an);

    Pose p = makeAnimatedPose(sk, &sk.animations[0], 0.5f);
    CHECK(near(p.slotColor[1][0], 0.5f) && near(p.slotColor[1][2], 0.5f) && near(p.slotColor[1][3], 0.5f));
    CHECK(p.slotColor[0] == Color());
    CHECK((p.drawOrder == std::vector<int>{2, 0, 1}));
    p = makeAnimatedPose(sk, &sk.animations[0], 0.25f);
    CHECK((p.drawOrder == std::vector<int>{0, 1, 2}));

    // Swapping slots keeps timelines and draw order keys pointing at the same slots.
    sk.swapSlots(0, 2);
    CHECK(sk.slots[0].name == "c" && sk.slots[2].name == "a");
    CHECK((sk.animations[0].drawOrder[0].order == std::vector<int>{0, 2, 1}));
    CHECK(sk.animations[0].slots.count(1) == 1);

    sk.removeSlot(1);  // "b"
    CHECK(sk.slots.size() == 2 && sk.slots[1].name == "a");
    CHECK(sk.animations[0].slots.empty());
    CHECK((sk.animations[0].drawOrder[0].order == std::vector<int>{0, 1}));
}

static void testEvents() {
    Skeleton sk = makeArm();
    EventDef step;
    step.name = "step";
    step.intValue = 3;
    EventDef hit;
    hit.name = "hit";
    sk.events = {step, hit};
    Animation an;
    setEventKey(an, 0.5f, 0, sk.events[0]);
    setEventKey(an, 0.5f, 1, sk.events[1]);
    setEventKey(an, 0.5f, 0, sk.events[0]);  // duplicate ignored
    setEventKey(an, 0.0f, 1, sk.events[1]);
    CHECK(an.events.size() == 3 && an.events[1].intValue == 3);
    CHECK(eventsInRange(an, -1, 0.1f).size() == 1);
    CHECK(eventsInRange(an, 0.0f, 0.5f).size() == 2);
    moveChannelKey(an, 0, Channel::Event, 0.5f, 0.75f);
    CHECK(eventsInRange(an, 0.6f, 1.0f).size() == 1);
    sk.animations.push_back(an);
    sk.removeEvent(0);
    CHECK(sk.animations[0].events.size() == 2);
    for (auto& k : sk.animations[0].events) CHECK(k.event == 0);
}

static void testVersion1Compat() {
    const char* v1 = R"({"format":"rig","version":1,"images":[],
        "bones":[{"name":"root","parent":-1,"length":0,"x":0,"y":0,"rotation":0,"scaleX":1,"scaleY":1}],
        "slots":[{"name":"s","bone":0,"active":-1,"color":[1,0.5,1,1],"attachments":[]}],
        "animations":[{"name":"a","duration":1,
          "bones":[{"bone":0,"rotate":[{"time":0,"value":0,"curve":"stepped"},{"time":1,"value":90,"curve":"linear"}],
                    "translate":[],"scale":[]}],
          "slots":[{"slot":0,"attachment":[{"time":0,"value":-1}]}]}]})";
    Skeleton sk;
    std::string err;
    CHECK(fromJson(v1, sk, &err));
    CHECK(sk.animations.size() == 1 && sk.animations[0].bones[0].rotate[0].curve.type == Curve::Stepped);
    CHECK(near(sk.slots[0].color[1], 0.5f));
    CHECK(!fromJson(R"({"format":"rig","version":99,"images":[],"bones":[],"slots":[],"animations":[]})", sk, &err));
}

static float trianglesArea(const std::vector<Vec2>& pts, const std::vector<int>& tris) {
    float a = 0;
    for (size_t t = 0; t + 2 < tris.size(); t += 3) a += std::fabs(cross(pts[tris[t + 1]] - pts[tris[t]], pts[tris[t + 2]] - pts[tris[t]])) * 0.5f;
    return a;
}

static bool hasEdge(const std::vector<int>& tris, int a, int b) {
    for (size_t t = 0; t + 2 < tris.size(); t += 3)
        for (int e = 0; e < 3; ++e) {
            int x = tris[t + e], y = tris[t + (e + 1) % 3];
            if ((x == a && y == b) || (x == b && y == a)) return true;
        }
    return false;
}

static void testTriangulate() {
    // Square with one interior point.
    std::vector<Vec2> sq = {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {5, 5}};
    std::vector<int> tris;
    CHECK(triangulate(sq, 4, tris));
    CHECK(tris.size() == 4 * 3);
    CHECK(near(trianglesArea(sq, tris), 100));
    // Concave L shape (clockwise) with interior points: hull edges are kept, area preserved.
    std::vector<Vec2> l = {{0, 0}, {0, 20}, {10, 20}, {10, 10}, {20, 10}, {20, 0}, {5, 5}, {15, 5}, {5, 15}};
    CHECK(triangulate(l, 6, tris));
    CHECK(near(trianglesArea(l, tris), 300, 0.01f));
    CHECK(tris.size() == (size_t)(2 * 3 + 6 - 2) * 3);
    for (int i = 0; i < 6; ++i) CHECK(hasEdge(tris, i, (i + 1) % 6));
    // Delaunay: the four interior points of a grid never produce the long diagonal.
    std::vector<Vec2> grid = {{0, 0}, {30, 0}, {30, 30}, {0, 30}, {10, 10}, {20, 10}, {20, 20}, {10, 20}};
    CHECK(triangulate(grid, 4, tris));
    CHECK(near(trianglesArea(grid, tris), 900));
    CHECK(!hasEdge(tris, 4, 6) || !hasEdge(tris, 5, 7));
    // A self-intersecting outline is rejected.
    std::vector<Vec2> bow = {{0, 0}, {10, 10}, {10, 0}, {0, 10}};
    CHECK(!triangulate(bow, 4, tris));
    CHECK(convexHull(bow).size() == 4);
}

static std::vector<unsigned char> circleImage(int w, int h, float cx, float cy, float r) {
    std::vector<unsigned char> px(size_t(w) * h * 4, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (length(Vec2{x + 0.5f - cx, y + 0.5f - cy}) <= r) px[(size_t(y) * w + x) * 4 + 3] = 255;
    return px;
}

static void testOutlineAndMeshBuild() {
    std::vector<unsigned char> px = circleImage(64, 64, 32, 32, 20);
    std::vector<Vec2> outline = traceAlphaOutline(px.data(), 64, 64, 8, 1.0f, 0.0f);
    CHECK(outline.size() >= 6);
    std::vector<int> idx(outline.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = (int)i;
    float area = 0;
    for (size_t i = 0; i < outline.size(); ++i) area += cross(outline[i], outline[(i + 1) % outline.size()]);
    area = std::fabs(area) * 0.5f;
    CHECK(area > 3.14159f * 400 * 0.8f && area < 3.14159f * 400 * 1.2f);
    std::vector<Vec2> padded = traceAlphaOutline(px.data(), 64, 64, 8, 1.0f, 4.0f);
    float area2 = 0;
    for (size_t i = 0; i < padded.size(); ++i) area2 += cross(padded[i], padded[(i + 1) % padded.size()]);
    CHECK(std::fabs(area2) * 0.5f > area);

    Skeleton sk = makeArm();
    sk.images.push_back({"c.png", 64, 64});
    Slot slot;
    slot.bone = 0;
    Attachment att;
    att.image = 0;
    att.transform.x = 100;  // image centre at (100, 0)
    slot.attachments.push_back(att);
    slot.activeAttachment = 0;
    sk.slots.push_back(slot);
    Attachment& a = sk.slots[0].attachments[0];
    Affine before = imageToLocal(sk, sk.slots[0], a, sk.images[0]);
    CHECK(buildMeshFromOutline(sk, sk.slots[0], a, sk.images[0], outline, 8));
    CHECK(a.type == AttachmentType::Mesh && a.hullLength >= (int)outline.size());
    CHECK(a.vertexCount() > outline.size());
    CHECK(near(trianglesArea(setupLocalVertices(sk, sk.slots[0], a), a.triangles), area, 1.0f));
    for (Vec2 uv : a.uvs) CHECK(uv.x >= 0 && uv.x <= 1 && uv.y >= 0 && uv.y <= 1);
    // The fitted mapping of the mesh matches the region mapping it came from.
    Affine after = imageToLocal(sk, sk.slots[0], a, sk.images[0]);
    CHECK(near(after.a, before.a) && near(after.d, before.d) && near(after.tx, before.tx, 0.01f) && near(after.ty, before.ty, 0.01f));

    // Add an interior vertex, then remove it again.
    size_t n = a.vertexCount(), t = a.triangles.size();
    int v = addMeshVertex(sk, sk.slots[0], a, &sk.images[0], {101, 1}, -1);
    CHECK(v == (int)n && a.vertexCount() == n + 1 && a.triangles.size() == t + 6);
    CHECK(near(a.uvs[v].x, 33.0f / 64) && near(a.uvs[v].y, 31.0f / 64));
    CHECK(removeMeshVertices(sk, sk.slots[0], a, {v}));
    CHECK(a.vertexCount() == n && a.triangles.size() == t);
    // Insert into the outline after hull vertex 0.
    int hullCount = a.hullLength;
    int hv = addMeshVertex(sk, sk.slots[0], a, &sk.images[0], (setupLocalVertices(sk, sk.slots[0], a)[0] + setupLocalVertices(sk, sk.slots[0], a)[1]) * 0.5f, 0);
    int hullBefore = a.hullLength;
    (void)hullBefore;
    CHECK(hv == 1 && a.hullLength == hullCount + 1);
    // The outline must keep at least three vertices.
    std::vector<int> all;
    for (int i = 0; i < a.hullLength; ++i) all.push_back(i);
    removeMeshVertices(sk, sk.slots[0], a, all);
    CHECK(a.hullLength >= 3);

    // Grid meshes get a hull from their boundary.
    Attachment g;
    g.image = 0;
    CHECK(convertRegionToMesh(g, sk.images[0], nullptr, 2, 2));
    CHECK(ensureHull(g) && g.hullLength == 8);
    CHECK(near(trianglesArea(g.vertices, g.triangles), 64 * 64));
}

static void testDeform() {
    Skeleton sk = makeMeshRig();
    Slot& s = sk.slots[0];
    convertRegionToMesh(s.attachments[0], sk.images[0], nullptr, 1, 2);
    Attachment& a = s.attachments[0];
    size_t n = a.vertexCount();
    Animation an;
    setDeformKey(an, 0, 0, 0, std::vector<Vec2>(n, Vec2{10, 0}));
    setDeformKey(an, 0, 0, 1, std::vector<Vec2>(n, Vec2{0, 0}));
    sk.animations.push_back(an);
    Pose setup = makeSetupPose(sk);
    Pose p = makeAnimatedPose(sk, &sk.animations[0], 0.5f);
    const std::vector<Vec2>* d = findDeform(p, 0, 0);
    CHECK(d && d->size() == n && near((*d)[0], {5, 0}));
    std::vector<Vec2> base = computeVertices(sk, setup, s, a), moved = computeVertices(sk, p, s, a, d);
    for (size_t i = 0; i < n; ++i) CHECK(near(moved[i] - base[i], {5, 0}));
    // Weighted: offsets move with the blended bones.
    bindMesh(sk, s, a, {1, 2});
    Animation bent;
    setRotateKey(bent, 1, 0, 90);  // upper bone now points left
    sk.animations.push_back(bent);
    Pose rotated = makeAnimatedPose(sk, &sk.animations[1], 0);
    std::vector<Vec2> off(n, Vec2{10, 0});
    std::vector<Vec2> r0 = computeVertices(sk, rotated, s, a), r1 = computeVertices(sk, rotated, s, a, &off);
    Affine basis = deformBasis(rotated, s, a, 0);
    CHECK(near(r1[0] - r0[0], basis.applyVector({10, 0})));
    // Keys with the wrong vertex count are dropped; removing an attachment drops its keys.
    sk.animations[0].slots[0].deform[0].push_back({2.0f, std::vector<Vec2>(n + 1), {}});
    CHECK(sk.dropStaleDeformKeys(0, 0) == 1);
    sk.removeAttachment(0, 0);
    CHECK(sk.animations[0].slots.empty());
    CHECK(sk.slots[0].activeAttachment == -1);
}

static void testWeightTools() {
    Skeleton sk = makeMeshRig();
    Slot& s = sk.slots[0];
    convertRegionToMesh(s.attachments[0], sk.images[0], nullptr, 2, 4);
    Attachment& a = s.attachments[0];
    bindMesh(sk, s, a, {0, 1, 2});
    auto total = [&](int v) {
        float t = 0;
        for (auto& w : a.weights[v]) t += w.weight;
        return t;
    };
    // Locked bone keeps its weight.
    int v = 7;
    float lockedBefore = vertexWeight(a, v, 2);
    setWeight(sk, s, a, v, 1, 0.9f, {2});
    CHECK(near(vertexWeight(a, v, 2), lockedBefore));
    CHECK(near(total(v), 1));
    CHECK(vertexWeight(a, v, 1) <= 1 - lockedBefore + 1e-4f);
    // Smoothing pulls a spike towards its neighbours.
    setWeight(sk, s, a, v, 0, 1.0f);
    float spike = vertexWeight(a, v, 0);
    smoothWeights(sk, s, a, {v}, 1.0f);
    CHECK(vertexWeight(a, v, 0) < spike);
    CHECK(near(total(v), 1));
    // Prune to one influence.
    pruneWeights(a, 0.0f, 1);
    for (size_t i = 0; i < a.weights.size(); ++i) CHECK(a.weights[i].size() == 1 && near(total((int)i), 1));
    // Vertex positions survive all of the above in the setup pose.
    Pose setup = makeSetupPose(sk);
    std::vector<Vec2> world = computeVertices(sk, setup, s, a);
    CHECK(near(world[0], computeVertices(sk, setup, s, a)[0]));
}

static void testClipping() {
    Geometry sq;
    sq.positions = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    sq.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    sq.triangles = {0, 1, 2, 0, 2, 3};
    Geometry out;
    // Half-plane triangle covering x < 5.
    clipGeometry(sq, clipTrianglesOf({{-10, -10}, {5, -10}, {5, 30}, {-10, 30}}), out);
    CHECK(near(trianglesArea(out.positions, out.triangles), 50));
    for (size_t i = 0; i < out.positions.size(); ++i) CHECK(near(out.uvs[i].x, out.positions[i].x / 10));
    // Concave L-shaped clip polygon.
    clipGeometry(sq, clipTrianglesOf({{0, 0}, {0, 10}, {5, 10}, {5, 5}, {10, 5}, {10, 0}}), out);
    CHECK(near(trianglesArea(out.positions, out.triangles), 75));
    // Nothing overlaps.
    clipGeometry(sq, clipTrianglesOf({{20, 20}, {30, 20}, {30, 30}}), out);
    CHECK(out.triangles.empty());
}

static void testPhase2RoundTrip() {
    Skeleton sk = makeMeshRig();
    Slot& s = sk.slots[0];
    std::vector<unsigned char> px = circleImage(40, 200, 20, 100, 18);
    std::vector<Vec2> outline = traceAlphaOutline(px.data(), 40, 200, 8, 1, 2);
    CHECK(buildMeshFromOutline(sk, s, s.attachments[0], sk.images[0], outline, 10));
    Attachment clip;
    clip.name = "clip";
    clip.type = AttachmentType::Clipping;
    makePolygon(clip, {{-10, 0}, {10, 0}, {0, 50}});
    clip.endSlot = 0;
    Attachment box;
    box.name = "box";
    box.type = AttachmentType::BoundingBox;
    makePolygon(box, {{0, 0}, {5, 0}, {5, 5}, {0, 5}});
    s.attachments.push_back(clip);
    s.attachments.push_back(box);
    Animation an;
    an.name = "deform";
    setDeformKey(an, 0, 0, 0.5f, std::vector<Vec2>(s.attachments[0].vertexCount(), Vec2{1, 2}));
    setKeyCurve(an, 0, Channel::Deform, 0.5f, CurveShape::stepped(), 0);
    sk.animations.push_back(an);
    std::string text = toJson(sk);
    Skeleton loaded;
    std::string err;
    CHECK(fromJson(text, loaded, &err));
    CHECK(toJson(loaded) == text);
    CHECK(loaded.slots[0].attachments[1].type == AttachmentType::Clipping && loaded.slots[0].attachments[1].hullLength == 3);
    CHECK(loaded.slots[0].attachments[0].hullLength == s.attachments[0].hullLength);
    CHECK(keyCurve(loaded.animations[0], 0, Channel::Deform, 0.5f, 0)->type == Curve::Stepped);
}

// Root at origin, arm pointing +x: upper (0..100), lower (100..200), plus a free target bone.
static Skeleton makeIkRig(Vec2 target) {
    Skeleton sk;
    int root = sk.addBone("root", -1, {}, 0);
    int upper = sk.addBone("upper", root, {}, 100);
    Transform lt;
    lt.x = 100;
    sk.addBone("lower", upper, lt, 100);
    Transform tt;
    tt.x = target.x;
    tt.y = target.y;
    sk.addBone("target", root, tt, 0);
    return sk;
}

static void testIk() {
    // Two-bone IK reaches a reachable target and the bend direction picks the side.
    Skeleton sk = makeIkRig({120, 80});
    Constraint ik;
    ik.type = ConstraintType::Ik;
    ik.bones = {1, 2};
    ik.target = 3;
    ik.values = defaultConstraintValues(ConstraintType::Ik);
    sk.constraints.push_back(ik);
    Pose p = makeSetupPose(sk);
    CHECK(near(boneTip(sk.bones[2], p.world[2]), {120, 80}, 0.05f));
    Vec2 elbowUp = p.world[2].origin();
    sk.constraints[0].values[2] = -1;
    p = makeSetupPose(sk);
    CHECK(near(boneTip(sk.bones[2], p.world[2]), {120, 80}, 0.05f));
    CHECK(length(p.world[2].origin() - elbowUp) > 10);
    // Local transforms (what gets keyed) are untouched; only `applied` changes.
    CHECK(near(p.local[1].rotation, 0) && !near(p.applied[1].rotation, 0));
    // Out of reach: the chain points straight at the target.
    sk.bones[3].setup = {};
    sk.bones[3].setup.y = 500;
    p = makeSetupPose(sk);
    CHECK(near(p.world[1].rotationDeg(), 90, 0.5f) && near(p.world[2].rotationDeg(), 90, 0.5f));
    // Stretch reaches it.
    sk.constraints[0].stretch = true;
    p = makeSetupPose(sk);
    CHECK(near(boneTip(sk.bones[2], p.world[2]), {0, 500}, 0.5f));
    // One bone, half mix: half way between 0 and 90 degrees.
    Skeleton one = makeIkRig({0, 100});
    Constraint ik1 = ik;
    ik1.bones = {2};
    ik1.values[0] = 0.5f;
    one.constraints.push_back(ik1);
    p = makeSetupPose(one);
    float full = radToDeg(std::atan2(100.0f, -100.0f));
    CHECK(near(p.world[2].rotationDeg(), full * 0.5f, 0.5f));
    // Mix animation key.
    Animation an;
    setConstraintKey(an, 0, 0, {0, 0, 1, 0, 0, 0});
    setConstraintKey(an, 0, 1, {1, 0, 1, 0, 0, 0});
    one.animations.push_back(an);
    p = makeAnimatedPose(one, &one.animations[0], 0.5f);
    CHECK(near(p.constraints[0][0], 0.5f));
}

static void testTransformConstraint() {
    Skeleton sk = makeIkRig({50, 60});
    sk.bones[3].setup.rotation = 30;
    sk.bones[3].setup.scaleX = 2;
    Constraint tc;
    tc.type = ConstraintType::Transform;
    tc.bones = {2};
    tc.target = 3;
    tc.values = defaultConstraintValues(ConstraintType::Transform);
    tc.offsetRotation = 10;
    sk.constraints.push_back(tc);
    Pose p = makeSetupPose(sk);
    CHECK(near(p.world[2].origin(), {50, 60}));
    CHECK(near(p.world[2].rotationDeg(), 40));
    CHECK(near(decompose(p.world[2]).scaleX, 2));
    // Only rotation, half mix.
    sk.constraints[0].values = {0.5f, 0, 0, 0, 0, 0};
    p = makeSetupPose(sk);
    CHECK(near(p.world[2].origin(), {100, 0}) && near(p.world[2].rotationDeg(), 20));
}

static void testPathConstraint() {
    // A straight path from (0,0) to (300,0); three 100-long bones follow it.
    Skeleton sk;
    int root = sk.addBone("root", -1, {}, 0);
    int a = sk.addBone("a", root, {}, 100);
    Transform t;
    t.rotation = 45;
    int b = sk.addBone("b", a, t, 100);
    int c = sk.addBone("c", b, t, 100);
    Slot slot;
    slot.name = "path";
    slot.bone = root;
    Attachment path;
    path.type = AttachmentType::Path;
    makePolygon(path, {{-50, 0}, {0, 0}, {50, 0}, {250, 0}, {300, 0}, {350, 0}});
    slot.attachments.push_back(path);
    slot.activeAttachment = 0;
    sk.slots.push_back(slot);
    Constraint pc;
    pc.type = ConstraintType::Path;
    pc.bones = {a, b, c};
    pc.target = 0;
    pc.values = defaultConstraintValues(ConstraintType::Path);
    sk.constraints.push_back(pc);
    Pose p = makeSetupPose(sk);
    CHECK(near(p.world[a].origin(), {0, 0}, 0.5f) && near(p.world[b].origin(), {100, 0}, 0.5f) && near(p.world[c].origin(), {200, 0}, 0.5f));
    CHECK(near(p.world[b].rotationDeg(), 0, 0.5f) && near(p.world[c].rotationDeg(), 0, 0.5f));
    // Position 50% moves the chain along.
    sk.constraints[0].values[0] = 0.5f;
    p = makeSetupPose(sk);
    CHECK(near(p.world[a].origin(), {150, 0}, 0.5f));
    PathCurve curve = buildPathCurve({{-50, 0}, {0, 0}, {50, 0}, {250, 0}, {300, 0}, {350, 0}}, false);
    CHECK(near(curve.total, 300, 0.5f));
}

static void testPhysics() {
    Skeleton sk;
    int root = sk.addBone("root", -1, {}, 0);
    int hair = sk.addBone("hair", root, {}, 50);
    Constraint ph;
    ph.type = ConstraintType::Physics;
    ph.bones = {hair};
    ph.values = defaultConstraintValues(ConstraintType::Physics);
    sk.constraints.push_back(ph);
    PhysicsState state;
    Pose p = makeSetupPose(sk);
    applyPhysics(sk, p, state, 1 / 60.0f);
    CHECK(near(p.world[hair].origin(), {0, 0}));
    // Move the root suddenly: the bone lags behind, then settles back.
    sk.bones[root].setup.x = 100;
    p = makeSetupPose(sk);
    applyPhysics(sk, p, state, 1 / 60.0f);
    CHECK(p.world[hair].origin().x < 99);
    for (int i = 0; i < 600; ++i) {
        p = makeSetupPose(sk);
        applyPhysics(sk, p, state, 1 / 60.0f);
    }
    CHECK(near(p.world[hair].origin(), {100, 0}, 0.5f));
    // Gravity pulls it down while active.
    sk.constraints[0].values[2] = 500;
    for (int i = 0; i < 120; ++i) {
        p = makeSetupPose(sk);
        applyPhysics(sk, p, state, 1 / 60.0f);
    }
    CHECK(p.world[hair].origin().y < -1);
}

static void testConstraintBookkeeping() {
    Skeleton sk = makeIkRig({120, 80});
    Constraint ik;
    ik.name = "ik";
    ik.bones = {1, 2};
    ik.target = 3;
    sk.constraints.push_back(ik);
    Constraint tc;
    tc.name = "tc";
    tc.type = ConstraintType::Transform;
    tc.bones = {2};
    tc.target = 1;
    sk.constraints.push_back(tc);
    Animation an;
    setConstraintKey(an, 1, 0.5f, {1, 1, 1, 1, 1, 1});
    sk.animations.push_back(an);
    sk.swapConstraints(0, 1);
    CHECK(sk.constraints[0].name == "tc" && sk.animations[0].constraints.count(0));
    std::string text = toJson(sk);
    Skeleton loaded;
    std::string err;
    CHECK(fromJson(text, loaded, &err));
    CHECK(toJson(loaded) == text);
    // Removing the IK target bone removes the IK constraint; the transform one survives.
    sk.removeBone(3);
    CHECK(sk.constraints.size() == 1 && sk.constraints[0].name == "tc");
    sk.removeBone(1);  // takes upper and lower with it
    CHECK(sk.constraints.empty() && sk.animations[0].constraints.empty());
}

static void testCopyPaste() {
    Animation src;
    setRotateKey(src, 1, 0.5f, 30);
    setKeyCurve(src, 1, Channel::Rotate, 0.5f, CurveShape::stepped());
    setTranslateKey(src, 1, 0.5f, {1, 2});
    setColorKey(src, 0, 0.75f, Color(1, 0, 0, 1));
    EventDef e;
    e.name = "e";
    setEventKey(src, 0.75f, 0, e);
    // Copy two keys into a clipboard, relative to the earliest one.
    Animation clip;
    copyKey(src, clip, 1, Channel::Rotate, -1, 0.5f, 0.0f);
    copyKey(src, clip, 0, Channel::Color, -1, 0.75f, 0.25f);
    copyKey(src, clip, -1, Channel::Event, -1, 0.75f, 0.25f);
    CHECK(clip.bones[1].rotate.size() == 1 && clip.bones[1].translate.empty());
    CHECK(clip.bones[1].rotate[0].curve.type == Curve::Stepped);
    // Paste at 2s.
    Animation dst;
    setRotateKey(dst, 1, 2.0f, 99);  // replaced by the paste
    mergeAnimation(dst, clip, 2.0f);
    CHECK(dst.bones[1].rotate.size() == 1 && near(dst.bones[1].rotate[0].value, 30) && near(dst.bones[1].rotate[0].time, 2));
    CHECK(dst.slots[0].color.size() == 1 && near(dst.slots[0].color[0].time, 2.25f));
    CHECK(dst.events.size() == 1 && near(dst.events[0].time, 2.25f));
    // Pasting twice does not duplicate keys.
    mergeAnimation(dst, clip, 2.0f);
    CHECK(dst.bones[1].rotate.size() == 1 && dst.events.size() == 1);
}

static void testMirrorWeights() {
    CHECK(mirrorName("arm_l") == "arm_r" && mirrorName("R_leg") == "L_leg" && mirrorName("upper_left_arm") == "upper_right_arm");
    CHECK(mirrorName("spine") == "");
    // A wide mesh centred on x = 0 with a left and a right bone.
    Skeleton sk;
    int root = sk.addBone("root", -1, {}, 0);
    Transform l, r;
    l.x = -50;
    r.x = 50;
    int bl = sk.addBone("arm_l", root, l, 40);
    int br = sk.addBone("arm_r", root, r, 40);
    sk.images.push_back({"w.png", 200, 40});
    Slot s;
    s.bone = root;
    Attachment a;
    a.image = 0;
    s.attachments.push_back(a);
    s.activeAttachment = 0;
    sk.slots.push_back(s);
    Attachment& m = sk.slots[0].attachments[0];
    convertRegionToMesh(m, sk.images[0], nullptr, 4, 1);
    bindMesh(sk, sk.slots[0], m, {bl, br});
    // Make the left side fully weighted to arm_l, the right side to root.
    Pose setup = makeSetupPose(sk);
    std::vector<Vec2> world = computeVertices(sk, setup, sk.slots[0], m);
    for (size_t v = 0; v < world.size(); ++v) setWeight(sk, sk.slots[0], m, (int)v, world[v].x < 0 ? bl : root, 1.0f);
    std::vector<int> mirror(sk.bones.size(), -1);
    mirror[bl] = br;
    mirror[br] = bl;
    int changed = mirrorWeights(sk, sk.slots[0], m, 0.0f, true, mirror);
    CHECK(changed > 0);
    for (size_t v = 0; v < world.size(); ++v)
        if (world[v].x > 1) CHECK(near(vertexWeight(m, (int)v, br), 1.0f));
    // Positions are unchanged by mirroring.
    std::vector<Vec2> after = computeVertices(sk, setup, sk.slots[0], m);
    for (size_t v = 0; v < world.size(); ++v) CHECK(near(after[v], world[v]));
}

static void testSkins() {
    // A slot with a placeholder "body" and two skins providing different images.
    Skeleton sk;
    int root = sk.addBone("root", -1, {}, 0);
    Transform ht;
    ht.y = 100;
    int hat = sk.addBone("hat", root, ht, 20);
    sk.bones[hat].skinRequired = true;
    sk.images = {{"a.png", 10, 10}, {"b.png", 20, 20}};
    Slot slot;
    slot.name = "body";
    slot.bone = root;
    Attachment ph;
    ph.name = "body";
    ph.skinPlaceholder = true;
    slot.attachments.push_back(ph);
    slot.activeAttachment = 0;
    sk.slots.push_back(slot);
    Slot hatSlot;
    hatSlot.name = "hat";
    hatSlot.bone = hat;
    Attachment hatImg;
    hatImg.name = "hat";
    hatImg.image = 0;
    hatSlot.attachments.push_back(hatImg);
    hatSlot.activeAttachment = 0;
    sk.slots.push_back(hatSlot);
    Skin boy, girl;
    boy.name = "boy";
    girl.name = "girl";
    Attachment a;
    a.name = "boy_body";
    a.image = 0;
    boy.attachments.push_back({0, "body", a});
    a.name = "girl_body";
    a.image = 1;
    girl.attachments.push_back({0, "body", a});
    girl.bones.push_back(hat);
    sk.skins = {boy, girl};

    Pose none = makeSetupPose(sk);
    CHECK(poseAttachment(sk, none, 0) == nullptr);  // nothing fills the placeholder
    CHECK(poseAttachment(sk, none, 1) == nullptr);  // hat bone inactive without the girl skin
    Pose pb = makeSetupPose(sk, {0});
    CHECK(poseAttachment(sk, pb, 0) && poseAttachment(sk, pb, 0)->image == 0);
    CHECK(!pb.boneActive[hat]);
    Pose pg = makeSetupPose(sk, {1});
    CHECK(poseAttachment(sk, pg, 0)->image == 1 && pg.boneActive[hat] && poseAttachment(sk, pg, 1));
    // Combining: the later skin wins for shared placeholders, bones from both are active.
    Pose both = makeSetupPose(sk, {1, 0});
    CHECK(poseAttachment(sk, both, 0)->image == 0 && both.boneActive[hat]);

    // Skin-required constraints only run while their skin is active.
    Constraint tc;
    tc.type = ConstraintType::Transform;
    tc.bones = {hat};
    tc.target = root;
    tc.values = defaultConstraintValues(ConstraintType::Transform);
    tc.skinRequired = true;
    sk.constraints.push_back(tc);
    sk.skins[1].constraints.push_back(0);
    Pose off = makeSetupPose(sk, {0});
    Pose on = makeSetupPose(sk, {1});
    CHECK(near(off.world[hat].origin(), {0, 100}) && near(on.world[hat].origin(), {0, 0}));

    // Round trip and bookkeeping.
    std::string text = toJson(sk);
    Skeleton loaded;
    std::string err;
    CHECK(fromJson(text, loaded, &err));
    CHECK(toJson(loaded) == text);
    CHECK(loaded.skins.size() == 2 && loaded.slots[0].attachments[0].skinPlaceholder && loaded.bones[hat].skinRequired);
    sk.swapSlots(0, 1);
    CHECK(sk.skins[0].attachments[0].slot == 1);
    sk.removeAttachment(1, 0);  // removing the placeholder drops its skin entries
    CHECK(sk.skins[0].attachments.empty() && sk.skins[1].attachments.empty());
    sk.removeConstraint(0);
    CHECK(sk.skins[1].constraints.empty());
    sk.removeBone(hat);
    CHECK(sk.skins[1].bones.empty());
}

static std::vector<unsigned char> solidImage(int w, int h, unsigned char r, unsigned char g, unsigned char b, int border) {
    std::vector<unsigned char> px(size_t(w) * h * 4, 0);
    for (int y = border; y < h - border; ++y)
        for (int x = border; x < w - border; ++x) {
            unsigned char* p = &px[(size_t(y) * w + x) * 4];
            p[0] = r;
            p[1] = g;
            p[2] = b;
            p[3] = 255;
        }
    return px;
}

static void testAtlas() {
    auto a = solidImage(40, 30, 255, 0, 0, 5);  // 30x20 opaque inside a transparent border
    auto b = solidImage(64, 64, 0, 255, 0, 0);
    auto c = solidImage(10, 50, 0, 0, 255, 0);
    std::vector<AtlasInput> in = {{"a", 40, 30, a.data()}, {"b", 64, 64, b.data()}, {"c", 10, 50, c.data()}};
    AtlasSettings st;
    st.maxWidth = st.maxHeight = 256;
    Atlas atlas;
    std::string err;
    CHECK(packAtlas(in, st, atlas, &err));
    CHECK(atlas.pages.size() == 1 && atlas.regions.size() == 3);
    const AtlasRegion& ra = atlas.regions[0];
    CHECK(ra.width == 30 && ra.height == 20 && ra.offsetX == 5 && ra.offsetY == 5 && ra.originalWidth == 40);
    // No overlaps (padding included) and pixels copied.
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = i + 1; j < 3; ++j) {
            const AtlasRegion &p = atlas.regions[i], &q = atlas.regions[j];
            bool apart = p.x + p.width + st.padding <= q.x || q.x + q.width + st.padding <= p.x || p.y + p.height + st.padding <= q.y ||
                         q.y + q.height + st.padding <= p.y;
            CHECK(apart);
        }
    const AtlasPage& page = atlas.pages[0];
    const unsigned char* px = &page.rgba[(size_t(ra.y) * page.width + ra.x) * 4];
    CHECK(px[0] == 255 && px[1] == 0 && px[3] == 255);
    // Small pages spill onto a second page.
    st.maxWidth = st.maxHeight = 72;
    CHECK(packAtlas(in, st, atlas, &err));
    CHECK(atlas.pages.size() >= 2);
    // Too large for a page.
    st.maxWidth = st.maxHeight = 32;
    CHECK(!packAtlas(in, st, atlas, &err));
    // Rotation lets a tall image fit a wide page.
    AtlasSettings rot;
    rot.maxWidth = 64;
    rot.maxHeight = 20;
    rot.allowRotate = true;
    rot.padding = 0;
    CHECK(packAtlas({{"c", 10, 50, c.data()}}, rot, atlas, &err));
    CHECK(atlas.regions[0].rotated && atlas.regions[0].width == 50);
    CHECK(atlasText(atlas, {"page.png"}).find("rotate: true") != std::string::npos);
}

static void testRaster() {
    // A 20x20 red region at the origin rendered into a 40x40 target (1 world unit = 1 pixel).
    Skeleton sk;
    sk.addBone("root", -1, {}, 0);
    sk.images.push_back({"red.png", 20, 20});
    Slot s;
    s.bone = 0;
    Attachment a;
    a.image = 0;
    s.attachments.push_back(a);
    s.activeAttachment = 0;
    s.color = Color(1, 1, 1, 0.5f);
    sk.slots.push_back(s);
    auto red = solidImage(20, 20, 255, 0, 0, 0);
    std::vector<RasterTexture> tex = {{20, 20, red.data()}};
    Pose pose = makeSetupPose(sk);
    Vec2 lo, hi;
    CHECK(poseBounds(sk, pose, lo, hi) && near(lo, {-10, -10}) && near(hi, {10, 10}));
    Affine toPx{1, 0, 0, -1, 20, 20};
    std::vector<unsigned char> img;
    renderPose(sk, pose, tex, toPx, 40, 40, img);
    const unsigned char* centre = &img[(size_t(20) * 40 + 20) * 4];
    CHECK(centre[0] == 255 && centre[1] == 0 && std::abs(centre[3] - 128) <= 1);
    CHECK(img[(size_t(2) * 40 + 2) * 4 + 3] == 0);
    // Supersampling: 2x render then downsample keeps the size and colour.
    std::vector<unsigned char> big;
    renderPose(sk, pose, tex, Affine{2, 0, 0, -2, 40, 40}, 80, 80, big);
    std::vector<unsigned char> small = downsample(big, 80, 80, 2);
    CHECK(small.size() == img.size() && small[(size_t(20) * 40 + 20) * 4] == 255);
    // GIF: header, loop extension, trailer.
    std::vector<uint8_t> gif = encodeGif({img, img}, 40, 40, 4, true);
    CHECK(gif.size() > 800 && std::string(gif.begin(), gif.begin() + 6) == "GIF89a" && gif.back() == 0x3B);
    CHECK(std::search(gif.begin(), gif.end(), std::begin("NETSCAPE2.0"), std::end("NETSCAPE2.0") - 1) != gif.end());
}

static void testBinaryAndRounding() {
    Skeleton sk = makeMeshRig();
    convertRegionToMesh(sk.slots[0].attachments[0], sk.images[0], nullptr, 2, 3);
    sk.bones[1].setup.rotation = 33.333333f;
    Animation an;
    an.name = "a";
    setRotateKey(an, 1, 0.5f, 12.3456f);
    sk.animations.push_back(an);
    std::vector<uint8_t> bin = toBinary(sk);
    CHECK(bin.size() > 5 && bin[0] == 'R' && bin[3] == 'B');
    Skeleton loaded;
    std::string err;
    CHECK(fromBinary(bin, loaded, &err));
    CHECK(toJson(loaded) == toJson(sk));
    CHECK(bin.size() < toJson(sk, -1).size());
    ExportOptions opt;
    opt.decimals = 2;
    opt.indent = -1;
    std::string rounded = toJson(sk, opt);
    CHECK(rounded.find("33.33") != std::string::npos && rounded.find("33.333") == std::string::npos);
    CHECK(fromJson(rounded, loaded, &err));
}

static void testMerge() {
    Skeleton a = makeMeshRig();
    Animation wave;
    wave.name = "wave";
    setRotateKey(wave, 1, 0, 10);
    a.animations.push_back(wave);
    EventDef e;
    e.name = "hit";
    a.events.push_back(e);
    Skeleton b = a;  // same names everywhere
    b.animations[0].bones.clear();
    setRotateKey(b.animations[0], 2, 0.5f, 20);
    setDrawOrderKey(b.animations[0], 0.25f, {0});
    Animation jump;
    jump.name = "jump";
    setEventKey(jump, 0.1f, 0, e);
    b.animations.push_back(jump);
    Constraint ik;
    ik.name = "ik";
    ik.bones = {1, 2};
    ik.target = 0;
    b.constraints.push_back(ik);
    std::string report = mergeSkeleton(a, b);
    CHECK(!report.empty());
    CHECK(a.bones.size() == 6 && a.bones[3].name == "root2" && a.bones[4].parent == 3);
    CHECK(a.slots.size() == 2 && a.slots[1].bone == 3 && a.slots[1].name != a.slots[0].name);
    CHECK(a.images.size() == 1 && a.events.size() == 1);
    CHECK(a.animations.size() == 2);
    CHECK(a.animations[0].bones.count(1) && a.animations[0].bones.count(5));  // merged "wave": old bone 1 + imported bone 2 -> 5
    CHECK(a.animations[1].events.size() == 1 && a.animations[1].events[0].event == 0);
    CHECK(a.constraints.size() == 1 && a.constraints[0].bones[0] == 4 && a.constraints[0].target == 3);
    CHECK((a.animations[0].drawOrder[0].order == std::vector<int>{0, 1}));
    // The merged skeleton is valid.
    Skeleton check;
    std::string err;
    CHECK(fromJson(toJson(a), check, &err));
}

// Minimal PSD writer for the parser test (8-bit RGB, one raw and one RLE layer).
struct BeWriter {
    std::vector<uint8_t> b;
    void u8(int v) { b.push_back((uint8_t)v); }
    void u16(int v) { u8(v >> 8); u8(v); }
    void u32(uint32_t v) { u16(v >> 16); u16(v & 0xFFFF); }
    void str(const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }
};

static void testPsd() {
    BeWriter w;
    w.str("8BPS");
    w.u16(1);
    for (int i = 0; i < 6; ++i) w.u8(0);
    w.u16(4);
    w.u32(10);  // height
    w.u32(20);  // width
    w.u16(8);
    w.u16(3);
    w.u32(0);  // colour mode data
    w.u32(0);  // image resources
    // Channel data for both layers.
    BeWriter data;
    auto rawChannel = [&](int value, int n) {
        data.u16(0);
        for (int i = 0; i < n; ++i) data.u8(value);
    };
    // Layer 1 (bottom, "背景"): 4x2 at (1,2), raw, channels R G B A.
    for (int v : {200, 100, 50, 255}) rawChannel(v, 8);
    // Layer 2 (top, "hat"): 3x2 at (10,5), RLE rows of "repeat 3x value".
    auto rleChannel = [&](int value) {
        data.u16(1);
        data.u16(2);
        data.u16(2);
        for (int row = 0; row < 2; ++row) {
            data.u8(0xFE);  // -2: repeat next byte 3 times
            data.u8(value);
        }
    };
    for (int v : {10, 20, 30, 128}) rleChannel(v);
    BeWriter layers;
    layers.u16(2);
    auto record = [&](int top, int left, int bottom, int right, const std::string& name, int chLen, bool hidden,
                      const std::string& unicode) {
        layers.u32(top);
        layers.u32(left);
        layers.u32(bottom);
        layers.u32(right);
        layers.u16(4);
        for (int id : {0, 1, 2, -1}) {
            layers.u16(id & 0xFFFF);
            layers.u32(chLen);
        }
        layers.str("8BIMnorm");
        layers.u8(255);
        layers.u8(0);
        layers.u8(hidden ? 0x02 : 0x00);
        layers.u8(0);
        BeWriter extra;
        extra.u32(0);
        extra.u32(0);
        extra.u8((int)name.size());
        extra.str(name);
        while ((extra.b.size() - 8) % 4) extra.u8(0);
        if (!unicode.empty()) {  // "luni" block with UTF-16BE characters
            std::vector<uint16_t> units = {0x80CC, 0x666F};  // 背景
            extra.str("8BIMluni");
            extra.u32(4 + (uint32_t)units.size() * 2);
            extra.u32((uint32_t)units.size());
            for (uint16_t u : units) extra.u16(u);
        }
        layers.u32((uint32_t)extra.b.size());
        layers.b.insert(layers.b.end(), extra.b.begin(), extra.b.end());
    };
    record(2, 1, 4, 5, "bg", 2 + 8, false, "yes");
    record(5, 10, 7, 13, "hat", 2 + 4 + 4, true, "");
    layers.b.insert(layers.b.end(), data.b.begin(), data.b.end());
    w.u32((uint32_t)layers.b.size() + 4);
    w.u32((uint32_t)layers.b.size());
    w.b.insert(w.b.end(), layers.b.begin(), layers.b.end());

    PsdDocument doc;
    std::string err;
    CHECK(parsePsd(w.b, doc, &err));
    CHECK(doc.width == 20 && doc.height == 10 && doc.layers.size() == 2);
    const PsdLayer& bg = doc.layers[0];
    CHECK(bg.name == "\xE8\x83\x8C\xE6\x99\xAF" && bg.left == 1 && bg.top == 2 && bg.width == 4 && bg.height == 2 && bg.visible);
    CHECK(bg.rgba[0] == 200 && bg.rgba[1] == 100 && bg.rgba[2] == 50 && bg.rgba[3] == 255);
    const PsdLayer& hat = doc.layers[1];
    CHECK(hat.name == "hat" && !hat.visible && hat.width == 3);
    CHECK(hat.rgba[(1 * 3 + 2) * 4 + 0] == 10 && hat.rgba[(1 * 3 + 2) * 4 + 3] == 128);
    std::vector<uint8_t> bad = w.b;
    bad[0] = 'X';
    CHECK(!parsePsd(bad, doc, &err));
}

static void testAudioMix() {
    // 0.5 s of a 440 Hz sine at 22050 Hz, full amplitude on both channels.
    AudioSamples tone;
    tone.sampleRate = 22050;
    for (int i = 0; i < 11025; ++i) {
        float v = std::sin(2 * 3.14159265f * 440 * i / 22050.0f);
        tone.stereo.push_back(v);
        tone.stereo.push_back(v);
    }
    CHECK(near(tone.duration(), 0.5f));
    std::vector<float> peaks = computePeaks(tone, 100);
    CHECK(peaks.size() == 51 && peaks[10] > 0.95f);  // 220-frame buckets, the last one partial
    // Placed at 0.25 s on a 1 s, 44100 Hz timeline, panned hard left at half volume.
    std::vector<float> mix = mixAudio({{&tone, 0.25f, 0.5f, -1.0f}}, 1.0f, 44100);
    CHECK(mix.size() == 44100 * 2);
    float before = 0, leftPeak = 0, rightPeak = 0, after = 0;
    for (int f = 0; f < 44100; ++f) {
        float l = std::fabs(mix[f * 2]), r = std::fabs(mix[f * 2 + 1]);
        if (f < 11000) before = std::max(before, l + r);
        else if (f < 33000) {
            leftPeak = std::max(leftPeak, l);
            rightPeak = std::max(rightPeak, r);
        } else if (f > 33200) after = std::max(after, l + r);
    }
    CHECK(before == 0 && after == 0 && near(leftPeak, 0.5f, 0.02f) && rightPeak == 0);
    // Negative start: begins part-way through the sound.
    std::vector<float> late = mixAudio({{&tone, -0.4f, 1, 0}}, 0.2f, 22050);
    float nonzero = 0;
    for (size_t f = 0; f < late.size() / 2; ++f) nonzero += std::fabs(late[f * 2]);
    CHECK(nonzero > 0 && std::fabs(late[(size_t)(0.15f * 22050) * 2]) == 0);
    // Overlaps are clamped.
    std::vector<float> loud = mixAudio({{&tone, 0, 1, 0}, {&tone, 0, 1, 0}}, 0.1f, 22050);
    for (float v : loud) CHECK(v <= 1.0f && v >= -1.0f);
    // WAV header.
    std::vector<uint8_t> wav = encodeWav(mix, 44100);
    CHECK(wav.size() == 44 + mix.size() * 2 && wav[0] == 'R' && wav[8] == 'W' && wav[22] == 2);
    // Event keys with audio.
    Skeleton sk;
    EventDef step, beep;
    step.name = "step";
    beep.name = "beep";
    beep.audioPath = "beep.wav";
    sk.events = {step, beep};
    Animation an;
    setEventKey(an, 0.1f, 0, step);
    setEventKey(an, 0.3f, 1, beep);
    std::vector<AudioEventKey> keys = audioEventKeys(sk, an);
    CHECK(keys.size() == 1 && keys[0].event == 1 && near(keys[0].time, 0.3f));
}

static void testChainIk() {
    // Three 100-long bones along +x; the tip must reach (120, 150).
    Skeleton sk;
    int root = sk.addBone("root", -1, {}, 0);
    int a = sk.addBone("a", root, {}, 100);
    Transform t;
    t.x = 100;
    int b = sk.addBone("b", a, t, 100);
    int c = sk.addBone("c", b, t, 100);
    Pose pose = makeSetupPose(sk);
    std::vector<float> rot = solveChainIk(sk, pose, {a, b, c}, {120, 150}, 30);
    CHECK(rot.size() == 3);
    for (int i = 0; i < 3; ++i) pose.local[std::vector<int>{a, b, c}[i]].rotation = rot[i];
    updateWorld(sk, pose);
    CHECK(near(boneTip(sk.bones[c], pose.world[c]), {120, 150}, 0.5f));
    // Unreachable: the chain points at the target.
    pose = makeSetupPose(sk);
    rot = solveChainIk(sk, pose, {a, b, c}, {0, 1000}, 30);
    for (int i = 0; i < 3; ++i) pose.local[std::vector<int>{a, b, c}[i]].rotation = rot[i];
    updateWorld(sk, pose);
    CHECK(near(pose.world[c].rotationDeg(), 90, 1.0f) && near(pose.world[a].rotationDeg(), 90, 1.0f));
    // A single bone just points at the target.
    pose = makeSetupPose(sk);
    rot = solveChainIk(sk, pose, {c}, {200, 100}, 5);
    CHECK(near(rot[0], 90, 0.5f));
}

static void testJsonRoundTrip() {
    Skeleton sk = makeMeshRig();
    Slot& s = sk.slots[0];
    convertRegionToMesh(s.attachments[0], sk.images[0], nullptr, 2, 2);
    bindMesh(sk, s, s.attachments[0], {1, 2});
    Animation an;
    an.name = "idle";
    setRotateKey(an, 1, 0.5f, 12);
    setTranslateKey(an, 2, 0.25f, {3, 4});
    setAttachmentKey(an, 0, 0.75f, -1);
    setShearKey(an, 1, 0.5f, {5, -5});
    setKeyCurve(an, 1, Channel::Rotate, 0.5f, CurveShape::bezier(0.1f, 0.2f, 0.7f, 1.1f));
    setColorKey(an, 0, 0.25f, Color(1, 0.5f, 0.25f, 1));
    setDrawOrderKey(an, 0.5f, {0});
    sk.bones[1].setup.shearY = 12;
    EventDef ev;
    ev.name = "footstep";
    ev.stringValue = "left";
    ev.audioPath = "step.wav";
    sk.events.push_back(ev);
    setEventKey(an, 0.4f, 0, ev);
    sk.animations.push_back(an);

    std::string text = toJson(sk);
    Skeleton loaded;
    std::string err;
    CHECK(fromJson(text, loaded, &err));
    CHECK(toJson(loaded) == text);

    CHECK(!fromJson("{\"format\":\"rig\",\"images\":[],\"bones\":[{\"name\":\"a\",\"parent\":0}],"
                    "\"slots\":[],\"animations\":[]}",
                    loaded, &err));
}

int main() {
    testWorldTransforms();
    testAnimationSampling();
    testReparentKeepsWorld();
    testMeshSkinning();
    testAlphaTrim();
    testRemoveBone();
    testShearMatrix();
    testCurves();
    testSlotColorAndDrawOrder();
    testEvents();
    testVersion1Compat();
    testTriangulate();
    testOutlineAndMeshBuild();
    testDeform();
    testWeightTools();
    testClipping();
    testPhase2RoundTrip();
    testIk();
    testTransformConstraint();
    testPathConstraint();
    testPhysics();
    testConstraintBookkeeping();
    testCopyPaste();
    testMirrorWeights();
    testSkins();
    testAtlas();
    testRaster();
    testBinaryAndRounding();
    testMerge();
    testPsd();
    testAudioMix();
    testChainIk();
    testJsonRoundTrip();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all core tests passed\n");
    return 0;
}
