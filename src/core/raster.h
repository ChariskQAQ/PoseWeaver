#pragma once
// CPU rendering of a posed skeleton (used for image / video export) and a GIF encoder.
#include "core/pose.h"

#include <cstdint>

namespace rig {

struct RasterTexture {
    int width = 0, height = 0;
    const unsigned char* rgba = nullptr;
};

// World-space rectangle covering every visible attachment of the pose (false if nothing).
bool poseBounds(const Skeleton& sk, const Pose& pose, Vec2& lo, Vec2& hi);

// Draws the pose into `rgba` (width * height * 4, straight alpha, already holding the
// background) with textured triangles, bilinear sampling, slot colours and clipping.
// `worldToPixel` maps world coordinates (y up) to pixels (y down).
void renderPose(const Skeleton& sk, const Pose& pose, const std::vector<RasterTexture>& textures, const Affine& worldToPixel,
                int width, int height, std::vector<unsigned char>& rgba);

// Composite geometry without post effects; pixel mode still selects nearest texture sampling.
void renderPoseBase(const Skeleton& sk, const Pose& pose, const std::vector<RasterTexture>& textures, const Affine& worldToPixel,
                    int width, int height, std::vector<unsigned char>& rgba);

// Box-filters an image down by an integer factor (supersampling).
std::vector<unsigned char> downsample(const std::vector<unsigned char>& rgba, int width, int height, int factor);

// Animated GIF (looping, 252-colour palette + transparency for alpha < 128).
std::vector<uint8_t> encodeGif(const std::vector<std::vector<unsigned char>>& framesRgba, int width, int height,
                               int delayCentiseconds, bool loop);

} // namespace rig
