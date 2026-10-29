#pragma once
#include "core/model.h"
// Minimal Photoshop (.psd) reader: 8-bit RGB documents, raster layers (raw or RLE), layer
// names (including Unicode), group paths, visibility and opacity.
#include <cstdint>
#include <string>
#include <vector>

namespace rig {

struct PsdLayer {
    std::string name;       // UTF-8
    std::string groupPath;  // "group/subgroup" (empty at the top level)
    int left = 0, top = 0, width = 0, height = 0;  // document pixels, y down
    bool visible = true;
    float opacity = 1;
    BlendMode blend = BlendMode::Normal;
    std::string blendKey = "norm";
    std::vector<unsigned char> rgba;  // width * height * 4
};

struct PsdDocument {
    int width = 0, height = 0;
    std::vector<PsdLayer> layers;  // bottom to top (draw order)
};

bool parsePsd(const std::vector<uint8_t>& bytes, PsdDocument& out, std::string* error);

} // namespace rig
