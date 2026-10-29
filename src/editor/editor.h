#pragma once
#include "core/anim_state.h"
#include "core/constraints.h"
#include "core/diagnostics.h"
#include "core/pose.h"
#include "core/resource_relink.h"
#include "platform/audio.h"
#include "platform/texture.h"
#include "platform/gpu_effects.h"
#include "editor/engine_export.h"
#include "editor/image_export.h"
#include "editor/export_transaction.h"

#include <imgui.h>

#include <map>
#include <functional>
#include <cstdint>
#include <optional>
#include <memory>
#include <set>
#include <string>
#include <vector>

enum class Mode { Setup, Animate };
enum class Tool { Rotate, Translate, Scale, Shear, Pose, CreateBone, MeshEdit, Weights };
enum class Axes { Local, Parent, World };

struct Selection {
    enum Kind { None, Skeleton, Bone, Slot, Attachment, Animation, Image, Event, Constraint, Skin } kind = None;
    int bone = -1;        // also set for slots/attachments (their bone), used by transform tools
    int slot = -1;
    int attachment = -1;  // attachment index inside `slot`
    int index = -1;       // animation, image, event, constraint or skin index
    std::vector<int> vertices;  // selected mesh vertices (MeshEdit tool)
    std::vector<int> bones;     // pose tool multi-selection (Ctrl+click); chains are posed with IK
    void clear() { *this = Selection{}; }
};

// Mouse drag in the viewport.
struct Drag {
    enum Kind { None, Bone, Attachment, Vertices, BoxSelect, CreateBone, Paint, BoneLength, PoseRotate, PoseIk } kind = None;
    ImVec2 startScreen;
    rig::Vec2 startWorld;
    rig::Vec2 pivotWorld;           // bone origin / attachment centre
    rig::Transform startTransform;  // bone local or attachment transform at drag start
    rig::Affine parentWorld;        // space the dragged transform lives in
    std::vector<rig::Vec2> startVertices;
    std::vector<rig::Vec2> startOffsets;  // deform drag: offsets at drag start
    std::vector<rig::Affine> deformBases; // deform drag: offset -> world per vertex
    rig::Affine imageMap;                 // mesh edit drag: image pixels -> slot-bone space
    Tool tool = Tool::Rotate;       // tool that started the drag
    int parentBone = -1;            // CreateBone
    bool moved = false;             // passed the drag threshold
    bool checkpointed = false;      // undo snapshot taken for this drag
    std::vector<int> chain;         // PoseIk: bones from the top of the chain to the dragged bone
    rig::Pose startPose;            // PoseIk: pose at drag start
};

// Animate-mode changes that have not been keyed yet (auto key off).
struct PendingChange {
    rig::Transform local;
    bool rot = false, trans = false, scale = false, shear = false;
    bool any() const { return rot || trans || scale || shear; }
};

enum class KeyState { None, Changed, Keyed };

class Editor {
public:
    explicit Editor(const std::string& settingsDirectory = {});
    ~Editor();

