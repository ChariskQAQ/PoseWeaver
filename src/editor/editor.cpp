#include "editor/editor.h"

#include "core/mesh_tools.h"
#include "core/serialize.h"
#include "editor/icons.h"
#include "editor/theme.h"
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
            time_ += dt * speed_ * playDir_;
            if (time_ > a->duration || time_ < 0) {
                if (loop_ && a->duration > 0) {
                    time_ = std::fmod(time_ + a->duration, a->duration);
                } else {
                    time_ = std::clamp(time_, 0.0f, a->duration);
                    playing_ = false;
                }
            }
        }
    }
    // Unkeyed changes only live on the frame they were made on.
    if (!pending_.empty() &&
        (mode_ != Mode::Animate || anim_ != pendingAnim_ || std::fabs(time_ - pendingTime_) > kKeyEpsilon))
        pending_.clear();
    updatePose();
    if (statusTime_ > 0) statusTime_ -= dt;

    drawMenuBar();
    drawStatusBar();
    ImGuiID dock = ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_None);
    if (firstFrame_ || resetLayout_) {
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(dock);
        if (resetLayout_ || !node || node->IsLeafNode()) setupDockLayout(dock);
        firstFrame_ = resetLayout_ = false;
    }
    handleShortcuts();
    if (frameAllIn_ > 0 && --frameAllIn_ == 0) frameAll();
    drawTree();
    drawProperties();
    if (tool_ == Tool::Weights) drawWeightsView();
    if (mode_ == Mode::Animate) drawDopesheet();
    drawViewport();
    drawCloseDialog();
    if (showDemo_) ImGui::ShowDemoWindow(&showDemo_);
}

void Editor::setupDockLayout(unsigned int dockspace) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
    ImGuiID center = dockspace;
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.22f, nullptr, &center);
    ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, nullptr, &right);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
    ImGui::DockBuilderDockWindow("###Viewport", center);
    ImGui::DockBuilderDockWindow("###Tree", right);
    ImGui::DockBuilderDockWindow("###Properties", rightBottom);
    ImGui::DockBuilderDockWindow("###Weights", rightBottom);
    ImGui::DockBuilderDockWindow("###Dopesheet", bottom);
    // The viewport is the workspace, not a tab: hide its tab bar like Spine does.
    if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(center)) n->LocalFlags |= ImGuiDockNodeFlags_HiddenTabBar;
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
    pending_.clear();
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

// ---------------------------------------------------------------- selection

void Editor::selectBone(int b) {
    int keepSlot = tool_ == Tool::Weights ? sel_.slot : -1;  // weights paint the selected bone onto the mesh
    sel_.clear();
    if (b < 0 || b >= (int)sk_.bones.size()) return;
    sel_.kind = Selection::Bone;
    sel_.bone = b;
    sel_.slot = keepSlot;
    scrollTreeToSelection_ = true;
}

void Editor::selectSlot(int s) {
    sel_.clear();
    if (s < 0 || s >= (int)sk_.slots.size()) return;
    sel_.kind = Selection::Slot;
    sel_.slot = s;
    sel_.bone = sk_.slots[s].bone;
    scrollTreeToSelection_ = true;
}

void Editor::selectAttachment(int s, int a) {
    selectSlot(s);
    if (sel_.slot < 0 || a < 0 || a >= (int)sk_.slots[s].attachments.size()) return;
    sel_.kind = Selection::Attachment;
    sel_.attachment = a;
}

void Editor::selectAnimation(int a) {
    sel_.clear();
    if (a < 0 || a >= (int)sk_.animations.size()) return;
    sel_.kind = Selection::Animation;
    sel_.index = a;
    if (anim_ != a) {
        anim_ = a;
        time_ = std::min(time_, sk_.animations[a].duration);
    }
    setMode(Mode::Animate);
}

