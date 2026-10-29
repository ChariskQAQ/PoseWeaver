#pragma once
#include "core/model.h"
#include <imgui.h>

// Non-normal slots use premultiplied textures and tint; reset ImGui state after drawing the slot.
void beginSlotBlend(ImDrawList* list, rig::BlendMode mode);
