#pragma once
#include "core/model.h"

#include <string>

namespace rig {

// Project file and export format are the same JSON document. Bones, slots and images are
// referenced by index; parents always appear as indices into "bones".
std::string toJson(const Skeleton& sk, int indent = 2);
bool fromJson(const std::string& text, Skeleton& out, std::string* error);

bool saveFile(const std::string& path, const Skeleton& sk, std::string* error);
bool loadFile(const std::string& path, Skeleton& out, std::string* error);

} // namespace rig
