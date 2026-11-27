# Foco

Foco 是一款轻量的 Windows 副屏减扰工具。鼠标离开选定的副屏后，Foco 会按设置降低画面亮度、冻结画面或显示屏保。鼠标移回副屏时，实时画面随即恢复。

[下载 Windows x64 免安装包](https://github.com/tonyleelyy/Foco/releases)

## 软件特性

| 功能 | 效果与使用场景 |
| --- | --- |
| **减少亮度** | 用软件遮罩调暗副屏，支持从 0% 完全透明到 100% 纯黑。需要随时查看副屏上的文档或面板时，可以保留实时画面并降低亮度。 |
| **冻结画面** | 定格鼠标离开时的画面，适合把图表或参考资料留在副屏。底层程序照常运行，鼠标移回时显示最新画面。 |
| **副屏屏保** | 全屏显示本地壁纸或纯色背景。可显示时钟，支持 12/24 小时制，并可调整秒数、日期、字号、颜色和位置。 |

变暗可叠加在实时桌面、冻结画面或屏保上。同时勾选冻结和屏保时，优先显示屏保。

- **渐变过渡。** 移出时淡入、移入时淡出可以分别开关。
- **托盘与快捷键。** 关闭设置窗口后，Foco 继续在系统托盘运行。可以从托盘打开设置、暂停、继续或退出，也可以用默认快捷键 `Ctrl + Alt + F` 暂停或继续。
- **本地设置。** 总开关、目标副屏、效果、壁纸和时钟设置会自动保存，重启后恢复。界面跟随 Windows 浅色和深色主题，也可选择登录 Windows 后运行。

## 快速开始

1. 从 [GitHub 发布页](https://github.com/tonyleelyy/Foco/releases) 下载 Windows x64 ZIP，解压后运行 `Foco.exe`。
2. 在设置页选择副屏。可以先点击「识别显示器」，核对每块屏幕的编号。
3. 勾选所需效果。Foco 首次启动默认启用，选好副屏和效果后就会开始监测。
4. 将鼠标移出副屏查看效果，再移回副屏恢复。

## 系统要求

- Windows 11 x64，至少两块显示器，Windows 显示模式设为「扩展这些显示器」。
- 冻结支持本地 SDR 扩展桌面。壁纸格式支持取决于系统安装的图像解码器。

## 下一版功能

以下功能已在本地开发版实现。公开下载以 [GitHub 发布页](https://github.com/tonyleelyy/Foco/releases) 上的预发布版为准。

- **可调离开延时。** 可选立即、150 毫秒（默认）、0.5 秒、1 秒或 3 秒。
- **定时暂停。** 从托盘暂停 15 分钟或 1 小时，到期自动恢复，也可以提前继续。
- **星期显示。** 屏保时钟可分别开关秒数、日期和星期。

## 从源码构建

需要 Visual Studio 2022 的 C++ 桌面开发工具、Windows SDK 和 CMake 3.21 或更新版本。

```powershell
cmake -S . -B build -G 'Visual Studio 17 2022' -A x64
cmake --build build --config Release --parallel
cpack --config build/CPackConfig.cmake -C Release -B out
```

可执行文件生成在 `build/Release/Foco.exe`，ZIP 发布包生成在 `out`。设置保存在 `%LOCALAPPDATA%\Foco\settings.txt`。

## 反馈

欢迎在 [GitHub Issues](https://github.com/tonyleelyy/Foco/issues) 提交问题或功能建议。反馈显示问题时，请附上 Windows 版本、显示器分辨率和缩放比例，以及复现步骤。

[开发计划](DEVELOPMENT_PLAN.md)
