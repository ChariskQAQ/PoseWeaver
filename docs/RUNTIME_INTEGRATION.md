# Unity / Godot 实时骨骼（1.1.1）

文件菜单的“导出 Unity / Godot 实时骨骼...”生成 `.pwskeleton` 和同名资源目录。一起复制到引擎项目内，不要只复制文件。它保存自有工程的全部动画、皮肤、网格、权重、变形、约束与事件；游戏中由共享 C++ 核心逐帧求值。旧的 `.pwframes` 烘焙导出继续使用。

## 当前范围

本次提供 Windows x64 原生库；Godot 插件要求 4.4 或以上，实测版本为本机 4.7.2 Steam。Unity 插件面向 2022.3 LTS 或以上，本机 Unity 6.3（6000.3.24f1）的真实程序集已通过离线编译检查；Editor 因缺少有效授权退出，按本次决定保留导入、渲染与 Player 待验收。其他平台需编译原生库并补齐插件平台配置，当前安装包不提供。

支持骨骼、蒙皮、普通及关联/跨皮肤网格、独立分量曲线、皮肤独立变形、绘制顺序、裁剪、IK/变换/路径/物理约束、多轨混合、队列、轨道透明度、换肤、附件显隐与动画事件。物理跟随逐帧时间；骨骼覆盖在动画之后、约束之前应用。事件通过回调或信号提供，音频路径只是事件元数据，接收方自行播放。

Normal/Add/Multiply 使用引擎材质混合。Screen、光源和像素处理在此版本会明确拒绝导出；需要这些效果时用 `.pwframes`。未接入 Spine 自有格式或 Spine 运行时。实时资源收集目前同步执行，适合在输出完成后继续编辑；普通帧/视频导出的后台进度与取消仍保留。

单工程最多 16 MiB JSON、4096 骨骼、10000 插槽/图片、8 条轨道；每次推进 0–1 秒，每帧几何最多一百万顶点。引擎与核心间目前使用 JSON 传递网格，每帧有序列化和分配开销，未做大量角色同屏性能承诺。CPU 求值和约束复用编辑器实现，Unity/Godot 负责网格和纹理绘制。

## Godot

把安装包的 `integrations/godot/addons/poseweaver_runtime` 复制到项目 `addons` 中，保留 `bin/poseweaver_runtime.dll`，重新启动编辑器。在“项目设置 → 插件”启用 Poseweaver Skeleton，再把骨骼文件和资源目录复制到项目内。`.pwskeleton` 会导入为 PackedScene，直接拖入场景即可，默认播放第一个动作；检查器可修改 `autoplay`。导入后的场景内嵌骨骼数据并引用图片和事件音频，重新导入会更新资源。已有的 Node2D + `skeleton.gd` / `skeleton_path` 接法继续保留。

```gdscript
$Character.play("walk", true)
$Character.play("wave", false, 1, 0.2)
$Character.set_track_alpha(1, 0.5)
$Character.set_skins(["ember"])
$Character.set_bone_position("foot_target_l", Vector2(65, 80))
$Character.animation_event.connect(func(event): print(event.name))
```

位置参数是该骨骼父空间内的像素坐标，Godot 的 Y 轴向下。`bone_position` 返回骨骼原点在角色空间的位置；与设置接口所在空间不同，有父变换时需自行转换。旋转参数为 Godot 方向的角度。`playing=false` 停止自动推进，可手动 `advance(delta)`。还有 `queue_animation`、`clear_bone`、`set_attachment`、`clear_attachment`、`fade_track`、`set_speed`、`clear_tracks`。加载前会验证数据、图片、自动播放名及首帧；失败保留之前的角色，错误在 `last_error` 和 `runtime_failed` 信号中。

使用导入后的场景和默认“导出全部资源”预设时，无需再手动添加 `*.pwskeleton` 过滤规则；Godot 会打包导入场景、图片、音频和原生库。Windows x64 Release 独立程序已通过 headless 播放及实际 OpenGL 绘制验收。仅选择部分场景导出的预设尚未通过：必须显式保留运行时插件目录及其资源，当前推荐默认全部资源模式。不要删除导出器复制到 exe 旁边的原生 DLL。

