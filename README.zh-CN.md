# BlackHole

[English](README.md) | **简体中文**

这是一个面向 Windows、使用原生 Direct3D 12 渲染器的实时黑洞可视化程序。光线追踪在 GPU 上通过 HLSL Compute Shader 执行，并直接写入 D3D12 合成阶段使用的 UAV 纹理。

## 针对 Windows/GPU 的改动

- 用原生 D3D12 Compute/Graphics 管线替换了 macOS 专用的 Metal 和 Objective-C++ 路径。
- 测地线积分仍全部放在显卡上，并移除了“显卡 → CPU → 显卡”的每帧回读/上传。
- 默认开启 GPU 光追，使用 `533×400` 工作分辨率、`6144` 步积分和 1 帧时间累积；输出为 `800×600`，DLSS 默认使用 `Quality（最高质量）` 模式。
- 光追也可以关闭，关闭后保留轻量的黑洞轮廓、吸积环和三维网格视图。
- 默认不限帧。VSync 仍关闭，实时 FPS 会显示在左上角。
- 为 NVIDIA Optimus 和 AMD PowerXpress 笔记本增加了优先选择独立显卡的导出标记。
- 增加了同一窗口内的现代化画质侧栏：使用深色卡片分组、状态摘要、大点击区域和清晰的当前值；左侧保持原来的渲染区域，右侧增加画质控制，不覆盖渲染画面。滑杆拖动时只预览，松开鼠标后才应用 GPU 资源重建。可实时调整光追、渲染比例、光线积分步数、DLSS 模式、时间累积采样数和 VSync，也可以一键恢复默认值。按 `F1` 显示或隐藏侧栏。
- 相机交互使用自适应预览路径：旋转或平移时，光线积分临时限制为 `1024` 步，避免最高画质设置在运动中掉到个位数帧；停止输入 `180ms` 后恢复面板选择的积分上限，并重新渲染一帧高质量结果。这个限制独立于面板里的最大步数，也不是 FPS 锁定。

## 环境要求

- Windows 10/11，以及支持 Direct3D 12 的显卡驱动
- Visual Studio 2022 C++ 工具，或其他 C++17 编译器
- CMake 3.20 以上
- Git（第一次配置时 CMake 会自动下载 GLM）

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

构建后，程序会把三个 HLSL Shader 复制到 EXE 同目录，因此也可以直接进入 `Release` 文件夹双击运行。

## 运行参数

所有参数都是可选的环境变量：

| 变量 | 默认值 | 作用 |
| --- | ---: | --- |
| `BLACKHOLE_WINDOW_WIDTH` / `BLACKHOLE_WINDOW_HEIGHT` | `800` / `600` | 左侧渲染区域/输出大小；整体窗口右侧会额外增加 320px 画质侧栏 |
| `BLACKHOLE_RENDER_SCALE` | `0.6667` | GPU 渲染分辨率相对窗口的比例，范围 `0.25` 到 `4.0` |
| `BLACKHOLE_RENDER_WIDTH` / `BLACKHOLE_RENDER_HEIGHT` | 窗口大小乘比例 | 显式指定 GPU 渲染分辨率 |
| `BLACKHOLE_RAYTRACE` | `1` | 设为 `0` 使用轻量光栅 fallback |
| `BLACKHOLE_MAX_STEPS` | `6144` | 每条光线的最大测地线积分步数 |
| `BLACKHOLE_TAA_SAMPLES` | `1` | 开启光追时的时间累积上限；`0` 表示不设上限 |
| `BLACKHOLE_DLSS` | `1` | Streamline 运行库可用时开启 NVIDIA DLSS |
| `BLACKHOLE_DLSS_MODE` | `quality` | `quality`、`balanced`、`performance` 或 `ultraperformance` |
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
- `F1`：显示或隐藏右侧详细画质侧栏

面板中的 `Quality（最高质量）` 是 DLSS Super Resolution 的最高质量档位。渲染比例和积分步数可以独立调整：前者控制输入分辨率，后者控制每条光线的计算精度，两者都不会改变窗口输出分辨率。面板内的改动只对当前运行有效；如需可复现的启动配置，请使用环境变量。

DLSS 使用 NVIDIA Streamline 的 D3D12 接入。公开头文件位于 `third_party/streamline/include`；签名运行库 DLL 不提交到仓库，需要放到 `third_party/streamline/bin`，或通过 `BLACKHOLE_STREAMLINE_RUNTIME_DIR` 指定目录。

移动相机会清空时间累积。关闭光追时，光追图层透明，但会保留轻量的黑洞轮廓、吸积环和三维透视网格。

## 项目结构

```text
BlackHole.cpp          Windows 入口和渲染循环
Scene.hpp/.cpp         相机、黑洞、场景常量和运行参数
D3D12Engine.hpp/.cpp   Win32 窗口、D3D12 资源/管线和三维网格
shaders/raytrace.hlsl  GPU 测地线光线追踪 Compute Shader
shaders/scene.hlsl     透视网格 Shader
shaders/composite.hlsl 全屏合成和光栅 fallback
CMakeLists.txt         Windows D3D12 CMake 构建配置
Draft/                 早期实验代码，不参与构建
```