    void frame(float dt);      // build all UI for one frame
    bool requestClose();       // returns true if the app may close now
    bool shouldQuit() const { return closeConfirmed_; }
    void onFilesDropped(const std::vector<std::string>& paths);
    void setUiScale(float s) { uiScale_ = s; }
    bool openProject(const std::string& path);
    // Start-up splash (splash.cpp).
    void openSplash() { splashRequest_ = true; }
    bool splashEnabled() const { return settings_.showSplash; }
    // On quit: keeps the current skeleton as the "restore last session" file.
    void saveSession();
    // UI language: "zh", "en" or "" (follow the system). `remember` stores it in the settings.
    void setLanguage(const std::string& code, bool remember);
    // Builds a two-bone skinned arm with a "wave" animation from `imagePath`.
    // scenario: "" (Animate at `time`), "weights", "mesh" (mesh tool), "clip" (adds a clipping slot).
    void buildDemo(const std::string& imagePath, float time, const std::string& scenario);
    // Brings the Graph tab to front with the first rotate key of the selected bone selected.
    // Import / export without dialogs (also used by the command line). type: json, bin, atlas,
    // png, sheet, gif, mp4, webm (export) or merge, psd (import).
    bool runExport(const std::string& type, const std::string& path);
    const std::string& status() const { return status_; }
    void openIssues() { showIssues_ = true; issuesDirty_ = true; focusIssues_ = 2; }
    void focusGraph() {
        focusGraph_ = 3;
        graphCurve_ = 0;
        graphKey_ = 0;
    }

private:
    friend struct EditorTestAccess;
    // --- document ---
    rig::Skeleton sk_;
    std::vector<Texture> textures_;  // parallel to sk_.images
    std::string projectPath_;        // UTF-8, empty = never saved
    bool dirty_ = false;
    uint64_t historyPosition_ = 0, nextHistoryPosition_ = 1;
    std::optional<uint64_t> savedPosition_{0};
    std::string settingsDirectory_;
    std::function<bool()> pendingDocumentChange_;
    bool pendingPreservesRecovery_ = false, pendingIsClose_ = false;
    enum class ChangeDecision { Save, Discard, Cancel };
    bool requestDocumentChange(std::function<bool()> change, bool preserveRecovery = false, bool closing = false);
    bool resolveDocumentChange(ChangeDecision decision);
    void resetProject();
    bool openProjectNow(const std::string& path);
    bool saveProjectTo(const std::string& path, bool collectResources = false);
    bool commitDocumentChange(rig::Skeleton document);
    void markSaved();
    void markUnsaved();
    bool relinkImage(int image, const std::string& path);
    bool showResourceRelink_ = false;
    std::vector<rig::ResourceMatch> resourceMatches_;
    std::string resourceRelinkDirectory_;
    struct ResourceScanJob {
        rig::ResourceSearchProgress progress;
        std::future<std::vector<rig::ResourceMatch>> result;
    };
    std::unique_ptr<ResourceScanJob> resourceScan_;
    std::string resourceScanNotice_;
    bool scanResourceMatches(const std::string& directory);
    void pollResourceScan();
    void resetResourceScan();
    bool applyResourceMatches();
    void drawResourceRelink();
    bool showExportRecovery_ = false;
    std::vector<ExportRecovery> exportRecoveries_;
    std::future<std::string> exportRecoveryTask_;
    std::string exportRecoveryEntry_;
    void refreshExportRecoveries();
    bool startExportRecovery(int index);
    void pollExportRecovery();
    void drawExportRecovery();
    bool setSlotBlend(int slot, rig::BlendMode blend);
    void buildDemoNow(const std::string& imagePath, float time, const std::string& scenario);
    void newFromImageDialog();
    void newFromPsd();
    void restoreSession();

    // Recoverable snapshots are separate from the user's saved project.
    bool recoveryPending_ = false, recoveryOwned_ = false, recoveryAvailable_ = false;
    bool showRecoveryDialog_ = false;
    float recoveryElapsed_ = 0;
    std::string recoveryError_;
    std::string configFile(const char* name) const;
    std::string recoveryPath() const;
    bool saveRecovery();
    bool restoreRecovery();
    void clearRecovery();
    void updateRecovery(float dt, bool editing);
    void drawRecoveryDialog();

    bool showIssues_ = false, issuesDirty_ = true;
    float issuesRefreshIn_ = 0;
    int issueFilter_ = 0;
    int focusIssues_ = 0;
    std::string selectedIssueKey_;
    std::vector<rig::Diagnostic> issues_, sessionIssues_;
    void refreshIssues();
    void drawIssues();
    bool canFocusIssue(const rig::Diagnostic& issue) const;
    bool focusIssue(const rig::Diagnostic& issue);
    void reportIssue(rig::Diagnostic issue);
    void reportIssue(const std::string& code, const std::string& detail, rig::Severity severity = rig::Severity::Error,
                     const std::string& path = {});
    bool failOperation(const std::string& code, const std::string& message, const std::string& path = {},
                       rig::IssueTarget target = rig::IssueTarget::File, int index = -1, int sub = -1);
    void dismissIssue(const std::string& code);
    bool checkExportResources();

