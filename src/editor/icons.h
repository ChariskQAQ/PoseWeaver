#pragma once
#include <imgui.h>

// Small vector icons drawn with ImDrawList (no icon font or image assets needed).
enum class Icon {
    Skeleton, Bone, Slot, Region, Mesh, Animation, Image, DrawOrder,
    Pose, Weights, Create, MeshTool, Rotate, Translate, Scale,
    Local, Parent, World, AutoKey, Key,
    First, Last, PrevKey, NextKey, Play, PlayBack, Pause, Loop, Lock,
    Zoom100, ZoomFit, Search
};

// Draws `icon` centred at `c`, fitting a square of `size` pixels.
void drawIcon(ImDrawList* dl, Icon icon, ImVec2 c, float size, ImU32 col);
// Default colour for an icon (bones white, rotate green, translate blue, ...).
ImU32 iconColor(Icon icon);

// Inline icon occupying one text-line square.
void iconInline(Icon icon, ImU32 col = 0);
// Button with an icon and an optional label. `active` draws it highlighted (selected tool).
bool iconButton(const char* id, Icon icon, const char* label, bool active, float width = 0, bool enabled = true);
// Square icon-only button.
bool iconOnlyButton(const char* id, Icon icon, bool active = false, const char* tooltip = nullptr, ImU32 col = 0);
// Key button: colour shows key state (theme::kKeyNone / kKeyChanged / kKeyKeyed).
bool keyButton(const char* id, ImU32 stateColor, const char* tooltip);
// Small round toggle (visibility dot style). Returns true when clicked.
bool dotToggle(const char* id, bool on, const char* tooltip = nullptr);
