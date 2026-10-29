# Unity / Godot 接入：帧动画资源包 v1

1.1.0 另提供 Windows x64 实时骨骼接入，见 [RUNTIME_INTEGRATION.md](RUNTIME_INTEGRATION.md)。本页说明保留的烘焙帧流程。

Poseweaver 可以把选中的动画和当前启用的皮肤组合烘焙为透明精灵表。`.pwframes` 记录动作名、帧区域、时长和共同原点；插件将它转换为可以拖入游戏场景的对象。

这一阶段面向二维帧动画。骨骼、网格变形、约束、物理以及启用的光源与像素效果会被烘焙；不导出可实时控制的骨骼、运行时换装、动画混合或事件音频。混合模式烘焙到透明图片，与游戏背景实时混合时可能有差异。像素模式固定 1x 采样，并自动增加必要留边，详见 [LIGHTING_PIXELS.md](LIGHTING_PIXELS.md)。

## 从编辑器导出

1. 选择皮肤，确认图片均可加载；打开“文件 → 导出 Unity / Godot 帧动画...”。
2. 勾选要导出的动作，分别设置是否循环；支持全选和全不选。设置帧率、导出比例、超采样及留边；Unity 每单位像素决定 Unity 中的尺寸，Godot 使用像素尺寸。
3. 点击“计算资源预算”。计算在后台执行，显示选中动作数、帧数、统一帧尺寸、图集页数、未压缩 RGBA 图集占用及绘制缓冲估算。预算不生成文件，不包括源图片副本、颜色查找缓存、PNG 编码临时内存和引擎开销，也不代表压缩后的 PNG 大小。更改动作选择、帧率、比例、超采样、留边或光源与像素参数后需要重新计算。
4. 输入输出路径或点击“浏览...”，选择 `角色.pwframes`，再点击“导出”。手动输入相对路径时，以已保存工程所在目录为基准；未保存工程则以当前工作目录为基准。配置保存完整路径，避免下次启动位置不同导致导出到别处。同时生成旁边的 `角色_frames/`。将 **清单和它引用的整个资源文件夹** 一起复制到引擎项目。

动作选择、各动作循环、帧率、比例、每单位像素、超采样、留边和输出路径按工程自动保存到现有用户设置目录的 `engine-exports.json`，不修改工程内容、保存标记或撤销记录。重新打开同一路径的工程可恢复配置；另存为会继承当前配置。未保存工程的配置只在当前会话保留，保存工程后才记住。配置按动作名匹配，新动作默认选中并循环；工程改名或移到另一路径后会作为新的配置项。损坏的配置会给出问题面板警告并使用默认值；无法解析的配置文件会保留，不自动覆盖。

预算计算和导出显示阶段与进度，可点击“取消任务”。任务使用点击按钮时的工程、皮肤、物理设置和图片副本；启动时复制大工程及图片仍有短暂开销。取消在当前帧计算或当前图集写入结束后生效，清理本次资源并保留旧清单和图集。最后原子提交阶段不再接受取消；关闭编辑器会请求取消并等待任务退出。命令行导出保持同步执行。

所选动作共享画布及原点，动作切换不因裁边而跳位。没有动画时提供单帧 `setup`。不重复采样循环末端，最后一帧可以更短以保持原动作时长。

每次导出建立新的资源目录（例如 `_frames_2`），最后原子替换清单。写入失败保留旧清单及资源；成功后旧目录也会保留。确认其他清单没有引用后，可手动删除旧目录。导出不修改工程内容或撤销历史。

命令行示例（在项目根目录执行）：

```powershell
.\build\Release\Poseweaver.exe --demo samples/arm.png --run engines arm.pwframes
.\build\Release\Poseweaver.exe character.rig.json --run engines character.pwframes
```

打开已保存工程的命令行导出也会读取该工程记住的动作和参数，命令行路径覆盖记住的输出路径。没有配置时使用全部动作、30 FPS、比例 1、超采样 2、循环、100 pixels/unit。

## Unity

插件目标版本为 **Unity 2022.3 LTS / Unity 6**，没有第三方包依赖。

1. 将 `integrations/unity` 文件夹复制到 Unity 项目的 `Assets/Poseweaver`。也可在 Package Manager 中选择“Add package from disk”，选择该目录的 `package.json`。
2. 将 `.pwframes` 和资源目录放入 `Assets` 下同一位置。等待编译和导入完成。
3. 将 `.pwframes` 从 Project 窗口拖入场景。生成的对象含 `SpriteRenderer` 和 `PoseweaverPlayer`，默认自动播放第一个动作。
4. 在 Inspector 中设置 `Animation Name` 和 `Speed`，或从游戏代码调用：

```csharp
using Poseweaver;
public class Character : UnityEngine.MonoBehaviour
{
    public PoseweaverPlayer player;
    void Start() { player.Play("wave"); }
    public void Pause() { player.Pause(); }
    public void Resume() { player.Resume(); }
}
```

播放器还提供 `Stop()` 和 `IsPlaying`；`Advance(seconds)` 用于显式推进时间。自定义时钟使用前请禁用组件，避免自动 `Update` 重复推进。非循环动画结束停在最后一帧，`Stop` 回到首帧。修改或重新导出清单会重新导入；PNG 页面也登记为导入依赖。保持动作名不变可维持精灵子资源标识。

