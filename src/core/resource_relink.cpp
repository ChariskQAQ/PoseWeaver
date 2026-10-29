#include "core/resource_relink.h"
#include <filesystem>
#include <algorithm>
#include <map>
#include <cctype>
#include <stdexcept>
namespace rig {
std::vector<ResourceMatch> findMissingResources(const Skeleton& skeleton,const std::string& directory,ResourceSearchProgress* progress) {
    namespace fs = std::filesystem;
    auto key = [](std::string name) { std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c) { return (char)std::tolower(c); }); return name; };
    std::map<std::string,std::vector<std::string>> files;
    std::vector<ResourceMatch> result;
    auto add = [&](bool audio,int index,const std::string& path) {
        if (audio && path.empty()) return;
        std::error_code error;
        if (fs::is_regular_file(fs::u8path(path),error)) return;
        ResourceMatch match; match.audio = audio; match.index = index; match.original = path;
        files[key(fs::u8path(path).filename().u8string())];
        result.push_back(std::move(match));
    };
    for (int i = 0; i < (int)skeleton.images.size(); ++i) { if (progress && progress->cancelled) return {}; add(false,i,skeleton.images[i].path); }
    for (int i = 0; i < (int)skeleton.events.size(); ++i) { if (progress && progress->cancelled) return {}; add(true,i,skeleton.events[i].audioPath); }
    if (result.empty() || (progress && progress->cancelled)) return {};
    size_t count = 0;
    const auto root = fs::absolute(fs::u8path(directory)).lexically_normal();
    for (const auto& file : fs::recursive_directory_iterator(root,fs::directory_options::skip_permission_denied)) {
        if (progress) { if (progress->cancelled) return {}; progress->visited = count+1; }
        if (++count > 50000) throw std::runtime_error("Resource search exceeds 50000 entries; select a smaller directory");
        auto found = files.find(key(file.path().filename().u8string()));
        if (found != files.end() && !file.is_symlink() && file.is_regular_file()) found->second.push_back(file.path().u8string());
    }
    for (auto& match : result) {
        match.candidates = files[key(fs::u8path(match.original).filename().u8string())];
        std::sort(match.candidates.begin(),match.candidates.end());
        if (match.candidates.size() == 1) match.selected = 0;
    }
    return result;
}
}
