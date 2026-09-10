#include "Engine.hpp"
#include "GpuRayTracer.hpp"
#include "Scene.hpp"

#include <GLFW/glfw3.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

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
}

int main(int argc, char** argv)
{
    try
    {
        const RenderSettings settings = loadRenderSettings();
        Engine engine(settings);
        GpuRayTracer gpuRayTracer(
            engine.colorTexture(),
            engine.materialTexture(),
            engine.RENDER_WIDTH,
            engine.RENDER_HEIGHT,
            settings.maxSteps,
            settings.temporalSampleLimit,
            resolveShaderPath(argc > 0 ? argv[0] : nullptr));
        setupCameraCallbacks(engine.window);

        std::cout << "Window: " << engine.WIDTH << " x " << engine.HEIGHT
                  << "\n";
        std::cout << "VSync: " << (settings.vsync ? "on" : "off") << "\n";

        using Clock = std::chrono::steady_clock;
        int frameCount = 0;
        double lastPrintTime = std::chrono::duration<double>(
            Clock::now().time_since_epoch()).count();

        while(!glfwWindowShouldClose(engine.window))
        {
            gpuRayTracer.render(engine.WIDTH, engine.HEIGHT);
            engine.renderScene(SagA.r_s);

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
