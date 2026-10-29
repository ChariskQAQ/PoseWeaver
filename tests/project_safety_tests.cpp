#include "core/diagnostics.h"
#include "core/file_io.h"
#include "core/serialize.h"
#include "core/project_resources.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <map>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace rig;
namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (false)

static std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << file.rdbuf();
    return bytes.str();
}

static Skeleton fixture() {
    Skeleton sk;
    sk.addBone("root", -1, {}, 10);
    sk.images.push_back({"image.png", 10, 10});
    Slot slot;
    slot.name = "body";
    slot.activeAttachment = 0;
    Attachment mesh;
    mesh.name = "body-mesh";
    mesh.type = AttachmentType::Mesh;
    mesh.image = 0;
    mesh.uvs = {{0, 0}, {1, 0}, {0, 1}};
    mesh.triangles = {0, 1, 2};
    mesh.hullLength = 3;
    mesh.weights = {{{0, {0, 0}, 1}}, {{0, {10, 0}, 1}}, {{0, {0, 10}, 1}}};
    slot.attachments.push_back(mesh);
    sk.slots.push_back(slot);
    Animation animation;
    animation.name = "idle";
    setRotateKey(animation, 0, 0, 0);
    setRotateKey(animation, 0, 1, 30);
    sk.animations.push_back(animation);
    return sk;
}

static bool has(const std::vector<Diagnostic>& issues, const std::string& code) {
    for (const auto& issue : issues) if (issue.code == code) return true;
    return false;
}

static std::map<fs::path, std::string> filesUnder(const fs::path& directory) {
    std::map<fs::path, std::string> files;
    if (fs::exists(directory)) for (const auto& file : fs::recursive_directory_iterator(directory))
        files[file.path().lexically_relative(directory)] = file.is_regular_file() ? read(file.path()) : "<directory>";
    return files;
}

