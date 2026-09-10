#include "GpuRayTracer.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace
{
GLuint compileComputeShader(const std::string& source)
{
    const char* sourceText = source.c_str();
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(shader, 1, &sourceText, nullptr);
    glCompileShader(shader);

    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if(success == GL_FALSE)
    {
        GLint logLength = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
        glGetShaderInfoLog(shader, logLength, nullptr, log.data());
        glDeleteShader(shader);
        throw std::runtime_error(
            "Ray-tracing compute shader compilation failed:\n" + log);
    }

    return shader;
}

GLuint createComputeProgram(const std::string& source)
{
    GLuint shader = compileComputeShader(source);
    GLuint program = glCreateProgram();
    glAttachShader(program, shader);
    glLinkProgram(program);
    glDeleteShader(shader);

    GLint success = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if(success == GL_FALSE)
    {
        GLint logLength = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
        glGetProgramInfoLog(program, logLength, nullptr, log.data());
        glDeleteProgram(program);
        throw std::runtime_error(
            "Ray-tracing compute program link failed:\n" + log);
    }

    return program;
}

std::string readTextFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if(!file)
        throw std::runtime_error("Unable to open compute shader: " + path);

    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

GLint uniformLocation(GLuint program, const char* name)
{
    const GLint location = glGetUniformLocation(program, name);
    if(location < 0)
        throw std::runtime_error(std::string("Missing compute shader uniform: ") + name);
    return location;
}
}

GpuRayTracer::GpuRayTracer(
    GLuint outputTextureValue,
    GLuint materialTextureValue,
    int renderWidthValue,
    int renderHeightValue,
    std::uint32_t maxStepsValue,
    std::uint32_t temporalSampleLimitValue,
    const std::string& shaderPath)
    : outputTexture(outputTextureValue),
      materialTexture(materialTextureValue),
      renderWidth(renderWidthValue),
      renderHeight(renderHeightValue),
      maxSteps(maxStepsValue),
      temporalSampleLimit(temporalSampleLimitValue)
{
    if(renderWidth <= 0 || renderHeight <= 0)
        throw std::runtime_error("Ray-tracing resolution must be positive");
    if(outputTexture == 0 || materialTexture == 0)
        throw std::runtime_error("Ray-tracing output textures are invalid");
    if(blackhole::dispatchCompute == nullptr)
        throw std::runtime_error(
            "OpenGL 4.3 compute shaders are required; the current GPU/context "
            "does not expose OpenGL 4.3");

    computeProgram = createComputeProgram(readTextFile(shaderPath));
    cacheUniformLocations();
    createStorageBuffers();

    std::cout << "GPU raytrace resolution: " << renderWidth << " x "
              << renderHeight << "\n";
    std::cout << "GPU MAX_STEPS: " << maxSteps << "\n";
    std::cout << "Accretion disk: " << DISK_R1_RS << " r_s -> "
              << DISK_R2_RS << " r_s\n";
    if(temporalSampleLimit == 0)
        std::cout << "Temporal anti-aliasing: unlimited accumulation\n";
    else
        std::cout << "Temporal anti-aliasing: " << temporalSampleLimit
                  << " samples\n";
}

GpuRayTracer::~GpuRayTracer()
{
    if(objectBuffer) glDeleteBuffers(1, &objectBuffer);
    if(materialAccumulationBuffer)
        glDeleteBuffers(1, &materialAccumulationBuffer);
    if(accumulationBuffer) glDeleteBuffers(1, &accumulationBuffer);
    if(computeProgram) glDeleteProgram(computeProgram);
}

