#pragma once
#include "core/pose.h"
#include "platform/texture.h"

#include <imgui.h>

#include <map>
#include <set>
#include <string>
#include <vector>

enum class Mode { Setup, Animate };
enum class Tool { Rotate, Translate, Scale, Pose, CreateBone, MeshEdit, Weights };
enum class Axes { Local, Parent, World };

struct Selection {
    enum Kind { None, Skeleton, Bone, Slot, Attachment, Animation, Image } kind = None;
    int bone = -1;        // also set for slots/attachments (their bone), used by transform tools
    int slot = -1;
    int attachment = -1;  // attachment index inside `slot`
    int index = -1;       // animation or image index
    std::vector<int> vertices;  // selected mesh vertices (MeshEdit tool)
    void clear() { *this = Selection{}; }
};

// Mouse drag in the viewport.
struct Drag {
    enum Kind { None, Bone, Attachment, Vertices, BoxSelect, CreateBone, Paint, BoneLength, PoseRotate } kind = None;
    ImVec2 startScreen;
    rig::Vec2 startWorld;
    rig::Vec2 pivotWorld;           // bone origin / attachment centre
    rig::Transform startTransform;  // bone local or attachment transform at drag start
    rig::Affine parentWorld;        // space the dragged transform lives in
    std::vector<rig::Vec2> startVertices;
    Tool tool = Tool::Rotate;       // tool that started the drag
    int parentBone = -1;            // CreateBone
    bool moved = false;             // passed the drag threshold
    bool checkpointed = false;      // undo snapshot taken for this drag
};

// Animate-mode changes that have not been keyed yet (auto key off).
struct PendingChange {
    rig::Transform local;
    bool rot = false, trans = false, scale = false;
};

enum class KeyState { None, Changed, Keyed };

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
        enum Kind { Overview, Bone, Slot, Property } kind;
        int index;                                  // bone or slot
        rig::Channel channel = rig::Channel::Rotate;  // Property rows
    };
    struct SheetDrag {
        bool scrubbing = false;
        bool movingKey = false;
        bool panning = false;
        bool checkpointed = false;
        SheetRow row{SheetRow::Overview, -1};
        float from = 0;
    } sheetDrag_;
    SheetRow sheetMenuRow_{SheetRow::Overview, -1};
    float sheetMenuTime_ = 0;
    float sheetPps_ = 0;      // pixels per second, 0 = fit to width
    float sheetScroll_ = 0;   // seconds scrolled off the left edge
    bool sheetLocked_ = false;
    std::vector<int> sheetLockedBones_;
    std::set<int> collapsedBones_;

    // --- mesh / weights tools ---
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
    bool resetLayout_ = false;
    int focusWeights_ = 0;  // frames left to bring the Weights tab to front (docking settles over a few frames)

    // frame parts
    void handleShortcuts();
    void drawMenuBar();
    void drawViewport();
    void drawTree();
    void drawProperties();
    void drawWeightsView();
    void drawDopesheet();
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
    void setTool(Tool t);

    // properties sections
    void propertiesSkeleton();
    void propertiesBone(int b);
    void propertiesSlot(int s);
    void propertiesAttachment(int s, int a);
    void propertiesAnimation(int a);
    void propertiesImage(int i);
    bool propertiesHeader(int icon, const std::string& type, const std::string& name, bool canRename, bool canDelete);

    // selection
    void selectBone(int b);
    void selectSlot(int s);
    void selectAttachment(int s, int a);
    void selectAnimation(int a);
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
    void setBoneLocal(int bone, const rig::Transform& local, bool rot, bool trans, bool scale);
    void keyBone(int bone, bool rot, bool trans, bool scale);
    void keyPending();  // key every pending change ("Key edited")
    KeyState boneKeyState(int bone, rig::Channel ch);
    KeyState slotKeyState(int slot);
    void setSlotAttachment(int slot, int attachment);
    rig::Attachment* selectedAttachment();
    // Region attachment edited by the transform tools in Setup mode, if any.
    rig::Attachment* transformTargetAttachment();
};