void Editor::validateSelection() {
    if (sel_.bone >= (int)sk_.bones.size()) sel_.bone = -1;
    if (sel_.slot >= (int)sk_.slots.size()) sel_.slot = -1;
    if (sel_.slot >= 0 && sel_.attachment >= (int)sk_.slots[sel_.slot].attachments.size()) sel_.attachment = -1;
    switch (sel_.kind) {
    case Selection::Bone: if (sel_.bone < 0) sel_.kind = Selection::None; break;
    case Selection::Slot: if (sel_.slot < 0) sel_.kind = Selection::None; break;
    case Selection::Attachment: if (sel_.slot < 0 || sel_.attachment < 0) sel_.kind = Selection::None; break;
    case Selection::Animation: if (sel_.index >= (int)sk_.animations.size()) sel_.kind = Selection::None; break;
    case Selection::Image: if (sel_.index >= (int)sk_.images.size()) sel_.kind = Selection::None; break;
    default: break;
    }
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

Attachment* Editor::selectedAttachment() {
    if (sel_.slot < 0 || sel_.slot >= (int)sk_.slots.size()) return nullptr;
    Slot& s = sk_.slots[sel_.slot];
    int a = s.activeAttachment;
    if (mode_ == Mode::Animate && sel_.slot < (int)pose_.slotAttachment.size()) a = pose_.slotAttachment[sel_.slot];
    if (a < 0 || a >= (int)s.attachments.size()) return nullptr;
    return &s.attachments[a];
}

Attachment* Editor::transformTargetAttachment() {
    if (mode_ != Mode::Setup) return nullptr;
    if (sel_.kind != Selection::Slot && sel_.kind != Selection::Attachment) return nullptr;
    Slot& s = sk_.slots[sel_.slot];
    Attachment* a = sel_.kind == Selection::Attachment ? &s.attachments[sel_.attachment] : selectedAttachment();
    return a && a->type == AttachmentType::Region ? a : nullptr;
}

// ---------------------------------------------------------------- pose / animation

void Editor::updatePose() {
    if (mode_ == Mode::Setup) {
        pose_ = makeSetupPose(sk_);
        return;
    }
    resetToSetup(sk_, pose_);
    if (Animation* an = currentAnimation()) applyAnimation(sk_, *an, time_, pose_);
    for (auto& [b, p] : pending_) {
        if (b >= (int)pose_.local.size()) continue;
        Transform& l = pose_.local[b];
        if (p.rot) l.rotation = p.local.rotation;
        if (p.trans) {
            l.x = p.local.x;
            l.y = p.local.y;
        }
        if (p.scale) {
            l.scaleX = p.local.scaleX;
            l.scaleY = p.local.scaleY;
        }
    }
    updateWorld(sk_, pose_);
}

Animation* Editor::currentAnimation() {
    if (anim_ < 0 || anim_ >= (int)sk_.animations.size()) return nullptr;
    return &sk_.animations[anim_];
}

float Editor::snapTime(float t) const { return std::round(t * fps_) / fps_; }

void Editor::setTime(float t) {
    Animation* an = currentAnimation();
    time_ = std::clamp(t, 0.0f, an ? an->duration : 0.0f);
}

void Editor::setMode(Mode m) {
    if (m == mode_) return;
    mode_ = m;
    playing_ = false;
    if (m == Mode::Animate) {
        if (tool_ == Tool::CreateBone || tool_ == Tool::MeshEdit || tool_ == Tool::Weights) setTool(Tool::Rotate);
        if (!currentAnimation()) {
            if (!sk_.animations.empty()) anim_ = 0;
            else createAnimation();
        }
    }
}

void Editor::setTool(Tool t) {
    bool setupOnly = t == Tool::CreateBone || t == Tool::MeshEdit || t == Tool::Weights;
    if (setupOnly && mode_ != Mode::Setup) {
        setStatus("该工具只能在 Setup 模式下使用");
        return;
    }
    if (t == tool_) return;
    prevTool_ = tool_;
    tool_ = t;
    if (t == Tool::Weights) focusWeights_ = 3;
}

int Editor::createAnimation() {
    checkpoint();
    Animation an;
    an.name = "animation" + std::to_string(sk_.animations.size() + 1);
    sk_.animations.push_back(an);
    anim_ = (int)sk_.animations.size() - 1;
    time_ = 0;
    return anim_;
}

static void writeBoneKeys(Animation& an, const Bone& bone, int index, float t, const Transform& local, bool rot,
                          bool trans, bool scale) {
    const Transform& s = bone.setup;
    if (rot) setRotateKey(an, index, t, local.rotation - s.rotation);
    if (trans) setTranslateKey(an, index, t, {local.x - s.x, local.y - s.y});
    if (scale) {
        auto ratio = [](float v, float base) { return std::fabs(base) > 1e-6f ? v / base : 1.0f; };
        setScaleKey(an, index, t, {ratio(local.scaleX, s.scaleX), ratio(local.scaleY, s.scaleY)});
    }
}

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
        updatePose();
        return;
    }
    if (!currentAnimation()) createAnimation();
    if (autoKey_) {
        writeBoneKeys(*currentAnimation(), sk_.bones[bone], bone, time_, local, rot, trans, scale);
        auto it = pending_.find(bone);
        if (it != pending_.end()) {
            if (rot) it->second.rot = false;
            if (trans) it->second.trans = false;
            if (scale) it->second.scale = false;
        }
    } else {
        PendingChange& p = pending_[bone];
        if (rot) {
            p.local.rotation = local.rotation;
            p.rot = true;
        }
        if (trans) {
            p.local.x = local.x;
            p.local.y = local.y;
            p.trans = true;
        }
        if (scale) {
            p.local.scaleX = local.scaleX;
            p.local.scaleY = local.scaleY;
            p.scale = true;
        }
        pendingTime_ = time_;
        pendingAnim_ = anim_;
    }
    updatePose();
}

