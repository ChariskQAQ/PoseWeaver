# RigEditor

类 Spine 的 2D 骨骼动画编辑器（C++17 + Dear ImGui + GLFW + OpenGL）。设计见 [docs/DESIGN.md](docs/DESIGN.md)。

## 功能

- 导入 PNG（拖进窗口或 文件 → 导入图片）
- 骨骼：建骨骼工具拖出骨骼链，层级面板拖拽改父级，旋转/移动/缩放工具
- Setup / Animate 双模式（Tab 切换）；Animate 模式下拖动骨骼自动打关键帧
- 网格：图片按透明区域生成网格，网格工具拖动/框选顶点
- 蒙皮：绑定多根骨骼并自动计算权重，权重笔刷涂抹（Ctrl 减少）
- 时间轴：多动画、播放/循环、拖动关键帧、右键删除或改为阶梯曲线
- 撤销/重做、保存/打开工程（`.rig.json`）、导出运行时 JSON

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

`--weights` 以权重视图打开 demo，`--time <秒>` 指定 demo 时间，`--screenshot out.png` 渲染几帧后截图退出（用于自动检查）。

## 快捷键

| 键 | 作用 |
|---|---|
| Tab | Setup / Animate 切换 |
| R / T / S | 旋转 / 移动 / 缩放 |
| B / M / W | 建骨骼 / 网格 / 权重（仅 Setup） |
| 中键或右键拖动、滚轮 | 平移、缩放视图；F 适配 |
| 空格 / ← → | 播放暂停 / 逐帧 |
| K | 所选骨骼打全部关键帧 |
| Ctrl+Z / Ctrl+Y / Ctrl+S | 撤销 / 重做 / 保存 |
| Del | 删除所选 |
