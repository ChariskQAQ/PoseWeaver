#pragma once
#include "core/model.h"

namespace rig {

enum class Severity { Warning, Error };
enum class IssueTarget { Skeleton, Bone, Slot, Attachment, Skin, Animation, Constraint, Image, Event, File };

struct Diagnostic {
    Severity severity = Severity::Warning;
    std::string code;
    std::string message;
    std::string detail;
    IssueTarget target = IssueTarget::Skeleton;
    int index = -1;
    int sub = -1;     // attachment or skin entry
    int vertex = -1;
    std::string path;
    int animation = -1;
    float time = -1;
};

// Resource paths are resolved against directory. Set resources=false for model-only validation.
std::vector<Diagnostic> diagnose(const Skeleton& sk, const std::string& directory = {}, bool resources = true);
bool validateSkeleton(const Skeleton& sk, std::string* error);

} // namespace rig