void Editor::keyBone(int bone, bool rot, bool trans, bool scale) {
    if (bone < 0 || bone >= (int)sk_.bones.size() || mode_ != Mode::Animate) return;
    checkpoint();
    if (!currentAnimation()) createAnimation();
    writeBoneKeys(*currentAnimation(), sk_.bones[bone], bone, time_, pose_.local[bone], rot, trans, scale);
    auto it = pending_.find(bone);
    if (it != pending_.end()) {
        if (rot) it->second.rot = false;
        if (trans) it->second.trans = false;
        if (scale) it->second.scale = false;
        if (!it->second.rot && !it->second.trans && !it->second.scale) pending_.erase(it);
    }
    updatePose();
}

void Editor::keyPending() {
    if (mode_ != Mode::Animate) return;
    bool any = false;
    for (auto& [b, p] : pending_) any |= p.rot || p.trans || p.scale;
    if (!any) {
        setStatus("没有未打帧的改动");
        return;
    }
    checkpoint();
    if (!currentAnimation()) createAnimation();
    for (auto& [b, p] : pending_)
        writeBoneKeys(*currentAnimation(), sk_.bones[b], b, time_, pose_.local[b], p.rot, p.trans, p.scale);
    pending_.clear();
    updatePose();
    setStatus("已为所有改动打关键帧");
}

KeyState Editor::boneKeyState(int bone, Channel ch) {
    auto it = pending_.find(bone);
    if (it != pending_.end()) {
        const PendingChange& p = it->second;
        if ((ch == Channel::Rotate && p.rot) || (ch == Channel::Translate && p.trans) || (ch == Channel::Scale && p.scale))
            return KeyState::Changed;
    }
    Animation* an = currentAnimation();
    return an && hasKey(*an, bone, ch, time_) ? KeyState::Keyed : KeyState::None;
}

KeyState Editor::slotKeyState(int slot) {
    Animation* an = currentAnimation();
    return an && hasKey(*an, slot, Channel::Attachment, time_) ? KeyState::Keyed : KeyState::None;
}

void Editor::setSlotAttachment(int slot, int attachment) {
    if (slot < 0 || slot >= (int)sk_.slots.size()) return;
    checkpoint();
    if (mode_ == Mode::Setup) {
        sk_.slots[slot].activeAttachment = attachment;
    } else {
        if (!currentAnimation()) createAnimation();
        setAttachmentKey(*currentAnimation(), slot, time_, attachment);
    }
    updatePose();
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
    selectBone(0);
    anim_ = -1;
    time_ = 0;
    playing_ = false;
    mode_ = Mode::Setup;
    pending_.clear();
    hiddenBones_.clear();
    hiddenSlots_.clear();
    collapsedBones_.clear();
    sheetLocked_ = false;
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
    selectBone(sk_.bones.empty() ? -1 : 0);
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

    selectAttachment((int)sk_.slots.size() - 1, 0);
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
    hiddenSlots_.clear();
}

