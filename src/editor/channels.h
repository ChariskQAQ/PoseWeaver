#pragma once
// Display helpers shared by the dopesheet and the graph view.
#include "core/model.h"
#include "editor/icons.h"
#include "editor/theme.h"

inline const char* channelName(rig::Channel ch) {
    switch (ch) {
    case rig::Channel::Rotate: return "旋转";
    case rig::Channel::Translate: return "移动";
    case rig::Channel::Scale: return "缩放";
    case rig::Channel::Shear: return "斜切";
    case rig::Channel::Attachment: return "附件";
    case rig::Channel::Color: return "颜色";
    case rig::Channel::DrawOrder: return "绘制顺序";
    case rig::Channel::Event: return "事件";
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
    }
    return Icon::Key;
}

// Bit per channel, used to merge keys of several channels on one row.
inline unsigned channelBit(rig::Channel ch) { return 1u << (unsigned)ch; }
