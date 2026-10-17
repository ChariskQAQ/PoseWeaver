#include "editor/editor.h"

#include "core/mesh_tools.h"
#include "core/serialize.h"
#include "platform/file_dialog.h"

#include <imgui_internal.h>

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;
using namespace rig;

static constexpr size_t kMaxUndo = 200;

Editor::Editor() { newProject(); }

Editor::~Editor() {
    for (auto& t : textures_) releaseTexture(t);
}

// ---------------------------------------------------------------- frame

void Editor::frame(float dt) {
    if (playing_) {
        Animation* a = currentAnimation();
        if (!a || mode_ != Mode::Animate) {
            playing_ = false;
        } else {
            time_ += dt;
            if (time_ > a->duration) {
                if (loop_ && a->duration > 0) {
                    time_ = std::fmod(time_, a->duration);
                } else {
                    time_ = a->duration;
                    playing_ = false;
                }
            }
        }
    }
    updatePose();
    if (statusTime_ > 0) statusTime_ -= dt;

    drawMenuBar();
    drawStatusBar();
    ImGuiID dock = ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_None);
    if (firstFrame_) {
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(dock);
        if (!node || node->IsLeafNode()) setupDockLayout(dock);
        firstFrame_ = false;
    }
    handleShortcuts();
    if (frameAllIn_ > 0 && --frameAllIn_ == 0) frameAll();
    drawHierarchy();
    drawInspector();
    drawTimeline();
    drawViewport();
    drawCloseDialog();
    if (showDemo_) ImGui::ShowDemoWindow(&showDemo_);
}

void Editor::setupDockLayout(unsigned int dockspace) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
    ImGuiID center = dockspace;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);
    ImGui::DockBuilderDockWindow("###Hierarchy", left);
    ImGui::DockBuilderDockWindow("###Inspector", right);
    ImGui::DockBuilderDockWindow("###Timeline", bottom);
    ImGui::DockBuilderDockWindow("###Viewport", center);
    ImGui::DockBuilderFinish(dockspace);
}

// ---------------------------------------------------------------- undo

void Editor::checkpoint() {
    undo_.push_back(toJson(sk_, -1));
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
    dirty_ = true;
}

void Editor::restore(const std::string& snapshot) {
    std::vector<std::string> oldPaths;
    for (auto& im : sk_.images) oldPaths.push_back(im.path);
    Skeleton s;
    std::string err;
    if (!fromJson(snapshot, s, &err)) {
        setStatus("撤销失败: " + err);
        return;
    }
    sk_ = std::move(s);
    std::vector<std::string> newPaths;
    for (auto& im : sk_.images) newPaths.push_back(im.path);
    if (newPaths != oldPaths) reloadTextures();
    if (anim_ >= (int)sk_.animations.size()) anim_ = (int)sk_.animations.size() - 1;
    validateSelection();
    dirty_ = true;
}

void Editor::undo() {
    if (undo_.empty()) return;
    redo_.push_back(toJson(sk_, -1));
    std::string s = std::move(undo_.back());
    undo_.pop_back();
    restore(s);
}

void Editor::redo() {
    if (redo_.empty()) return;
    undo_.push_back(toJson(sk_, -1));
    std::string s = std::move(redo_.back());
    redo_.pop_back();
    restore(s);
}

void Editor::validateSelection() {
    if (sel_.bone >= (int)sk_.bones.size()) sel_.bone = -1;
    if (sel_.slot >= (int)sk_.slots.size()) sel_.slot = -1;
    Attachment* a = selectedAttachment();
    if (!a || a->type != AttachmentType::Mesh) {
        sel_.vertices.clear();
    } else {
        sel_.vertices.erase(std::remove_if(sel_.vertices.begin(), sel_.vertices.end(),
                                           [&](int v) { return v >= (int)a->vertexCount(); }),
                            sel_.vertices.end());
    }
    bindBones_.erase(std::remove_if(bindBones_.begin(), bindBones_.end(),
                                    [&](int b) { return b >= (int)sk_.bones.size(); }),
                     bindBones_.end());
}

