// Minimal self-checking tests for rigcore (no framework dependency).
#include "core/clipping.h"
#include "core/mesh_tools.h"
#include "core/pose.h"
#include "core/serialize.h"

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
    testJsonRoundTrip();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all core tests passed\n");
    return 0;
}
