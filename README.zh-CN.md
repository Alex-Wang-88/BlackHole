# BlackHole

[English](README.md) | **简体中文**

这是一个可以在 Windows、Linux 以及其他支持 OpenGL 4.3 的系统上运行的实时黑洞可视化程序。光线追踪在 GPU 上通过 OpenGL Compute Shader 执行，并直接写入 OpenGL 合成阶段使用的纹理。

## 针对 Windows/GPU 的改动

- 用 OpenGL 4.3 Compute Shader 替换了 macOS 专用的 Metal 和 Objective-C++ 路径。
- 测地线积分仍全部放在显卡上，并移除了“显卡 → CPU → 显卡”的每帧回读/上传。
- 光追可以关闭，关闭后保留轻量的黑洞轮廓、吸积环和三维网格视图。
- 默认目标帧率为 30 FPS。VSync 仍关闭，由程序自身稳定限帧。
- 为 NVIDIA Optimus 和 AMD PowerXpress 笔记本增加了优先选择独立显卡的导出标记。
- 分辨率、积分步数、时间累积采样数和垂直同步均可通过环境变量调整。

## 环境要求

- Windows 10/11、Linux 或其他支持 OpenGL 4.3 的系统
- 能提供 OpenGL 4.3 Compute Shader 的显卡驱动
- Visual Studio 2022 C++ 工具，或其他 C++17 编译器
- CMake 3.20 以上
- Git（第一次配置时 CMake 会自动下载 GLFW 和 GLM）

## Windows 快速运行

在 PowerShell 中执行：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\run.ps1
```

也可以手动构建：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
.\build\Release\BlackHole.exe
```

构建后，程序会把 `raytrace.comp` 复制到 EXE 同目录，因此也可以直接进入 `Release` 文件夹双击运行。

## 运行参数

所有参数都是可选的环境变量：

| 变量 | 默认值 | 作用 |
| --- | ---: | --- |
| `BLACKHOLE_WINDOW_WIDTH` / `BLACKHOLE_WINDOW_HEIGHT` | `800` / `600` | 窗口大小 |
| `BLACKHOLE_RENDER_SCALE` | `1.0` | GPU 渲染分辨率相对窗口的比例，范围 `0.25` 到 `4.0` |
| `BLACKHOLE_RENDER_WIDTH` / `BLACKHOLE_RENDER_HEIGHT` | 窗口大小乘比例 | 显式指定 GPU 渲染分辨率 |
| `BLACKHOLE_RAYTRACE` | `0` | 设为 `1` 开启 GPU 光追 |
| `BLACKHOLE_MAX_STEPS` | `16000` | 每条光线的最大测地线积分步数 |
| `BLACKHOLE_TAA_SAMPLES` | `0` | 开启光追时的时间累积上限；`0` 表示不设上限 |
| `BLACKHOLE_TARGET_FPS` | `30` | 目标帧率；`0` 表示不限帧 |
| `BLACKHOLE_VSYNC` | `0` | 设为 `1` 开启垂直同步 |

例如使用 2560×1600 GPU 渲染并保持不限帧：

```powershell
$env:BLACKHOLE_RENDER_WIDTH = '2560'
$env:BLACKHOLE_RENDER_HEIGHT = '1600'
$env:BLACKHOLE_VSYNC = '0'
.\build\Release\BlackHole.exe
```

`BLACKHOLE_MAX_STEPS` 保留的是数值积分的安全上限，不是 FPS 限制；完全删除它可能导致异常光线或驱动超时，进而卡死显卡。

## 操作方式

- 鼠标左键拖动：环绕黑洞旋转相机
- `Shift` + 鼠标左键拖动：平移相机目标
- 鼠标滚轮：拉近或拉远

移动相机会清空时间累积。关闭光追时，光追图层透明，但会保留轻量的黑洞轮廓、吸积环和三维透视网格。

## 项目结构

```text
BlackHole.cpp          跨平台入口和渲染循环
Scene.hpp/.cpp         相机、黑洞、场景常量和运行参数
Engine.hpp/.cpp        GLFW 窗口、OpenGL 合成和三维网格
GpuRayTracer.hpp/.cpp  OpenGL Compute 调度和 GPU 缓冲区
OpenGLLoader.*         基于 GLFW 的现代 OpenGL 函数加载器
shaders/raytrace.comp  GPU 测地线光线追踪 Compute Shader
CMakeLists.txt         跨平台 CMake 构建配置
Draft/                 早期实验代码，不参与构建
```