// ---------------------------------------------------------------- pose / animation

void Editor::updatePose() {
    pose_ = mode_ == Mode::Animate ? makeAnimatedPose(sk_, currentAnimation(), time_) : makeSetupPose(sk_);
}

Animation* Editor::currentAnimation() {
    if (anim_ < 0 || anim_ >= (int)sk_.animations.size()) return nullptr;
    return &sk_.animations[anim_];
}

float Editor::snapTime(float t) const { return std::round(t * fps_) / fps_; }

void Editor::setBoneLocal(int bone, const Transform& local, bool rot, bool trans, bool scale) {
    if (bone < 0 || bone >= (int)sk_.bones.size()) return;
    if (mode_ == Mode::Setup) {
        Transform& s = sk_.bones[bone].setup;
        if (rot) s.rotation = local.rotation;
        if (trans) {
            s.x = local.x;
            s.y = local.y;
        }
        if (scale) {
            s.scaleX = local.scaleX;
            s.scaleY = local.scaleY;
        }
        return;
    }
    if (!currentAnimation()) {
        sk_.animations.push_back({});
        sk_.animations.back().name = "animation";
        anim_ = (int)sk_.animations.size() - 1;
    }
    Animation& an = *currentAnimation();
    const Transform& s = sk_.bones[bone].setup;
    float t = time_;
    if (rot) setRotateKey(an, bone, t, local.rotation - s.rotation);
    if (trans) setTranslateKey(an, bone, t, {local.x - s.x, local.y - s.y});
    if (scale) {
        auto ratio = [](float v, float base) { return std::fabs(base) > 1e-6f ? v / base : 1.0f; };
        setScaleKey(an, bone, t, {ratio(local.scaleX, s.scaleX), ratio(local.scaleY, s.scaleY)});
    }
    updatePose();
}

void Editor::keyBone(int bone, bool rot, bool trans, bool scale) {
    if (bone < 0 || bone >= (int)sk_.bones.size() || mode_ != Mode::Animate) return;
    checkpoint();
    Transform local = pose_.local[bone];
    setBoneLocal(bone, local, rot, trans, scale);
}

void Editor::setSlotAttachment(int slot, int attachment) {
    if (slot < 0 || slot >= (int)sk_.slots.size()) return;
    checkpoint();
    if (mode_ == Mode::Setup) {
        sk_.slots[slot].activeAttachment = attachment;
    } else if (Animation* an = currentAnimation()) {
        setAttachmentKey(*an, slot, time_, attachment);
    }
    updatePose();
}

Attachment* Editor::selectedAttachment() {
    if (sel_.slot < 0 || sel_.slot >= (int)sk_.slots.size()) return nullptr;
    Slot& s = sk_.slots[sel_.slot];
    int a = s.activeAttachment;
    if (mode_ == Mode::Animate && sel_.slot < (int)pose_.slotAttachment.size()) a = pose_.slotAttachment[sel_.slot];
    if (a < 0 || a >= (int)s.attachments.size()) return nullptr;
    return &s.attachments[a];
}

// ---------------------------------------------------------------- document

void Editor::setStatus(const std::string& msg) {
    status_ = msg;
    statusTime_ = 6.0f;
}

void Editor::newProject() {
    for (auto& t : textures_) releaseTexture(t);
    textures_.clear();
    sk_ = Skeleton{};
    sk_.addBone("root", -1, {}, 0);
    projectPath_.clear();
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    sel_.clear();
    sel_.bone = 0;
    anim_ = -1;
    time_ = 0;
    playing_ = false;
    mode_ = Mode::Setup;
    viewOffset_ = {0, 0};
    zoom_ = 1;
}

std::string Editor::resolveImagePath(const std::string& stored) const {
    fs::path p = fs::u8path(stored);
    if (p.is_absolute() || projectPath_.empty()) return stored;
    return (fs::u8path(projectPath_).parent_path() / p).lexically_normal().u8string();
}

