#pragma once
#include "core/pose.h"
#include "platform/texture.h"

#include <imgui.h>

#include <string>
#include <vector>

enum class Mode { Setup, Animate };
enum class Tool { Rotate, Translate, Scale, CreateBone, MeshEdit, Weights };

struct Selection {
    int bone = -1;
    int slot = -1;
    std::vector<int> vertices;  // selected mesh vertices (MeshEdit tool)
    void clear() { *this = Selection{}; }
};

// Mouse drag in the viewport.
struct Drag {
    enum Kind { None, Bone, Attachment, Vertices, BoxSelect, CreateBone, Paint, Pan } kind = None;
    ImVec2 startScreen;
    rig::Vec2 startWorld;
    rig::Vec2 pivotWorld;           // bone origin / attachment centre
    rig::Transform startTransform;  // bone local or attachment transform at drag start
    rig::Affine parentWorld;        // space the dragged transform lives in
    std::vector<rig::Vec2> startVertices;
    int parentBone = -1;            // CreateBone
    bool moved = false;             // passed the drag threshold
    bool checkpointed = false;      // undo snapshot taken for this drag
};

class Editor {
public:
    Editor();
    ~Editor();

    void frame(float dt);      // build all UI for one frame
    bool requestClose();       // returns true if the app may close now
    bool shouldQuit() const { return closeConfirmed_; }
    void onFilesDropped(const std::vector<std::string>& paths);
    void setUiScale(float s) { uiScale_ = s; }
    bool openProject(const std::string& path);
    // Builds a two-bone skinned arm with a "wave" animation from `imagePath`.
    // weightsView = stay in Setup mode with the weights tool showing the lower bone.
    void buildDemo(const std::string& imagePath, float time, bool weightsView);

private:
    // --- document ---
    rig::Skeleton sk_;
    std::vector<Texture> textures_;  // parallel to sk_.images
    std::string projectPath_;        // UTF-8, empty = never saved
    bool dirty_ = false;

    // --- undo ---
    std::vector<std::string> undo_, redo_;
    void checkpoint();  // call before every document change
    void undo();
    void redo();
    void restore(const std::string& snapshot);

    // --- editing state ---
    Mode mode_ = Mode::Setup;
    Tool tool_ = Tool::Rotate;
    Selection sel_;
    int anim_ = -1;  // current animation index
    float time_ = 0;
    bool playing_ = false;
    bool loop_ = true;
    float fps_ = 30;
    rig::Pose pose_;  // what the viewport shows this frame

    // --- view ---
    ImVec2 viewOffset_{0, 0};
    float zoom_ = 1.0f;
    ImVec2 canvasOrigin_, canvasSize_;
    bool showBones_ = true, showImages_ = true, showMesh_ = true;
    Drag drag_;
    float uiScale_ = 1.0f;

    // --- timeline ---
    struct TimelineRow {
        enum Kind { Summary, Bone, Slot } kind;
        int index;
    };
    struct TimelineDrag {
        bool scrubbing = false;
        bool movingKey = false;
        bool checkpointed = false;
        TimelineRow row{TimelineRow::Summary, -1};
        float from = 0;
    } tlDrag_;
    TimelineRow tlMenuRow_{TimelineRow::Summary, -1};
    float tlMenuTime_ = 0;
    std::string renameBuf_;

    // --- mesh / weights tools ---
    int gridCols_ = 6, gridRows_ = 6;
    float brushRadius_ = 40, brushStrength_ = 0.5f;
    std::vector<int> bindBones_;  // bones checked in the bind dialog

    // --- misc ---
    std::string status_;
    float statusTime_ = 0;
    bool firstFrame_ = true;
    int frameAllIn_ = 0;  // frames until frameAll() runs (canvas size is known after one frame)
    bool showCloseDialog_ = false;
    bool closeConfirmed_ = false;
    bool showDemo_ = false;

    // frame parts
    void handleShortcuts();
    void drawMenuBar();
    void drawToolbar();
    void drawViewport();
    void drawHierarchy();
    void drawInspector();
    void drawTimeline();
    void drawStatusBar();
    void drawCloseDialog();
    void setupDockLayout(unsigned int dockspace);

    // inspector sections
    void inspectBone(int b);
    void inspectSlot(int s);
    void inspectAttachment(int s, int a);

    // viewport helpers
    ImVec2 toScreen(rig::Vec2 w) const;
    rig::Vec2 toWorld(ImVec2 s) const;
    int pickBone(ImVec2 screen) const;
    int pickSlot(rig::Vec2 world) const;
    int pickVertex(ImVec2 screen) const;
    void viewportMouseDown(ImVec2 mouse, bool shift);
    void viewportDrag(ImVec2 mouse, bool shift, bool ctrl, float dt);
    void viewportMouseUp(ImVec2 mouse, bool shift);
    void drawSlots(ImDrawList* dl);
    void drawBones(ImDrawList* dl);
    void drawMeshOverlay(ImDrawList* dl);
    void frameAll();

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
    void moveSlot(int slot, int delta);  // change draw order
    void validateSelection();
    void setStatus(const std::string& msg);
    std::string resolveImagePath(const std::string& stored) const;

    // pose / animation helpers
    void updatePose();
    rig::Animation* currentAnimation();
    float snapTime(float t) const;
    // Applies a new local transform for a bone: edits setup in Setup mode, keys in Animate mode.
    void setBoneLocal(int bone, const rig::Transform& local, bool rot, bool trans, bool scale);
    void keyBone(int bone, bool rot, bool trans, bool scale);
    void setSlotAttachment(int slot, int attachment);
    rig::Attachment* selectedAttachment();
    bool canEditSetup() const { return mode_ == Mode::Setup; }
};
