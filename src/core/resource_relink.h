#pragma once
#include "core/model.h"
#include <atomic>
namespace rig {
struct ResourceMatch {
    bool audio = false;
    int index = -1, selected = -1;
    std::string original;
    std::vector<std::string> candidates;
};
// Read-only search; ambiguous basenames are left unselected for explicit review.
struct ResourceSearchProgress { std::atomic<bool> cancelled{false}; std::atomic<size_t> visited{0}; };
std::vector<ResourceMatch> findMissingResources(const Skeleton& skeleton,const std::string& directory,ResourceSearchProgress* progress = nullptr);
}