void Editor::reloadTextures() {
    for (auto& t : textures_) releaseTexture(t);
    textures_.assign(sk_.images.size(), Texture{});
    std::string missing;
    for (size_t i = 0; i < sk_.images.size(); ++i) {
        std::string err;
        if (loadTexture(sk_.images[i].path, textures_[i], &err)) {
            sk_.images[i].width = textures_[i].width;
            sk_.images[i].height = textures_[i].height;
        } else {
            missing += (missing.empty() ? "" : ", ") + fs::u8path(sk_.images[i].path).filename().u8string();
        }
    }
    if (!missing.empty()) setStatus("找不到图片: " + missing);
}

void Editor::openProjectDialog() {
    std::string path = openFileDialog("RigEditor 工程 (*.rig.json)", "*.rig.json;*.json");
    if (!path.empty()) openProject(path);
}

bool Editor::openProject(const std::string& path) {
    Skeleton s;
    std::string err;
    if (!loadFile(path, s, &err)) {
        setStatus("打开失败: " + err);
        return false;
    }
    newProject();
    sk_ = std::move(s);
    projectPath_ = path;
    // Keep absolute image paths in memory; they are made relative again on save.
    for (auto& im : sk_.images) im.path = resolveImagePath(im.path);
    reloadTextures();
    sel_.clear();
    if (!sk_.bones.empty()) sel_.bone = 0;
    anim_ = sk_.animations.empty() ? -1 : 0;
    dirty_ = false;
    frameAllIn_ = 5;
    setStatus("已打开 " + fs::u8path(path).filename().u8string());
    return true;
}

// Copy of the skeleton with image paths relative to `dir` where possible.
static Skeleton withRelativePaths(const Skeleton& sk, const fs::path& dir) {
    Skeleton out = sk;
    for (auto& im : out.images) {
        std::error_code ec;
        fs::path rel = fs::relative(fs::u8path(im.path), dir, ec);
        // A path on another drive cannot be made relative; keep it absolute.
        if (!ec && !rel.empty() && !rel.has_root_name()) im.path = rel.generic_u8string();
    }
    return out;
}

bool Editor::saveProject(bool saveAs) {
    std::string path = projectPath_;
    if (saveAs || path.empty()) {
        path = saveFileDialog("RigEditor 工程 (*.rig.json)", "*.rig.json", "rig.json");
        if (path.empty()) return false;
    }
    std::string err;
    Skeleton out = withRelativePaths(sk_, fs::u8path(path).parent_path());
    if (!saveFile(path, out, &err)) {
        setStatus("保存失败: " + err);
        return false;
    }
    projectPath_ = path;
    dirty_ = false;
    setStatus("已保存 " + fs::u8path(path).filename().u8string());
    return true;
}

void Editor::exportRuntimeJson() {
    std::string path = saveFileDialog("运行时 JSON (*.json)", "*.json", "json");
    if (path.empty()) return;
    std::string err;
    Skeleton out = withRelativePaths(sk_, fs::u8path(path).parent_path());
    if (saveFile(path, out, &err))
        setStatus("已导出 " + fs::u8path(path).filename().u8string());
    else
        setStatus("导出失败: " + err);
}

void Editor::importImageDialog() {
    std::string path = openFileDialog("图片 (png/jpg/bmp/tga)", "*.png;*.jpg;*.jpeg;*.bmp;*.tga");
    if (!path.empty()) importImage(path);
}

bool Editor::importImage(const std::string& path) {
    Texture tex;
    std::string err;
    if (!loadTexture(path, tex, &err)) {
        setStatus("无法读取图片: " + err);
        return false;
    }
    checkpoint();
    if (sk_.bones.empty()) sk_.addBone("root", -1, {}, 0);

    std::string abs = fs::absolute(fs::u8path(path)).lexically_normal().u8string();
    int image = -1;
    for (size_t i = 0; i < sk_.images.size(); ++i)
        if (sk_.images[i].path == abs) image = (int)i;
    if (image < 0) {
        sk_.images.push_back({abs, tex.width, tex.height});
        textures_.push_back(std::move(tex));
        image = (int)sk_.images.size() - 1;
    } else {
        releaseTexture(tex);
    }

    std::string stem = fs::u8path(path).stem().u8string();
    Slot slot;
    slot.name = sk_.uniqueSlotName(stem);
    slot.bone = sel_.bone >= 0 ? sel_.bone : 0;
    Attachment att;
    att.name = stem;
    att.image = image;
    // Place the image at the world origin regardless of which bone it is attached to.
    Pose setup = makeSetupPose(sk_);
    att.transform = decompose(setup.world[slot.bone].inverse());
    slot.attachments.push_back(att);
    slot.activeAttachment = 0;
    sk_.slots.push_back(slot);

    sel_.slot = (int)sk_.slots.size() - 1;
    sel_.bone = slot.bone;
    setStatus("已导入 " + stem);
    return true;
}

