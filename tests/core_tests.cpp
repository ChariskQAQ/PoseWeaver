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
    testJsonRoundTrip();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all core tests passed\n");
    return 0;
}
