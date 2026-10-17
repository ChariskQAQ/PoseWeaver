// Minimal self-checking tests for rigcore (no framework dependency).
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
    setKeyCurve(an, 1, Channel::Rotate, 0.0f, Curve::Stepped);
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
    testJsonRoundTrip();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all core tests passed\n");
    return 0;
}
