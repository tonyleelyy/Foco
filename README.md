# Foco

Foco 是一款 Windows 副屏减扰工具。鼠标离开选定的副屏后，可以变暗、定格桌面，或显示壁纸和自定义时钟。移回副屏即可恢复互动。

## 使用方法

1. 在 Windows 11 x64 上解压发布包并打开 `Foco.exe`。需要至少两块设置为扩展桌面的显示器。
2. 从列表选择副屏。可以点击「识别显示器」核对显示器编号。
3. 勾选所需效果。Foco 首次启动默认启用，之后会恢复上次的开关状态和设置。「移出时淡入」和「移入时淡出」可以分别设置。
4. 关闭设置窗口后，从系统托盘重新打开、暂停或退出。也可以用默认快捷键 `Ctrl + Alt + F` 暂停或继续。

冻结保留鼠标离开时捕获的一帧画面，底层程序照常运行。屏保成功显示时优先于冻结，变暗可以叠加在两者上。冻结支持本地 SDR 扩展桌面。壁纸由 Windows 图像解码器读取，内置可用的格式因系统而异。

## 构建

需要 Visual Studio 2022 的 C++ 桌面开发工具、Windows SDK 和 CMake 3.21 或更新版本。

```powershell
cmake -S . -B build -G 'Visual Studio 17 2022' -A x64
cmake --build build --config Release --parallel
cpack --config build/CPackConfig.cmake -C Release -B out
```

可执行文件生成在 `build/Release/Foco.exe`，ZIP 发布包生成在 `out`。程序使用 Windows 原生 API，设置保存在 `%LOCALAPPDATA%\Foco\settings.txt`。
