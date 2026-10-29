#include "editor/editor.h"
#include "editor/i18n.h"
#include "core/serialize.h"
#include "core/file_io.h"
#include "core/mesh_tools.h"
#include <imgui_internal.h>
#include <json.hpp>
#ifdef _WIN32
#include <objbase.h>
#endif
#include <GLFW/glfw3.h>

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cmath>
#include <limits>
#include <thread>

namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (false)

// Exercise the editor's actual actions without driving native file picker dialogs.
struct EditorTestAccess {
    static rig::Skeleton& skeleton(Editor& e) { return e.sk_; }
    static bool save(Editor& e, const fs::path& path, bool collect = false) { return e.saveProjectTo(path.u8string(), collect); }
    static bool dirty(const Editor& e) { return e.dirty_; }
    static bool blend(Editor& e, int slot, rig::BlendMode mode) { return e.setSlotBlend(slot, mode); }
    static void setup(Editor& e) { e.setMode(Mode::Setup); }
    static bool pending(const Editor& e) { return (bool)e.pendingDocumentChange_; }
    static void move(Editor& e, float x) {
        e.setMode(Mode::Setup);
        e.checkpoint();
        auto t = e.sk_.bones[1].setup;
        t.x = x;
        e.transformBone(1, t, false, true, false, false);
    }
    static void moveLocal(Editor& e, float x) {
        e.setMode(Mode::Setup); e.checkpointBone(1);
        auto t = e.sk_.bones[1].setup; t.x = x;
        e.transformBone(1,t,false,true,false,false);
    }
    static void renameLocal(Editor& e,const std::string& name) { e.selectBone(1); e.renameSelection(name); }
    static void weightsLocal(Editor& e) {
        e.setMode(Mode::Setup); e.selectAttachment(0,0); e.checkpointAttachment();
        auto* attachment = e.selectedAttachment();
        const int bone = vertexWeight(*attachment,0,2) > 0.5f ? 1 : 2;
        setWeight(e.sk_,e.sk_.slots[0],*attachment,0,bone,0.9f,{});
    }
    static void unbindLocal(Editor& e) { e.checkpointAttachment(); unbindMesh(e.sk_,e.sk_.slots[0],*e.selectedAttachment()); }
    static void skinActive(Editor& e,int skin) { e.activeSkins_={skin}; }
    static void addBoneFull(Editor& e) { e.checkpoint(); e.sk_.addBone("extra",0,{},5); }
    static bool lastIsLocal(const Editor& e) { return !e.undo_.empty() && e.undo_.back().kind != Editor::UndoEntry::Document && e.undo_.back().snapshot.empty(); }
    static void badLocalUndo(Editor& e) { Editor::UndoEntry entry; entry.kind = Editor::UndoEntry::Bone; entry.index = 999999; e.undo_.push_back(std::move(entry)); }
    static void trimLocalUndo(Editor& e) {
        e.settings_.undoMemoryMiB = 16;
        e.undo_.back().attachment.weights.reserve(1024*1024);
        e.trimUndoHistory();
    }
    static void compensateBone(Editor& e,bool enabled) { e.compBones_=enabled; }
    static void key(Editor& e) {
        e.setMode(Mode::Animate);
        e.setTime(0.4f);
        e.checkpoint();
        auto t = e.pose_.local[1];
        t.rotation += 15;
        e.setBoneLocal(1, t, true, false, false);
    }
    static void undo(Editor& e) { e.undo(); }
    static void redo(Editor& e) { e.redo(); }
    static void newProject(Editor& e) { e.newProject(); }
    static bool cancel(Editor& e) { return e.resolveDocumentChange(Editor::ChangeDecision::Cancel); }
    static bool discard(Editor& e) { return e.resolveDocumentChange(Editor::ChangeDecision::Discard); }
    static bool saveAndContinue(Editor& e) { return e.resolveDocumentChange(Editor::ChangeDecision::Save); }
    static void autosave(Editor& e, float dt, bool editing = false) { e.updateRecovery(dt, editing); }
    static std::string recovery(const Editor& e) { return e.recoveryPath(); }
    static bool available(const Editor& e) { return e.recoveryAvailable_; }
    static bool recover(Editor& e) { return e.restoreRecovery(); }
    static void check(Editor& e) { e.refreshIssues(); }
    static const std::vector<rig::Diagnostic>& issues(const Editor& e) { return e.issues_; }
    static void focus(Editor& e, const rig::Diagnostic& issue) { e.focusIssue(issue); }
    static Selection selection(const Editor& e) { return e.sel_; }
    static void breakSavePath(Editor& e, const fs::path& path) { e.projectPath_ = path.u8string(); }
    static bool relink(Editor& e, int image, const std::string& path) { return e.relinkImage(image, path); }
    static bool autoMesh(Editor& e) { return e.autoMesh(0, 0); }
    static float time(const Editor& e) { return e.time_; }
    static int animation(const Editor& e) { return e.anim_; }
    static void hideTreeObjects(Editor& e) { e.treeFilter_ = "unrelated"; e.treeShowBones_ = e.treeShowSlots_ = e.treeShowAttachments_ = false; }
    static bool treeVisible(const Editor& e) { return e.treeFilter_.empty() && e.treeShowBones_ && e.treeShowSlots_ && e.treeShowAttachments_; }
    static bool canFocus(const Editor& e, const rig::Diagnostic& issue) { return e.canFocusIssue(issue); }
    static rig::Attachment* attachment(Editor& e) { return e.selectedAttachment(); }
    static void ffmpeg(Editor& e, const std::string& path) { e.io_.ffmpegPath = path; }
    static size_t undoSize(const Editor& e) { return e.undo_.size(); }
    static size_t redoSize(const Editor& e) { return e.redo_.size(); }
    static uint64_t undoBytes(const Editor& e) { return e.undoMemoryBytes(); }
    static void budgetUndo(Editor& e, size_t reserve) {
        e.settings_.undoMemoryMiB = 16;
        for (auto& item : e.undo_) item.snapshot.reserve(reserve);
        e.trimUndoHistory(); e.saveSettings();
    }
    static int undoBudget(const Editor& e) { return e.settings_.undoMemoryMiB; }
    static ImageExportJob* imageJob(Editor& e) { return e.imageJob_.get(); }
    static bool startImages(Editor& e, const fs::path& path) { e.io_.imageAnim = 0; e.io_.imageSupersample = 1; return e.startImageExport(path.u8string()); }
    static void pollImages(Editor& e) { e.pollImageExport(); }
    static const std::string& projectPath(const Editor& e) { return e.projectPath_; }
    static bool commit(Editor& e, rig::Skeleton document) { return e.commitDocumentChange(std::move(document)); }
    static void setCollect(Editor& e, bool enabled) { e.settings_.collectResources = enabled; e.saveSettings(); }
    static bool collect(const Editor& e) { return e.settings_.collectResources; }
    static void badUndo(Editor& e) { e.undo_.push_back({"{broken", 999}); }
    static void engineOptions(Editor& e, float fps, float scale, bool loop = true) {
        e.engineOptions_.fps = fps; e.engineOptions_.scale = scale; e.engineOptions_.supersample = 1;
        e.engineOptions_.pixelsPerUnit = 64;
        e.syncEngineClips();
        for (auto& clip : e.engineOptions_.clips) clip.loop = loop;
    }
    static EngineExportOptions& engine(Editor& e) { return e.engineOptions_; }
    static EngineExportBudget budget(const Editor& e) { return e.engineBudget_; }
    static EngineExportJob* job(Editor& e) { return e.engineJob_.get(); }
    static bool startEngine(Editor& e, bool estimate) { return e.startEngineExport(estimate); }
    static void pollEngine(Editor& e) { e.pollEngineExport(); }
    static void saveEngine(Editor& e) { e.saveEngineExportSettings(); }
    static bool engineError(const Editor& e) { return e.engineNoticeError_; }
    static bool effects(Editor& e, const rig::RenderEffects& value) { return e.setRenderEffects(value); }
    static void showEffects(Editor& e) { e.showEffects_ = true; }
    static const Texture& effectsTexture(const Editor& e) { return e.effectsTexture_; }
    static uint64_t effectsStamp(const Editor& e) { return e.effectsHistory_; }
    static void renameOnly(Editor& e) { e.checkpoint(); e.sk_.bones[0].name += "_renamed"; }
    static void attachmentDrag(Editor& e) {
        auto& a = e.sk_.slots[0].attachments[0];
        if (!a.weights.empty()) a.weights[0][0].local.x += 15;
        else if (!a.vertices.empty()) a.vertices[0].x += 15;
        else a.transform.x += 15;
    }
    static bool scanResources(Editor& e,const fs::path& directory) {
        if (!e.scanResourceMatches(directory.u8string())) return false;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while (e.resourceScan_ && std::chrono::steady_clock::now()<deadline) { e.pollResourceScan(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        return !e.resourceScan_ && e.resourceScanNotice_.empty();
    }
    static bool startScan(Editor& e,const fs::path& path) { return e.scanResourceMatches(path.u8string()); }
    static void cancelScan(Editor& e) { if (e.resourceScan_) e.resourceScan_->progress.cancelled=true; }
    static void finishScan(Editor& e) { if (e.resourceScan_) e.resourceScan_->result.wait(); e.pollResourceScan(); }
    static size_t pendingExports(Editor& e) { return e.exportRecoveries_.size(); }
    static bool recoverExport(Editor& e) { return e.startExportRecovery(0); }
    static void finishExportRecovery(Editor& e) { if (e.exportRecoveryTask_.valid()) e.exportRecoveryTask_.wait(); e.pollExportRecovery(); }
    static auto retainPixels(Editor& e) { return e.textures_[0].pixels; }
    static void unloadPixels(Editor& e) { releaseTexture(e.textures_[0]); }
    static auto& matches(Editor& e) { return e.resourceMatches_; }
    static bool applyResources(Editor& e) { return e.applyResourceMatches(); }
    static ImageExportOptions& images(Editor& e) { return e.io_; }
    static std::string& imagePath(Editor& e) { return e.imageOutputPath_; }
    static void saveImages(Editor& e) { e.saveImageExportSettings(); }
    static void gpuEffects(Editor& e, bool enabled) { e.setEffectsGpu(enabled); }
    static bool gpuSelected(const Editor& e) { return e.settings_.effectsGpu; }
    static bool usingGpu(const Editor& e) { return e.effectsUsingGpu_; }
    static unsigned int gpuTexture(const Editor& e) { return e.effectsGpu_.texture(); }
    static void failGpu(Editor& e) { e.effectsCacheValid_ = false; glEnable(0xffffffff); }
    static std::vector<unsigned char> sourcePixels(const Editor& e) { return e.textures_[0].rgba(); }
    static void panEffects(Editor& e) { e.viewOffset_.x += 50; e.zoom_ *= 1.5f; }
    static bool texturesLoaded(const Editor& e) {
        for (const auto& texture : e.textures_) if (!texture.id || texture.rgba().empty()) return false;
        return !e.textures_.empty();
    }
};

static std::string snapshot(Editor& e) { return rig::toJson(EditorTestAccess::skeleton(e)); }
static bool hasIssue(const Editor& e, const char* code) {
    for (const auto& issue : EditorTestAccess::issues(e)) if (issue.code == code) return true;
    return false;
}
struct ExportTransactionTestAccess {
    static void interrupt(ExportTransaction& transaction) { transaction.prepare(); transaction.preserve_=true; }
};

static bool waitEngine(Editor& editor) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (EditorTestAccess::job(editor) && std::chrono::steady_clock::now() < deadline) {
        EditorTestAccess::pollEngine(editor);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(!EditorTestAccess::job(editor));
    return !EditorTestAccess::job(editor);
}

int main() {
#ifdef _WIN32
    struct UiApartment {
        HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ~UiApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    } apartment;
    CHECK(SUCCEEDED(apartment.result));
#endif
    if (!glfwInit()) { std::puts("GLFW initialization failed"); return 1; }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(64, 64, "Editor regression", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplaySize = ImVec2(1280, 800);
    io.DeltaTime = 1.0f / 60;
    unsigned char* pixels = nullptr;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const fs::path parent = fs::current_path();
    const fs::path root = parent / ("poseweaver-editor-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!fs::create_directory(root)) return 1;
    const std::string arm = (fs::u8path(POSEWEAVER_SOURCE_DIR) / "samples/arm.png").u8string();
    const fs::path project = root / fs::u8path("角色.rig.json");
    std::string expected, error;
    i18n::setLanguage(i18n::Lang::En);
    CHECK(std::string(tr("问题")) == "Issues");
    {
        Editor editor((root / "profile").u8string());
        editor.buildDemo(arm, 0, "");
        CHECK(EditorTestAccess::skeleton(editor).bones.size() == 3);
        CHECK(EditorTestAccess::save(editor, project));
        const std::string saved = snapshot(editor);
        EditorTestAccess::move(editor, 42);
        CHECK(EditorTestAccess::dirty(editor));
        EditorTestAccess::undo(editor);
        CHECK(snapshot(editor) == saved && !EditorTestAccess::dirty(editor));
        EditorTestAccess::redo(editor);
        CHECK(EditorTestAccess::dirty(editor));
        EditorTestAccess::key(editor);
        expected = snapshot(editor);
        CHECK(EditorTestAccess::save(editor, project));
        CHECK(!EditorTestAccess::dirty(editor));

        // Every destructive action must defer until a decision is made.
        EditorTestAccess::move(editor, 99);
        const std::string edited = snapshot(editor);
        EditorTestAccess::newProject(editor);
        CHECK(EditorTestAccess::pending(editor) && snapshot(editor) == edited);
        CHECK(EditorTestAccess::cancel(editor) && snapshot(editor) == edited);
        editor.onFilesDropped({project.u8string(), arm});
        CHECK(EditorTestAccess::pending(editor) && snapshot(editor) == edited);
        CHECK(EditorTestAccess::discard(editor));
        CHECK(snapshot(editor) == expected);
        CHECK(!EditorTestAccess::dirty(editor));

        EditorTestAccess::move(editor, 123);
        CHECK(!editor.requestClose());
        CHECK(EditorTestAccess::cancel(editor) && !editor.shouldQuit());
        EditorTestAccess::newProject(editor);
        CHECK(EditorTestAccess::saveAndContinue(editor));
        CHECK(EditorTestAccess::skeleton(editor).bones.size() == 1);
        CHECK(editor.openProject(project.u8string()));
        CHECK(EditorTestAccess::skeleton(editor).bones[1].setup.x == 123);

        // A failed save must leave the pending action and all edits intact.
        EditorTestAccess::move(editor, 222);
        EditorTestAccess::breakSavePath(editor, root / "absent-directory/fail.rig.json");
        const std::string beforeFailure = snapshot(editor);
        EditorTestAccess::newProject(editor);
        CHECK(!EditorTestAccess::saveAndContinue(editor));
        CHECK(EditorTestAccess::pending(editor) && snapshot(editor) == beforeFailure);
        EditorTestAccess::cancel(editor);
        CHECK(EditorTestAccess::save(editor, project));
        expected = snapshot(editor);
        CHECK(!editor.openProject((root / "missing.rig.json").u8string()));
        CHECK(snapshot(editor) == expected);

        // Timer snapshots remain separate from the primary project and survive process loss.
        EditorTestAccess::move(editor, 333);
        expected = snapshot(editor);
        EditorTestAccess::autosave(editor, 29);
        CHECK(!fs::exists(fs::u8path(EditorTestAccess::recovery(editor))));
        EditorTestAccess::autosave(editor, 2, true);
        CHECK(!fs::exists(fs::u8path(EditorTestAccess::recovery(editor))));
        EditorTestAccess::autosave(editor, 0);
        CHECK(fs::exists(fs::u8path(EditorTestAccess::recovery(editor))));
        rig::Skeleton savedProject;
        CHECK(rig::loadFile(project.u8string(), savedProject, &error));
        CHECK(savedProject.bones[1].setup.x == 222);
    }
    {
        Editor recovered((root / "profile").u8string());
        CHECK(EditorTestAccess::available(recovered));
        CHECK(EditorTestAccess::recover(recovered));
        CHECK(snapshot(recovered) == expected && EditorTestAccess::dirty(recovered));
        CHECK(EditorTestAccess::save(recovered, root / "recovered.rig.json"));
        CHECK(!fs::exists(fs::u8path(EditorTestAccess::recovery(recovered))));
        CHECK(recovered.runExport("json", (root / "runtime.json").u8string()));
        rig::Skeleton runtime;
        CHECK(rig::loadFile((root / "runtime.json").u8string(), runtime, &error));
        CHECK(runtime.bones[1].setup.x == 333);

        // Resource diagnostics locate actual image objects and stop incomplete rendering.
        auto& sk = EditorTestAccess::skeleton(recovered);
        const std::string image = sk.images[0].path;
        sk.images[0].path = (root / "absent.png").u8string();
        EditorTestAccess::check(recovered);
        CHECK(hasIssue(recovered, "MISSING_IMAGE"));
        for (const auto& issue : EditorTestAccess::issues(recovered)) if (issue.code == "MISSING_IMAGE") {
            EditorTestAccess::focus(recovered, issue);
            CHECK(EditorTestAccess::selection(recovered).kind == Selection::Image);
        }
        CHECK(!recovered.runExport("png", (root / "incomplete.png").u8string()));
        EditorTestAccess::check(recovered);
        for (const auto& issue : EditorTestAccess::issues(recovered)) if (issue.code == "EXPORT_FAILED")
            CHECK(issue.target == rig::IssueTarget::Image && issue.index == 0 && !issue.path.empty() && !issue.detail.empty());
        const std::string missingImage = sk.images[0].path;
        CHECK(!EditorTestAccess::relink(recovered, 0, (root / "still-absent.png").u8string()));
        CHECK(sk.images[0].path == missingImage);
        CHECK(EditorTestAccess::relink(recovered, 0, image));
        CHECK(!hasIssue(recovered, "MISSING_IMAGE"));
        EditorTestAccess::undo(recovered);
        CHECK(sk.images[0].path == missingImage);
        EditorTestAccess::redo(recovered);
        CHECK(sk.images[0].path == image);
        // Operation failures survive status expiry, carry paths, and are replaced on retry.
        const fs::path failedData = root / "absent-directory/runtime.json";
        CHECK(!recovered.runExport("json", failedData.u8string()));
        CHECK(!recovered.runExport("json", failedData.u8string()));
        EditorTestAccess::check(recovered);
        int dataErrors = 0;
        for (const auto& issue : EditorTestAccess::issues(recovered)) if (issue.code == "DATA_EXPORT_FAILED") {
            ++dataErrors;
            CHECK(issue.path == failedData.u8string() && !issue.message.empty());
            CHECK(!EditorTestAccess::canFocus(recovered, issue));
        }
        CHECK(dataErrors == 1);
        // An earlier file-operation error must not block a valid image export.
        CHECK(recovered.runExport("png", (root / fs::u8path("动画.png")).u8string()));
        CHECK(fs::exists(root / fs::u8path("动画_0000.png")));
        CHECK(recovered.runExport("json", (root / "runtime.json").u8string()));
        EditorTestAccess::check(recovered);
        CHECK(!hasIssue(recovered, "DATA_EXPORT_FAILED"));
        for (const char* type : {"png", "jpg", "sheet", "gif", "atlas"}) {
            CHECK(!recovered.runExport(type, (root / "absent-directory/output").u8string()));
            EditorTestAccess::check(recovered);
            CHECK(hasIssue(recovered, std::string(type) == "atlas" ? "ATLAS_EXPORT_FAILED" : "IMAGE_EXPORT_FAILED"));
        }
        const std::string beforeImport = snapshot(recovered);
        CHECK(!recovered.runExport("merge", (root / "missing.rig.json").u8string()));
        CHECK(!recovered.runExport("psd", (root / "missing.psd").u8string()));
        CHECK(snapshot(recovered) == beforeImport);
        EditorTestAccess::check(recovered);
        CHECK(hasIssue(recovered, "IMPORT_FAILED") && hasIssue(recovered, "PSD_IMPORT_FAILED"));
        EditorTestAccess::ffmpeg(recovered, (root / "missing-ffmpeg.exe").u8string());
        CHECK(!recovered.runExport("mp4", (root / "video.mp4").u8string()));
        EditorTestAccess::check(recovered);
        CHECK(hasIssue(recovered, "VIDEO_EXPORT_FAILED"));
        // A mesh failure points back to the attachment, including when the tree is filtered.
        const int imageIndex = sk.slots[0].attachments[0].image;
        sk.slots[0].attachments[0].image = -1;
        CHECK(!EditorTestAccess::autoMesh(recovered));
        EditorTestAccess::check(recovered);
        CHECK(hasIssue(recovered, "MESH_EDIT_FAILED"));
        for (const auto& issue : EditorTestAccess::issues(recovered)) if (issue.code == "MESH_EDIT_FAILED") {
            EditorTestAccess::hideTreeObjects(recovered);
            EditorTestAccess::focus(recovered, issue);
            CHECK(EditorTestAccess::selection(recovered).kind == Selection::Attachment);
            CHECK(EditorTestAccess::treeVisible(recovered));
        }
        sk.slots[0].attachments[0].image = imageIndex;
        // Invalid animation keys retain their object and exact time for navigation.
        auto& key = sk.animations[0].bones[2].rotate[1];
        const rig::CurveShape originalCurve = key.curve;
        key.curve = rig::CurveShape::bezier(2, 0, 0.5f, 1);
        EditorTestAccess::check(recovered);
        CHECK(hasIssue(recovered, "INVALID_KEY"));
        for (const auto& issue : EditorTestAccess::issues(recovered)) if (issue.code == "INVALID_KEY") {
            CHECK(issue.target == rig::IssueTarget::Bone && issue.index == 2 && issue.animation == 0 && issue.time == 0.5f);
            EditorTestAccess::focus(recovered, issue);
            CHECK(EditorTestAccess::selection(recovered).kind == Selection::Bone && EditorTestAccess::selection(recovered).bone == 2);
            CHECK(EditorTestAccess::animation(recovered) == 0 && EditorTestAccess::time(recovered) == 0.5f);
        }
        key.curve = originalCurve;
        rig::Diagnostic stale;
        stale.target = rig::IssueTarget::Attachment;
        stale.index = 999;
        CHECK(!EditorTestAccess::canFocus(recovered, stale));
        // Locating an inactive attachment must not accidentally edit the active one.
        sk.slots[0].attachments.push_back(sk.slots[0].attachments[0]);
        sk.slots[0].attachments[1].name = "inactive mesh";
        sk.slots[0].attachments[1].weights[0][0].weight *= 0.5f;
        const std::string beforeLocate = snapshot(recovered);
        EditorTestAccess::check(recovered);
        for (const auto& issue : EditorTestAccess::issues(recovered)) if (issue.code == "WEIGHT_SUM" && issue.sub == 1) {
            EditorTestAccess::focus(recovered, issue);
            CHECK(EditorTestAccess::selection(recovered).attachment == 1);
            CHECK(EditorTestAccess::attachment(recovered) == &sk.slots[0].attachments[1]);
            CHECK(snapshot(recovered) == beforeLocate && sk.slots[0].activeAttachment == 0);
        }
        sk.slots[0].attachments.pop_back();
        auto& weight = sk.slots[0].attachments[0].weights[0][0].weight;
        const float originalWeight = weight;
        weight *= 0.5f;
        EditorTestAccess::check(recovered);
        CHECK(hasIssue(recovered, "WEIGHT_SUM"));
        for (const auto& issue : EditorTestAccess::issues(recovered)) if (issue.code == "WEIGHT_SUM") {
            EditorTestAccess::focus(recovered, issue);
            const auto selection = EditorTestAccess::selection(recovered);
            CHECK(selection.kind == Selection::Attachment && selection.slot == 0 && selection.attachment == 0);
            CHECK(selection.vertices == std::vector<int>{0});
        }
        weight = originalWeight;
        recovered.openIssues();
        ImGui::NewFrame();
        recovered.frame(1.0f / 60);
        ImGui::Render();
        CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    }
    {
        Editor corrupt((root / "corrupt-profile").u8string());
        const std::string file = EditorTestAccess::recovery(corrupt);
        CHECK(rig::writeFileAtomic(file, "{broken recovery", &error));
        const std::string original = snapshot(corrupt);
        CHECK(!EditorTestAccess::recover(corrupt));
        CHECK(snapshot(corrupt) == original && fs::exists(fs::u8path(file)));
        EditorTestAccess::check(corrupt);
        CHECK(hasIssue(corrupt, "RECOVERY_FAILED"));
    }
    {
        Editor collecting((root / "collect-profile").u8string());
        collecting.buildDemo(arm, 0, "");
        CHECK(EditorTestAccess::save(collecting, root / "before-collect.rig.json"));
        EditorTestAccess::move(collecting, 77);
        const std::string beforeCollect = snapshot(collecting);
        const size_t undoBefore = EditorTestAccess::undoSize(collecting);
        const fs::path destination = root / fs::u8path("完整工程");
        fs::create_directory(destination);
        const fs::path file = destination / fs::u8path("角色.rig.json");
        CHECK(EditorTestAccess::save(collecting, file, true));
        CHECK(!EditorTestAccess::dirty(collecting) && EditorTestAccess::undoSize(collecting) == undoBefore + 1);
        const std::string collected = snapshot(collecting);
        CHECK(collected != beforeCollect);
        {
            Editor reopened((root / "collected-reopen-profile").u8string());
            CHECK(reopened.openProject(file.u8string()));
            CHECK(snapshot(reopened) == collected && EditorTestAccess::texturesLoaded(reopened));
            CHECK(!EditorTestAccess::dirty(reopened));
        }
        const fs::path collectedImage = fs::u8path(EditorTestAccess::skeleton(collecting).images[0].path);
        CHECK(fs::is_regular_file(collectedImage) && fs::is_regular_file(fs::u8path(arm)));
        CHECK(EditorTestAccess::save(collecting, file, true));
        CHECK(EditorTestAccess::undoSize(collecting) == undoBefore + 1); // Already-collected resources are a no-op.
        EditorTestAccess::undo(collecting);
        CHECK(snapshot(collecting) == beforeCollect && EditorTestAccess::dirty(collecting));
        CHECK(EditorTestAccess::projectPath(collecting) == file.u8string() && fs::exists(collectedImage));
        const size_t redoBefore = EditorTestAccess::redoSize(collecting);
        CHECK(!EditorTestAccess::commit(collecting, EditorTestAccess::skeleton(collecting)));
        CHECK(EditorTestAccess::redoSize(collecting) == redoBefore);
        EditorTestAccess::redo(collecting);
        CHECK(snapshot(collecting) == collected && !EditorTestAccess::dirty(collecting));
        EditorTestAccess::undo(collecting);
        CHECK(EditorTestAccess::save(collecting, file)); // Move the save marker to this earlier history position.
        CHECK(!EditorTestAccess::dirty(collecting));
        EditorTestAccess::redo(collecting);
        CHECK(EditorTestAccess::dirty(collecting));
        EditorTestAccess::undo(collecting);
        CHECK(!EditorTestAccess::dirty(collecting));
        rig::Skeleton transaction = EditorTestAccess::skeleton(collecting);
        transaction.bones[1].setup.x = 88;
        transaction.bones[2].setup.y = 22;
        CHECK(EditorTestAccess::commit(collecting, std::move(transaction)));
        CHECK(EditorTestAccess::redoSize(collecting) == 0);
        const std::string beforeFailure = snapshot(collecting);
        const size_t historyBeforeFailure = EditorTestAccess::undoSize(collecting);
        const fs::path blocked = destination / "blocked.rig.json";
        fs::create_directory(blocked);
        CHECK(!EditorTestAccess::save(collecting, blocked, true));
        CHECK(snapshot(collecting) == beforeFailure && EditorTestAccess::undoSize(collecting) == historyBeforeFailure);
        CHECK(EditorTestAccess::projectPath(collecting) == file.u8string() && EditorTestAccess::dirty(collecting));
        EditorTestAccess::undo(collecting);
        CHECK(snapshot(collecting) == beforeCollect && !EditorTestAccess::dirty(collecting));
        EditorTestAccess::setCollect(collecting, true);
        CHECK(!EditorTestAccess::dirty(collecting));
        EditorTestAccess::badUndo(collecting);
        const size_t badHistory = EditorTestAccess::undoSize(collecting);
        EditorTestAccess::undo(collecting);
        CHECK(snapshot(collecting) == beforeCollect && !EditorTestAccess::dirty(collecting));
        CHECK(EditorTestAccess::undoSize(collecting) == badHistory);
    }
    {
        Editor preference((root / "collect-profile").u8string());
        CHECK(EditorTestAccess::collect(preference));
        CHECK(!EditorTestAccess::dirty(preference));
    }
    {
        // Open/merge must resolve audio relative to the imported file before collecting it elsewhere.
        const fs::path sourceDirectory = root / "audio-source";
        fs::create_directory(sourceDirectory);
        const fs::path voice = sourceDirectory / "voice.wav";
        fs::copy_file(fs::u8path(POSEWEAVER_SOURCE_DIR) / "samples/swoosh.wav", voice);
        rig::Skeleton source;
        source.addBone("sound-root", -1, {}, 0);
        rig::EventDef sound;
        sound.name = "imported voice";
        sound.audioPath = "voice.wav";
        source.events.push_back(sound);
        const fs::path sourceFile = sourceDirectory / "audio.rig.json";
        CHECK(rig::saveFile(sourceFile.u8string(), source, &error));
        Editor audioProject((root / "audio-profile").u8string());
        CHECK(audioProject.openProject(sourceFile.lexically_relative(parent).u8string()));
        CHECK(EditorTestAccess::skeleton(audioProject).events[0].audioPath == voice.u8string());
        Editor merged((root / "merge-audio-profile").u8string());
        CHECK(merged.runExport("merge", sourceFile.u8string()));
        CHECK(EditorTestAccess::skeleton(merged).events[0].audioPath == voice.u8string());
        CHECK(EditorTestAccess::save(merged, root / "audio-collected.rig.json", true));
        CHECK(EditorTestAccess::skeleton(merged).events[0].audioPath != voice.u8string());
        CHECK(fs::is_regular_file(fs::u8path(EditorTestAccess::skeleton(merged).events[0].audioPath)));
    }
    {
        Editor bounded((root / "history-profile").u8string());
        CHECK(EditorTestAccess::save(bounded, root / "history.rig.json"));
        for (int x = 1; x <= 220; ++x) {
            rig::Skeleton document = EditorTestAccess::skeleton(bounded);
            document.bones[0].setup.x = (float)x;
            CHECK(EditorTestAccess::commit(bounded, std::move(document)));
        }
        CHECK(EditorTestAccess::undoSize(bounded) == 200);
        for (int x = 0; x < 200; ++x) EditorTestAccess::undo(bounded);
        CHECK(EditorTestAccess::skeleton(bounded).bones[0].setup.x == 20 && EditorTestAccess::dirty(bounded));
        CHECK(EditorTestAccess::save(bounded, root / "history.rig.json"));
        EditorTestAccess::redo(bounded);
        CHECK(EditorTestAccess::dirty(bounded));
        EditorTestAccess::undo(bounded);
        CHECK(!EditorTestAccess::dirty(bounded));
    }
    {
        const fs::path profile = root / "startup-profile";
        fs::create_directory(profile);
        CHECK(rig::writeFileAtomic((profile / "autosave.rig.json").u8string(), expected, &error));
        Editor startup(profile.u8string());
        startup.openSplash();
        ImGui::NewFrame();
        startup.frame(1.0f / 60);
        CHECK(ImGui::IsPopupOpen("Recovery copy found###Recovery"));
        CHECK(!ImGui::IsPopupOpen("##splash"));
        ImGui::Render();
    }
    {
        Editor editor((root / "blend-profile").u8string());
        editor.buildDemo(arm, 0, "");
        CHECK(!EditorTestAccess::blend(editor, 0, rig::BlendMode::Additive)); // Animate mode
        EditorTestAccess::setup(editor);
        CHECK(EditorTestAccess::save(editor, root / "blend.rig.json"));
        const auto history = EditorTestAccess::undoSize(editor);
        CHECK(EditorTestAccess::blend(editor, 0, rig::BlendMode::Additive));
        CHECK(EditorTestAccess::undoSize(editor) == history + 1 && EditorTestAccess::dirty(editor));
        CHECK(!EditorTestAccess::blend(editor, 0, rig::BlendMode::Additive));
        CHECK(!EditorTestAccess::blend(editor, -1, rig::BlendMode::Screen));
        CHECK(!EditorTestAccess::blend(editor, 0, (rig::BlendMode)88));
        CHECK(EditorTestAccess::undoSize(editor) == history + 1);
        EditorTestAccess::undo(editor);
        CHECK(EditorTestAccess::skeleton(editor).slots[0].blend == rig::BlendMode::Normal && !EditorTestAccess::dirty(editor));
        EditorTestAccess::redo(editor);
        CHECK(EditorTestAccess::skeleton(editor).slots[0].blend == rig::BlendMode::Additive);
        CHECK(EditorTestAccess::save(editor, root / "blend.rig.json"));
        Editor reopened((root / "blend-reopened").u8string());
        CHECK(reopened.openProject((root / "blend.rig.json").u8string()));
        CHECK(EditorTestAccess::skeleton(reopened).slots[0].blend == rig::BlendMode::Additive);
        CHECK(reopened.runExport("png", (root / "blend.png").u8string()));
        CHECK(fs::exists(root / "blend_0000.png"));
    }
    {
        std::ifstream input(fs::u8path(POSEWEAVER_SOURCE_DIR) / "samples/character.psd", std::ios::binary);
        std::string psd((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto mode = psd.find("8BIMnorm");
        CHECK(mode != std::string::npos);
        if (mode != std::string::npos) {
            psd.replace(mode + 4, 4, "over");
            const auto path = root / "unsupported.psd";
            CHECK(rig::writeFileAtomic(path.u8string(), psd, &error));
            Editor editor((root / "psd-blend-profile").u8string());
            CHECK(editor.runExport("psd", path.u8string()));
            EditorTestAccess::check(editor);
            CHECK(hasIssue(editor, "PSD_BLEND_UNSUPPORTED"));
            for (const auto& issue : EditorTestAccess::issues(editor)) if (issue.code == "PSD_BLEND_UNSUPPORTED") {
                CHECK(issue.severity == rig::Severity::Warning && issue.detail.find("over") != std::string::npos);
                CHECK(issue.path == path.u8string());
            }
            psd.replace(mode + 4, 4, "mul ");
            CHECK(rig::writeFileAtomic(path.u8string(), psd, &error));
            CHECK(editor.runExport("psd", path.u8string()));
            EditorTestAccess::check(editor);
            CHECK(!hasIssue(editor, "PSD_BLEND_UNSUPPORTED"));
            bool multiply = false;
            for (const auto& slot : EditorTestAccess::skeleton(editor).slots) multiply |= slot.blend == rig::BlendMode::Multiply;
            CHECK(multiply);
        }
    }
    {
        Editor editor((root / "runtime-export-profile").u8string());
        editor.buildDemo(arm,0,"skin");
        CHECK(EditorTestAccess::save(editor,root / "runtime-source.rig.json"));
        const auto before=snapshot(editor); const auto history=EditorTestAccess::undoSize(editor);
        const auto output=root / "runtime.pwskeleton";
        CHECK(editor.runExport("skeleton",output.u8string()));
        rig::Skeleton exported; std::string error;
        CHECK(rig::loadFile(output.u8string(),exported,&error));
        CHECK(exported.skins.size()==2 && exported.animations.size()==1);
        for (const auto& image : exported.images) CHECK(!fs::u8path(image.path).is_absolute() && fs::is_regular_file(root / fs::u8path(image.path)));
        CHECK(snapshot(editor)==before && EditorTestAccess::undoSize(editor)==history && !EditorTestAccess::dirty(editor));
        auto& sk=EditorTestAccess::skeleton(editor);
        sk.effects.light.enabled=true;
        CHECK(!editor.runExport("skeleton",(root / "unsupported-light.pwskeleton").u8string()) && !fs::exists(root / "unsupported-light.pwskeleton"));
        sk.effects={}; sk.slots[0].blend=rig::BlendMode::Screen;
        CHECK(!editor.runExport("skeleton",(root / "unsupported-blend.pwskeleton").u8string()) && !fs::exists(root / "unsupported-blend.pwskeleton"));
    }
    {
        Editor editor((root / "engine-profile").u8string());
        editor.buildDemo(arm, 0, "");
        auto& sk = EditorTestAccess::skeleton(editor);
        sk.animations[0].duration = 1.1f;
        sk.animations.push_back(sk.animations[0]);
        sk.animations.back().name = "second action";
        sk.animations.back().duration = 0.5f;
        EditorTestAccess::engineOptions(editor, 4, 0.5f, false);
        EditorTestAccess::move(editor, 42);
        const auto before = snapshot(editor);
        const auto history = EditorTestAccess::undoSize(editor);
        const auto manifest = root / fs::u8path("角色.pwframes");
        auto readManifest = [&]() {
            std::ifstream stream(manifest);
            return nlohmann::json::parse(stream);
        };
        CHECK(editor.runExport("engines", manifest.u8string()));
        if (fs::is_regular_file(manifest)) {
            auto doc = readManifest();
            CHECK(doc["format"] == "poseweaver.frames" && doc["version"] == 1 && doc["pixelsPerUnit"] == 64);
            CHECK(doc["animations"].size() == 2 && doc["animations"][0]["frames"].size() == 5 && doc["animations"][1]["frames"].size() == 2);
            CHECK(!doc["animations"][0]["loop"].get<bool>());
            CHECK(doc["originX"] >= 0 && doc["originX"] <= doc["frameWidth"] && doc["originY"] >= 0 && doc["originY"] <= doc["frameHeight"]);
            for (const auto& page : doc["pages"]) {
                Texture texture;
                CHECK(loadTexture((root / fs::u8path(page["file"].get<std::string>())).u8string(), texture, &error));
                CHECK(texture.width == page["width"] && texture.height == page["height"]);
                CHECK(std::any_of(texture.rgba().begin(), texture.rgba().end(), [](unsigned char pixel) { return pixel != 0; }));
                releaseTexture(texture);
            }
            for (const auto& clip : doc["animations"]) {
                double duration = 0;
                for (const auto& frame : clip["frames"]) {
                    auto& page = doc["pages"][frame["page"].get<size_t>()];
                    CHECK(frame["width"] == doc["frameWidth"] && frame["height"] == doc["frameHeight"]);
                    CHECK(frame["x"].get<int>() + frame["width"].get<int>() <= page["width"].get<int>());
                    CHECK(frame["y"].get<int>() + frame["height"].get<int>() <= page["height"].get<int>());
                    duration += frame["duration"].get<double>();
                }
                CHECK(std::abs(duration - clip["duration"].get<double>()) < 0.00001);
            }
            const auto previousPage = root / fs::u8path(doc["pages"][0]["file"].get<std::string>());
            CHECK(editor.runExport("engines", manifest.u8string()));
            auto next = readManifest();
            CHECK(next["pages"][0]["file"] != doc["pages"][0]["file"] && fs::is_regular_file(previousPage));
            CHECK(snapshot(editor) == before && EditorTestAccess::dirty(editor) && EditorTestAccess::undoSize(editor) == history);
            // The manifest commit fails after rendering: clean up only this attempt's files.
            const auto blocked = root / "blocked.pwframes";
            fs::create_directory(blocked);
            CHECK(rig::writeFileAtomic((blocked / "keep.txt").u8string(), "keep", &error));
            CHECK(!editor.runExport("engines", blocked.u8string()));
            CHECK(!fs::exists(root / "blocked_frames") && fs::exists(blocked / "keep.txt"));
            CHECK(readManifest() == next && fs::exists(previousPage));
            EditorTestAccess::check(editor);
            CHECK(hasIssue(editor, "ENGINE_EXPORT_FAILED"));
        }
        EditorTestAccess::engineOptions(editor, std::numeric_limits<float>::quiet_NaN(), 1);
        CHECK(!editor.runExport("engines", manifest.u8string()));
        EditorTestAccess::engineOptions(editor, 4, 0.5f);
        sk.animations[1].name = sk.animations[0].name;
        CHECK(!editor.runExport("engines", manifest.u8string()));
        sk.animations[1].name = "second action";
        sk.animations[0].duration = 2501;
        CHECK(!editor.runExport("engines", manifest.u8string()));
        // Force multiple 4096px pages without baking thousands of frames.
        sk.animations[0].duration = 1;
        EditorTestAccess::engineOptions(editor, 30, 4);
        CHECK(editor.runExport("engines", manifest.u8string()));
        if (fs::is_regular_file(manifest)) {
            const auto doc = readManifest();
            CHECK(doc["pages"].size() > 1);
            CHECK(doc["animations"][0]["frames"].size() == 30);
            for (const auto& clip : doc["animations"]) for (const auto& frame : clip["frames"]) {
                const auto& page = doc["pages"][frame["page"].get<size_t>()];
                CHECK(frame["x"].get<int>() + frame["width"].get<int>() <= page["width"].get<int>());
                CHECK(frame["y"].get<int>() + frame["height"].get<int>() <= page["height"].get<int>());
            }
        }
        EditorTestAccess::engineOptions(editor, 4, 0.5f);
        sk.animations.clear();
        CHECK(editor.runExport("engines", manifest.u8string()));
        if (fs::is_regular_file(manifest)) CHECK(readManifest()["animations"][0]["frames"].size() == 1);
        EditorTestAccess::check(editor);
        CHECK(!hasIssue(editor, "ENGINE_EXPORT_FAILED"));
        sk.images[0].path = (root / "absent.png").u8string();
        CHECK(!editor.runExport("engines", (root / "missing.pwframes").u8string()));
        CHECK(!fs::exists(root / "missing_frames"));
    }
    {
        const auto profile = root / "engine-job-profile";
        const auto projectA = root / "engine-a.rig.json", projectB = root / "engine-b.rig.json";
        const auto manifest = root / "engine-job.pwframes";
        Editor editor(profile.u8string());
        editor.buildDemo(arm, 0, "");
        auto& sk = EditorTestAccess::skeleton(editor);
        sk.animations.push_back(sk.animations[0]);
        sk.animations[1].name = "once";
        sk.animations[1].duration = 0.55f;
        CHECK(EditorTestAccess::save(editor, projectA));
        EditorTestAccess::engineOptions(editor, 10, 0.5f);
        auto& options = EditorTestAccess::engine(editor);
        options.path = manifest.u8string();
        options.clips[0].selected = false;
        options.clips[1].loop = false;
        const auto saved = snapshot(editor);
        const auto history = EditorTestAccess::undoSize(editor);
        EditorTestAccess::saveEngine(editor);
        CHECK(snapshot(editor) == saved && !EditorTestAccess::dirty(editor) && EditorTestAccess::undoSize(editor) == history);
        options.path = "relative.pwframes";
        EditorTestAccess::saveEngine(editor);
        {
            Editor relative(profile.u8string());
            CHECK(relative.openProject(projectA.u8string()));
            CHECK(EditorTestAccess::engine(relative).path == (root / "relative.pwframes").u8string());
        }
        options.path = manifest.u8string();
        EditorTestAccess::saveEngine(editor);
        CHECK(EditorTestAccess::startEngine(editor, true));
        CHECK(!EditorTestAccess::startEngine(editor, false));
        CHECK(waitEngine(editor) && !EditorTestAccess::engineError(editor));
        const auto budget = EditorTestAccess::budget(editor);
        CHECK(budget.animations == 1 && budget.frames == 6 && budget.width > 0 && budget.height > 0 && budget.pages > 0);
        CHECK(!fs::exists(manifest) && !fs::exists(root / "engine-job_frames"));
        CHECK(EditorTestAccess::startEngine(editor, false));
        // The worker owns its input; subsequent edits cannot alter this export.
        sk.bones[1].setup.x += 1000;
        options.fps = 120;
        CHECK(waitEngine(editor) && !EditorTestAccess::engineError(editor));
        sk.bones[1].setup.x -= 1000;
        options.fps = 10;
        if (fs::is_regular_file(manifest)) {
            std::ifstream stream(manifest);
            const auto doc = nlohmann::json::parse(stream);
            CHECK(doc["fps"] == 10 && doc["animations"].size() == 1 && doc["animations"][0]["name"] == "once");
            CHECK(!doc["animations"][0]["loop"].get<bool>() && doc["animations"][0]["frames"].size() == 6);
            CHECK(doc["frameWidth"] == budget.width && doc["frameHeight"] == budget.height && doc["pages"].size() == (size_t)budget.pages);
            uint64_t bytes = 0;
            for (const auto& page : doc["pages"]) bytes += page["width"].get<uint64_t>() * page["height"].get<uint64_t>() * 4;
            CHECK(bytes == budget.atlasBytes);
        }
        {
            Editor other(profile.u8string());
            other.buildDemo(arm, 0, "");
            CHECK(EditorTestAccess::save(other, projectB));
            EditorTestAccess::engine(other).fps = 12;
            EditorTestAccess::saveEngine(other);
        }
        {
            Editor reopened(profile.u8string());
            CHECK(reopened.openProject(projectA.u8string()));
            auto& remembered = EditorTestAccess::engine(reopened);
            CHECK(remembered.path == manifest.u8string() && remembered.fps == 10 && remembered.scale == 0.5f && remembered.pixelsPerUnit == 64);
            CHECK(remembered.clips.size() == 2 && !remembered.clips[0].selected && !remembered.clips[1].loop);
            CHECK(reopened.openProject(projectB.u8string()));
            CHECK(EditorTestAccess::engine(reopened).fps == 12);
        }
        // Cancellation during baking must remove a partially written new resource set.
        sk.animations[0].duration = 200;
        options.clips[0].selected = true; options.clips[1].selected = false;
        options.fps = 30; options.scale = 1;
        CHECK(EditorTestAccess::startEngine(editor, true));
        CHECK(EditorTestAccess::job(editor)->progress->cancel());
        CHECK(waitEngine(editor) && !EditorTestAccess::engineError(editor) && EditorTestAccess::budget(editor).frames == 0);
        std::ifstream previousStream(manifest, std::ios::binary);
        const std::string previous((std::istreambuf_iterator<char>(previousStream)), {});
        previousStream.close();
        CHECK(EditorTestAccess::startEngine(editor, false));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!fs::exists(root / "engine-job_frames_2/page_0.png") && std::chrono::steady_clock::now() < deadline &&
               EditorTestAccess::job(editor)->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(fs::exists(root / "engine-job_frames_2/page_0.png"));
        CHECK(EditorTestAccess::job(editor)->progress->cancel());
        CHECK(waitEngine(editor) && !EditorTestAccess::engineError(editor));
        CHECK(!fs::exists(root / "engine-job_frames_2"));
        std::ifstream afterStream(manifest, std::ios::binary);
        CHECK(std::string(std::istreambuf_iterator<char>(afterStream), {}) == previous);
        CHECK(fs::exists(root / "engine-job_frames/page_0.png"));
        EngineExportProgress committing;
        committing.gate = EngineExportProgress::Committing;
        CHECK(!committing.cancel());
        // Closing the editor also cancels and joins its worker safely.
        {
            Editor closing((root / "engine-close-profile").u8string());
            closing.buildDemo(arm, 0, "");
            EditorTestAccess::skeleton(closing).animations[0].duration = 200;
            EditorTestAccess::engine(closing).path = (root / "closing.pwframes").u8string();
            CHECK(EditorTestAccess::startEngine(closing, false));
        }
        CHECK(!fs::exists(root / "closing.pwframes") && !fs::exists(root / "closing_frames"));
    }
    {
        const auto profile = root / "engine-corrupt-profile";
        Editor editor(profile.u8string());
        editor.buildDemo(arm, 0, "");
        const auto project = root / "engine-corrupt.rig.json";
        CHECK(EditorTestAccess::save(editor, project));
        const auto settings = profile / "engine-exports.json";
        CHECK(rig::writeFileAtomic(settings.u8string(), "{broken", &error));
        CHECK(editor.openProject(project.u8string()));
        EditorTestAccess::check(editor);
        CHECK(hasIssue(editor, "ENGINE_SETTINGS_FAILED") && EditorTestAccess::engine(editor).fps == 30);
        EditorTestAccess::saveEngine(editor);
        std::ifstream stream(settings);
        CHECK(std::string(std::istreambuf_iterator<char>(stream), {}) == "{broken");
    }
    {
        Editor editor((root / "effects-profile").u8string());
        editor.buildDemo(arm,0,"");
        const auto source = EditorTestAccess::sourcePixels(editor);
        rig::RenderEffects effects;
        effects.light.enabled = effects.pixel.enabled = effects.pixel.outline = effects.pixel.dither = true;
        effects.pixel.size = 4;
        const auto plain = snapshot(editor);
        const auto history = EditorTestAccess::undoSize(editor);
        CHECK(EditorTestAccess::effects(editor,effects));
        CHECK(EditorTestAccess::undoSize(editor) == history + 1);
        CHECK(!EditorTestAccess::effects(editor,effects));
        auto invalid = effects; invalid.light.radius = 0;
        CHECK(!EditorTestAccess::effects(editor,invalid) && EditorTestAccess::skeleton(editor).effects == effects);
        CHECK(EditorTestAccess::save(editor,root / "effects.rig.json"));
        EditorTestAccess::undo(editor); CHECK(snapshot(editor) == plain && EditorTestAccess::dirty(editor));
        EditorTestAccess::redo(editor); CHECK(EditorTestAccess::skeleton(editor).effects == effects && !EditorTestAccess::dirty(editor));
        CHECK(editor.runExport("png",(root / "effects.png").u8string()));
        Texture rendered;
        CHECK(loadTexture((root / "effects_0000.png").u8string(), rendered, &error));
        for (size_t i = 0; i < rendered.rgba().size(); i += 4) {
            CHECK(rendered.rgba()[i+3] == 0 || rendered.rgba()[i+3] == 255);
            if (!rendered.rgba()[i+3]) continue;
            bool found = false;
            for (const auto& color : effects.pixel.palette)
                found |= rendered.rgba()[i] == (unsigned char)std::lround(color[0]*255) &&
                    rendered.rgba()[i+1] == (unsigned char)std::lround(color[1]*255) && rendered.rgba()[i+2] == (unsigned char)std::lround(color[2]*255);
            CHECK(found);
        }
        releaseTexture(rendered);
        CHECK(editor.runExport("engines",(root / "effects.pwframes").u8string()));
        {
            std::ifstream stream(root / "effects.pwframes");
            const auto doc = nlohmann::json::parse(stream);
            CHECK(doc["filter"] == "nearest");
        }
        Editor reopened((root / "effects-reopen-profile").u8string());
        CHECK(reopened.openProject((root / "effects.rig.json").u8string()));
        CHECK(EditorTestAccess::skeleton(reopened).effects == effects);
        EditorTestAccess::showEffects(reopened);
        for (int frame = 0; frame < 2; ++frame) { ImGui::NewFrame(); reopened.frame(1.0f/60); ImGui::Render(); }
        CHECK(EditorTestAccess::effectsTexture(reopened).id != 0);
        const auto preview = EditorTestAccess::effectsTexture(reopened).rgba();
        EditorTestAccess::panEffects(reopened);
        ImGui::NewFrame(); reopened.frame(1.0f/60); ImGui::Render();
        CHECK(EditorTestAccess::effectsTexture(reopened).rgba() == preview);
        const auto saved = snapshot(reopened);
        const auto undoCount = EditorTestAccess::undoSize(reopened);
        EditorTestAccess::gpuEffects(reopened,true);
        CHECK(EditorTestAccess::gpuSelected(reopened));
        CHECK(snapshot(reopened) == saved && !EditorTestAccess::dirty(reopened));
        CHECK(EditorTestAccess::undoSize(reopened) == undoCount);
        ImGui::NewFrame(); reopened.frame(1.0f/60); ImGui::Render();
        CHECK(EditorTestAccess::usingGpu(reopened) && EditorTestAccess::gpuTexture(reopened) != 0);
        CHECK(reopened.runExport("png",(root / "effects-gpu.png").u8string()));
        Texture cpuExport, gpuChoiceExport;
        CHECK(loadTexture((root / "effects_0000.png").u8string(),cpuExport,&error));
        CHECK(loadTexture((root / "effects-gpu_0000.png").u8string(),gpuChoiceExport,&error));
        CHECK(cpuExport.width == gpuChoiceExport.width && cpuExport.height == gpuChoiceExport.height && cpuExport.rgba() == gpuChoiceExport.rgba());
        releaseTexture(cpuExport); releaseTexture(gpuChoiceExport);
        const auto gpuTexture = EditorTestAccess::gpuTexture(reopened);
        EditorTestAccess::panEffects(reopened);
        ImGui::NewFrame(); reopened.frame(1.0f/60); ImGui::Render();
        CHECK(EditorTestAccess::usingGpu(reopened) && EditorTestAccess::gpuTexture(reopened) == gpuTexture);
        Editor restored((root / "effects-reopen-profile").u8string());
        CHECK(EditorTestAccess::gpuSelected(restored));
        EditorTestAccess::failGpu(reopened);
        ImGui::NewFrame(); reopened.frame(1.0f/60); ImGui::Render();
        CHECK(!EditorTestAccess::usingGpu(reopened) && EditorTestAccess::effectsTexture(reopened).id != 0);
        CHECK(hasIssue(reopened,"GPU_EFFECTS_FALLBACK"));
        EditorTestAccess::gpuEffects(reopened,true);
        ImGui::NewFrame(); reopened.frame(1.0f/60); ImGui::Render();
        CHECK(EditorTestAccess::usingGpu(reopened) && !hasIssue(reopened,"GPU_EFFECTS_FALLBACK"));
        EditorTestAccess::gpuEffects(reopened,false);
        CHECK(EditorTestAccess::gpuTexture(reopened) == 0 && !EditorTestAccess::gpuSelected(reopened));
        ImGui::NewFrame(); reopened.frame(1.0f/60); ImGui::Render();
        CHECK(!EditorTestAccess::usingGpu(reopened) && !EditorTestAccess::dirty(reopened));
        CHECK(EditorTestAccess::sourcePixels(editor) == source);
    }
    {
        Editor editor((root / "undo-budget-profile").u8string()); editor.buildDemo(arm,0,"");
        for (int i = 0; i < 4; ++i) EditorTestAccess::move(editor,10.f+i);
        CHECK(EditorTestAccess::save(editor,root / "undo-budget.rig.json"));
        const auto saved = snapshot(editor);
        EditorTestAccess::move(editor,25);
        EditorTestAccess::budgetUndo(editor,9 * 1024 * 1024);
        CHECK(EditorTestAccess::undoBytes(editor) <= 16 * 1024 * 1024 && EditorTestAccess::undoSize(editor) == 1);
        EditorTestAccess::undo(editor); CHECK(snapshot(editor) == saved && !EditorTestAccess::dirty(editor));
        EditorTestAccess::redo(editor); CHECK(EditorTestAccess::dirty(editor));
        CHECK(EditorTestAccess::undoBytes(editor) <= 16 * 1024 * 1024);
        EditorTestAccess::budgetUndo(editor,17 * 1024 * 1024);
        EditorTestAccess::check(editor);
        CHECK(EditorTestAccess::undoSize(editor) == 0 && hasIssue(editor,"UNDO_BUDGET"));
        Editor reopened((root / "undo-budget-profile").u8string()); CHECK(EditorTestAccess::undoBudget(reopened) == 16);
    }
    {
        Editor editor((root / "local-undo-profile").u8string()); editor.buildDemo(arm,0,"");
        CHECK(EditorTestAccess::save(editor,root / "local-undo.rig.json"));
        const auto saved=snapshot(editor);
        EditorTestAccess::moveLocal(editor,27); const auto moved=snapshot(editor);
        CHECK(EditorTestAccess::lastIsLocal(editor));
        EditorTestAccess::renameLocal(editor,"renamed_upper"); const auto renamed=snapshot(editor);
        CHECK(EditorTestAccess::lastIsLocal(editor) && EditorTestAccess::undoBytes(editor)<saved.size());
        EditorTestAccess::weightsLocal(editor); const auto weighted=snapshot(editor);
        CHECK(EditorTestAccess::lastIsLocal(editor) && weighted!=renamed);
        EditorTestAccess::addBoneFull(editor); const auto structural=snapshot(editor);
        for (const auto& state : {weighted,renamed,moved,saved}) { EditorTestAccess::undo(editor); CHECK(snapshot(editor)==state); }
        CHECK(!EditorTestAccess::dirty(editor));
        for (const auto& state : {moved,renamed,weighted,structural}) { EditorTestAccess::redo(editor); CHECK(snapshot(editor)==state); }
        EditorTestAccess::undo(editor); EditorTestAccess::undo(editor);
        CHECK(EditorTestAccess::save(editor,root / "local-undo.rig.json"));
        EditorTestAccess::undo(editor); CHECK(EditorTestAccess::dirty(editor));
        EditorTestAccess::redo(editor); CHECK(!EditorTestAccess::dirty(editor));
        EditorTestAccess::moveLocal(editor,83); CHECK(EditorTestAccess::redoSize(editor)==0 && EditorTestAccess::lastIsLocal(editor));
        EditorTestAccess::undo(editor); CHECK(snapshot(editor)==renamed && !EditorTestAccess::dirty(editor));
        EditorTestAccess::compensateBone(editor,true);
        EditorTestAccess::moveLocal(editor,102); CHECK(!EditorTestAccess::lastIsLocal(editor));
        EditorTestAccess::undo(editor); CHECK(snapshot(editor)==renamed && !EditorTestAccess::dirty(editor));
        EditorTestAccess::compensateBone(editor,false);
        const auto history=EditorTestAccess::undoSize(editor);
        EditorTestAccess::badLocalUndo(editor); EditorTestAccess::undo(editor); EditorTestAccess::check(editor);
        CHECK(EditorTestAccess::undoSize(editor)==history+1 && snapshot(editor)==renamed && hasIssue(editor,"UNDO_FAILED"));
    }
    {
        Editor editor((root / "local-undo-budget-profile").u8string()); editor.buildDemo(arm,0,"");
        EditorTestAccess::weightsLocal(editor); const auto weighted=snapshot(editor);
        EditorTestAccess::trimLocalUndo(editor);
        CHECK(EditorTestAccess::undoSize(editor)==0 && EditorTestAccess::undoBytes(editor)<=16*1024*1024 && snapshot(editor)==weighted);
    }
    {
        Editor editor((root / "local-skin-undo-profile").u8string()); editor.buildDemo(arm,0,"skin");
        CHECK(EditorTestAccess::save(editor,root / "local-skin-undo.rig.json"));
        const auto before=snapshot(editor);
        EditorTestAccess::weightsLocal(editor); const auto weighted=snapshot(editor);
        CHECK(EditorTestAccess::lastIsLocal(editor) && before!=weighted);
        EditorTestAccess::skinActive(editor,0);
        EditorTestAccess::undo(editor); CHECK(snapshot(editor)==before && !EditorTestAccess::dirty(editor));
        EditorTestAccess::redo(editor); CHECK(snapshot(editor)==weighted);
        EditorTestAccess::skinActive(editor,1);
        EditorTestAccess::unbindLocal(editor); const auto unbound=snapshot(editor);
        CHECK(EditorTestAccess::lastIsLocal(editor) && unbound!=weighted);
        EditorTestAccess::undo(editor); CHECK(snapshot(editor)==weighted);
        EditorTestAccess::redo(editor); CHECK(snapshot(editor)==unbound);
    }
    {
        Editor editor((root / "image-job-profile").u8string()); editor.buildDemo(arm,0,"");
        CHECK(EditorTestAccess::startImages(editor,root / "image-job.png"));
        CHECK(!EditorTestAccess::startImages(editor,root / "duplicate.png"));
        CHECK(!EditorTestAccess::startEngine(editor,true));
        auto retained=EditorTestAccess::retainPixels(editor);
        EditorTestAccess::unloadPixels(editor);
        CHECK(retained && !retained->empty());
        retained.reset();
        EditorTestAccess::skeleton(editor).slots.clear();
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(20);
        while (EditorTestAccess::imageJob(editor) && std::chrono::steady_clock::now()<deadline) { EditorTestAccess::pollImages(editor); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        CHECK(!EditorTestAccess::imageJob(editor) && fs::is_regular_file(root / "image-job_0000.png"));
    }
    {
        const auto profile = root / "image-settings-profile", projectA = root / "image-a.rig.json", projectB = root / "image-b.rig.json";
        Editor editor(profile.u8string()); editor.buildDemo(arm,0,""); CHECK(EditorTestAccess::save(editor,projectA));
        const auto unchanged = snapshot(editor); const auto undoCount = EditorTestAccess::undoSize(editor);
        auto& options = EditorTestAccess::images(editor); options.imageAnim = -1; options.imageFps = 17; options.imageScale = 0.5f; options.cleanOldFrames = false;
        EditorTestAccess::imagePath(editor) = "remembered.png"; EditorTestAccess::saveImages(editor);
        const auto settingsPath=profile/"image-exports.json";
        const auto oldWrite=fs::last_write_time(settingsPath)-std::chrono::hours(1);
        fs::last_write_time(settingsPath,oldWrite);
        EditorTestAccess::saveImages(editor); CHECK(fs::last_write_time(settingsPath)==oldWrite);
        CHECK(snapshot(editor) == unchanged && !EditorTestAccess::dirty(editor) && EditorTestAccess::undoSize(editor) == undoCount);
        Editor reopened(profile.u8string()); CHECK(reopened.openProject(projectA.u8string()));
        CHECK(EditorTestAccess::images(reopened) == options && fs::u8path(EditorTestAccess::imagePath(reopened)) == root / "remembered.png");
        CHECK(EditorTestAccess::save(reopened,projectB));
        Editor inherited(profile.u8string()); CHECK(inherited.openProject(projectB.u8string())); CHECK(EditorTestAccess::images(inherited) == options);
        const auto settings = profile / "image-exports.json"; CHECK(rig::writeFileAtomic(settings.u8string(),"{broken",&error));
        Editor broken(profile.u8string()); CHECK(broken.openProject(projectA.u8string())); CHECK(hasIssue(broken,"IMAGE_SETTINGS_FAILED"));
        EditorTestAccess::saveImages(broken); std::ifstream input(settings); std::stringstream text; text << input.rdbuf(); CHECK(text.str() == "{broken");
        input.close();
        nlohmann::json invalidRoot = nlohmann::json::object(); invalidRoot[fs::absolute(projectA).lexically_normal().u8string()] = {{"version",1},{"type",4294967296ull}};
        const auto invalidText = invalidRoot.dump(); CHECK(rig::writeFileAtomic(settings.u8string(),invalidText,&error));
        Editor invalid(profile.u8string()); CHECK(invalid.openProject(projectA.u8string())); CHECK(hasIssue(invalid,"IMAGE_SETTINGS_FAILED"));
        EditorTestAccess::saveImages(invalid); std::ifstream kept(settings); std::stringstream keptText; keptText << kept.rdbuf(); CHECK(keptText.str() == invalidText);
    }
    {
        Editor editor((root / "cache-profile").u8string()); editor.buildDemo(arm,0,"");
        rig::RenderEffects effects; effects.light.enabled = true; CHECK(EditorTestAccess::effects(editor,effects));
        ImGui::NewFrame(); editor.frame(1.f/60); ImGui::Render();
        const auto stamp = EditorTestAccess::effectsStamp(editor);
        const auto pixels = EditorTestAccess::effectsTexture(editor).rgba();
        EditorTestAccess::renameOnly(editor);
        ImGui::NewFrame(); ImGui::SetActiveID(0xF00D,ImGui::GetCurrentWindow()); editor.frame(1.f/60); ImGui::Render();
        CHECK(EditorTestAccess::effectsStamp(editor) == stamp && EditorTestAccess::effectsTexture(editor).rgba() == pixels);
        EditorTestAccess::attachmentDrag(editor);
        ImGui::NewFrame(); editor.frame(1.f/60); ImGui::Render();
        CHECK(EditorTestAccess::effectsStamp(editor) != stamp);
        EditorTestAccess::undo(editor); ImGui::NewFrame(); editor.frame(1.f/60); ImGui::Render();
    }
    {
        const auto assets = root / "relink-assets"; fs::create_directory(assets); fs::create_directory(assets / "one"); fs::create_directory(assets / "two");
        fs::copy_file(fs::u8path(arm),assets / "arm.png");
        fs::copy_file(fs::u8path(arm),assets / "one/duplicate.png"); fs::copy_file(fs::u8path(arm),assets / "two/duplicate.png");
        fs::copy_file(fs::u8path(POSEWEAVER_SOURCE_DIR)/"samples/swoosh.wav",assets / "swoosh.wav");
        Editor editor((root / "batch-relink-profile").u8string()); editor.buildDemo(arm,0,"");
        auto& sk = EditorTestAccess::skeleton(editor);
        sk.images[0].path = (root / "missing/arm.png").u8string(); sk.events[0].audioPath = (root / "missing/swoosh.wav").u8string();
        const auto missing = snapshot(editor);
        const auto history = EditorTestAccess::undoSize(editor);
        CHECK(EditorTestAccess::scanResources(editor,assets)); CHECK(EditorTestAccess::matches(editor).size() == 2);
        CHECK(EditorTestAccess::applyResources(editor) && EditorTestAccess::undoSize(editor) == history+1);
        CHECK(EditorTestAccess::texturesLoaded(editor));
        EditorTestAccess::undo(editor); CHECK(snapshot(editor) == missing);
        EditorTestAccess::redo(editor); CHECK(EditorTestAccess::skeleton(editor).images[0].path == (assets / "arm.png").u8string());
        sk.images[0].path = (root / "missing/duplicate.png").u8string();
        CHECK(EditorTestAccess::scanResources(editor,assets));
        CHECK(EditorTestAccess::matches(editor).size() == 1 && EditorTestAccess::matches(editor)[0].candidates.size() == 2 && EditorTestAccess::matches(editor)[0].selected == -1);
        CHECK(!EditorTestAccess::applyResources(editor));
        EditorTestAccess::matches(editor)[0].selected = 1; CHECK(EditorTestAccess::applyResources(editor));
        sk.images[0].path = (root / "missing/arm.png").u8string(); sk.events[0].audioPath = (root / "missing/swoosh.wav").u8string();
        CHECK(EditorTestAccess::scanResources(editor,assets));
        const auto beforeFailure = snapshot(editor); const auto historyBeforeFailure = EditorTestAccess::undoSize(editor);
        { std::ofstream invalidAudio(assets / "swoosh.wav",std::ios::binary); invalidAudio << "not audio"; }
        CHECK(!EditorTestAccess::applyResources(editor));
        CHECK(snapshot(editor) == beforeFailure && EditorTestAccess::undoSize(editor) == historyBeforeFailure);
        CHECK(EditorTestAccess::startScan(editor,assets));
        CHECK(!EditorTestAccess::startScan(editor,assets) && !EditorTestAccess::applyResources(editor));
        EditorTestAccess::cancelScan(editor); EditorTestAccess::finishScan(editor);
        CHECK(EditorTestAccess::matches(editor).empty() && snapshot(editor)==beforeFailure);
        CHECK(EditorTestAccess::startScan(editor,assets));
        EditorTestAccess::newProject(editor); CHECK(EditorTestAccess::discard(editor));
        EditorTestAccess::finishScan(editor);
        CHECK(EditorTestAccess::matches(editor).empty() && EditorTestAccess::skeleton(editor).images.empty());
    }
    {
        Editor robot((root / "robot-profile").u8string());
        CHECK(robot.openProject((fs::u8path(POSEWEAVER_SOURCE_DIR)/"samples/robot-starter.rig.json").u8string()));
        auto& sk = EditorTestAccess::skeleton(robot);
        CHECK(sk.bones.size() == 15 && sk.skins.size() == 2 && sk.constraints.size() == 2 && sk.animations.size() == 3);
        CHECK(EditorTestAccess::texturesLoaded(robot));
        for (int skin : {0,1}) for (const auto& animation : sk.animations) {
            auto pose = rig::makeAnimatedPose(sk,&animation,animation.duration*0.5f,{skin});
            for (const auto& world : pose.world) CHECK(std::isfinite(world.tx) && std::isfinite(world.ty));
        }
    }
    {
        const auto profile=root/"export-recovery-profile", output=root/"interrupted";
        fs::create_directory(output);
        { std::ofstream old(output/"frame.png"); old<<"previous"; }
        {
            ExportTransaction transaction(output,profile/"export-recovery");
            { std::ofstream next(transaction.directory/"output.png"); next<<"next"; }
            transaction.add(transaction.directory/"output.png",output/"frame.png");
            ExportTransactionTestAccess::interrupt(transaction);
            fs::rename(output/"frame.png",transaction.directory/"backup-0");
            fs::rename(transaction.directory/"output.png",output/"frame.png");
        }
        Editor editor(profile.u8string());
        CHECK(EditorTestAccess::pendingExports(editor)==1);
        CHECK(EditorTestAccess::recoverExport(editor));
        CHECK(!EditorTestAccess::startImages(editor,output/"blocked.png"));
        EditorTestAccess::finishExportRecovery(editor);
        CHECK(EditorTestAccess::pendingExports(editor)==0);
        std::ifstream restored(output/"frame.png"); std::string value; restored>>value; CHECK(value=="previous");
    }
    if (root.parent_path() == parent) fs::remove_all(root);
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    std::printf("editor workflows: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
