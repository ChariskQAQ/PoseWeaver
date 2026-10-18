#pragma once
// Texture atlas packing (MaxRects, best-short-side-fit) with transparent-border trimming.
#include <string>
#include <vector>

namespace rig {

struct AtlasInput {
    std::string name;
    int width = 0, height = 0;
    const unsigned char* rgba = nullptr;  // width * height * 4
};

struct AtlasSettings {
    int maxWidth = 2048, maxHeight = 2048;
    int padding = 2;          // pixels between regions (and around the page edge)
    bool trim = true;         // drop fully transparent borders
    bool allowRotate = false; // regions may be stored rotated 90 degrees clockwise
    bool powerOfTwo = false;  // page sizes rounded up to powers of two
    unsigned char alphaThreshold = 0;
    float scale = 1.0f;       // images are resized by this factor before packing
    bool autoScale = false;   // shrink further (steps of 10%) until everything fits on one page
    float minScale = 0.1f;    // lower bound for autoScale
};

struct AtlasRegion {
    std::string name;
    int page = 0;
    int x = 0, y = 0, width = 0, height = 0;  // in the page, as stored (swapped when rotated)
    bool rotated = false;
    int offsetX = 0, offsetY = 0;             // trimmed area inside the original image (top-left origin)
    int originalWidth = 0, originalHeight = 0;
};

struct AtlasPage {
    int width = 0, height = 0;
    std::vector<unsigned char> rgba;
};

struct Atlas {
    std::vector<AtlasPage> pages;
    std::vector<AtlasRegion> regions;  // same order as the inputs; sizes are in scaled pixels
    float scale = 1.0f;                // scale the images were packed at
};

// Resizes an RGBA image with area averaging (premultiplied, so transparent pixels do not bleed colour).
std::vector<unsigned char> resizeRgba(const unsigned char* rgba, int w, int h, int newW, int newH);

bool packAtlas(const std::vector<AtlasInput>& inputs, const AtlasSettings& settings, Atlas& out, std::string* error);
// Text description (one block per page, one entry per region) referring to `pageFiles`.
std::string atlasText(const Atlas& atlas, const std::vector<std::string>& pageFiles);

} // namespace rig