void GpuRayTracer::createStorageBuffers()
{
    const std::size_t pixelCount =
        static_cast<std::size_t>(renderWidth) *
        static_cast<std::size_t>(renderHeight);
    const std::ptrdiff_t accumulationBytes = static_cast<std::ptrdiff_t>(
        pixelCount * sizeof(float) * 4u);

    glGenBuffers(1, &accumulationBuffer);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, accumulationBuffer);
    glBufferData(
        GL_SHADER_STORAGE_BUFFER,
        accumulationBytes,
        nullptr,
        GL_DYNAMIC_DRAW);

    glGenBuffers(1, &materialAccumulationBuffer);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, materialAccumulationBuffer);
    glBufferData(
        GL_SHADER_STORAGE_BUFFER,
        accumulationBytes,
        nullptr,
        GL_DYNAMIC_DRAW);

    // The scene currently contains one object. The buffer is resized from the
    // vector on each render, so adding more objects does not require a shader
    // or allocation change here.
    glGenBuffers(1, &objectBuffer);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, objectBuffer);
    glBufferData(
        GL_SHADER_STORAGE_BUFFER,
        static_cast<std::ptrdiff_t>(sizeof(Object)),
        nullptr,
        GL_STREAM_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void GpuRayTracer::cacheUniformLocations()
{
    cameraPosLocation = uniformLocation(computeProgram, "uCameraPos");
    targetLocation = uniformLocation(computeProgram, "uTarget");
    fovYRadiansLocation = uniformLocation(computeProgram, "uFovYRadians");
    aspectLocation = uniformLocation(computeProgram, "uAspect");
    renderWidthLocation = uniformLocation(computeProgram, "uRenderWidth");
    renderHeightLocation = uniformLocation(computeProgram, "uRenderHeight");
    maxStepsLocation = uniformLocation(computeProgram, "uMaxSteps");
    dLambdaLocation = uniformLocation(computeProgram, "uDLambda");
    escapeRLocation = uniformLocation(computeProgram, "uEscapeR");
    horizonRLocation = uniformLocation(computeProgram, "uHorizonR");
    diskR1Location = uniformLocation(computeProgram, "uDiskR1");
    diskR2Location = uniformLocation(computeProgram, "uDiskR2");
    objectCountLocation = uniformLocation(computeProgram, "uObjectCount");
    sampleIndexLocation = uniformLocation(computeProgram, "uSampleIndex");
    jitterLocation = uniformLocation(computeProgram, "uJitter");
}

void GpuRayTracer::render(int displayWidth, int displayHeight)
{
    if(displayWidth <= 0 || displayHeight <= 0) return;

    const bool cameraChanged =
        !hasPreviousCamera ||
        glm::length(camera.pos - previousCameraPos) > 1.0e4f ||
        glm::length(camera.target - previousCameraTarget) > 1.0e4f ||
        std::fabs(camera.fovY - previousCameraFovY) > 1.0e-5f;

    if(cameraChanged)
    {
        accumulatedSampleCount = 0;
        previousCameraPos = camera.pos;
        previousCameraTarget = camera.target;
        previousCameraFovY = camera.fovY;
        hasPreviousCamera = true;
    }

    if(temporalSampleLimit != 0 &&
       accumulatedSampleCount >= temporalSampleLimit)
        return;

    static constexpr std::array<std::array<float, 2>, 8> jitterOffsets =
    {{
        {{ 0.000f,  0.000f}},
        {{-0.250f, -0.250f}},
        {{ 0.250f,  0.250f}},
        {{ 0.250f, -0.250f}},
        {{-0.250f,  0.250f}},
        {{-0.375f,  0.000f}},
        {{ 0.375f,  0.000f}},
        {{ 0.000f,  0.000f}}
    }};

    const std::size_t jitterIndex = accumulatedSampleCount % jitterOffsets.size();
    const double inverseSchwarzschildRadius = 1.0 / SagA.r_s;
    const float cameraScale = static_cast<float>(inverseSchwarzschildRadius);
    const float dLambda = static_cast<float>(D_LAMBDA_METERS * inverseSchwarzschildRadius);
    const float escapeR = static_cast<float>(ESCAPE_R_METERS * inverseSchwarzschildRadius);

    std::vector<Object> normalizedObjects = objects;
    for(Object& object : normalizedObjects)
    {
        object.posRadius.x = static_cast<float>(
            object.posRadius.x * inverseSchwarzschildRadius);
        object.posRadius.y = static_cast<float>(
            object.posRadius.y * inverseSchwarzschildRadius);
        object.posRadius.z = static_cast<float>(
            object.posRadius.z * inverseSchwarzschildRadius);
        object.posRadius.w = static_cast<float>(
            object.posRadius.w * inverseSchwarzschildRadius);
    }

    glUseProgram(computeProgram);
    glUniform3f(
        cameraPosLocation,
        camera.pos.x * cameraScale,
        camera.pos.y * cameraScale,
        camera.pos.z * cameraScale);
    glUniform3f(
        targetLocation,
        camera.target.x * cameraScale,
        camera.target.y * cameraScale,
        camera.target.z * cameraScale);
    glUniform1f(fovYRadiansLocation, glm::radians(camera.fovY));
    glUniform1f(
        aspectLocation,
        static_cast<float>(displayWidth) / static_cast<float>(displayHeight));
    glUniform1ui(renderWidthLocation, static_cast<GLuint>(renderWidth));
    glUniform1ui(renderHeightLocation, static_cast<GLuint>(renderHeight));
    glUniform1ui(maxStepsLocation, maxSteps);
    glUniform1f(dLambdaLocation, dLambda);
    glUniform1f(escapeRLocation, escapeR);
    glUniform1f(horizonRLocation, 1.0f);
    glUniform1f(diskR1Location, DISK_R1_RS);
    glUniform1f(diskR2Location, DISK_R2_RS);
    glUniform1ui(
        objectCountLocation,
        static_cast<GLuint>(normalizedObjects.size()));
    glUniform1ui(sampleIndexLocation, accumulatedSampleCount);
    glUniform2f(
        jitterLocation,
        jitterOffsets[jitterIndex][0],
        jitterOffsets[jitterIndex][1]);

    glBindBufferBase(
        GL_SHADER_STORAGE_BUFFER,
        0,
        accumulationBuffer);
    glBindBufferBase(
        GL_SHADER_STORAGE_BUFFER,
        1,
        materialAccumulationBuffer);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, objectBuffer);
    const std::size_t objectBytes = std::max<std::size_t>(
        normalizedObjects.size(), 1u) * sizeof(Object);
    glBufferData(
        GL_SHADER_STORAGE_BUFFER,
        static_cast<std::ptrdiff_t>(objectBytes),
        normalizedObjects.empty() ? nullptr : normalizedObjects.data(),
        GL_STREAM_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, objectBuffer);

    glBindImageTexture(
        0,
        outputTexture,
        0,
        GL_FALSE,
        0,
        GL_WRITE_ONLY,
        GL_RGBA8);
    glBindImageTexture(
        1,
        materialTexture,
        0,
        GL_FALSE,
        0,
        GL_WRITE_ONLY,
        GL_RGBA8);

    const GLuint groupsX = static_cast<GLuint>((renderWidth + 7) / 8);
    const GLuint groupsY = static_cast<GLuint>((renderHeight + 7) / 8);
    glDispatchCompute(groupsX, groupsY, 1);

    glMemoryBarrier(
        GL_SHADER_STORAGE_BARRIER_BIT |
        GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
        GL_TEXTURE_FETCH_BARRIER_BIT);

    if(accumulatedSampleCount < std::numeric_limits<std::uint32_t>::max() - 1u)
        ++accumulatedSampleCount;
}