示例位于 `integrations/unity/Samples~/Demo`，通过 UPM 安装时可从 Samples 导入；直接放入 Assets 的安装方式需手动将该 Demo 文件夹复制到 Assets 的其他位置（Unity 忽略名称带 `~` 的目录）。拖入 `arm.pwframes` 即可播放，不需要另装动画控制器。

批处理检查入口（需要已安装的 Unity Editor，测试示例使用默认导出参数）：

```powershell
Unity.exe -batchmode -nographics -quit -projectPath <项目路径> -executeMethod Poseweaver.Editor.FramesSmokeTest.Run -poseweaverSample Assets/Demo/arm.pwframes -logFile unity-frames-test.log
```

## Godot

插件使用 Godot 4 的 `EditorImportPlugin` 和原生 `AnimatedSprite2D`。实际验证版本记录见下文；未声明 Godot 3 兼容。

1. 将 `integrations/godot/addons/poseweaver` 复制到项目的 `addons/poseweaver`。
2. 在“项目 → 项目设置 → 插件”启用 **Poseweaver Frames**。
3. 将 `.pwframes` 和资源目录放入项目。导入后直接拖入场景，它是原生 `AnimatedSprite2D`，会自动播放第一个动作。
4. 像普通动画精灵一样操作：

```gdscript
@onready var character: AnimatedSprite2D = $Character
func _ready() -> void:
    character.play("wave")
```

可使用 `pause()`、`play()`、`stop()`、`speed_scale` 和 `animation_finished`。像素原点转换为 `offset`，节点位置对应 Poseweaver 的世界原点。需要修改生成资源时，使用继承场景或在游戏代码中修改，避免下一次导入覆盖手工修改。

完整示例工程为 `integrations/godot/project.godot`，直接打开后等待导入，运行主场景。批处理自检：

```powershell
godot --headless --editor --path integrations/godot --import
godot --headless --path integrations/godot --script res://tests/frames_test.gd
```

## 数据约定与边界

- 清单为 UTF-8 JSON，`format="poseweaver.frames"`、`version=1`；图片为直通 RGBA PNG。
- `fps` 为每秒帧数；`pixelsPerUnit` 为 Unity 尺度。`frameWidth/frameHeight` 对全部帧一致。可选 `filter` 为 `nearest` 或 `linear`，省略时为 `linear`；配套插件据此设置像素采样。
- `originX/originY` 是每一帧左上角到世界原点的像素偏移，横向向右、纵向向下。Unity 翻转 Y 后换算为 Sprite pivot，Godot 转换为居中精灵的 offset。
- `pages` 中每项含相对路径 `file`、`width/height`；`animations` 中每项含唯一 `name`、秒数 `duration`、布尔 `loop` 和 `frames` 数组。
- 每帧含 `page`（从 0 开始）、左上角像素 `x/y`、`width/height`、秒数 `duration`。Godot 转换为 SpriteFrames 的相对帧时长。
- 目前限制：1–120 FPS，比例 0.05–4，最多 256 个选中动作、10000 帧；帧和单页边长不超过 4096，单帧超采样缓冲不超过 64 MiB，清单不超过 4 MiB。过大时降低比例/超采样或拆分工程。
- 帧动画内存随尺寸、帧率和时长增长。两套导入器验证版本、相对路径、PNG 尺寸、帧区域、正数时长和总帧数，拒绝不完整资源。

## 验证与后续

编辑流程回归覆盖动作筛选及独立循环、预算与实际图集尺寸和字节数一致、后台任务快照、估算不写文件、重复导出、写入图集后取消清理、关闭时取消、原子提交取消边界、配置按工程隔离与重启恢复、损坏配置保留，以及原有的分页、空动画、缺失图片和保存失败场景。

光源与像素示例另经实际 Godot 导入、播放及最近邻过滤设置检查；核心和编辑流程测试验证色板输出、描边、硬边透明、效果保存与撤销，以及视口缓存。

Unity 验收工程可在源码目录运行 `scripts/verify-unity.ps1 -PrepareOnly` 生成；提供已安装 Editor 路径后可运行导入/播放检查与 Windows 构建。当前按约定仅准备，尚未实机执行。入口与待验收项见 [RELEASE_ACCEPTANCE.md](RELEASE_ACCEPTANCE.md)。

Godot 使用实际安装的 **4.7.2 stable Steam** 做导入和播放检查，包括多动作、多页图集、重新导入、帧区域、原点、非整数时长、暂停、循环和非循环结束；同时手工验证错误版本、路径越界、过大尺寸和缺失图片被拒绝。Unity 提供批处理检查脚本；本机未安装 Unity Editor，尚未验证其引擎内编译和导入。正式承诺 Unity 兼容前，必须在目标 Editor 和游戏构建中执行检查。当前未验证两种引擎的发布构建。

实时骨骼流程复用核心姿态求值，并适配 Unity / Godot 网格与纹理；当前范围和验收记录见 [RUNTIME_INTEGRATION.md](RUNTIME_INTEGRATION.md)。

API 依据：[Unity ScriptedImporter](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/AssetImporters.ScriptedImporter.html)、[Unity Sprite.Create](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/Sprite.Create.html)、[Godot 导入插件](https://docs.godotengine.org/en/stable/tutorials/plugins/editor/import_plugins.html)、[Godot SpriteFrames](https://docs.godotengine.org/en/stable/classes/class_spriteframes.html)。