void Editor::onFilesDropped(const std::vector<std::string>& paths) {
    for (auto& p : paths) {
        std::string ext = fs::u8path(p).extension().u8string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (ext == ".json")
            openProject(p);
        else
            importImage(p);
    }
}

void Editor::removeSlot(int slot) {
    if (slot < 0 || slot >= (int)sk_.slots.size()) return;
    sk_.slots.erase(sk_.slots.begin() + slot);
    for (auto& an : sk_.animations) {
        std::map<int, SlotTimeline> st;
        for (auto& [i, tl] : an.slots) {
            if (i == slot) continue;
            st[i > slot ? i - 1 : i] = std::move(tl);
        }
        an.slots = std::move(st);
    }
}

void Editor::moveSlot(int slot, int delta) {
    int other = slot + delta;
    if (slot < 0 || other < 0 || slot >= (int)sk_.slots.size() || other >= (int)sk_.slots.size()) return;
    checkpoint();
    std::swap(sk_.slots[slot], sk_.slots[other]);
    for (auto& an : sk_.animations) {
        auto a = an.slots.find(slot), b = an.slots.find(other);
        SlotTimeline ta, tb;
        bool ha = a != an.slots.end(), hb = b != an.slots.end();
        if (ha) ta = std::move(a->second);
        if (hb) tb = std::move(b->second);
        an.slots.erase(slot);
        an.slots.erase(other);
        if (ha) an.slots[other] = std::move(ta);
        if (hb) an.slots[slot] = std::move(tb);
    }
    if (sel_.slot == slot) sel_.slot = other;
}

void Editor::deleteSelection() {
    if (sel_.slot >= 0) {
        checkpoint();
        removeSlot(sel_.slot);
        sel_.slot = -1;
        sel_.vertices.clear();
        setStatus("已删除插槽");
        return;
    }
    if (sel_.bone >= 0) {
        if (mode_ != Mode::Setup) {
            setStatus("请在 Setup 模式下删除骨骼");
            return;
        }
        if (sk_.bones.size() == 1) {
            setStatus("至少保留一根骨骼");
            return;
        }
        checkpoint();
        int parent = sk_.bones[sel_.bone].parent;
        sk_.removeBone(sel_.bone);
        sel_.bone = parent >= 0 && parent < (int)sk_.bones.size() ? parent : -1;
        validateSelection();
        setStatus("已删除骨骼");
    }
}

// ---------------------------------------------------------------- close

bool Editor::requestClose() {
    if (!dirty_ || closeConfirmed_) return true;
    showCloseDialog_ = true;
    return false;
}

