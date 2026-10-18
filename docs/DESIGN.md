# RigEditor 设计文档

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
| `src/core/serialize.h/.cpp` | JSON 读写（工程文件 = 导出格式） |
| `src/editor/*` | 编辑器状态、撤销栈、工具、各面板 UI；骨骼/网格/权重/关键帧都通过 ImDrawList 绘制 |
| `src/platform/*` | 纹理加载、文件对话框 |

`core/` 不依赖 ImGui/OpenGL，以后可以单独抽出来做游戏运行时。

## 数据模型

```
Skeleton
  bones[]      : name, parent(索引, -1 为根), length, setup{x,y,rotation,scaleX,scaleY,shearX,shearY}
                 计算世界矩阵时用 Skeleton::updateOrder() 保证父骨骼先于子骨骼
  slots[]      : name, bone, attachments[], activeAttachment(-1 为空)   ← 数组顺序 = 绘制顺序
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

## 尚未实现

见 [ROADMAP.md](ROADMAP.md)：按 Spine 专业版功能表列出全部待做功能与分阶段计划。