## Unity

将安装包 `integrations/unity` 放入项目 `Assets/Poseweaver`，或作为本地 UPM 包安装。必须保留 `Runtime/Plugins/x86_64/poseweaver_runtime.dll` 与 `PoseweaverSkeleton.shader`。把 `.pwskeleton` 及资源目录放到 Assets，导入后将生成的 prefab 拖入场景，设置 Autoplay 动作名。默认每 100 像素为 1 Unity 单位。纹理不能被导入设置缩小，否则导入器要求重新导出或修正纹理设置。

```csharp
var player = GetComponent<Poseweaver.PoseweaverSkeleton>();
player.Play("walk");
player.Play("wave", false, 1, .2f);
player.SetSkins("ember");
player.SetBonePosition("foot_target_l", new Vector2(65, -80));
player.AnimationEvent += item => Debug.Log(item.name);
```

位置参数是父空间内的像素坐标，Unity Y 轴向上；`BonePosition` 返回角色空间像素坐标。接口还提供 Queue、SetTrackAlpha、FadeTrack、ClearBone、SetAttachment、ClearAttachment、SetSpeed、ClearTracks、Advance。加载前会验证首帧和自动播放名，失败不替换已有实例。调用 Load 后 Unity 的 Start 不再重复加载，避免覆盖开局设置的动作；运行中无效缩放会明确返回错误。纹理导入设置变化会触发骨骼资产重新检查。材质使用内置渲染管线 Shader，URP/HDRP 兼容尚未验收。

## 可重复验收

源码构建后：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify-runtime.ps1 -GodotPath <Godot.exe绝对路径> -WindowsTemplate <windows_release_x86_64.exe绝对路径>
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify-unity.ps1 -PrepareOnly
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify-unity-compile.ps1 -EditorPath <Unity.exe绝对路径>
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify-unity.ps1 -EditorPath <Unity.exe绝对路径> -BuildPlayer
```

Godot 验收新建独立工程，启用导入插件，测试资源导出、首次导入为场景、图片与音频依赖、15 骨骼/2 皮肤/3 动作、12 个绘制网格、事件、换肤、IK 目标、多轨混合、队列、淡出、错误动作及失败加载后的旧实例。Unity 准备工程包含烘焙和实时两种示例、独立测试入口及 Windows 构建入口；准备成功不代表 Unity 验收通过。

Godot headless 首次导入会触发已确认的 [扩展文档生成退出竞争](https://github.com/godotengine/godot/issues/111048)。验收脚本在导入命令中加 `--frame-delay 800`，让文档生成回调完成后退出；仍检查正常退出码与错误日志，不忽略崩溃。

1.1.1 的 Godot 验收还使用本机已有的官方 Windows Release 模板，构建 exe + PCK + DLL，运行同一套换肤、混合、事件及失败加载测试，并实际绘制角色与 Normal/Add/Multiply 三种模式。输出截图和日志保存在生成的 `build/runtime-acceptance-*` 目录。渲染检查不使用 `--headless`，通过隐藏窗口和独立 SubViewport 取图，避免把空的渲染设备当作绘制通过。图像素误差阈值 0.03，只核对混合模式及可见角色，不代表全面的跨设备视觉一致性。

`verify-unity-compile.ps1` 使用 Unity 自带 Roslyn、引擎和 Editor 程序集分别编译运行时与编辑器两套插件，输出到项目 build 内；不运行 Unity Editor，不模拟授权，不把编译成功当作导入/渲染通过。实际 Editor 启动日志记录退出码 198 / 无有效授权，待在 Unity Hub 完成授权后继续运行完整脚本。

[本轮验收结果](runtime-integration-acceptance.json) · [独立程序的实际渲染](runtime-godot-render.png)（顶部色块为三种混合模式的检查区域）。另覆盖工程根目录与子目录两种骨骼资源位置。
