#pragma once
#include "editor/export_transaction.h"
#include "editor/image_export.h"
#include <json.hpp>
#include <fstream>
#include <functional>
#include <iomanip>
#include <set>
#include <sstream>

inline std::string sequenceFingerprint(const std::filesystem::path& file,const std::function<void()>& check) {
    std::ifstream input(file,std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read sequence frame");
    uint64_t hash = 14695981039346656037ull; char bytes[65536];
    while (input) {
        check(); input.read(bytes,sizeof(bytes));
        for (std::streamsize i = 0; i < input.gcount(); ++i) { hash ^= (unsigned char)bytes[i]; hash *= 1099511628211ull; }
    }
    if (!input.eof()) throw std::runtime_error("Cannot fingerprint sequence frame");
    std::ostringstream text; text << std::hex << std::setw(16) << std::setfill('0') << hash; return text.str();
}

inline void stageSequenceManifest(ExportTransaction& transaction,const std::filesystem::path& destination,const char* extension,
    bool clean,ImageExportResult& result,const std::function<void()>& check) {
    namespace fs = std::filesystem;
    using nlohmann::json;
    const auto manifest = fs::u8path(destination.u8string()+".pwexport.json");
    json records = json::array(); std::set<std::string> generated;
    for (const auto& entry : fs::directory_iterator(transaction.directory)) {
        if (entry.path().extension() != extension) continue;
        auto name = destination.stem().u8string()+entry.path().filename().u8string().substr(6);
        generated.insert(name);
        records.push_back({{"file",name},{"size",fs::file_size(entry.path())},{"fingerprint",sequenceFingerprint(entry.path(),check)}});
    }
    if (fs::exists(manifest)) {
        if (!fs::is_regular_file(fs::symlink_status(manifest)) || fs::file_size(manifest) > 4*1024*1024) throw std::runtime_error("Invalid sequence manifest");
        std::ifstream input(manifest); auto previous = json::parse(input);
        if (previous.at("format") != "poseweaver.sequence" || previous.at("version") != 1 || previous.at("destination") != destination.filename().u8string() ||
            !previous.at("files").is_array() || previous["files"].size() > 10000) throw std::runtime_error("Invalid sequence manifest");
        std::set<std::string> known;
        const std::string prefix = destination.stem().u8string()+"_", suffix = extension;
        for (const auto& record : previous["files"]) {
            check(); const auto name = record.at("file").get<std::string>();
            if (name.size() != prefix.size()+4+suffix.size() || name.compare(0,prefix.size(),prefix) || name.compare(name.size()-suffix.size(),suffix.size(),suffix) ||
                name.find_first_not_of("0123456789",prefix.size()) != prefix.size()+4 || !known.insert(name).second ||
                !record.at("size").is_number_unsigned() || record.at("size").get<uint64_t>() > 512ull*1024*1024 ||
                record.at("fingerprint").get<std::string>().size() != 16) throw std::runtime_error("Invalid sequence frame record");
            if (generated.count(name)) continue;
            const auto file = destination.parent_path()/fs::u8path(name);
            auto status = fs::symlink_status(file);
            if (!fs::exists(status)) continue;
            if (!fs::is_regular_file(status) || fs::file_size(file) != record["size"].get<uint64_t>() || sequenceFingerprint(file,check) != record["fingerprint"].get<std::string>()) { ++result.preservedFrames; continue; }
            if (clean) transaction.remove(file); else records.push_back(record);
        }
    }
    json document{{"format","poseweaver.sequence"},{"version",1},{"destination",destination.filename().u8string()},{"files",records}};
    const auto staged = transaction.directory/"sequence.json";
    std::ofstream out(staged,std::ios::binary); const auto text = document.dump(2); out.write(text.data(),(std::streamsize)text.size()); out.close();
    if (!out) throw std::runtime_error("Cannot write sequence manifest");
    transaction.add(staged,manifest);
}
