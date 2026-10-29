#include "editor/editor.h"
#include "editor/i18n.h"
#include "core/project_resources.h"
#include <filesystem>
bool Editor::exportRuntimePath(const std::string& path) {
    if (imageJob_ || engineJob_ || exportRecoveryTask_.valid() || !checkExportResources()) return false;
    if (std::filesystem::u8path(path).extension()!=".pwskeleton") return failOperation("RUNTIME_EXPORT_FAILED",tr("实时骨骼文件需使用 .pwskeleton 扩展名。"),path);
    if (!(sk_.effects==rig::RenderEffects{})) return failOperation("RUNTIME_EXPORT_FAILED",tr("实时骨骼暂不支持光源与像素烘焙，请使用帧动画导出或重置效果。"),path);
    for (const auto& slot : sk_.slots) if (slot.blend==rig::BlendMode::Screen)
        return failOperation("RUNTIME_EXPORT_FAILED",tr("实时骨骼暂不支持滤色，请使用帧动画导出或更改混合模式。"),path);
    rig::Skeleton collected; std::string error;
    const auto directory=projectPath_.empty() ? std::string() : std::filesystem::u8path(projectPath_).parent_path().u8string();
    if (!rig::saveCollectedProject(path,sk_,directory,collected,&error)) return failOperation("RUNTIME_EXPORT_FAILED",error,path);
    dismissIssue("RUNTIME_EXPORT_FAILED"); setStatus(tr("已导出实时骨骼与资源，请将文件及旁边的资源目录一起导入引擎。")); return true;
}
