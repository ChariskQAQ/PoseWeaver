# RigEditor

类 Spine 的 2D 骨骼动画编辑器（C++17 + Dear ImGui + GLFW + OpenGL）。设计见 [docs/DESIGN.md](docs/DESIGN.md)。

## 功能

- 导入 PNG（拖进窗口或 文件 → 导入图片）
- 骨骼：建骨骼工具拖出骨骼链，层级面板拖拽改父级，旋转/移动/缩放工具
- Setup / Animate 双模式（Tab 切换）；Animate 模式下拖动骨骼自动打关键帧
- 网格：按图片轮廓自动生成（约束 Delaunay），Alt+点击加顶点、Del 删除，UV 跟随或变形模式
- 变形关键帧：Animate 模式下用网格工具拖顶点
- 剪裁、边界框、路径附件（插槽属性里 + 剪裁 / + 边界框 / + 路径）
- 约束：IK、变换、路径、物理（骨骼属性 → 新建约束，或树的“约束”节点右键），数值可打关键帧，物理实时预览并可烘焙
- 补偿：工具栏“补偿 → 骨骼/图片”，调整骨骼时子骨骼/附件保持原位
- 皮肤：树的“皮肤”节点新建/激活（可多选组合），附件“移入皮肤...”生成占位，占位属性里为每个皮肤换图/复制；骨骼与约束可设为皮肤专用
- 蒙皮：绑定多根骨骼并自动计算权重，笔刷涂抹（Ctrl 减少）、锁定骨骼、数值设置、平滑、修剪、左右镜像
- 时间轴：多动画、播放/倒放/循环/变速；Dopesheet 多选/框选、整体拖动、复制粘贴、右键改曲线；洋葱皮
- Graph 曲线编辑：贝塞尔手柄、缓入缓出等预设
- 斜切、插槽颜色关键帧、绘制顺序关键帧、事件（播放时在视口提示）
- 撤销/重做、保存/打开工程（`.rig.json`）、导出运行时 JSON

待做功能与开发顺序见 [docs/ROADMAP.md](docs/ROADMAP.md)。

## 构建（Windows）

需要 Visual Studio 2022 Build Tools（含 C++ 工作负载与 CMake）。依赖会在首次配置时自动下载。

```bat
scripts\build.bat Release
```

产物：`build\Release\RigEditor.exe`。脚本会顺带运行 `rigcore_tests`。

## 运行

```bat
build\Release\RigEditor.exe
build\Release\RigEditor.exe --demo samples\arm.png        :: 两骨骼蒙皮手臂 + wave 动画
build\Release\RigEditor.exe path\to\project.rig.json
```

`--weights` / `--mesh` / `--clip` / `--ik` / `--path` / `--ghost` / `--bake` / `--skin` 以对应场景打开 demo（`--skin` 需要 `samples/arm_alt.png`），`--graph` 打开 Graph 视图，`--time <秒>` 指定 demo 时间，`--screenshot out.png` 渲染几帧后截图退出（用于自动检查）。

## 界面

布局参考 Spine（见 [docs/UI.md](docs/UI.md)）：视口居中，左上角 SETUP/ANIMATE 标签点击切换模式；
视口底部浮动工具栏包含工具、变换数值与打帧按钮、坐标轴、自动关键帧和显示选项；右侧为树与属性/权重视图；
Animate 模式下方为 Dopesheet。打帧按钮：绿 = 无关键帧，橙 = 有未打帧改动，红 = 当前帧已有关键帧。

## 快捷键

| 键 | 作用 |
|---|---|
| Tab | Setup / Animate 切换 |
| C / V / X / Z | 旋转 / 移动 / 缩放 / 斜切 |
| B | 姿势工具 |
| N / G | 建骨骼 / 权重（仅 Setup） |
| M | 网格工具：Setup 编辑顶点（Alt+点击加、Del 删），Animate 打变形关键帧 |
| 右键拖动、滚轮 | 平移、缩放视图；右键单击切回上一个工具 |
| F / Ctrl+F | 适配骨架 / 100% |
| Esc、双击空白 | 取消选择 |
| 空格 / ← → | 播放暂停 / 逐帧 |
| K | 为所有未打帧的改动打关键帧（关闭自动关键帧时） |
| F2 | 重命名所选 |
| Dopesheet 内：Ctrl 点击 / 拖框 / Ctrl+A | 多选关键帧；Del 删除，Ctrl+C / Ctrl+V 复制粘贴到当前帧 |
| Ctrl+Z / Ctrl+Y / Ctrl+S | 撤销 / 重做 / 保存 |
| Del | 删除所选 |
