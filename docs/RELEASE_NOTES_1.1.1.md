# Poseweaver 1.1.1

Windows x64 的 2D 骨骼动画编辑器，提供完整源码、安装版、绿色版及 Unity / Godot 接入代码。

## 下载

- `Poseweaver-1.1.1-setup.exe`：安装版。
- `Poseweaver-1.1.1-win64.zip`：解压后运行 `Poseweaver.exe`。
- `Poseweaver-animation-demo-zh-1080p.mp4`：78 秒骨骼动画制作演示，含中文字幕。
- `SHA256SUMS.txt`：发行附件校验值。

源码可在仓库浏览，或下载 GitHub 自动生成的 Source code。图片和 GIF 导出无需另装编码器；MP4、WebM、AVI、MOV 需要用户本机安装 FFmpeg。

## 动画制作

支持骨骼层级、网格与权重、Setup / Animate、关键帧与独立分量曲线、皮肤与关联网格、IK 和其他约束、前后帧参考、多轨混合、音频事件，以及图片和视频导出。

提供 `.pwframes` 烘焙帧和 `.pwskeleton` 实时骨骼的 Unity / Godot 接入，详见仓库内接入文档。1.1.1 改进了 Godot 场景导入与独立导出资源收集，以及两套播放器的自动播放、失败加载保护和生命周期处理。

## 验收与范围

- 发布前 12 组 CTest 全部通过。
- Godot 实际导入、播放、独立 Windows 导出及 OpenGL 渲染已验收。
- Unity Runtime / Editor 程序集编译通过；受本机 Editor 授权限制，Unity Editor 导入、渲染和 Player 仍待实机验收。
- 当前原生运行时二进制仅提供 Windows x64。工程最高使用自有 v8 格式；重要工程请保留旧版本副本。
- 项目自身暂未指定开源许可证；第三方许可全文随源码和发行包提供。
