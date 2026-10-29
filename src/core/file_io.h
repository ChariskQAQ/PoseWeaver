#pragma once
#include <string>
#include <string_view>

namespace rig {

// Flush a sibling temporary file, then replace the destination without deleting it first.
bool writeFileAtomic(const std::string& path, std::string_view bytes, std::string* error);

} // namespace rig
