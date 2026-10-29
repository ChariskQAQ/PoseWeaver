# Poseweaver 设计文档

类 Spine 的 2D 骨骼动画编辑器。C++17 + Dear ImGui（docking）+ GLFW + OpenGL。

## 目标（MVP）

- 导入 PNG（拖拽或文件对话框），挂到 slot 上作为 Region 附件
- 骨骼：创建、父子层级、选择、旋转/移动/缩放，Setup 模式与 Animate 模式
- 网格：从图片生成网格（按 alpha 裁剪的规则网格），编辑顶点
- 蒙皮：把网格绑定到多根骨骼，自动权重 + 笔刷绘制权重
- 动画：多动画，骨骼旋转/位移/缩放关键帧、slot 附件切换关键帧，线性/阶梯插值，播放与拖动时间轴
- 撤销/重做、保存/打开工程（JSON），导出供运行时使用的 JSON

## 模块划分

| 文件 | 职责 |
|---|---|
| `src/core/math.h` | `Vec2`、`Affine`（2x3 矩阵）等数学工具 |
| `src/core/model.h/.cpp` | 文档数据：`Skeleton`、`Bone`、`Slot`、`Attachment`、`Animation` |
| `src/core/pose.h/.cpp` | 运行时求值：采样动画 → 本地变换 → 世界矩阵 → 网格顶点（蒙皮） |
| `src/core/mesh_tools.h/.cpp` | 生成网格、绑定骨骼、自动权重、权重笔刷 |
| `src/core/mesh_edit.cpp` | 轮廓追踪、约束 Delaunay 三角化、增删顶点 |
| `src/core/clipping.h/.cpp` | 剪裁多边形分解与三角形裁剪 |
| `src/core/constraints.h/.cpp` | IK / 变换 / 路径 / 物理约束 |
| `src/core/serialize.h/.cpp` | JSON / 二进制读写（工程文件 = 导出格式） |
| `src/core/merge.h/.cpp` | 合并导入另一个工程 |
| `src/core/anim_state.h/.cpp` | 动画混合：多轨道、交叉淡化、队列、事件（AnimationState） |
| `src/core/atlas.h/.cpp` | 纹理打包（MaxRects，缩放 / 自动缩放） |
| `src/core/raster.h/.cpp` | CPU 光栅化（导出图片/视频）与 GIF 编码 |
| `src/core/psd.h/.cpp` | PSD 图层读取 |
| `src/core/audio_mix.h/.cpp` | 波形峰值、音频事件混音、WAV 编码 |
| `src/editor/*` | 编辑器状态、撤销栈、工具、各面板 UI；骨骼/网格/权重/关键帧都通过 ImDrawList 绘制 |
| `src/editor/splash.cpp` | 启动页、快捷键窗口、用户设置（最近文件）与上次会话文件 |
| `src/platform/*` | 纹理加载、文件对话框、音频播放与解码（miniaudio） |

`core/` 不依赖 ImGui/OpenGL；`src/runtime/` 已通过 C ABI 共享库复用它，为 Unity/Godot 提供实时骨骼求值。

## 数据模型

```
Skeleton
  bones[]      : name, parent(索引, -1 为根), length, setup{x,y,rotation,scaleX,scaleY,shearX,shearY}
                 计算世界矩阵时用 Skeleton::updateOrder() 保证父骨骼先于子骨骼
  slots[]      : name, bone, attachments[], activeAttachment(-1 为空), color, blend(normal|additive|multiply|screen)
                 数组顺序 = 绘制顺序；混合模式是 Setup 属性，不设动画关键帧
  attachment   : Region { image, x,y,rotation,scaleX,scaleY }
                 Mesh   { image, uvs[], triangles[], hullLength（前 N 个顶点按顺序构成外轮廓）,
                          vertices[] (未绑定: 相对 slot 骨骼的坐标),
                          weights[][] (已绑定: 每顶点若干 {bone, 骨骼局部坐标, weight}) }
                 Clipping / BoundingBox { vertices[] 或 weights[][]（多边形）, Clipping 另有 endSlot }
  images[]     : 相对工程文件的路径
  events[]     : name, int, float, string, audio（事件定义，关键帧可覆盖数值）
  skins[]      : name, attachments[] {slot, placeholder 名, attachment}, bones[], constraints[]
                 插槽里 skinPlaceholder=true 的附件只是占位，显示时按名字从激活的皮肤取实际附件
                 （多个激活皮肤：后激活的优先）；bone/constraint.skinRequired 时需被激活皮肤列出才生效
  constraints[]: type(ik|transform|path|physics), bones[], target（骨骼；路径约束为插槽）,
                 values[6]（可打关键帧的混合值等）, 各类型的参数；数组顺序 = 生效顺序
  animations[] : name, duration,
                 boneTimelines[bone] { rotate/translate/scale/shear 关键帧 }
                 slotTimelines[slot] { attachment / color 关键帧, deform[附件] { 每顶点偏移 } }
                 drawOrder[] { time, 插槽排列 }   events[] { time, event, 覆盖值 }
                 曲线: linear | stepped | bezier(cx1,cy1,cx2,cy2)，作用于该帧到下一帧
```

## 坐标与变换

