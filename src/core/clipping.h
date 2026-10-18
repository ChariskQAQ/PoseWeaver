#pragma once
#include "core/pose.h"

#include <array>

namespace rig {

using ClipTriangles = std::vector<std::array<Vec2, 3>>;

// Splits a simple (possibly concave) polygon into counter-clockwise triangles for clipping.
ClipTriangles clipTrianglesOf(const std::vector<Vec2>& polygon);
// Clips `in` against the area covered by `clip` (from clipTrianglesOf); uvs are interpolated.
void clipGeometry(const Geometry& in, const ClipTriangles& clip, Geometry& out);

} // namespace rig