    // --- undo ---
    struct UndoEntry {
        std::string snapshot;
        uint64_t position = 0;
        enum Kind { Document, Bone, Name, Attachment } kind = Document;
        int index = -1, slot = -1, skin = -1;
        Selection::Kind nameKind = Selection::None;
        rig::Bone bone;
        rig::Attachment attachment;
        std::string name;
        uint64_t memoryBytes() const;
    };
    std::vector<UndoEntry> undo_, redo_;
    void checkpoint();  // call before every document change
    void checkpointBone(int bone);
    void checkpointName();
    void checkpointAttachment();
    void pushCheckpoint(UndoEntry entry);
    bool captureHistory(const UndoEntry& shape, UndoEntry& entry);
    rig::Attachment* historyAttachment(const UndoEntry& entry);
    std::string* historyName(const UndoEntry& entry);
    void undo();
    void redo();
    bool restore(const UndoEntry& entry);
    uint64_t undoMemoryBytes() const;
    void trimUndoHistory(bool preferUndo = true);

    // --- editing state ---
    Mode mode_ = Mode::Setup;
    Tool tool_ = Tool::Rotate;
    Tool prevTool_ = Tool::Translate;  // right click in the viewport swaps back to it
    Axes axes_ = Axes::Local;
    bool autoKey_ = true;
    Selection sel_;
    int anim_ = -1;  // current animation index
    float time_ = 0;
    bool playing_ = false;
    int playDir_ = 1;  // 1 forward, -1 backward
    bool loop_ = true;
    float speed_ = 1.0f;
    float fps_ = 30;
    rig::Pose pose_;  // what the viewport shows this frame
    std::map<int, PendingChange> pending_;  // per bone
    float pendingTime_ = -1;
    int pendingAnim_ = -1;

    // --- view ---
    ImVec2 viewOffset_{0, 0};
    float zoom_ = 1.0f;
    ImVec2 canvasOrigin_, canvasSize_;
    struct ViewOptions {
        bool bonesVisible = true, bonesSelectable = true, boneNames = false;
        bool imagesVisible = true, imagesSelectable = true;
        bool meshVisible = true;
        bool polygonsVisible = true;  // clipping and bounding box outlines
    } view_;
    std::set<int> hiddenBones_, hiddenSlots_;  // editor-only visibility
    Drag drag_;
    float uiScale_ = 1.0f;
    ImVec2 rightClickStart_;

    // --- tree ---
    std::string treeFilter_;
    bool treeShowBones_ = true, treeShowSlots_ = true, treeShowAttachments_ = true;
    bool scrollTreeToSelection_ = false;
    std::string renameBuf_;
    bool openRename_ = false;

    // --- dopesheet ---
    struct SheetRow {
        enum Kind { Overview, Bone, Slot, Property, Audio } kind;  // Audio: index = event
        int index;                                  // bone or slot (-1 for draw order / events)
        rig::Channel channel = rig::Channel::Rotate;  // Property rows
        int sub = -1;                                 // Deform rows: attachment index
    };
    struct SheetDrag {
        bool scrubbing = false;
        bool movingKey = false;
        bool panning = false;
        bool pressedEmpty = false;  // left press on empty space: click = set time, drag = box select
        bool boxSelecting = false;
        bool scaling = false;       // dragging a handle of the selected keys' time span
        bool checkpointed = false;
        float pivot = 0, origEdge = 0;
        std::vector<float> origTimes;  // selected key times when scaling started
        ImVec2 start;
        SheetRow row{SheetRow::Overview, -1};
        float from = 0;
    } sheetDrag_;
    struct SheetKey {  // a selected key: the row it was picked on and its time
        SheetRow row;
        float time;
    };
    std::vector<SheetKey> sheetSel_;
    int sheetSelAnim_ = -1;
    rig::Animation sheetClip_;  // copied keys, times relative to the earliest one
    float sheetClipLength_ = 0;
    bool hasSheetClip_ = false;
    bool sheetActive_ = false;  // dopesheet hovered/focused last frame (owns Del / Ctrl+C / Ctrl+V)
    SheetRow sheetMenuRow_{SheetRow::Overview, -1};
    float sheetMenuTime_ = 0;
    float sheetPps_ = 0;      // pixels per second, 0 = fit to width
    float sheetScroll_ = 0;   // seconds scrolled off the left edge
    bool sheetLocked_ = false;
    std::vector<int> sheetLockedBones_;
    std::set<int> collapsedBones_;

    // --- graph ---
    unsigned graphHidden_ = 0;             // bit per curve id hidden by the user
    float graphPps_ = 0, graphScroll_ = 0;  // time zoom (0 = fit) and scroll (seconds)
    bool graphAutoY_ = true, graphPanning_ = false;
    float graphVMin_ = -1, graphVMax_ = 1;   // value range when not auto-fitted
    int graphCurve_ = -1, graphKey_ = -1;  // selected key (curve id, key index)
    struct GraphDrag {
        enum Kind { None, Key, Handle1, Handle2, Scrub } kind = None;
        bool checkpointed = false;
    } graphDrag_;

