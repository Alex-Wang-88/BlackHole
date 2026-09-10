#ifndef BLACK_HOLE_GPU_RAY_TRACER_HPP
#define BLACK_HOLE_GPU_RAY_TRACER_HPP

#include "OpenGLLoader.hpp"
#include "Scene.hpp"

#include <cstdint>
#include <string>

class GpuRayTracer
{
public:
    GpuRayTracer(
        GLuint outputTexture,
        GLuint materialTexture,
        int renderWidth,
        int renderHeight,
        std::uint32_t maxSteps,
        std::uint32_t temporalSampleLimit,
        const std::string& shaderPath);
    ~GpuRayTracer();

    GpuRayTracer(const GpuRayTracer&) = delete;
    GpuRayTracer& operator=(const GpuRayTracer&) = delete;

    void render(int displayWidth, int displayHeight);

private:
    GLuint computeProgram = 0;
    GLuint accumulationBuffer = 0;
    GLuint materialAccumulationBuffer = 0;
    GLuint objectBuffer = 0;
    GLuint outputTexture = 0;
    GLuint materialTexture = 0;

    int renderWidth = 0;
    int renderHeight = 0;
    std::uint32_t maxSteps = 0;
    std::uint32_t temporalSampleLimit = 0;
    std::uint32_t accumulatedSampleCount = 0;

    bool hasPreviousCamera = false;
    glm::vec3 previousCameraPos = glm::vec3(0.0f);
    glm::vec3 previousCameraTarget = glm::vec3(0.0f);
    float previousCameraFovY = 0.0f;

    GLint cameraPosLocation = -1;
    GLint targetLocation = -1;
    GLint fovYRadiansLocation = -1;
    GLint aspectLocation = -1;
    GLint renderWidthLocation = -1;
    GLint renderHeightLocation = -1;
    GLint maxStepsLocation = -1;
    GLint dLambdaLocation = -1;
    GLint escapeRLocation = -1;
    GLint horizonRLocation = -1;
    GLint diskR1Location = -1;
    GLint diskR2Location = -1;
    GLint objectCountLocation = -1;
    GLint sampleIndexLocation = -1;
    GLint jitterLocation = -1;

    void createStorageBuffers();
    void cacheUniformLocations();
};

#endif
