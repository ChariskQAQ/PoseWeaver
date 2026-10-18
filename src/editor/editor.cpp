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

Editor::Editor() : audio_(std::make_unique<AudioPlayer>()) {
    newProject();
    loadSettings();
}

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
            float prev = time_;
            time_ += dt * speed_ * playDir_;
            bool wrapped = false;
            if (time_ > a->duration || time_ < 0) {
                if (loop_ && a->duration > 0) {
                    time_ = std::fmod(time_ + a->duration, a->duration);
                    wrapped = true;
                } else {
                    time_ = std::clamp(time_, 0.0f, a->duration);
                    playing_ = false;
                }
            }
            if (playDir_ > 0) {
                if (wrapped) {
                    triggerEvents(prev, a->duration);
                    triggerEvents(-1, time_);
                } else {
                    triggerEvents(prev, time_);
                }
            }
        }
    }
    // Mix preview: the AnimationState advances on its own clock; dopesheet playback takes the viewport back.
    if (playing_ || mode_ != Mode::Animate) mixActive_ = false;
    if (mixActive_ && mixPlaying_)
        for (const rig::FiredEvent& e : mixState_.update(sk_, dt * speed_)) fireEvent(e.key, 0);
    // Audio follows playback: start sounds already in progress, stop everything on pause.
    if (playing_ && !wasPlaying_ && playDir_ > 0) startAudioAt(time_);
    if (!playing_ && wasPlaying_) audio_->stopAll();
    wasPlaying_ = playing_;
    audio_->update();
    for (auto& e : firedEvents_) e.timeLeft -= dt;
    firedEvents_.erase(std::remove_if(firedEvents_.begin(), firedEvents_.end(), [](const FiredEvent& e) { return e.timeLeft <= 0; }),
                       firedEvents_.end());
    // Unkeyed changes only live on the frame they were made on.
    if (!pending_.empty() &&
        (mode_ != Mode::Animate || anim_ != pendingAnim_ || std::fabs(time_ - pendingTime_) > kKeyEpsilon))
        pending_.clear();
    physicsDt_ = dt;  // physics advances once per frame; other updatePose calls only re-apply it
    updatePose();
    physicsDt_ = 0;
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
    if (mode_ == Mode::Animate) {
        drawDopesheet();
        drawGraph();
        drawMixPreview();
    }
    drawViewport();
    drawCloseDialog();
    drawSplash();
    drawShortcuts();
    drawIoDialogs();
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
    ImGui::DockBuilderDockWindow("###Graph", bottom);
    ImGui::DockBuilderDockWindow("###Preview", bottom);
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
    mixActive_ = false;  // edits key the current animation: show it again instead of the mix
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

void Editor::selectEvent(int e) {
    sel_.clear();
    if (e < 0 || e >= (int)sk_.events.size()) return;
    sel_.kind = Selection::Event;
    sel_.index = e;
}

void Editor::selectConstraint(int c) {
    sel_.clear();
    if (c < 0 || c >= (int)sk_.constraints.size()) return;
    sel_.kind = Selection::Constraint;
    sel_.index = c;
}

void Editor::selectSkin(int skin) {
    sel_.clear();
    if (skin < 0 || skin >= (int)sk_.skins.size()) return;
    sel_.kind = Selection::Skin;
    sel_.index = skin;
}

void Editor::validateSelection() {
    activeSkins_.erase(std::remove_if(activeSkins_.begin(), activeSkins_.end(), [&](int s) { return s >= (int)sk_.skins.size(); }),
                       activeSkins_.end());
    if (sel_.bone >= (int)sk_.bones.size()) sel_.bone = -1;
    if (sel_.slot >= (int)sk_.slots.size()) sel_.slot = -1;
    if (sel_.slot >= 0 && sel_.attachment >= (int)sk_.slots[sel_.slot].attachments.size()) sel_.attachment = -1;
    switch (sel_.kind) {
    case Selection::Bone: if (sel_.bone < 0) sel_.kind = Selection::None; break;
    case Selection::Slot: if (sel_.slot < 0) sel_.kind = Selection::None; break;
    case Selection::Attachment: if (sel_.slot < 0 || sel_.attachment < 0) sel_.kind = Selection::None; break;
    case Selection::Animation: if (sel_.index >= (int)sk_.animations.size()) sel_.kind = Selection::None; break;
    case Selection::Image: if (sel_.index >= (int)sk_.images.size()) sel_.kind = Selection::None; break;
    case Selection::Event: if (sel_.index >= (int)sk_.events.size()) sel_.kind = Selection::None; break;
    case Selection::Constraint: if (sel_.index >= (int)sk_.constraints.size()) sel_.kind = Selection::None; break;
    case Selection::Skin: if (sel_.index >= (int)sk_.skins.size()) sel_.kind = Selection::None; break;
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
    return sk_.resolve(activeSkins_, sel_.slot, a);
}

Attachment* Editor::transformTargetAttachment() {
    if (mode_ != Mode::Setup) return nullptr;
    if (sel_.kind != Selection::Slot && sel_.kind != Selection::Attachment) return nullptr;
    Slot& s = sk_.slots[sel_.slot];
    (void)s;
    Attachment* a = sel_.kind == Selection::Attachment ? sk_.resolve(activeSkins_, sel_.slot, sel_.attachment) : selectedAttachment();
    return a && a->type == AttachmentType::Region ? a : nullptr;
}

// ---------------------------------------------------------------- pose / animation

void Editor::updatePose() {
    if (mode_ == Mode::Setup) {
        pose_ = makeSetupPose(sk_, activeSkins_);
        if (physicsOn_) applyPhysics(sk_, pose_, physics_, physicsDt_);
        return;
    }
    resetToSetup(sk_, pose_);
    pose_.skins = activeSkins_;
    if (mixActive_) {
        mixState_.apply(sk_, pose_);
        updateWorld(sk_, pose_);
        if (physicsOn_) applyPhysics(sk_, pose_, physics_, physicsDt_);
        return;
    }
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
        if (p.shear) {
            l.shearX = p.local.shearX;
            l.shearY = p.local.shearY;
        }
    }
    updateWorld(sk_, pose_);
    if (physicsOn_) applyPhysics(sk_, pose_, physics_, physicsDt_);
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
        if (tool_ == Tool::CreateBone || tool_ == Tool::Weights) setTool(Tool::Rotate);
        if (!currentAnimation()) {
            if (!sk_.animations.empty()) anim_ = 0;
            else createAnimation();
        }
    }
}

