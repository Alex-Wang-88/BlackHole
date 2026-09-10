# BlackHole

**English** | [简体中文](README.zh-CN.md)

A real-time black-hole visualization for Windows using a native Direct3D 12
renderer. The ray-tracing stage runs in an HLSL compute shader on the GPU and
writes directly into UAV textures used by the D3D12 compositor.

## What changed for Windows/GPU

- Replaced the macOS-only Metal and Objective-C++ path with a native D3D12
  compute and graphics backend.
- Kept the heavy ray integration on the GPU and removed the GPU-to-CPU-to-GPU
  frame copy.
- GPU ray tracing is enabled by default with a 256×192 working resolution,
  6144 integration steps, and one temporal sample for a smoother 60 FPS
  preset on the tested RTX 5060 Laptop GPU. More temporal samples remain
  available through `BLACKHOLE_TAA_SAMPLES` when image quality is preferred.
- Ray tracing can still be disabled for a lightweight raster black-hole
  silhouette and grid view.
- The default target is 60 FPS. VSync remains disabled so the application uses
  its own stable frame pacing.
- Added discrete-GPU preference exports for NVIDIA Optimus and AMD PowerXpress
  laptops.
- Added runtime settings for resolution, integration steps, temporal samples,
  and VSync.

## Requirements

- Windows 10/11 with a Direct3D 12-capable GPU driver
- Visual Studio 2022 C++ tools or another C++17 compiler
- CMake 3.20 or newer
- Git (CMake downloads GLM on the first configure)

## Quick start on Windows

From PowerShell:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\run.ps1
```

Or build manually:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
.\build\Release\BlackHole.exe
```

The executable and the three HLSL shader files are placed together, so
launching the EXE from its Release directory also works.

## Runtime settings

All settings are optional environment variables:

| Variable | Default | Meaning |
| --- | ---: | --- |
| `BLACKHOLE_WINDOW_WIDTH` / `BLACKHOLE_WINDOW_HEIGHT` | `800` / `600` | Window size |
| `BLACKHOLE_RENDER_SCALE` | `0.32` | GPU render size relative to the window, `0.25` to `4.0` |
| `BLACKHOLE_RENDER_WIDTH` / `BLACKHOLE_RENDER_HEIGHT` | scaled window size | Explicit GPU render size |
| `BLACKHOLE_RAYTRACE` | `1` | Set to `0` to use the lightweight raster fallback |
| `BLACKHOLE_MAX_STEPS` | `6144` | Maximum geodesic integration steps per ray |
| `BLACKHOLE_TAA_SAMPLES` | `1` | Temporal accumulation limit when ray tracing is enabled; `0` means unlimited |
| `BLACKHOLE_TARGET_FPS` | `60` | Frame-rate target; `0` means unlimited |
| `BLACKHOLE_VSYNC` | `0` | Set to `1` to enable VSync |

For example, to render at 2560×1600 with VSync disabled:

```powershell
$env:BLACKHOLE_RENDER_WIDTH = '2560'
$env:BLACKHOLE_RENDER_HEIGHT = '1600'
$env:BLACKHOLE_VSYNC = '0'
.\build\Release\BlackHole.exe
```

`BLACKHOLE_MAX_STEPS` remains a safety bound for the numerical integrator;
removing it entirely would allow a bad ray or driver timeout to hang the GPU.

## Controls

- Left mouse drag: orbit around the black hole
- `Shift` + left mouse drag: pan the camera target
- Mouse wheel: zoom in or out

Camera movement resets temporal accumulation. With ray tracing disabled, a
lightweight raster black-hole silhouette and accretion ring remain visible;
the expensive ray-traced layer is transparent.

## Project structure

```text
BlackHole.cpp          Windows application entry point and render loop
Scene.hpp/.cpp         Camera, black hole, scene constants, and settings
D3D12Engine.hpp/.cpp   Win32 window, D3D12 resources, pipelines, and 3D grid
shaders/raytrace.hlsl  GPU geodesic ray-tracing compute shader
shaders/scene.hlsl     Perspective grid shaders
shaders/composite.hlsl Fullscreen compositor and raster fallback
CMakeLists.txt         Windows D3D12 CMake build configuration
Draft/                 Earlier experimental implementation, not built
```