void Editor::drawCloseDialog() {
    if (showCloseDialog_) {
        ImGui::OpenPopup("未保存的修改");
        showCloseDialog_ = false;
    }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("未保存的修改", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("工程有未保存的修改，退出前要保存吗？");
        ImGui::Spacing();
        if (ImGui::Button("保存并退出")) {
            if (saveProject(false)) closeConfirmed_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("不保存")) {
            closeConfirmed_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------- shortcuts / menus

void Editor::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
    bool ctrl = io.KeyCtrl, shift = io.KeyShift;
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };

    if (ctrl) {
        if (pressed(ImGuiKey_Z)) shift ? redo() : undo();
        if (pressed(ImGuiKey_Y)) redo();
        if (pressed(ImGuiKey_S)) saveProject(shift);
        if (pressed(ImGuiKey_O)) openProjectDialog();
        if (pressed(ImGuiKey_N)) {
            if (!dirty_) newProject();
            else setStatus("有未保存的修改，请先保存或从菜单新建");
        }
        if (pressed(ImGuiKey_I)) importImageDialog();
        return;
    }
    if (pressed(ImGuiKey_R)) tool_ = Tool::Rotate;
    if (pressed(ImGuiKey_T)) tool_ = Tool::Translate;
    if (pressed(ImGuiKey_S)) tool_ = Tool::Scale;
    if (mode_ == Mode::Setup) {
        if (pressed(ImGuiKey_B)) tool_ = Tool::CreateBone;
        if (pressed(ImGuiKey_M)) tool_ = Tool::MeshEdit;
        if (pressed(ImGuiKey_W)) tool_ = Tool::Weights;
    }
    if (pressed(ImGuiKey_Tab)) {
        mode_ = mode_ == Mode::Setup ? Mode::Animate : Mode::Setup;
        if (mode_ == Mode::Animate && tool_ > Tool::Scale) tool_ = Tool::Rotate;
        playing_ = false;
    }
    if (pressed(ImGuiKey_Delete)) deleteSelection();
    if (pressed(ImGuiKey_F)) frameAll();
    if (mode_ == Mode::Animate) {
        if (pressed(ImGuiKey_Space)) playing_ = !playing_;
        if (pressed(ImGuiKey_K) && sel_.bone >= 0) keyBone(sel_.bone, true, true, true);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) time_ = std::max(0.0f, snapTime(time_) - 1.0f / fps_);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            Animation* a = currentAnimation();
            time_ = std::min(a ? a->duration : 0.0f, snapTime(time_) + 1.0f / fps_);
        }
    }
}

void Editor::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("文件")) {
        if (ImGui::MenuItem("新建", "Ctrl+N")) newProject();
        if (ImGui::MenuItem("打开...", "Ctrl+O")) openProjectDialog();
        if (ImGui::MenuItem("保存", "Ctrl+S")) saveProject(false);
        if (ImGui::MenuItem("另存为...", "Ctrl+Shift+S")) saveProject(true);
        ImGui::Separator();
        if (ImGui::MenuItem("导入图片...", "Ctrl+I")) importImageDialog();
        if (ImGui::MenuItem("导出运行时 JSON...")) exportRuntimeJson();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("编辑")) {
        if (ImGui::MenuItem("撤销", "Ctrl+Z", false, !undo_.empty())) undo();
        if (ImGui::MenuItem("重做", "Ctrl+Y", false, !redo_.empty())) redo();
        ImGui::Separator();
        if (ImGui::MenuItem("删除所选", "Del")) deleteSelection();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("视图")) {
        ImGui::MenuItem("显示骨骼", nullptr, &showBones_);
        ImGui::MenuItem("显示图片", nullptr, &showImages_);
        ImGui::MenuItem("显示网格线", nullptr, &showMesh_);
        if (ImGui::MenuItem("适配视图", "F")) frameAll();
        ImGui::Separator();
        ImGui::MenuItem("ImGui Demo", nullptr, &showDemo_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("帮助")) {
        ImGui::TextUnformatted(
            "Tab 切换 Setup/Animate 模式\n"
            "R 旋转  T 移动  S 缩放  B 建骨骼  M 网格  W 权重\n"
            "鼠标中键/右键拖动平移，滚轮缩放，F 适配视图\n"
            "Animate 模式：拖动骨骼自动打关键帧，K 为所选骨骼打全部关键帧\n"
            "空格 播放/暂停，左右方向键 逐帧\n"
            "把 PNG 拖进窗口即可导入");
        ImGui::EndMenu();
    }
    std::string title = projectPath_.empty() ? "未命名" : fs::u8path(projectPath_).filename().u8string();
    if (dirty_) title += " *";
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::CalcTextSize(title.c_str()).x - 16);
    ImGui::TextDisabled("%s", title.c_str());
    ImGui::EndMainMenuBar();
}

