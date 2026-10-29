#include "platform/blend.h"
#include <GLFW/glfw3.h>
#include <cstdint>

static void applyBlend(const ImDrawList*, const ImDrawCmd* command) {
    const auto mode = (rig::BlendMode)(intptr_t)command->UserCallbackData;
    // The viewport destination is opaque; exports handle transparent destinations in core/raster.
    switch (mode) {
    case rig::BlendMode::Additive: glBlendFunc(GL_ONE, GL_ONE); break;
    case rig::BlendMode::Multiply: glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA); break;
    case rig::BlendMode::Screen: glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR); break;
    default: break;
    }
}

void beginSlotBlend(ImDrawList* list, rig::BlendMode mode) {
    if (mode != rig::BlendMode::Normal) list->AddCallback(applyBlend, (void*)(intptr_t)mode);
}
