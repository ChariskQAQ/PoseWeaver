#pragma once
#include "core/model.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rig {

// Project file and export format are the same JSON document. Bones, slots and images are
// referenced by index; parents always appear as indices into "bones".
std::string toJson(const Skeleton& sk, int indent = 2);
bool fromJson(const std::string& text, Skeleton& out, std::string* error);

// Export options for runtime data.
struct ExportOptions {
    bool binary = false;  // MessagePack encoding of the JSON document, "RIGB" header
    int indent = 2;       // JSON only; -1 = compact
    int decimals = -1;    // round floats to this many decimals (-1 = full precision)
};
std::string toJson(const Skeleton& sk, const ExportOptions& options);
// Binary: 'R' 'I' 'G' 'B', format version byte, then MessagePack.
std::vector<uint8_t> toBinary(const Skeleton& sk, int decimals = -1);
bool fromBinary(const std::vector<uint8_t>& data, Skeleton& out, std::string* error);

bool saveFile(const std::string& path, const Skeleton& sk, std::string* error);
bool exportFile(const std::string& path, const Skeleton& sk, const ExportOptions& options, std::string* error);
// Reads JSON or binary (detected from the content).
bool loadFile(const std::string& path, Skeleton& out, std::string* error);

} // namespace rig