    // --- mesh / weights tools ---
    bool meshDeformMode_ = false;  // Setup mesh editing: move vertices without changing uvs
    float outlineTolerance_ = 2.0f, outlinePadding_ = 2.0f;
    int meshDensity_ = 6;          // interior points across the image's longer side (0 = none)
    std::vector<int> lockedBones_;
    float smoothAmount_ = 0.5f, pruneThreshold_ = 0.05f, setWeightValue_ = 1.0f;
    int pruneMax_ = 4;
    int gridCols_ = 6, gridRows_ = 6;
    float brushRadius_ = 40, brushStrength_ = 0.5f;
    std::vector<int> bindBones_;  // bones checked in the bind list

    // --- misc ---
    std::string status_;
    float statusTime_ = 0;
    bool firstFrame_ = true;
    int frameAllIn_ = 0;  // frames until frameAll() runs (canvas size is known after one frame)
    bool showCloseDialog_ = false;
    bool closeConfirmed_ = false;
    bool showDemo_ = false;

    // --- import / export dialogs ---
    struct IoState : ImageExportOptions {
        enum Dialog { None, Data, Atlas, Images, Psd, Engine } open = None, openPopup = None;
        bool dataBinary = false, dataPretty = true;
        int dataDecimals = -1;
        int atlasMaxSize = 2048, atlasPadding = 2;
        bool atlasTrim = true, atlasRotate = false, atlasPow2 = false, atlasAutoScale = false;
        float atlasScale = 1.0f;
        bool psdIncludeHidden = true;
    } io_;
    void drawIoDialogs();
    bool importMergePath(const std::string& path);
    bool importPsdPath(const std::string& path, bool includeHidden);
    bool exportDataPath(const std::string& path);
    bool exportAtlasPath(const std::string& path);
    bool exportImagesPath(const std::string& path);
    ImageExportOptions imageOptions() const;
    std::unique_ptr<ImageExportJob> imageJob_;
    bool startImageExport(const std::string& path, bool estimate = false);
    std::string imageOutputPath_;
    ImageExportResult imageBudget_;
    ImageExportOptions imageBudgetOptions_;
    uint64_t imageBudgetHistory_ = UINT64_MAX;
    std::vector<int> imageBudgetSkins_;
    bool imageBudgetPhysics_ = false;
    void saveImageExportSettings();
    void loadImageExportSettings();
    bool imageSettingsDirty_ = false;
    bool imageSettingsReadOnly_ = false;
    float imageSettingsDelay_ = 0;
    void pollImageExport();
    void resetImageExport();
    bool finishImageExport(const ImageExportResult& result, const std::string& path, bool video);
    void drawImageExportProgress();
    bool exportEnginePath(const std::string& path);
    bool exportRuntimePath(const std::string& path);
    EngineExportOptions engineOptions_;
    EngineExportBudget engineBudget_;
    std::unique_ptr<EngineExportJob> engineJob_;
    std::string engineNotice_;
    bool engineNoticeError_ = false, engineSettingsDirty_ = false;
    float engineSettingsDelay_ = 0;
    void syncEngineClips();
    void loadEngineExportSettings();
    void saveEngineExportSettings();
    void resetEngineExport();
    bool startEngineExport(bool estimate);
    void pollEngineExport();
    bool finishEngineExport(const EngineExportResult& result, bool estimate, const std::string& path);
    void drawEngineExportDialog();
    bool resetLayout_ = false;
    bool showEffects_ = false, effectsEditing_ = false, effectsCacheValid_ = false;
    int effectsColor_ = 0;
    Texture effectsTexture_;
    GpuEffects effectsGpu_;
    bool effectsUsingGpu_ = false;
    std::string effectsGpuError_;
    std::vector<float> effectsPoseKey_;
    rig::RenderEffects effectsCached_;
    uint64_t effectsHistory_ = 0;
    rig::Vec2 effectsLo_, effectsHi_;
    bool setRenderEffects(const rig::RenderEffects& effects, bool continuous = false);
    void setEffectsGpu(bool enabled);
    void drawRenderEffects();
    bool drawEffectsPreview(ImDrawList* list);
    rig::PhysicsState physics_;
    std::unique_ptr<AudioPlayer> audio_;
    bool audioOn_ = true;
    bool wasPlaying_ = false;
    // Starts every audio event that should be sounding at `time` (playback start, seeking).
    void startAudioAt(float time);
    std::vector<int> activeSkins_;  // skins shown / edited (later ones win), editor state only
    void toggleSkin(int skin);
    int createSkin(const std::string& name);
    // The attachment that a slot attachment index stands for with the active skins (null when a
    // placeholder is not filled).
    rig::Attachment* editableAttachment(int slot, int attachment);
    // Moves a slot attachment into `skin` and turns the slot entry into a placeholder.
    void moveAttachmentToSkin(int slot, int attachment, int skin);
    bool physicsOn_ = true;
    float physicsDt_ = 0;  // time to simulate in the next updatePose (set once per frame)
    struct FiredEvent { std::string text; float timeLeft; };
    std::vector<FiredEvent> firedEvents_;
    int focusWeights_ = 0;
    int focusGraph_ = 0;
    int focusPreview_ = 0;
    // Persistent user settings (settings.json in the per-user config folder).
    struct AppSettings {
        std::vector<std::string> recent;  // most recent first
        bool showSplash = true;
        bool collectResources = false;
        bool effectsGpu = false;
        int undoMemoryMiB = 128;
        std::string language;  // "zh" / "en", empty = follow the system
    } settings_;
    bool splashRequest_ = false, showShortcuts_ = false;
    void loadSettings();
    void saveSettings();
    void addRecent(const std::string& path);
    std::string sessionPath() const;
    std::string findSample(const std::string& name) const;
    void drawSplash();
    void drawShortcuts();
    void drawLanguageMenu();  // items for the View menu
    // Mix preview (Animate mode): an AnimationState with up to kMixTracks tracks drives the viewport.
    static constexpr int kMixTracks = 4;
    rig::AnimationState mixState_;
    bool mixActive_ = false, mixPlaying_ = true;
    struct MixTrackUi {
        int anim = -1;
        bool loop = true;
        float alpha = 1.0f;
    } mixUi_[kMixTracks];
    float mixQueueDelay_ = 0;
    int mixPairFrom_ = -1, mixPairTo_ = -1;
    float mixPairDuration_ = 0.3f;
    // Onion skin (Animate mode).
    bool ghostOn_ = false;
    int ghostBefore_ = 2, ghostAfter_ = 2, ghostStep_ = 3;  // frames
    float ghostAlpha_ = 0.35f;
    // Compensation: keep children / attachments in place while transforming a bone.
    bool compBones_ = false, compImages_ = false;
    // Weight mirror options.
    int mirrorAxis_ = 0;  // 0 mesh centre, 1 selected bone origin, 2 world x = 0
    bool mirrorLeftToRight_ = true;  // frames left to bring the Weights tab to front (docking settles over a few frames)

