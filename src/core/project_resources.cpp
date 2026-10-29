#include "core/project_resources.h"
#include "core/diagnostics.h"
#include "core/serialize.h"

#include <filesystem>
#include <map>
#include <stdexcept>

namespace rig {
namespace fs = std::filesystem;
namespace {
bool inside(const fs::path& file, const fs::path& directory) {
    const fs::path relative = file.lexically_relative(directory);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != ".." && relative != ".";
}
} // namespace

bool saveCollectedProject(const std::string& path, const Skeleton& input, const std::string& sourceDirectory,
                          Skeleton& collected, std::string* error) {
    if (error) error->clear();
    fs::path root;
    std::vector<fs::path> createdFiles, createdDirectories;
    auto rollback = [&] {
        std::error_code ignored;
        // Only remove paths owned by this save; never recursively remove an existing resource folder.
        for (auto i = createdFiles.rbegin(); i != createdFiles.rend(); ++i)
            if (inside(*i, root)) {
                fs::permissions(*i, fs::perms::owner_write, fs::perm_options::add, ignored);
                fs::remove(*i, ignored);
            }
        for (auto i = createdDirectories.rbegin(); i != createdDirectories.rend(); ++i)
            if (*i == root || inside(*i, root)) fs::remove(*i, ignored);
    };
    try {
        if (!validateSkeleton(input, error)) return false;
        const fs::path project = fs::absolute(fs::u8path(path)).lexically_normal();
        const fs::path directory = project.parent_path();
        std::string name = project.filename().u8string();
        const std::string suffix = ".rig.json";
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            name.resize(name.size() - suffix.size());
        else name = project.stem().u8string();
        root = directory / fs::u8path(name + "_assets");
        const fs::path canonicalRoot = fs::weakly_canonical(root);
        const fs::path sourceBase = sourceDirectory.empty() ? fs::current_path() : fs::absolute(fs::u8path(sourceDirectory));
        Skeleton result = input;
        struct Resource { std::string* reference; fs::path source; const char* kind; };
        std::vector<Resource> external;
        auto prepare = [&](std::string& reference, const char* kind) {
            fs::path source = fs::u8path(reference);
            if (source.is_relative()) source = sourceBase / source;
            std::error_code ec;
            if (reference.empty() || !fs::is_regular_file(source, ec))
                throw std::runtime_error(source.u8string() + ": " + (ec ? ec.message() : "resource file is missing"));
            source = fs::canonical(source);
            reference = source.u8string();
            if (!inside(source, canonicalRoot)) external.push_back({&reference, source, kind});
        };
        for (auto& image : result.images) prepare(image.path, "images");
        for (auto& event : result.events) if (!event.audioPath.empty()) prepare(event.audioPath, "audio");

        if (!external.empty()) {
            if (fs::create_directory(root)) createdDirectories.push_back(root);
            // A fresh directory prevents collection from overwriting resources used by an older save.
            fs::path bundle;
            for (int n = 1; n <= 10000; ++n) {
                bundle = root / (n == 1 ? "resources" : "resources_" + std::to_string(n));
                if (fs::create_directory(bundle)) { createdDirectories.push_back(bundle); break; }
                if (n == 10000) throw std::runtime_error("cannot create a resource collection directory");
            }
            std::map<fs::path, fs::path> copied;
            for (auto& resource : external) {
                const auto found = copied.find(resource.source);
                if (found != copied.end()) { *resource.reference = found->second.u8string(); continue; }
                const fs::path kindDirectory = bundle / resource.kind;
                if (fs::create_directory(kindDirectory)) createdDirectories.push_back(kindDirectory);
                fs::path destination = kindDirectory / resource.source.filename();
                for (int n = 2; fs::exists(destination); ++n)
                    destination = kindDirectory / fs::u8path(resource.source.stem().u8string() + "_" + std::to_string(n) + resource.source.extension().u8string());
                createdFiles.push_back(destination);
                fs::copy_file(resource.source, destination); // This directory was created exclusively by this save.
                copied.emplace(resource.source, destination);
                *resource.reference = destination.u8string();
            }
        }

        Skeleton document = result;
        for (auto& image : document.images) image.path = fs::relative(fs::u8path(image.path), directory).generic_u8string();
        for (auto& event : document.events)
            if (!event.audioPath.empty()) event.audioPath = fs::relative(fs::u8path(event.audioPath), directory).generic_u8string();
        if (!saveFile(project.u8string(), document, error)) { rollback(); return false; }
        collected = std::move(result);
        return true;
    } catch (const std::exception& e) {
        rollback();
        if (error) *error = e.what();
        return false;
    }
}

} // namespace rig
