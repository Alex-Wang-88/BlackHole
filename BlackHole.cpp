#include "D3D12Engine.hpp"
#include "Scene.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

#ifdef _WIN32
// Ask Windows hybrid-GPU systems to prefer the discrete adapter for this
// application. The exports are ignored on systems with only one GPU.
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

std::filesystem::path resolveShaderDirectory(const char* executablePath)
{
    namespace fs = std::filesystem;

    const fs::path executable =
        executablePath != nullptr ? fs::path(executablePath) : fs::path();
    const fs::path executableDirectory = executable.parent_path();
    if(fs::exists(executableDirectory / "raytrace.hlsl"))
        return executableDirectory;

    const fs::path currentDirectory = fs::current_path() / "shaders";
    if(fs::exists(currentDirectory / "raytrace.hlsl"))
        return currentDirectory;

    return executableDirectory;
}

}

int main(int argc, char** argv)
{
    try
    {
        const RenderSettings settings = loadRenderSettings();
        D3D12Engine engine(settings, resolveShaderDirectory(
            argc > 0 ? argv[0] : nullptr));

        std::cout << "Window: " << engine.WIDTH << " x " << engine.HEIGHT
                  << "\n";
        std::cout << "Renderer: Direct3D 12\n";
        std::cout << "Ray tracing: " << (settings.rayTracing ? "on" : "off")
                  << "\n";
        std::cout << "DLSS: " << (engine.dlssEnabled()
                                       ? "on (Quality / MaxQuality, preset K)"
                                       : "off (native compositor fallback)")
                  << "\n";
        std::cout << "VSync: " << (settings.vsync ? "on" : "off") << "\n";
        std::cout << "Frame pacing: unlimited (use the quality panel for VSync)\n";

        using Clock = std::chrono::steady_clock;
#ifdef _WIN32
        TimerResolutionGuard timerResolution;
#endif
        int frameCount = 0;
        double lastPrintTime = std::chrono::duration<double>(
            Clock::now().time_since_epoch()).count();

        while(!engine.shouldClose())
        {
            engine.processMessages();
            if(engine.shouldClose()) break;

            engine.render(SagA.r_s, engine.rayTracingEnabled());

            ++frameCount;
            const double currentTime = std::chrono::duration<double>(
                Clock::now().time_since_epoch()).count();
            const double elapsed = currentTime - lastPrintTime;

            if(elapsed >= 1.0)
            {
                const double fps = static_cast<double>(frameCount) / elapsed;
                engine.updateFps(fps);
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