    // frame parts
    void handleShortcuts();
    void drawMenuBar();
    void drawViewport();
    void drawTree();
    void drawProperties();
    void drawWeightsView();
    void drawDopesheet();
    void drawGraph();
    void drawMixPreview();  // panel_mix.cpp
    void drawStatusBar();
    void drawCloseDialog();
    void setupDockLayout(unsigned int dockspace);

    // viewport parts (panel_viewport.cpp / viewport_toolbar.cpp)
    void drawModeLabel();
    void drawMainToolbar();
    void drawZoomControls();
    ImVec2 toScreen(rig::Vec2 w) const;
    rig::Vec2 toWorld(ImVec2 s) const;
    int pickBone(ImVec2 screen) const;
    int pickSlot(rig::Vec2 world) const;
    int pickVertex(ImVec2 screen) const;
    bool overBoneTip(ImVec2 screen) const;
    void viewportMouseDown(ImVec2 mouse, bool shift, bool ctrl);
    void viewportDrag(ImVec2 mouse, bool shift, bool ctrl, float dt);
    void viewportMouseUp(ImVec2 mouse, bool shift);
    void drawSlots(ImDrawList* dl);
    void drawBones(ImDrawList* dl);
    void drawMeshOverlay(ImDrawList* dl);
    void frameAll();
    void zoomTo(float zoom);
    // Deform offsets of an attachment in the displayed pose (Animate mode), or null.
    const std::vector<rig::Vec2>* poseDeform(int slot, const rig::Attachment* att) const;
    std::vector<rig::Vec2> attachmentVertices(int slot, const rig::Attachment& att) const;  // displayed pose, world
    int attachmentIndex(int slot, const rig::Attachment* att) const;
    // Mesh / polygon editing (Setup mode).
    void meshAddVertexAt(ImVec2 mouse);
    void meshDeleteSelectedVertices();
    bool autoMesh(int slot, int attachment);
    void createPolygonAttachment(int slot, rig::AttachmentType type);
    void afterTopologyChange(int slot, int attachment);
    void setTool(Tool t);

