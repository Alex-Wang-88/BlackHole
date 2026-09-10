#include "Engine.hpp"
#include "GpuRayTracer.hpp"
#include "Scene.hpp"

#include <GLFW/glfw3.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

#ifdef _WIN32
// Ask Windows hybrid-GPU systems to prefer the discrete adapter for this
// OpenGL application. The NVIDIA and AMD drivers honor these exports when
// present; the code still works on systems with only one GPU.
extern "C"
{
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001UL;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace
{
#ifdef _WIN32
class TimerResolutionGuard
{
public:
    TimerResolutionGuard()
    {
        timeBeginPeriod(1);
    }

    ~TimerResolutionGuard()
    {
        timeEndPeriod(1);
    }
};
#endif

std::string resolveShaderPath(const char* executablePath)
{
    namespace fs = std::filesystem;

    const fs::path executable =
        executablePath != nullptr ? fs::path(executablePath) : fs::path();
    const fs::path executableDirectory = executable.parent_path();
    const fs::path nextToExecutable = executableDirectory / "raytrace.comp";
    if(fs::exists(nextToExecutable)) return nextToExecutable.string();

    const fs::path currentDirectoryShader = fs::path("shaders") / "raytrace.comp";
    if(fs::exists(currentDirectoryShader)) return currentDirectoryShader.string();

    return nextToExecutable.string();
}

template<typename Clock>
void paceUntil(typename Clock::time_point deadline)
{
    while(true)
    {
        const auto now = Clock::now();
        if(now >= deadline) return;

        const auto remaining = deadline - now;
        if(remaining > std::chrono::milliseconds(2))
        {
            // Leave a small tail for a precise yield/spin. This avoids the
            // several-millisecond oversleep that is common on Windows.
            std::this_thread::sleep_for(
                remaining - std::chrono::milliseconds(1));
        }
        else
        {
            std::this_thread::yield();
        }
    }
}
}

int main(int argc, char** argv)
{
    try
    {
        const RenderSettings settings = loadRenderSettings();
        Engine engine(settings);
        std::unique_ptr<GpuRayTracer> gpuRayTracer;
        if(settings.rayTracing)
        {
            gpuRayTracer = std::make_unique<GpuRayTracer>(
                engine.colorTexture(),
                engine.materialTexture(),
                engine.RENDER_WIDTH,
                engine.RENDER_HEIGHT,
                settings.maxSteps,
                settings.temporalSampleLimit,
                resolveShaderPath(argc > 0 ? argv[0] : nullptr));
        }
        setupCameraCallbacks(engine.window);

        std::cout << "Window: " << engine.WIDTH << " x " << engine.HEIGHT
                  << "\n";
        std::cout << "Ray tracing: " << (settings.rayTracing ? "on" : "off")
                  << "\n";
        std::cout << "VSync: " << (settings.vsync ? "on" : "off") << "\n";
        std::cout << "Target FPS: " << (settings.targetFps == 0 ? "unlimited" :
                                          std::to_string(settings.targetFps))
                  << "\n";

        using Clock = std::chrono::steady_clock;
#ifdef _WIN32
        TimerResolutionGuard timerResolution;
#endif
        int frameCount = 0;
        double lastPrintTime = std::chrono::duration<double>(
            Clock::now().time_since_epoch()).count();
        const auto frameBudget = settings.targetFps > 0
            ? std::chrono::duration_cast<Clock::duration>(
                  std::chrono::duration<double>(1.0 / settings.targetFps))
            : Clock::duration::zero();

        while(!glfwWindowShouldClose(engine.window))
        {
            const auto frameStart = Clock::now();

            if(gpuRayTracer)
                gpuRayTracer->render(engine.WIDTH, engine.HEIGHT);
            engine.renderScene(SagA.r_s);

            if(frameBudget > Clock::duration::zero())
                paceUntil<Clock>(frameStart + frameBudget);

            ++frameCount;
            const double currentTime = std::chrono::duration<double>(
                Clock::now().time_since_epoch()).count();
            const double elapsed = currentTime - lastPrintTime;

            if(elapsed >= 1.0)
            {
                const double fps = static_cast<double>(frameCount) / elapsed;
                std::cout << "FPS: " << fps << std::endl;
                frameCount = 0;
                lastPrintTime = currentTime;
            }
        }
    }
    catch(const std::exception& error)
    {
        std::cerr << "Fatal error: " << error.what() << "\n";
        return 1;
    }

    return 0;
}