void Editor::setTool(Tool t) {
    bool setupOnly = t == Tool::CreateBone || t == Tool::Weights;  // the mesh tool keys deforms in Animate mode
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
                          bool trans, bool scale, bool shear) {
    const Transform& s = bone.setup;
    if (rot) setRotateKey(an, index, t, local.rotation - s.rotation);
    if (trans) setTranslateKey(an, index, t, {local.x - s.x, local.y - s.y});
    if (scale) {
        auto ratio = [](float v, float base) { return std::fabs(base) > 1e-6f ? v / base : 1.0f; };
        setScaleKey(an, index, t, {ratio(local.scaleX, s.scaleX), ratio(local.scaleY, s.scaleY)});
    }
    if (shear) setShearKey(an, index, t, {local.shearX - s.shearX, local.shearY - s.shearY});
}

void Editor::setBoneLocal(int bone, const Transform& local, bool rot, bool trans, bool scale, bool shear) {
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
        if (shear) {
            s.shearX = local.shearX;
            s.shearY = local.shearY;
        }
        updatePose();
        return;
    }
    if (!currentAnimation()) createAnimation();
    if (autoKey_) {
        writeBoneKeys(*currentAnimation(), sk_.bones[bone], bone, time_, local, rot, trans, scale, shear);
        auto it = pending_.find(bone);
        if (it != pending_.end()) {
            if (rot) it->second.rot = false;
            if (trans) it->second.trans = false;
            if (scale) it->second.scale = false;
            if (shear) it->second.shear = false;
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
        if (shear) {
            p.local.shearX = local.shearX;
            p.local.shearY = local.shearY;
            p.shear = true;
        }
        pendingTime_ = time_;
        pendingAnim_ = anim_;
    }
    updatePose();
}

void Editor::keyBone(int bone, bool rot, bool trans, bool scale, bool shear) {
    if (bone < 0 || bone >= (int)sk_.bones.size() || mode_ != Mode::Animate) return;
    checkpoint();
    if (!currentAnimation()) createAnimation();
    writeBoneKeys(*currentAnimation(), sk_.bones[bone], bone, time_, pose_.local[bone], rot, trans, scale, shear);
    auto it = pending_.find(bone);
    if (it != pending_.end()) {
        if (rot) it->second.rot = false;
        if (trans) it->second.trans = false;
        if (scale) it->second.scale = false;
        if (shear) it->second.shear = false;
        if (!it->second.any()) pending_.erase(it);
    }
    updatePose();
}

void Editor::keyPending() {
    if (mode_ != Mode::Animate) return;
    bool any = false;
    for (auto& [b, p] : pending_) any |= p.any();
    if (!any) {
        setStatus("没有未打帧的改动");
        return;
    }
    checkpoint();
    if (!currentAnimation()) createAnimation();
    for (auto& [b, p] : pending_)
        writeBoneKeys(*currentAnimation(), sk_.bones[b], b, time_, pose_.local[b], p.rot, p.trans, p.scale, p.shear);
    pending_.clear();
    updatePose();
    setStatus("已为所有改动打关键帧");
}

KeyState Editor::boneKeyState(int bone, Channel ch) {
    auto it = pending_.find(bone);
    if (it != pending_.end()) {
        const PendingChange& p = it->second;
        if ((ch == Channel::Rotate && p.rot) || (ch == Channel::Translate && p.trans) || (ch == Channel::Scale && p.scale) ||
            (ch == Channel::Shear && p.shear))
            return KeyState::Changed;
    }
    Animation* an = currentAnimation();
    return an && hasKey(*an, bone, ch, time_) ? KeyState::Keyed : KeyState::None;
}

KeyState Editor::slotKeyState(int slot) {
    Animation* an = currentAnimation();
    return an && hasKey(*an, slot, Channel::Attachment, time_) ? KeyState::Keyed : KeyState::None;
}

KeyState Editor::slotColorKeyState(int slot) {
    Animation* an = currentAnimation();
    return an && hasKey(*an, slot, Channel::Color, time_) ? KeyState::Keyed : KeyState::None;
}

void Editor::setSlotColor(int slot, const Color& color) {
    if (slot < 0 || slot >= (int)sk_.slots.size()) return;
    if (mode_ == Mode::Setup) {
        sk_.slots[slot].color = color;
    } else {
        if (!currentAnimation()) createAnimation();
        setColorKey(*currentAnimation(), slot, time_, color);
    }
    updatePose();
}

void Editor::moveDrawOrder(int slot, int delta) {
    if (mode_ == Mode::Setup) {
        moveSlot(slot, delta);
        return;
    }
    std::vector<int> order = pose_.drawOrder;
    auto it = std::find(order.begin(), order.end(), slot);
    if (it == order.end()) return;
    int pos = (int)(it - order.begin()), other = pos + delta;
    if (other < 0 || other >= (int)order.size()) return;
    checkpoint();
    if (!currentAnimation()) createAnimation();
    std::swap(order[pos], order[other]);
    setDrawOrderKey(*currentAnimation(), time_, order);
    updatePose();
}

