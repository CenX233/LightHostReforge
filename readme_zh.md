Light Host Reforge
---

[English](readme.md)|简体中文

[LightHost](https://github.com/opencma/LightHost)重制版分支，主要改动有：

- 移植到JUCE8
- 添加对Waves插件的支持（已测试Waves V15）
- 添加效果链预设系统
- 添加效果器窗口工具栏
- 添加Loopback音频设备类型，可捕获桌面音频送入效果器链（仅Windows，不支持输出）
- 支持改变效果器窗口大小，部分支持HiDPI
- 支持插件窗口保持最前
- 添加插件旁通状态显示
- 添加音频链路更改时的淡入淡出过渡
- 添加效果器延时和链路总延时的显示
- 更改为CMake构建系统

注意事项：

- 暂时禁用了对VST2/AU格式插件的支持
- 目前仅在Windows上完成构建和测试，之后会在Linux上构建，对于macOS无法提供支持，因为我没有苹果设备

## 构建

Windows（使用VS2022 + vcpkg）：

```Bash
vcpkg install juce asiosdk
mkdir build
cd build
cmake -DCMAKE_TOOLCHAIN_FILE=path\to\vcpkg.cmake ..
MSBuild .\LightHostReforge.sln /p:Configuration=Release
```

---

Light Host
---

A simple VST/AU host for OS X, Windows, and Linux that sits in the menu/task bar.

### Features

See [#1](https://github.com/rolandoislas/LightHost/issues/1)

### Screenshot

![Light Host 1.2](http://i.imgur.com/UF9SWfC.jpg)