    // properties sections
    void propertiesSkeleton();
    void propertiesBone(int b);
    void propertiesSlot(int s);
    void propertiesAttachment(int s, int a);
    void propertiesAnimation(int a);
    void propertiesImage(int i);
    void propertiesEvent(int e);
    void propertiesConstraint(int c);
    void propertiesSkin(int skin);
    void propertiesPlaceholder(int slot, int attachment);
    bool propertiesHeader(int icon, const std::string& type, const std::string& name, bool canRename, bool canDelete);

    // selection
    void selectBone(int b);
    void selectSlot(int s);
    void selectAttachment(int s, int a);
    void selectAnimation(int a);
    void selectEvent(int e);
    void selectConstraint(int c);
    void selectSkin(int skin);
    void validateSelection();

    // document operations
    void newProject();
    void openProjectDialog();
    bool saveProject(bool saveAs);
    void exportRuntimeJson();
    void importImageDialog();
    bool importImage(const std::string& path);
    void reloadTextures();
    void deleteSelection();
    void removeSlot(int slot);
    void moveSlot(int slot, int delta, bool snapshot = true);  // change draw order
    int createAnimation();
    void renameSelection(const std::string& name);
    void setStatus(const std::string& msg);
    std::string resolveImagePath(const std::string& stored) const;

    // pose / animation helpers
    void updatePose();
    rig::Animation* currentAnimation();
    float snapTime(float t) const;
    void setMode(Mode m);
    void setTime(float t);
    // Applies a new local transform for a bone: edits setup in Setup mode; in Animate mode
    // keys it (auto key) or records it as a pending change.
    void setBoneLocal(int bone, const rig::Transform& local, bool rot, bool trans, bool scale, bool shear = false);
    void keyBone(int bone, bool rot, bool trans, bool scale, bool shear = false);
    void setSlotColor(int slot, const rig::Color& color);  // setup colour, or a colour key in Animate mode
    KeyState slotColorKeyState(int slot);
    // Constraints: create from the current selection, key current values, set values (Setup: setup
    // values, Animate: a key at the current time).
    int createConstraint(rig::ConstraintType type);
    void keyConstraint(int c);
    void bakePhysics(int constraint);  // -1 = every physics constraint
    // User transform of a bone (tools, numeric entry): setBoneLocal plus compensation.
    void transformBone(int bone, const rig::Transform& local, bool rot, bool trans, bool scale, bool shear);
    void drawGhosts(ImDrawList* dl);
    void drawPoseImages(ImDrawList* dl, const rig::Pose& pose, ImVec4 tint);
    void mirrorSelectedWeights();
    void setConstraintValues(int c, const rig::ConstraintValues& v);
    void drawConstraints(ImDrawList* dl);
    void drawPaths(ImDrawList* dl);
    int addPathSlot(int bone, rig::Vec2 fromLocal, rig::Vec2 toLocal);
    // Moves a slot one step in the draw order: setup order in Setup mode, a draw order key in Animate mode.
    void moveDrawOrder(int slot, int delta);
    void keyEvent(int event);  // event key at the current time
    void triggerEvents(float from, float to);  // playback: show fired events in the viewport
    void fireEvent(const rig::EventKey& k, float late);  // shows the event, plays its sound `late` seconds in
    void keyPending();  // key every pending change ("Key edited")
    KeyState boneKeyState(int bone, rig::Channel ch);
    KeyState slotKeyState(int slot);
    void setSlotAttachment(int slot, int attachment);
    rig::Attachment* selectedAttachment();
    // Region attachment edited by the transform tools in Setup mode, if any.
    rig::Attachment* transformTargetAttachment();
};
