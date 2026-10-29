#pragma once
#include "core/model.h"

namespace rig {

// Copy referenced images and event audio, then atomically save the project.
// On success, collected contains absolute paths to the saved resources.
// On failure, the input, output and previous project/resources are preserved.
bool saveCollectedProject(const std::string& path, const Skeleton& input, const std::string& sourceDirectory,
                          Skeleton& collected, std::string* error);

} // namespace rig
