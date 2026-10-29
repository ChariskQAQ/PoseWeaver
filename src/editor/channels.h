#pragma once
// Display helpers shared by the dopesheet and the graph view.
#include "core/model.h"
#include "editor/icons.h"
#include "editor/i18n.h"
#include "editor/theme.h"

inline const char* channelName(rig::Channel ch) {
    switch (ch) {
    case rig::Channel::Rotate: return tr("旋转");
    case rig::Channel::Translate: return tr("移动");
    case rig::Channel::Scale: return tr("缩放");
    case rig::Channel::Shear: return tr("斜切");
    case rig::Channel::Attachment: return tr("附件");
    case rig::Channel::Color: return tr("颜色");
    case rig::Channel::DrawOrder: return tr("绘制顺序");
    case rig::Channel::Event: return tr("事件");
    case rig::Channel::Deform: return tr("变形");
    case rig::Channel::Constraint: return tr("约束");
    }
    return "";
}

inline ImU32 channelColor(rig::Channel ch) {
    switch (ch) {
    case rig::Channel::Rotate: return theme::kRotate;
    case rig::Channel::Translate: return theme::kTranslate;
    case rig::Channel::Scale: return theme::kScale;
    case rig::Channel::Shear: return theme::kShear;
    case rig::Channel::Attachment: return theme::kAttachment;
    case rig::Channel::Color: return theme::kColor;
    case rig::Channel::DrawOrder: return theme::kDrawOrder;
    case rig::Channel::Event: return theme::kEvent;
    case rig::Channel::Deform: return theme::kDeform;
    case rig::Channel::Constraint: return theme::kConstraint;
    }
    return theme::kMixed;
}

inline Icon channelIcon(rig::Channel ch) {
    switch (ch) {
    case rig::Channel::Rotate: return Icon::Rotate;
    case rig::Channel::Translate: return Icon::Translate;
    case rig::Channel::Scale: return Icon::Scale;
    case rig::Channel::Shear: return Icon::Shear;
    case rig::Channel::Attachment: return Icon::Region;
    case rig::Channel::Color: return Icon::Color;
    case rig::Channel::DrawOrder: return Icon::DrawOrder;
    case rig::Channel::Event: return Icon::Event;
    case rig::Channel::Deform: return Icon::Deform;
    case rig::Channel::Constraint: return Icon::Constraint;
    }
    return Icon::Key;
}

// Bit per channel, used to merge keys of several channels on one row.
inline unsigned channelBit(rig::Channel ch) { return 1u << (unsigned)ch; }

inline Icon constraintIcon(rig::ConstraintType t) {
    switch (t) {
    case rig::ConstraintType::Ik: return Icon::Ik;
    case rig::ConstraintType::Transform: return Icon::TransformC;
    case rig::ConstraintType::Path: return Icon::PathC;
    case rig::ConstraintType::Physics: return Icon::Physics;
    }
    return Icon::Constraint;
}

inline const char* constraintTypeName(rig::ConstraintType t) {
    switch (t) {
    case rig::ConstraintType::Ik: return tr("IK 约束");
    case rig::ConstraintType::Transform: return tr("变换约束");
    case rig::ConstraintType::Path: return tr("路径约束");
    case rig::ConstraintType::Physics: return tr("物理约束");
    }
    return tr("约束");
}
