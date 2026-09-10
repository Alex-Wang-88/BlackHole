#include "D3D12Engine.hpp"
#include "Scene.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

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

template<typename Clock>
void paceUntil(typename Clock::time_point deadline)
{
    while(true)
    {
        const auto now = Clock::now();
        if(now >= deadline) return;

        const auto remaining = deadline - now;
        if(remaining > std::chrono::milliseconds(3))
        {
            // Short sleeps avoid crossing the next scheduler quantum on
            // Windows, then the final few milliseconds are handled by yield.
            // This keeps the 60 FPS cap from falling to an accidental 30 FPS
            // cadence on systems with coarse sleep granularity.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
        D3D12Engine engine(settings, resolveShaderDirectory(
            argc > 0 ? argv[0] : nullptr));

        std::cout << "Window: " << engine.WIDTH << " x " << engine.HEIGHT
                  << "\n";
        std::cout << "Renderer: Direct3D 12\n";
        std::cout << "Ray tracing: " << (settings.rayTracing ? "on" : "off")
                  << "\n";
        std::cout << "VSync: " << (settings.vsync ? "on" : "off") << "\n";
        std::cout << "Target FPS: " << (settings.targetFps == 0
                                             ? "unlimited"
                                             : std::to_string(settings.targetFps))
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

        while(!engine.shouldClose())
        {
            engine.processMessages();
            if(engine.shouldClose()) break;

            const auto frameStart = Clock::now();
            engine.render(SagA.r_s, settings.rayTracing);

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