void Editor::drawStatusBar() {
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    float h = ImGui::GetFrameHeight();
    if (ImGui::BeginViewportSideBar("##status", ImGui::GetMainViewport(), ImGuiDir_Down, h, flags)) {
        if (ImGui::BeginMenuBar()) {
            ImGui::TextUnformatted(mode_ == Mode::Setup ? "Setup 模式" : "Animate 模式");
            ImGui::Separator();
            ImGui::Text("骨骼 %d  插槽 %d  动画 %d", (int)sk_.bones.size(), (int)sk_.slots.size(),
                        (int)sk_.animations.size());
            ImGui::Separator();
            ImGui::Text("缩放 %.0f%%", zoom_ * 100);
            if (statusTime_ > 0) {
                ImGui::Separator();
                ImGui::TextUnformatted(status_.c_str());
            }
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

void Editor::frameAll() {
    // Fit the setup-pose bounding box of bones and attachments into the canvas.
    Pose p = mode_ == Mode::Animate ? pose_ : makeSetupPose(sk_);
    if (p.world.size() != sk_.bones.size()) return;
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    auto add = [&](Vec2 v) {
        minX = std::min(minX, v.x);
        minY = std::min(minY, v.y);
        maxX = std::max(maxX, v.x);
        maxY = std::max(maxY, v.y);
    };
    for (size_t i = 0; i < sk_.bones.size(); ++i) {
        add(p.world[i].origin());
        add(boneTip(sk_.bones[i], p.world[i]));
    }
    for (size_t s = 0; s < sk_.slots.size(); ++s) {
        int a = s < p.slotAttachment.size() ? p.slotAttachment[s] : -1;
        if (a < 0 || a >= (int)sk_.slots[s].attachments.size()) continue;
        for (Vec2 v : computeVertices(sk_, p, sk_.slots[s], sk_.slots[s].attachments[a])) add(v);
    }
    if (minX > maxX) return;
    float w = std::max(maxX - minX, 50.0f), h = std::max(maxY - minY, 50.0f);
    float cw = canvasSize_.x > 0 ? canvasSize_.x : 800, ch = canvasSize_.y > 0 ? canvasSize_.y : 600;
    zoom_ = std::clamp(std::min(cw / w, ch / h) * 0.85f, 0.05f, 20.0f);
    float cx = (minX + maxX) / 2, cy = (minY + maxY) / 2;
    viewOffset_ = {-cx * zoom_, cy * zoom_};
}

// ---------------------------------------------------------------- demo

void Editor::buildDemo(const std::string& imagePath, float time, bool weightsView) {
    newProject();
    if (!importImage(imagePath)) return;
    Transform upper;
    upper.y = -120;
    upper.rotation = 90;
    int up = sk_.addBone("upper", 0, upper, 120);
    Transform lower;
    lower.x = 120;
    int low = sk_.addBone("lower", up, lower, 120);

    Slot& slot = sk_.slots[0];
    Attachment& att = slot.attachments[0];
    const Texture& tex = textures_[att.image];
    convertRegionToMesh(att, sk_.images[att.image], tex.rgba.data(), 3, 12);
    bindMesh(sk_, slot, att, {up, low});

    Animation an;
    an.name = "wave";
    an.duration = 1.0f;
    setRotateKey(an, low, 0.0f, 0.0f);
    setRotateKey(an, low, 0.5f, -75.0f);
    setRotateKey(an, low, 1.0f, 0.0f);
    setRotateKey(an, up, 0.0f, 0.0f);
    setRotateKey(an, up, 0.5f, 20.0f);
    setRotateKey(an, up, 1.0f, 0.0f);
    sk_.animations.push_back(an);
    anim_ = 0;

    undo_.clear();
    redo_.clear();
    dirty_ = false;
    sel_.clear();
    sel_.slot = 0;
    sel_.bone = low;
    if (weightsView) {
        mode_ = Mode::Setup;
        tool_ = Tool::Weights;
    } else {
        mode_ = Mode::Animate;
        time_ = time;
    }
    frameAllIn_ = 5;
}