- 世界坐标 y 轴向上（和 Spine 一致），视口绘制时翻转。
- 旋转、斜切用角度制。局部矩阵的 X 轴方向 = rotation + shearX，Y 轴方向 = rotation + 90 + shearY，长度分别为 scaleX、scaleY；世界矩阵 = 父世界矩阵 × 局部矩阵。
- 动画值是相对 setup 的增量：旋转/位移/斜切为相加，缩放为相乘；插槽颜色为绝对值。
- 已绑定网格的顶点位置 = Σ wᵢ · Boneᵢ.world × localᵢ，localᵢ 在绑定时由 setup 姿态求逆得到。
- 变形偏移：未绑定顶点加在 slot 骨骼空间；已绑定顶点加到每个 localᵢ 上（随骨骼一起转动）。
- 剪裁：剪裁多边形先三角化成凸块，后续插槽的三角形对每个凸块做 Sutherland–Hodgman 裁剪，直到 endSlot。
- 网格三角化：外轮廓耳切 → 插入内部点 → Lawson 翻边（不翻轮廓边），得到约束 Delaunay 三角网。
- 约束：Pose.local 是 setup+动画（打关键帧读它），Pose.applied 是约束后的局部变换（世界矩阵由它算）。
  每个约束读当前世界矩阵、算出被约束骨骼的新世界矩阵，再换回 applied 并更新子树。
- 物理约束依赖上一帧，不是时间的纯函数：状态（PhysicsState）由编辑器/运行时持有，每帧推进。

## 撤销

每次“提交”一次编辑前把整个 Skeleton 序列化快照入栈（上限 200）。文档规模小，实现简单可靠。

每个快照带独立的历史位置编号，保存成功后记录该位置；撤销/重做恢复位置并据此计算未保存状态，分支编辑使用新编号，历史裁剪不会把其他位置误当成保存点。复合修改先在文档副本上准备，再经 `commitDocumentChange` 提交一个撤销步骤；无变化时保留现有撤销/重做记录。无效快照恢复失败时不弹出历史。

## 文件安全与诊断

- `core/file_io` 以同目录临时文件、刷新、替换写入工程与设置，失败保留原文件；`core/diagnostics` 在加载和保存边界验证模型，并提供可定位的问题列表。
- `core/project_resources` 先验证与收集图片、事件音频，再提交相对资源路径的工程文件。新副本使用独占创建的资源子目录避免覆盖旧文件，失败只回滚本次创建的路径；成功后编辑器提交一次资源引用事务并设置保存点。保存对话框与文件菜单提供开关，默认关闭。
- `editor/project_safety` 统一工程切换与退出确认，每 30 秒在空闲时写入独立恢复副本。恢复后先作为未保存工程检查、另存为；未处理或损坏的旧恢复副本不会被自动覆盖。
- `editor/panel_issues` 沿用现有底部停靠面板，提供错误/警告筛选、资源重新检查和对象/顶点定位。资源缺失阻止图像导出，结构错误阻止数据写入，警告允许继续保存。
- `project_safety_tests` 覆盖写入失败与验证边界；`editor_flow_tests` 直接调用编辑器动作，覆盖编辑、打帧、撤销重做、保存、切换、恢复、重新定位资源和导出，使用隔离设置目录与隐藏图形窗口。

## 插槽混合与格式版本

- 新读取器支持工程格式 1–8。默认效果且仅使用正常混合的工程仍写 version 5；使用叠加、正片叠底或滤色时写 version 6 和插槽的 `blend` 字段。修改光源或像素设置后写 version 7 的全局 `effects` 字段，包括关闭但保留的设置。使用独立分量曲线/关键帧、关联网格或皮肤独立变形时写 version 8。旧版软件会拒绝不支持的版本，避免静默丢失效果。未知模式或无效效果设置拒绝加载并保留当前工程。
- CPU 导出采用 straight RGBA 存储，计算时转为预乘颜色。正常、正片叠底、滤色使用 source-over alpha；叠加累加预乘颜色，alpha 仍取 source-over。透明背景上无底层时保留源图颜色与透明度；透明像素不参与叠色。三角形采用半开边缘规则，避免网格公共边被叠色两次。
- 视口底色不透明。非正常模式按需创建并缓存预乘纹理，插槽颜色也乘以透明度，通过 OpenGL 绘制回调设置混合因子；每个插槽之后恢复 ImGui 状态。普通图片预览和洋葱皮保持原绘制方式。每张使用非正常模式的图片额外占用一份 RGBA 纹理，换图/关闭时释放。
- PSD 支持 `norm`、`lddg`、`mul `、`scrn`；其他图层模式以正常模式导入，并在问题面板列出图层名与原模式。尚不模拟调整图层、蒙版、组混合与 Photoshop 色彩管理。
- `blend_render_tests` 在隐藏 OpenGL 3.3 窗口中比较四种模式的 GPU/CPU 像素结果，并验证界面混合状态恢复与纹理释放。性能基准见 [PERFORMANCE.md](PERFORMANCE.md)。

光源和像素处理由 `render_effects.cpp` 提供，共用 CPU 光栅输出：先合成透明角色，再执行世界坐标点光源、像素采样、限色抖动与描边，最后合成导出背景。无效果时保留原渲染路径。视口可选 CPU 处理或 `platform/gpu_effects.cpp` 的 OpenGL 3.3 片元着色器，GPU 从 `renderPoseBase` 的透明合成图生成预览纹理，失败回退 CPU；导出仍共用 CPU 路径。后端选择保存在用户设置，不改变工程。两种预览都缓存处理结果，姿态或工程变化时刷新；像素模式用最近邻和硬边透明，导出强制 1x 采样。使用边界见 [LIGHTING_PIXELS.md](LIGHTING_PIXELS.md)。

## 尚未实现

见 [ROADMAP.md](ROADMAP.md)：按 Spine 专业版功能表列出全部待做功能与分阶段计划。

工程格式 v8 扩展独立分量关键帧/缓动、关联网格引用与皮肤独立变形；读取 v1–v8，未使用这些字段的工程沿用已有写入版本。局部撤销按 Bone/Name/Attachment 记录逆操作，Document 快照仍处理结构和复杂补偿。
