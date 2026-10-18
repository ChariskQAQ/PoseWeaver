#pragma once
#include "core/model.h"

#include <string>

namespace rig {

// Merges `src` into `dst` (Import Data / Import Project):
// - bones, slots and constraints are appended (renamed on name clashes),
// - images are shared by path, events and skins are matched by name,
// - animations with the same name receive the imported keys, others are appended.
// Every index inside the imported data is remapped. Returns a short human-readable report.
std::string mergeSkeleton(Skeleton& dst, const Skeleton& src);

} // namespace rig
