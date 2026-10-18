#pragma once
#include <imgui.h>

// Neutral gray editor theme with a blue accent (layout and conventions modelled on Spine,
// colours are our own).
namespace theme {

void apply(float scale);

// Fonts: [0] UI font, [1] large Latin font used for the SETUP / ANIMATE mode label.
void loadFonts(float scale);
ImFont* largeFont();

// Accent / state colours.
constexpr ImU32 kAccent = IM_COL32(41, 141, 217, 255);
constexpr ImU32 kSelect = IM_COL32(64, 214, 255, 255);     // selected items in the viewport
constexpr ImU32 kBone = IM_COL32(225, 225, 225, 255);
constexpr ImU32 kBoneOutline = IM_COL32(20, 20, 22, 200);
constexpr ImU32 kPlayhead = IM_COL32(56, 208, 230, 255);
constexpr ImU32 kAnimEnd = IM_COL32(255, 150, 40, 255);

// Key button states (Spine convention): no key / changed but unkeyed / keyed.
constexpr ImU32 kKeyNone = IM_COL32(92, 196, 92, 255);
constexpr ImU32 kKeyChanged = IM_COL32(255, 160, 40, 255);
constexpr ImU32 kKeyKeyed = IM_COL32(232, 64, 64, 255);

// Timeline / channel colours.
constexpr ImU32 kRotate = IM_COL32(92, 204, 92, 255);
constexpr ImU32 kTranslate = IM_COL32(77, 158, 242, 255);
constexpr ImU32 kScale = IM_COL32(235, 84, 84, 255);
constexpr ImU32 kShear = IM_COL32(236, 200, 70, 255);
constexpr ImU32 kAttachment = IM_COL32(186, 120, 242, 255);
constexpr ImU32 kColor = IM_COL32(240, 120, 190, 255);
constexpr ImU32 kDrawOrder = IM_COL32(120, 160, 255, 255);
constexpr ImU32 kEvent = IM_COL32(170, 110, 235, 255);
constexpr ImU32 kDeform = IM_COL32(90, 210, 220, 255);
constexpr ImU32 kConstraint = IM_COL32(240, 110, 110, 255);
constexpr ImU32 kMixed = IM_COL32(240, 240, 240, 255);

// Viewport.
constexpr ImU32 kCheckerA = IM_COL32(86, 86, 89, 255);
constexpr ImU32 kCheckerB = IM_COL32(96, 96, 99, 255);
constexpr ImU32 kCheckerAnimA = IM_COL32(78, 88, 86, 255);
constexpr ImU32 kCheckerAnimB = IM_COL32(88, 98, 96, 255);
constexpr ImU32 kPanelBg = IM_COL32(52, 52, 55, 235);  // floating toolbar groups

} // namespace theme