void Editor::moveSlot(int slot, int delta, bool snapshot) {
    int other = slot + delta;
    if (slot < 0 || other < 0 || slot >= (int)sk_.slots.size() || other >= (int)sk_.slots.size()) return;
    if (snapshot) checkpoint();
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
    else if (sel_.slot == other) sel_.slot = slot;
    hiddenSlots_.clear();
}

void Editor::renameSelection(const std::string& name) {
    if (name.empty()) return;
    switch (sel_.kind) {
    case Selection::Bone:
        if (name == sk_.bones[sel_.bone].name) return;
        checkpoint();
        sk_.bones[sel_.bone].name = sk_.uniqueBoneName(name);
        break;
    case Selection::Slot:
        if (name == sk_.slots[sel_.slot].name) return;
        checkpoint();
        sk_.slots[sel_.slot].name = sk_.uniqueSlotName(name);
        break;
    case Selection::Attachment:
        checkpoint();
        sk_.slots[sel_.slot].attachments[sel_.attachment].name = name;
        break;
    case Selection::Animation:
        checkpoint();
        sk_.animations[sel_.index].name = name;
        break;
    default:
        break;
    }
}

void Editor::deleteSelection() {
    switch (sel_.kind) {
    case Selection::Slot:
        checkpoint();
        removeSlot(sel_.slot);
        sel_.clear();
        setStatus("已删除插槽");
        return;
    case Selection::Attachment: {
        if (mode_ != Mode::Setup) {
            setStatus("请在 Setup 模式下删除附件");
            return;
        }
        checkpoint();
        int s = sel_.slot, cur = sel_.attachment;
        Slot& slot = sk_.slots[s];
        slot.attachments.erase(slot.attachments.begin() + cur);
        if (slot.activeAttachment == cur) slot.activeAttachment = -1;
        else if (slot.activeAttachment > cur) --slot.activeAttachment;
        for (auto& an : sk_.animations) {
            auto it = an.slots.find(s);
            if (it == an.slots.end()) continue;
            for (auto& k : it->second.attachment) {
                if (k.value == cur) k.value = -1;
                else if (k.value > cur) --k.value;
            }
        }
        selectSlot(s);
        setStatus("已删除附件");
        return;
    }
    case Selection::Animation:
        checkpoint();
        sk_.animations.erase(sk_.animations.begin() + sel_.index);
        anim_ = std::min(anim_, (int)sk_.animations.size() - 1);
        sel_.clear();
        playing_ = false;
        setStatus("已删除动画");
        return;
    case Selection::Image: {
        int img = sel_.index;
        for (auto& s : sk_.slots)
            for (auto& a : s.attachments)
                if (a.image == img) {
                    setStatus("图片仍被附件使用，不能删除");
                    return;
                }
        checkpoint();
        releaseTexture(textures_[img]);
        textures_.erase(textures_.begin() + img);
        sk_.images.erase(sk_.images.begin() + img);
        for (auto& s : sk_.slots)
            for (auto& a : s.attachments)
                if (a.image > img) --a.image;
        sel_.clear();
        return;
    }
    case Selection::Bone: {
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
        hiddenBones_.clear();
        hiddenSlots_.clear();
        pending_.clear();
        selectBone(parent >= 0 && parent < (int)sk_.bones.size() ? parent : -1);
        validateSelection();
        setStatus("已删除骨骼");
        return;
    }
    default:
        return;
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
        if (pressed(ImGuiKey_F)) shift ? frameAll() : zoomTo(1.0f);
        return;
    }
    // Tool hotkeys follow Spine: C rotate, V translate, X scale, B pose, N create, G weights.
    if (pressed(ImGuiKey_C)) setTool(Tool::Rotate);
    if (pressed(ImGuiKey_V)) setTool(Tool::Translate);
    if (pressed(ImGuiKey_X)) setTool(Tool::Scale);
    if (pressed(ImGuiKey_B)) setTool(Tool::Pose);
    if (pressed(ImGuiKey_N)) setTool(Tool::CreateBone);
    if (pressed(ImGuiKey_G)) setTool(Tool::Weights);
    if (pressed(ImGuiKey_M)) setTool(Tool::MeshEdit);
    if (pressed(ImGuiKey_Tab)) setMode(mode_ == Mode::Setup ? Mode::Animate : Mode::Setup);
    if (pressed(ImGuiKey_Escape)) {
        sel_.clear();
        drag_ = Drag{};
    }
    if (pressed(ImGuiKey_Delete)) deleteSelection();
    if (pressed(ImGuiKey_F)) frameAll();
    if (mode_ == Mode::Animate) {
        if (pressed(ImGuiKey_Space)) {
            playDir_ = 1;
            playing_ = !playing_;
        }
        if (pressed(ImGuiKey_K)) keyPending();
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) setTime(snapTime(time_) - 1.0f / fps_);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) setTime(snapTime(time_) + 1.0f / fps_);
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
        if (ImGui::MenuItem("取消选择", "Esc")) sel_.clear();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("视图")) {
        if (ImGui::MenuItem("适配骨架", "F")) frameAll();
        if (ImGui::MenuItem("100% 缩放", "Ctrl+F")) zoomTo(1.0f);
        if (ImGui::MenuItem("重置窗口布局")) resetLayout_ = true;
        ImGui::Separator();
        ImGui::MenuItem("ImGui Demo", nullptr, &showDemo_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("帮助")) {
        ImGui::TextUnformatted(
            "Tab        Setup / Animate 模式\n"
            "C / V / X  旋转 / 移动 / 缩放\n"
            "B / N / G  姿势 / 建骨骼 / 权重      M 网格\n"
            "右键拖动平移，滚轮缩放；右键单击切回上一个工具\n"
            "F 适配骨架，Ctrl+F 100%，Esc 取消选择\n"
            "Animate：空格 播放，K 为所有改动打帧，← → 逐帧\n"
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
            ImGui::TextUnformatted(mode_ == Mode::Setup ? "Setup" : "Animate");
            ImGui::Separator();
            ImGui::Text("骨骼 %d  插槽 %d  动画 %d", (int)sk_.bones.size(), (int)sk_.slots.size(),
                        (int)sk_.animations.size());
            if (!pending_.empty()) {
                ImGui::Separator();
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kKeyChanged), "有未打帧的改动（K 打帧）");
            }
            if (statusTime_ > 0) {
                ImGui::Separator();
                ImGui::TextUnformatted(status_.c_str());
            }
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

void Editor::zoomTo(float zoom) {
    // Zoom around the canvas centre.
    float k = zoom / zoom_;
    viewOffset_.x *= k;
    viewOffset_.y *= k;
    zoom_ = zoom;
}

void Editor::frameAll() {
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
    ch -= ImGui::GetFrameHeightWithSpacing() * 4.5f;  // keep clear of the bottom toolbar
    zoom_ = std::clamp(std::min(cw / w, std::max(ch, 100.0f) / h) * 0.8f, 0.05f, 20.0f);
    float cx = (minX + maxX) / 2, cy = (minY + maxY) / 2;
    viewOffset_ = {-cx * zoom_, cy * zoom_ - ImGui::GetFrameHeightWithSpacing() * 2.0f};
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
    setTranslateKey(an, up, 0.0f, {0, 0});
    setTranslateKey(an, up, 0.5f, {0, 12});
    setTranslateKey(an, up, 1.0f, {0, 0});
    sk_.animations.push_back(an);
    anim_ = 0;

    undo_.clear();
    redo_.clear();
    dirty_ = false;
    if (weightsView) {
        mode_ = Mode::Setup;
        selectSlot(0);
        tool_ = Tool::Weights;
        focusWeights_ = 3;
        selectBone(low);
    } else {
        mode_ = Mode::Animate;
        time_ = time;
        selectBone(low);
    }
    frameAllIn_ = 5;
}
