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
  bones[]      : name, parent(索引, -1 为根), length, setup{x,y,rotation,scaleX,scaleY}
                 计算世界矩阵时用 Skeleton::updateOrder() 保证父骨骼先于子骨骼
  slots[]      : name, bone, attachments[], activeAttachment(-1 为空)   ← 数组顺序 = 绘制顺序
  attachment   : Region { image, x,y,rotation,scaleX,scaleY }
                 Mesh   { image, uvs[], triangles[],
                          vertices[] (未绑定: 相对 slot 骨骼的坐标),
                          weights[][] (已绑定: 每顶点若干 {bone, 骨骼局部坐标, weight}) }
  images[]     : 相对工程文件的路径
  animations[] : name, duration,
                 boneTimelines[bone] { rotate/translate/scale 关键帧 }
                 slotTimelines[slot] { attachment 关键帧 }
                 每个关键帧: time, value, curve(linear|stepped)
```

## 坐标与变换

- 世界坐标 y 轴向上（和 Spine 一致），视口绘制时翻转。
- 旋转用角度制。骨骼世界矩阵 = 父世界矩阵 × T(x,y) × R(rot) × S(sx,sy)。
- 动画值是相对 setup 的增量：旋转/位移为相加，缩放为相乘。
- 已绑定网格的顶点位置 = Σ wᵢ · Boneᵢ.world × localᵢ，localᵢ 在绑定时由 setup 姿态求逆得到。

## 撤销

每次“提交”一次编辑前把整个 Skeleton 序列化快照入栈（上限 200）。文档规模小，实现简单可靠。

## 尚未实现（后续路线）

- 曲线编辑器（贝塞尔插值）、网格 deform 关键帧、slot 颜色关键帧
- IK / 变换约束 / 路径约束
- 皮肤（skins）、事件关键帧、多选骨骼
- 任意轮廓网格（手绘外轮廓 + Delaunay 三角化），目前只有按透明度裁剪的规则网格
- 纹理图集打包、游戏运行时（`core/` 可直接复用）