void Editor::keyEvent(int event) {
    if (event < 0 || event >= (int)sk_.events.size() || mode_ != Mode::Animate) return;
    checkpoint();
    if (!currentAnimation()) createAnimation();
    setEventKey(*currentAnimation(), time_, event, sk_.events[event]);
    setStatus("已在第 " + std::to_string((int)std::round(time_ * fps_)) + " 帧插入事件 " + sk_.events[event].name);
}

void Editor::triggerEvents(float from, float to) {
    Animation* an = currentAnimation();
    if (!an) return;
    for (const EventKey* k : eventsInRange(*an, from, to)) fireEvent(*k, to - k->time);
}

void Editor::fireEvent(const EventKey& k, float late) {
    if (k.event < 0 || k.event >= (int)sk_.events.size()) return;
    const EventDef& def = sk_.events[k.event];
    std::string text = def.name;
    if (!k.stringValue.empty()) text += " \"" + k.stringValue + "\"";
    if (!def.audioPath.empty()) {
        text += "  ♪";
        if (audioOn_) audio_->play(def.audioPath, std::max(0.0f, late), def.volume, def.balance, speed_);
    }
    firedEvents_.push_back({text, 1.2f});
}

void Editor::startAudioAt(float t) {
    Animation* an = currentAnimation();
    if (!an || !audioOn_) return;
    for (const AudioEventKey& k : audioEventKeys(sk_, *an)) {
        const EventDef& def = sk_.events[k.event];
        const AudioSamples* s = audio_->samples(def.audioPath);
        // Keys exactly at `t` fire through triggerEvents on the next step.
        if (s && k.time < t && t < k.time + s->duration() / std::max(speed_, 0.01f))
            audio_->play(def.audioPath, (t - k.time) * speed_, def.volume, def.balance, speed_);
    }
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
    physics_.reset();
    activeSkins_.clear();
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
    for (auto& e : sk_.events)
        if (!e.audioPath.empty()) e.audioPath = resolveImagePath(e.audioPath);
    reloadTextures();
    selectBone(sk_.bones.empty() ? -1 : 0);
    anim_ = sk_.animations.empty() ? -1 : 0;
    if (!sk_.skins.empty()) activeSkins_ = {0};
    dirty_ = false;
    frameAllIn_ = 5;
    addRecent(path);
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
    for (auto& e : out.events) {
        if (e.audioPath.empty()) continue;
        std::error_code ec;
        fs::path rel = fs::relative(fs::u8path(e.audioPath), dir, ec);
        if (!ec && !rel.empty() && !rel.has_root_name()) e.audioPath = rel.generic_u8string();
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
    addRecent(path);
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
    sk_.removeSlot(slot);
    hiddenSlots_.clear();
}

void Editor::moveSlot(int slot, int delta, bool snapshot) {
    int other = slot + delta;
    if (slot < 0 || other < 0 || slot >= (int)sk_.slots.size() || other >= (int)sk_.slots.size()) return;
    if (snapshot) checkpoint();
    sk_.swapSlots(slot, other);
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
        if (sk_.slots[sel_.slot].attachments[sel_.attachment].skinPlaceholder) {
            // Skin entries are linked to the placeholder by name.
            const std::string old = sk_.slots[sel_.slot].attachments[sel_.attachment].name;
            for (auto& skin : sk_.skins)
                for (auto& e : skin.attachments)
                    if (e.slot == sel_.slot && e.placeholder == old) e.placeholder = name;
        }
        sk_.slots[sel_.slot].attachments[sel_.attachment].name = name;
        break;
    case Selection::Animation:
        checkpoint();
        sk_.animations[sel_.index].name = name;
        break;
    case Selection::Event:
        if (name == sk_.events[sel_.index].name) return;
        checkpoint();
        sk_.events[sel_.index].name = sk_.uniqueEventName(name);
        break;
    case Selection::Skin:
        if (name == sk_.skins[sel_.index].name) return;
        checkpoint();
        sk_.skins[sel_.index].name = sk_.uniqueSkinName(name);
        break;
    case Selection::Constraint:
        if (name == sk_.constraints[sel_.index].name) return;
        checkpoint();
        sk_.constraints[sel_.index].name = sk_.uniqueConstraintName(name);
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
        int s = sel_.slot;
        sk_.removeAttachment(s, sel_.attachment);
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
    case Selection::Event:
        checkpoint();
        sk_.removeEvent(sel_.index);
        sel_.clear();
        setStatus("已删除事件");
        return;
    case Selection::Skin: {
        checkpoint();
        int skin = sel_.index;
        sk_.removeSkin(skin);
        std::vector<int> kept;
        for (int s : activeSkins_)
            if (s != skin) kept.push_back(s > skin ? s - 1 : s);
        activeSkins_ = kept;
        sel_.clear();
        setStatus("已删除皮肤");
        return;
    }
    case Selection::Constraint:
        checkpoint();
        sk_.removeConstraint(sel_.index);
        physics_.reset();
        sel_.clear();
        setStatus("已删除约束");
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

// ---------------------------------------------------------------- skins

Attachment* Editor::editableAttachment(int slot, int attachment) { return sk_.resolve(activeSkins_, slot, attachment); }

void Editor::toggleSkin(int skin) {
    auto it = std::find(activeSkins_.begin(), activeSkins_.end(), skin);
    if (it != activeSkins_.end()) activeSkins_.erase(it);
    else activeSkins_.push_back(skin);
    physics_.reset();
    validateSelection();
    updatePose();
}

int Editor::createSkin(const std::string& name) {
    checkpoint();
    Skin skin;
    skin.name = sk_.uniqueSkinName(name);
    sk_.skins.push_back(skin);
    int index = (int)sk_.skins.size() - 1;
    activeSkins_ = {index};
    updatePose();
    return index;
}

void Editor::moveAttachmentToSkin(int s, int a, int skin) {
    if (s < 0 || s >= (int)sk_.slots.size() || skin < 0 || skin >= (int)sk_.skins.size()) return;
    Attachment& att = sk_.slots[s].attachments[a];
    if (att.skinPlaceholder) return;
    checkpoint();
    Skin& target = sk_.skins[skin];
    if (Attachment* existing = target.find(s, att.name)) *existing = att;
    else target.attachments.push_back({s, att.name, att});
    Attachment placeholder;
    placeholder.name = att.name;
    placeholder.type = att.type;
    placeholder.skinPlaceholder = true;
    att = placeholder;
    if (std::find(activeSkins_.begin(), activeSkins_.end(), skin) == activeSkins_.end()) activeSkins_.push_back(skin);
    updatePose();
    setStatus("已移入皮肤 " + target.name + "：其他皮肤可以为这个占位提供自己的附件");
}

// ---------------------------------------------------------------- compensation

void Editor::transformBone(int bone, const Transform& local, bool rot, bool trans, bool scale, bool shear) {
    if (bone < 0 || bone >= (int)sk_.bones.size()) return;
    Pose before = pose_;
    setBoneLocal(bone, local, rot, trans, scale, shear);
    if (!compBones_ && !compImages_) return;
    const Affine& newWorld = pose_.world[bone];
    const Affine& oldWorld = before.world[bone];
    if (compBones_) {
        // Children keep their world transform: give them the inverse change.
        for (int c : sk_.children(bone)) {
            Transform want = decompose(newWorld.inverse() * before.world[c]);
            const Transform& cur = pose_.local[c];
            auto differs = [](float a, float b) { return std::fabs(a - b) > 1e-4f; };
            bool r = differs(want.rotation, cur.rotation);
            bool t = differs(want.x, cur.x) || differs(want.y, cur.y);
            bool s = differs(want.scaleX, cur.scaleX) || differs(want.scaleY, cur.scaleY);
            bool sh = differs(want.shearX, cur.shearX) || differs(want.shearY, cur.shearY);
            want.rotation = cur.rotation + wrapDeg(want.rotation - cur.rotation);
            if (r || t || s || sh) setBoneLocal(c, want, r, t, s, sh);
        }
    }
    if (compImages_) {
        if (mode_ != Mode::Setup) {
            setStatus("图片补偿只在 Setup 模式下生效");
            return;
        }
        // Attachments on the bone keep their world placement (unweighted ones; weighted
        // vertices follow their bones by design).
        Affine change = newWorld.inverse() * oldWorld;
        for (auto& slot : sk_.slots) {
            if (slot.bone != bone) continue;
            for (auto& att : slot.attachments) {
                if (att.type == AttachmentType::Region) att.transform = decompose(change * att.transform.matrix());
                else if (!att.isWeighted())
                    for (auto& v : att.vertices) v = change.apply(v);
            }
        }
        updatePose();
    }
}

void Editor::bakePhysics(int only) {
    Animation* an = currentAnimation();
    if (!an || mode_ != Mode::Animate) {
        setStatus("请在 Animate 模式下选择要烘焙的动画");
        return;
    }
    std::vector<int> cons;
    for (int i = 0; i < (int)sk_.constraints.size(); ++i)
        if (sk_.constraints[i].type == ConstraintType::Physics && (only < 0 || only == i) && !sk_.constraints[i].bones.empty())
            cons.push_back(i);
    if (cons.empty()) {
        setStatus("没有可烘焙的物理约束");
        return;
    }
    checkpoint();
    const int frames = std::max(1, (int)std::round(an->duration * fps_));
    const float step = 1.0f / fps_;
    // Disable the baked constraints for this animation, then simulate: one warm-up pass when
    // looping so the first frames match the end of the loop.
    std::vector<ConstraintValues> saved;
    for (int c : cons) saved.push_back(sk_.constraints[c].values);
    PhysicsState state;
    std::map<int, std::vector<Transform>> baked;  // bone -> applied local per frame
    for (int pass = loop_ ? 0 : 1; pass < 2; ++pass) {
        for (int f = 0; f <= frames; ++f) {
            Pose p = makeAnimatedPose(sk_, an, f * step, activeSkins_);
            applyPhysics(sk_, p, state, f == 0 && pass == 0 ? 0.0f : step);
            if (pass == 1)
                for (int c : cons) baked[sk_.constraints[c].bones[0]].push_back(p.applied[sk_.constraints[c].bones[0]]);
        }
    }
    for (auto& [b, locals] : baked) {
        const Transform& s = sk_.bones[b].setup;
        BoneTimeline& tl = an->bones[b];
        tl.rotate.clear();
        tl.translate.clear();
        for (int f = 0; f <= frames; ++f) {
            const Transform& l = locals[f];
            setRotateKey(*an, b, f * step, l.rotation - s.rotation);
            setTranslateKey(*an, b, f * step, {l.x - s.x, l.y - s.y});
        }
    }
    for (size_t i = 0; i < cons.size(); ++i) {
        ConstraintValues v = saved[i];
        v[0] = 0;  // mix 0: this animation plays the baked keys instead of simulating
        an->constraints[cons[i]].clear();
        setConstraintKey(*an, cons[i], 0, v);
    }
    physics_.reset();
    updatePose();
    setStatus("已烘焙 " + std::to_string(cons.size()) + " 个物理约束，" + std::to_string(frames + 1) + " 帧（该动画中物理混合已设为 0）");
}

void Editor::mirrorSelectedWeights() {
    Attachment* att = selectedAttachment();
    if (!att || !att->isWeighted()) return;
    const Slot& slot = sk_.slots[sel_.slot];
    Pose setup = makeSetupPose(sk_);
    float axis = 0;
    if (mirrorAxis_ == 0) {
        std::vector<Vec2> world = computeVertices(sk_, setup, slot, *att);
        float lo = 1e9f, hi = -1e9f;
        for (Vec2 p : world) {
            lo = std::min(lo, p.x);
            hi = std::max(hi, p.x);
        }
        axis = (lo + hi) * 0.5f;
    } else if (mirrorAxis_ == 1 && sel_.bone >= 0) {
        axis = setup.world[sel_.bone].origin().x;
    }
    std::vector<int> mirror(sk_.bones.size(), -1);
    int pairs = 0;
    for (int b = 0; b < (int)sk_.bones.size(); ++b) {
        std::string m = mirrorName(sk_.bones[b].name);
        int mb = m.empty() ? -1 : sk_.findBone(m);
        if (mb >= 0) {
            mirror[b] = mb;
            ++pairs;
        }
    }
    checkpoint();
    int changed = mirrorWeights(sk_, slot, *att, axis, mirrorLeftToRight_, mirror);
    setStatus("已镜像 " + std::to_string(changed) + " 个顶点的权重（配对骨骼 " + std::to_string(pairs / 2) + " 对）");
}

// ---------------------------------------------------------------- constraints

int Editor::addPathSlot(int bone, Vec2 a, Vec2 b) {
    Slot slot;
    slot.name = sk_.uniqueSlotName("path");
    slot.bone = bone;
    Attachment path;
    path.name = "path";
    path.type = AttachmentType::Path;
    Vec2 d = (b - a) * (1.0f / 3);
    makePolygon(path, {a - d, a, a + d, b - d, b, b + d});
    slot.attachments.push_back(path);
    slot.activeAttachment = 0;
    sk_.slots.push_back(slot);
    return (int)sk_.slots.size() - 1;
}

int Editor::createConstraint(ConstraintType type) {
    int b = sel_.bone;
    if (b < 0 || b >= (int)sk_.bones.size()) {
        setStatus("先选择要约束的骨骼");
        return -1;
    }
    if (mode_ != Mode::Setup) {
        setStatus("请在 Setup 模式下新建约束");
        return -1;
    }
    checkpoint();
    Pose setup = makeSetupPose(sk_);
    int root = 0;
    while (root < (int)sk_.bones.size() && sk_.bones[root].parent >= 0) ++root;
    auto targetBone = [&](const std::string& name, Vec2 world) {
        Transform t = decompose(setup.world[root].inverse() * Affine::fromTRS(world.x, world.y, 0, 1, 1));
        return sk_.addBone(name, root, t, 0);
    };
    Constraint c;
    c.type = type;
    c.values = defaultConstraintValues(type);
    const std::string bn = sk_.bones[b].name;  // copy: adding target bones reallocates sk_.bones
    switch (type) {
    case ConstraintType::Ik: {
        int parent = sk_.bones[b].parent;
        bool two = parent >= 0 && sk_.bones[parent].length > 0;
        c.bones = two ? std::vector<int>{parent, b} : std::vector<int>{b};
        c.target = targetBone(bn + "_ik", boneTip(sk_.bones[b], setup.world[b]));
        c.name = sk_.uniqueConstraintName(bn + "_ik");
        break;
    }
    case ConstraintType::Transform:
        c.bones = {b};
        c.target = targetBone(bn + "_target", setup.world[b].origin());
        c.name = sk_.uniqueConstraintName(bn + "_transform");
        break;
    case ConstraintType::Path: {
        // The chain starting at the bone (following first children) lies along a new path.
        std::vector<int> chain{b};
        float total = sk_.bones[b].length;
        for (int cur = b; chain.size() < 16;) {
            std::vector<int> kids = sk_.children(cur);
            if (kids.empty()) break;
            cur = kids[0];
            chain.push_back(cur);
            total += sk_.bones[cur].length;
        }
        Vec2 start = setup.world[b].origin();
        Vec2 dir = setup.world[b].applyVector({1, 0});
        float dl = std::max(length(dir), 1e-4f);
        Vec2 end = start + dir * (std::max(total, 100.0f) / dl);
        Affine toRoot = setup.world[root].inverse();
        c.bones = chain;
        c.target = addPathSlot(root, toRoot.apply(start), toRoot.apply(end));
        c.name = sk_.uniqueConstraintName(bn + "_path");
        break;
    }
    case ConstraintType::Physics:
        c.bones = {b};
        c.name = sk_.uniqueConstraintName(bn + "_physics");
        break;
    }
    sk_.constraints.push_back(c);
    physics_.reset();
    int index = (int)sk_.constraints.size() - 1;
    selectConstraint(index);
    updatePose();
    static const char* hints[] = {"已新建 IK 约束：移动目标骨骼 (V) 看效果", "已新建变换约束：移动/旋转目标骨骼看效果",
                                  "已新建路径约束：用网格工具 (M) 编辑路径", "已新建物理约束：拖动父骨骼或播放动画看效果"};
    setStatus(hints[(int)type]);
    return index;
}

void Editor::keyConstraint(int c) {
    if (c < 0 || c >= (int)sk_.constraints.size() || mode_ != Mode::Animate) return;
    checkpoint();
    if (!currentAnimation()) createAnimation();
    setConstraintKey(*currentAnimation(), c, time_, c < (int)pose_.constraints.size() ? pose_.constraints[c] : sk_.constraints[c].values);
    updatePose();
}

void Editor::setConstraintValues(int c, const ConstraintValues& v) {
    if (c < 0 || c >= (int)sk_.constraints.size()) return;
    if (mode_ == Mode::Setup) {
        sk_.constraints[c].values = v;
    } else {
        if (!currentAnimation()) createAnimation();
        setConstraintKey(*currentAnimation(), c, time_, v);
    }
    updatePose();
}

// ---------------------------------------------------------------- mesh & polygon editing

void Editor::afterTopologyChange(int slot, int attachment) {
    int dropped = sk_.dropStaleDeformKeys(slot, attachment);
    if (dropped > 0) setStatus("顶点数变化，已删除 " + std::to_string(dropped) + " 个变形关键帧");
    sel_.vertices.clear();
    updatePose();
}

void Editor::meshAddVertexAt(ImVec2 mouse) {
    Attachment* att = selectedAttachment();
    if (!att || !att->hasVertices()) {
        setStatus("先选择一个网格、剪裁或边界框附件");
        return;
    }
    int s = sel_.slot, a = attachmentIndex(s, att);
    const Slot& slot = sk_.slots[s];
    if (att->type == AttachmentType::Path) {
        // Paths grow by whole points: in-handle, point, out-handle, appended at the end.
        if (att->isWeighted()) {
            setStatus("已绑定骨骼的路径不能加点，请先解除绑定");
            return;
        }
        checkpoint();
        Pose setup = makeSetupPose(sk_);
        Vec2 p = setup.world[slot.bone].inverse().apply(toWorld(mouse));
        Vec2 last = att->vertices.size() >= 2 ? att->vertices[att->vertices.size() - 2] : p - Vec2{60, 0};
        Vec2 d = (p - last) * (1.0f / 3);
        att->vertices.insert(att->vertices.end(), {p - d, p, p + d});
        att->hullLength = (int)att->vertices.size();
        afterTopologyChange(s, a);
        sel_.vertices = {(int)att->vertices.size() - 2};
        return;
    }
    checkpoint();
    if (!att->isPolygon() && att->hullLength < 3 && !ensureHull(*att)) {
        setStatus("这个网格有多个外轮廓，无法加顶点：请先“自动生成网格”");
        return;
    }
    Pose setup = makeSetupPose(sk_);
    Vec2 local = setup.world[slot.bone].inverse().apply(toWorld(mouse));
    std::vector<Vec2> verts = computeVertices(sk_, setup, slot, *att);
    int hull = att->isPolygon() ? (int)verts.size() : att->hullLength;
    // Nearest outline edge (screen space) decides where an outline vertex goes.
    int edge = -1;
    float best = 1e9f;
    std::vector<Vec2> outline;
    for (int i = 0; i < hull; ++i) {
        outline.push_back(verts[i]);
        float d = distToSegment({mouse.x, mouse.y}, {toScreen(verts[i]).x, toScreen(verts[i]).y},
                                {toScreen(verts[(i + 1) % hull]).x, toScreen(verts[(i + 1) % hull]).y});
        if (d < best) {
            best = d;
            edge = i;
        }
    }
    bool inside = !att->isPolygon() && pointInPolygon(toWorld(mouse), outline);
    int hullAfter = (inside && best > 8.0f * uiScale_) ? -1 : edge;
    const Image* img = att->image >= 0 && att->image < (int)sk_.images.size() ? &sk_.images[att->image] : nullptr;
    int v = addMeshVertex(sk_, slot, *att, img, local, hullAfter);
    afterTopologyChange(s, a);
    if (v >= 0) sel_.vertices = {v};
}

void Editor::meshDeleteSelectedVertices() {
    Attachment* att = selectedAttachment();
    if (!att || !att->hasVertices() || sel_.vertices.empty()) return;
    int s = sel_.slot, a = attachmentIndex(s, att);
    if (att->type == AttachmentType::Path) {
        // Remove whole path points; a path keeps at least two.
        std::vector<int> points;
        for (int v : sel_.vertices) points.push_back(v / 3);
        std::sort(points.begin(), points.end());
        points.erase(std::unique(points.begin(), points.end()), points.end());
        int count = (int)att->vertexCount() / 3;
        if (count - (int)points.size() < 2 || att->isWeighted()) {
            setStatus("路径至少需要 2 个点（已绑定的路径不能删点）");
            return;
        }
        checkpoint();
        for (auto it = points.rbegin(); it != points.rend(); ++it)
            att->vertices.erase(att->vertices.begin() + *it * 3, att->vertices.begin() + *it * 3 + 3);
        att->hullLength = (int)att->vertices.size();
        afterTopologyChange(s, a);
        return;
    }
    checkpoint();
    if (!removeMeshVertices(sk_, sk_.slots[s], *att, sel_.vertices)) {
        setStatus("外轮廓至少需要 3 个顶点");
        return;
    }
    afterTopologyChange(s, a);
}

bool Editor::autoMesh(int s, int a) {
    Attachment* target = editableAttachment(s, a);
    if (!target) {
        setStatus("皮肤占位没有被激活的皮肤填充");
        return false;
    }
    Attachment& att = *target;
    if (att.image < 0 || att.image >= (int)textures_.size() || textures_[att.image].rgba.empty()) {
        setStatus("图片未加载，无法生成网格");
        return false;
    }
    const Texture& tex = textures_[att.image];
    std::vector<Vec2> outline = traceAlphaOutline(tex.rgba.data(), tex.width, tex.height, 8, outlineTolerance_, outlinePadding_);
    if (outline.empty()) {
        setStatus("图片完全透明，无法生成网格");
        return false;
    }
    std::vector<int> bones = att.isWeighted() ? meshBones(att) : std::vector<int>{};
    Attachment copy = att;
    if (copy.isWeighted()) unbindMesh(sk_, sk_.slots[s], copy);
    float spacing = meshDensity_ > 0 ? std::max(tex.width, tex.height) / (float)meshDensity_ : 0.0f;
    if (!buildMeshFromOutline(sk_, sk_.slots[s], copy, sk_.images[att.image], outline, spacing)) {
        setStatus("生成网格失败");
        return false;
    }
    if (!bones.empty()) bindMesh(sk_, sk_.slots[s], copy, bones);
    checkpoint();
    att = std::move(copy);
    afterTopologyChange(s, a);
    setStatus("已生成网格：" + std::to_string(att.vertexCount()) + " 个顶点" + (bones.empty() ? "" : "，已按原骨骼重新绑定"));
    return true;
}

void Editor::createPolygonAttachment(int s, AttachmentType type) {
    if (s < 0 || s >= (int)sk_.slots.size()) return;
    bool clipping = type == AttachmentType::Clipping;
    // Outline: bounding box of the slot's visible attachment, else a square around the bone.
    Pose setup = makeSetupPose(sk_);
    const Slot& src = sk_.slots[s];
    std::vector<Vec2> pts;
    if (const Attachment* shown = sk_.resolve(activeSkins_, s, src.activeAttachment)) {
        std::vector<Vec2> world = computeVertices(sk_, setup, src, *shown);
        if (!world.empty()) {
            Vec2 lo = world[0], hi = world[0];
            for (Vec2 p : world) {
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
            }
            Affine inv = setup.world[src.bone].inverse();
            pts = {inv.apply(lo), inv.apply({hi.x, lo.y}), inv.apply(hi), inv.apply({lo.x, hi.y})};
        }
    }
    if (pts.empty()) pts = {{-50, -50}, {50, -50}, {50, 50}, {-50, 50}};

    checkpoint();
    Slot slot;
    slot.name = sk_.uniqueSlotName(src.name + (clipping ? "_clip" : "_bbox"));
    slot.bone = src.bone;
    Attachment att;
    att.name = clipping ? "clipping" : "boundingbox";
    att.type = type;
    makePolygon(att, pts);
    slot.attachments.push_back(att);
    slot.activeAttachment = 0;
    sk_.slots.push_back(slot);
    int index = (int)sk_.slots.size() - 1;
    if (clipping) {
        // A clipping slot clips the slots drawn after it: put it right below the source slot.
        for (int i = index; i > s; --i) sk_.swapSlots(i - 1, i);
        index = s;
        sk_.slots[index].attachments[0].endSlot = s + 1;
    }
    hiddenSlots_.clear();
    selectAttachment(index, 0);
    setTool(Tool::MeshEdit);
    setStatus(clipping ? "已添加剪裁：拖动顶点调整形状，Alt+点击加顶点" : "已添加边界框：拖动顶点调整形状，Alt+点击加顶点");
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
    if (pressed(ImGuiKey_Z)) setTool(Tool::Shear);
    if (pressed(ImGuiKey_B)) setTool(Tool::Pose);
    if (pressed(ImGuiKey_N)) setTool(Tool::CreateBone);
    if (pressed(ImGuiKey_G)) setTool(Tool::Weights);
    if (pressed(ImGuiKey_M)) setTool(Tool::MeshEdit);
    if (pressed(ImGuiKey_Tab)) setMode(mode_ == Mode::Setup ? Mode::Animate : Mode::Setup);
    if (pressed(ImGuiKey_Escape)) {
        sel_.clear();
        drag_ = Drag{};
    }
    if (pressed(ImGuiKey_Delete) && !sheetActive_) {
        if (tool_ == Tool::MeshEdit && mode_ == Mode::Setup && !sel_.vertices.empty()) meshDeleteSelectedVertices();
        else deleteSelection();
    }
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
        if (ImGui::MenuItem("导入 PSD...")) io_.open = IoState::Psd;
        if (ImGui::MenuItem("导入工程 / 数据（合并）...")) {
            std::string path = openFileDialog("RigEditor 工程或数据 (*.rig.json;*.json;*.rigb)", "*.rig.json;*.json;*.rigb");
            if (!path.empty()) importMergePath(path);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("导出数据 (JSON / 二进制)...")) io_.open = IoState::Data;
        if (ImGui::MenuItem("纹理打包...")) io_.open = IoState::Atlas;
        if (ImGui::MenuItem("导出图片 / GIF / 视频...")) {
            io_.imageAnim = anim_;
            io_.open = IoState::Images;
        }
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
        if (ImGui::MenuItem("启动页...")) openSplash();
        if (ImGui::MenuItem("快捷键...")) showShortcuts_ = true;
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
            if (!sk_.skins.empty()) {
                ImGui::Separator();
                std::string names;
                for (int k : activeSkins_) names += (names.empty() ? "" : " + ") + sk_.skins[k].name;
                ImGui::Text("皮肤: %s", names.empty() ? "(无)" : names.c_str());
            }
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
        const Attachment* att = poseAttachment(sk_, p, (int)s);
        if (!att) continue;
        for (Vec2 v : computeVertices(sk_, p, sk_.slots[s], *att)) add(v);
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

void Editor::buildDemo(const std::string& imagePath, float time, const std::string& scenario) {
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
    std::vector<Vec2> outline = traceAlphaOutline(tex.rgba.data(), tex.width, tex.height, 8, 2.0f, 2.0f);
    if (!buildMeshFromOutline(sk_, slot, att, sk_.images[att.image], outline, tex.height / 12.0f))
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
    setKeyCurve(an, low, Channel::Rotate, 0.0f, CurveShape::bezier(0.42f, 0, 0.58f, 1));
    setKeyCurve(an, low, Channel::Rotate, 0.5f, CurveShape::bezier(0.42f, 0, 0.58f, 1));
    setColorKey(an, 0, 0.0f, Color(1, 1, 1, 1));
    setColorKey(an, 0, 0.5f, Color(1, 0.75f, 0.75f, 1));
    setColorKey(an, 0, 1.0f, Color(1, 1, 1, 1));
    EventDef peak;
    peak.name = "wave_peak";
    // The demo's peak event plays samples/swoosh.wav when it exists next to the image.
    fs::path swoosh = fs::absolute(fs::u8path(imagePath)).parent_path() / "swoosh.wav";
    if (fs::exists(swoosh)) peak.audioPath = swoosh.lexically_normal().u8string();
    sk_.events.push_back(peak);
    setEventKey(an, 0.5f, 0, peak);
    // Deform: the hand end bulges outwards at the peak of the wave.
    {
        // Widen the top third sideways (world x), expressed in each vertex's deform space.
        Pose setup = makeSetupPose(sk_);
        std::vector<Vec2> world = computeVertices(sk_, setup, slot, att);
        std::vector<Vec2> bulge(att.vertexCount());
        for (size_t v = 0; v < bulge.size(); ++v)
            if (world[v].y > 50)
                bulge[v] = deformBasis(setup, slot, att, (int)v).inverse().applyVector({world[v].x * 0.6f * (world[v].y - 50) / 80, 0});
        setDeformKey(an, 0, 0, 0.0f, std::vector<Vec2>(att.vertexCount()));
        setDeformKey(an, 0, 0, 0.5f, bulge);
        setDeformKey(an, 0, 0, 1.0f, std::vector<Vec2>(att.vertexCount()));
    }
    sk_.animations.push_back(an);
    anim_ = 0;

    undo_.clear();
    redo_.clear();
    dirty_ = false;
    if (scenario == "skin") {
        // Two skins for the arm: the original stripes and a recoloured variant.
        std::string alt = (std::filesystem::u8path(imagePath).parent_path() / "arm_alt.png").u8string();
        int blue = createSkin("stripes");
        moveAttachmentToSkin(0, 0, blue);
        int red = createSkin("plain");
        Texture altTex;
        std::string err;
        if (loadTexture(alt, altTex, &err)) {
            sk_.images.push_back({alt, altTex.width, altTex.height});
            textures_.push_back(std::move(altTex));
            Attachment copy = *sk_.skins[blue].find(0, "arm");
            copy.name = "arm_plain";
            copy.image = (int)sk_.images.size() - 1;
            sk_.skins[red].attachments.push_back({0, "arm", copy});
        }
        activeSkins_ = {red};
        undo_.clear();
        dirty_ = false;
        mode_ = Mode::Animate;
        time_ = time;
        selectAttachment(0, 0);
        frameAllIn_ = 5;
        return;
    }
    if (scenario == "mix") {
        // A second animation crossfading in on track 0, a colour layer at half weight on track 1.
        Animation raise;
        raise.name = "raise";
        setRotateKey(raise, up, 0.0f, -50.0f);
        setRotateKey(raise, low, 0.0f, 60.0f);
        Animation tint;
        tint.name = "tint";
        setColorKey(tint, 0, 0.0f, Color(0.5f, 0.8f, 1, 1));
        sk_.animations.push_back(raise);
        sk_.animations.push_back(tint);
        mode_ = Mode::Animate;
        mixState_.setMix(0, 1, 0.4f);
        mixState_.setMix(-1, 2, 0.0f);  // the colour layer appears at once
        mixUi_[0] = {1, true, 1.0f};
        mixUi_[1] = {2, true, 0.5f};
        mixState_.setAnimation(0, 0, true);
        mixState_.update(sk_, std::max(0.0f, time));
        mixState_.setAnimation(0, 1, true);
        mixState_.update(sk_, 0.2f);  // halfway through the 0.4 s crossfade
        mixState_.setTrackAlpha(1, 0.5f);
        mixState_.setAnimation(1, 2, true);
        mixState_.update(sk_, 0.0f);
        mixActive_ = true;
        mixPlaying_ = false;
        focusPreview_ = 3;
        frameAllIn_ = 5;
        return;
    }
    if (scenario == "graphcolor") {
        mode_ = Mode::Animate;
        time_ = time;
        selectSlot(0);
        focusGraph_ = 3;
        frameAllIn_ = 5;
        return;
    }
    if (scenario == "ghost") {
        mode_ = Mode::Animate;
        time_ = time;
        ghostOn_ = true;
        ghostBefore_ = ghostAfter_ = 2;
        ghostStep_ = 4;
        selectBone(low);
        sheetSel_ = {{{SheetRow::Overview, -1}, 0.0f}, {{SheetRow::Overview, -1}, 0.5f}};
        sheetSelAnim_ = 0;
        frameAllIn_ = 5;
        return;
    }
    if (scenario == "bake") {
        mode_ = Mode::Setup;
        selectBone(low);
        createConstraint(ConstraintType::Physics);
        setMode(Mode::Animate);
        time_ = time;
        bakePhysics(-1);
        selectBone(low);
        undo_.clear();
        dirty_ = false;
        frameAllIn_ = 5;
        return;
    }
    if (scenario == "ik" || scenario == "path") {
        mode_ = Mode::Setup;
        tool_ = Tool::Translate;
        if (scenario == "ik") {
            selectBone(low);
            int c = createConstraint(ConstraintType::Ik);
            int target = sk_.constraints[c].target;
            sk_.bones[target].setup.x = 90;
            sk_.bones[target].setup.y = 40;
            selectConstraint(c);
        } else {
            selectBone(up);
            int c = createConstraint(ConstraintType::Path);
            Attachment& path = sk_.slots[sk_.constraints[c].target].attachments[0];
            makePolygon(path, {{-160, -150}, {-120, -120}, {-80, -90}, {40, 0}, {60, 30}, {80, 60}, {200, 60}, {240, 120}, {280, 180}});
            selectConstraint(c);
        }
        undo_.clear();
        dirty_ = false;
        updatePose();
    } else if (scenario == "clip") {
        // A clipping slot below the arm that cuts it off with a slanted edge.
        createPolygonAttachment(0, AttachmentType::Clipping);
        Attachment& clip = sk_.slots[0].attachments[0];
        makePolygon(clip, {{-60, -40}, {60, 10}, {60, 260}, {-60, 260}});
        undo_.clear();
        dirty_ = false;
        mode_ = Mode::Animate;
        time_ = time;
        tool_ = Tool::Rotate;
        selectAttachment(0, 0);
    } else if (scenario == "mesh") {
        mode_ = Mode::Setup;
        tool_ = Tool::MeshEdit;
        selectAttachment(0, 0);
        sel_.vertices = {0, 1, 2};
    } else if (scenario == "weights") {
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