static void testCollectedResources(const fs::path& root) {
    const fs::path sourceA = root / fs::u8path("来源甲"), sourceB = root / fs::u8path("来源乙");
    const fs::path destination = root / fs::u8path("打包工程");
    fs::create_directory(sourceA);
    fs::create_directory(sourceB);
    fs::create_directory(destination);
    std::string error;
    const fs::path imageA = sourceA / fs::u8path("角色.png"), imageB = sourceB / fs::u8path("角色.png");
    CHECK(writeFileAtomic(imageA.u8string(), "first image", &error));
    CHECK(writeFileAtomic(imageB.u8string(), "different image", &error));
    CHECK(writeFileAtomic((sourceA / "voice.wav").u8string(), "audio fixture", &error));
    Skeleton input = fixture(), output;
    input.images = {{imageA.lexically_relative(root).u8string(), 10, 10},
                    {imageB.lexically_relative(root).u8string(), 10, 10},
                    {imageA.lexically_relative(root).u8string(), 10, 10}};
    EventDef sound;
    sound.name = "voice";
    sound.audioPath = (sourceA / "voice.wav").lexically_relative(root).u8string();
    input.events.push_back(sound);
    const std::string before = toJson(input);
    const fs::path project = destination / fs::u8path("角色.rig.json");
    const fs::path assets = destination / fs::u8path("角色_assets");
    fs::create_directories(assets / "resources/images");
    const fs::path existing = assets / "resources/images" / fs::u8path("角色.png");
    CHECK(writeFileAtomic(existing.u8string(), "existing asset", &error));
    CHECK(saveCollectedProject(project.u8string(), input, root.u8string(), output, &error));
    CHECK(toJson(input) == before && read(existing) == "existing asset");
    CHECK(fs::u8path(output.images[0].path).is_absolute());
    CHECK(output.images[0].path == output.images[2].path && output.images[0].path != output.images[1].path);
    CHECK(read(fs::u8path(output.images[0].path)) == "first image");
    CHECK(read(fs::u8path(output.images[1].path)) == "different image");
    CHECK(read(fs::u8path(output.events[0].audioPath)) == "audio fixture");
    CHECK(read(imageA) == "first image" && read(imageB) == "different image");
    Skeleton stored;
    CHECK(loadFile(project.u8string(), stored, &error));
    for (const auto& image : stored.images) {
        CHECK(!fs::u8path(image.path).is_absolute());
        CHECK(fs::is_regular_file(destination / fs::u8path(image.path)));
    }
    CHECK(!fs::u8path(stored.events[0].audioPath).is_absolute());
    const auto files = filesUnder(assets);
    const std::string projectBytes = read(project), collectedSnapshot = toJson(output);
    CHECK(saveCollectedProject(project.u8string(), output, destination.u8string(), output, &error));
    CHECK(filesUnder(assets) == files && toJson(output) == collectedSnapshot);
    Skeleton missing = input;
    missing.events[0].audioPath = "absent.wav";
    CHECK(!saveCollectedProject(project.u8string(), missing, root.u8string(), output, &error));
    CHECK(filesUnder(assets) == files && read(project) == projectBytes && toJson(output) == collectedSnapshot);
    Skeleton invalid = input;
    invalid.bones[0].setup.x = std::numeric_limits<float>::quiet_NaN();
    CHECK(!saveCollectedProject(project.u8string(), invalid, root.u8string(), output, &error));
    CHECK(filesUnder(assets) == files && read(project) == projectBytes && toJson(output) == collectedSnapshot);
#ifdef _WIN32
    fs::permissions(imageA, fs::perms::owner_write, fs::perm_options::remove);
    HANDLE lock = CreateFileW(project.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(lock != INVALID_HANDLE_VALUE);
    CHECK(!saveCollectedProject(project.u8string(), input, root.u8string(), output, &error));
    CHECK(filesUnder(assets) == files && read(project) == projectBytes && toJson(output) == collectedSnapshot);
    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    fs::permissions(imageA, fs::perms::owner_write, fs::perm_options::add);
#endif
    // A destination-write failure also removes any newly created collection folder.
    const fs::path blocked = destination / "blocked.rig.json";
    fs::create_directory(blocked);
    CHECK(!saveCollectedProject(blocked.u8string(), input, root.u8string(), output, &error));
    CHECK(fs::is_directory(blocked) && !fs::exists(destination / "blocked_assets"));
    // The project can be moved with its resource folder after the original sources disappear.
    const fs::path moved = root / "moved";
    fs::copy(destination, moved, fs::copy_options::recursive);
    fs::remove(imageA);
    fs::remove(imageB);
    fs::remove(sourceA / "voice.wav");
    CHECK(loadFile((moved / project.filename()).u8string(), stored, &error));
    CHECK(read(moved / fs::u8path(stored.images[0].path)) == "first image");
    CHECK(read(moved / fs::u8path(stored.images[1].path)) == "different image");
    CHECK(read(moved / fs::u8path(stored.events[0].audioPath)) == "audio fixture");
}

int main() {
    const fs::path parent = fs::current_path();
    const fs::path root = parent / ("poseweaver-safety-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!fs::create_directory(root)) return 1;
    const fs::path project = root / fs::u8path("安全保存.rig.json");
    std::string error;
    Skeleton sk = fixture(), loaded;
    CHECK(saveFile(project.u8string(), sk, &error));
    CHECK(loadFile(project.u8string(), loaded, &error));
    CHECK(toJson(loaded) == toJson(sk));

    // Invalid edits must fail before an existing destination is touched.
    const std::string original = read(project);
    Skeleton invalid = sk;
    invalid.bones[0].setup.x = std::numeric_limits<float>::quiet_NaN();
    CHECK(!saveFile(project.u8string(), invalid, &error));
    CHECK(read(project) == original);
    CHECK(!error.empty());

    // Replacement failure must preserve the old file and remove only our temporary file.
#ifdef _WIN32
    HANDLE lock = CreateFileW(project.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(lock != INVALID_HANDLE_VALUE);
    sk.bones[0].setup.x = 25;
    CHECK(!saveFile(project.u8string(), sk, &error));
    CHECK(read(project) == original);
    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
#endif
    CHECK(saveFile(project.u8string(), sk, &error));
    CHECK(loadFile(project.u8string(), loaded, &error) && loaded.bones[0].setup.x == sk.bones[0].setup.x);
    const fs::path directory = root / "destination-directory";
    fs::create_directory(directory);
    CHECK(!writeFileAtomic(directory.u8string(), "new data", &error));
    CHECK(fs::is_directory(directory));
    for (const auto& entry : fs::directory_iterator(root)) CHECK(entry.path().filename().u8string().find(".tmp-") == std::string::npos);

    ExportOptions binary;
    binary.binary = true;
    const fs::path packed = root / "export.rigb";
    CHECK(exportFile(packed.u8string(), sk, binary, &error));
    CHECK(loadFile(packed.u8string(), loaded, &error) && toJson(loaded) == toJson(sk));

    // Warnings are repairable; unsafe topology/references are refused at load boundaries.
    CHECK(has(diagnose(sk, root.u8string()), "MISSING_IMAGE"));
    for (const auto& issue : diagnose(sk, root.u8string())) if (issue.code == "MISSING_IMAGE")
        CHECK(fs::u8path(issue.path) == root / "image.png");
    CHECK(writeFileAtomic((root / "image.png").u8string(), "fixture", &error));
    CHECK(!has(diagnose(sk, root.u8string()), "MISSING_IMAGE"));
    Skeleton warning = sk;
    warning.slots[0].attachments[0].weights[1][0].weight = 0.5f;
    const auto warnings = diagnose(warning, {}, false);
    CHECK(has(warnings, "WEIGHT_SUM"));
    for (const auto& issue : warnings) if (issue.code == "WEIGHT_SUM") CHECK(issue.index == 0 && issue.sub == 0 && issue.vertex == 1);
    CHECK(fromJson(toJson(warning), loaded, &error));

    invalid = sk;
    invalid.slots[0].attachments[0].triangles.pop_back();
    CHECK(!fromJson(toJson(invalid), loaded, &error));
    invalid = sk;
    invalid.slots[0].activeAttachment = -2;
    CHECK(!fromJson(toJson(invalid), loaded, &error));
    invalid = sk;
    Skin skin;
    skin.name = "alternate";
    skin.attachments.push_back({0, "body-mesh", sk.slots[0].attachments[0]});
    skin.attachments[0].attachment.weights[0][0].bone = 999;
    invalid.skins.push_back(skin);
    CHECK(has(diagnose(invalid, {}, false), "INVALID_WEIGHT"));
    CHECK(!fromJson(toJson(invalid), loaded, &error));
    invalid = sk;
    invalid.bones.push_back({"child", 0, 1, {}});
    invalid.bones[0].parent = 1;
    CHECK(has(diagnose(invalid, {}, false), "BONE_CYCLE"));
    CHECK(!fromJson(toJson(invalid), loaded, &error));
    invalid = sk;
    invalid.animations[0].bones[0].rotate[0].time = -1;
    CHECK(!fromJson(toJson(invalid), loaded, &error));
    invalid = sk;
    invalid.animations[0].bones[0].rotate[1].curve = CurveShape::bezier(2, 0, 0.5f, 1);
    for (const auto& issue : diagnose(invalid, {}, false)) if (issue.code == "INVALID_KEY")
        CHECK(issue.target == IssueTarget::Bone && issue.index == 0 && issue.animation == 0 && issue.time == 1);
    loaded = sk;
    CHECK(!fromJson("{broken", loaded, &error));
    CHECK(toJson(loaded) == toJson(sk));

    testCollectedResources(root);

    // root was created by this process as a direct child of the test working directory.
    if (root.parent_path() == parent) fs::remove_all(root);
    std::printf("project safety: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
