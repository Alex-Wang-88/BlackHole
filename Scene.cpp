#include "Scene.hpp"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
constexpr double GRAVITATIONAL_CONSTANT = 6.67430e-11;
constexpr double SPEED_OF_LIGHT = 299792458.0;
constexpr float PI = 3.14159265358979323846f;

int readIntEnvironment(
    const char* name,
    int fallback,
    int minimum,
    int maximum)
{
    const char* value = std::getenv(name);
    if(value == nullptr || *value == '\0') return fallback;

    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if(end == value || *end != '\0' ||
       parsed < static_cast<long>(minimum) ||
       parsed > static_cast<long>(maximum))
    {
        std::cerr << "Ignoring invalid " << name << "='" << value
                  << "' (expected " << minimum << ".." << maximum << ")\n";
        return fallback;
    }

    return static_cast<int>(parsed);
}

std::uint32_t readUnsignedEnvironment(
    const char* name,
    std::uint32_t fallback,
    std::uint32_t minimum,
    std::uint32_t maximum)
{
    const char* value = std::getenv(name);
    if(value == nullptr || *value == '\0') return fallback;

    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if(end == value || *end != '\0' ||
       parsed < static_cast<unsigned long long>(minimum) ||
       parsed > static_cast<unsigned long long>(maximum))
    {
        std::cerr << "Ignoring invalid " << name << "='" << value
                  << "' (expected " << minimum << ".." << maximum << ")\n";
        return fallback;
    }

    return static_cast<std::uint32_t>(parsed);
}

bool readBooleanEnvironment(const char* name, bool fallback)
{
    const char* value = std::getenv(name);
    if(value == nullptr || *value == '\0') return fallback;

    if(std::string(value) == "1" ||
       std::string(value) == "true" ||
       std::string(value) == "TRUE" ||
       std::string(value) == "on" ||
       std::string(value) == "ON")
        return true;

    if(std::string(value) == "0" ||
       std::string(value) == "false" ||
       std::string(value) == "FALSE" ||
       std::string(value) == "off" ||
       std::string(value) == "OFF")
        return false;

    std::cerr << "Ignoring invalid " << name << "='" << value
              << "' (expected 0/1)\n";
    return fallback;
}

float readScaleEnvironment()
{
    const char* value = std::getenv("BLACKHOLE_RENDER_SCALE");
    if(value == nullptr || *value == '\0') return DEFAULT_RENDER_SCALE;

    char* end = nullptr;
    const float parsed = std::strtof(value, &end);
    if(end == value || *end != '\0' || !std::isfinite(parsed) ||
       parsed < 0.25f || parsed > 4.0f)
    {
        std::cerr << "Ignoring invalid BLACKHOLE_RENDER_SCALE='" << value
                  << "' (expected 0.25..4.0)\n";
        return DEFAULT_RENDER_SCALE;
    }

    return parsed;
}

DlssQualityMode readDlssModeEnvironment()
{
    const char* value = std::getenv("BLACKHOLE_DLSS_MODE");
    if(value == nullptr || *value == '\0')
        return DlssQualityMode::Quality;

    const std::string mode(value);
    if(mode == "quality" || mode == "Quality" || mode == "maxquality")
        return DlssQualityMode::Quality;
    if(mode == "balanced" || mode == "Balanced")
        return DlssQualityMode::Balanced;
    if(mode == "performance" || mode == "Performance")
        return DlssQualityMode::Performance;
    if(mode == "ultraperformance" || mode == "UltraPerformance")
        return DlssQualityMode::UltraPerformance;

    std::cerr << "Ignoring invalid BLACKHOLE_DLSS_MODE='" << value
              << "' (expected quality/balanced/performance/ultraperformance)\n";
    return DlssQualityMode::Quality;
}
}

RenderSettings loadRenderSettings()
{
    RenderSettings settings;

    settings.windowWidth = readIntEnvironment(
        "BLACKHOLE_WINDOW_WIDTH",
        DEFAULT_WINDOW_WIDTH,
        320,
        8192);
    settings.windowHeight = readIntEnvironment(
        "BLACKHOLE_WINDOW_HEIGHT",
        DEFAULT_WINDOW_HEIGHT,
        240,
        8192);

    const float scale = readScaleEnvironment();
    settings.renderScale = scale;
    const int scaledWidth = static_cast<int>(
        std::lround(static_cast<float>(settings.windowWidth) * scale));
    const int scaledHeight = static_cast<int>(
        std::lround(static_cast<float>(settings.windowHeight) * scale));

    settings.renderWidth = readIntEnvironment(
        "BLACKHOLE_RENDER_WIDTH",
        std::clamp(scaledWidth, 64, 8192),
        64,
        8192);
    settings.renderHeight = readIntEnvironment(
        "BLACKHOLE_RENDER_HEIGHT",
        std::clamp(scaledHeight, 64, 8192),
        64,
        8192);
    settings.maxSteps = readUnsignedEnvironment(
        "BLACKHOLE_MAX_STEPS",
        DEFAULT_MAX_STEPS,
        64,
        1000000);
    settings.temporalSampleLimit = readUnsignedEnvironment(
        "BLACKHOLE_TAA_SAMPLES",
        DEFAULT_TEMPORAL_SAMPLE_LIMIT,
        0,
        1000000);
    settings.rayTracing = readBooleanEnvironment(
        "BLACKHOLE_RAYTRACE",
        DEFAULT_RAYTRACING);
    settings.dlss = readBooleanEnvironment(
        "BLACKHOLE_DLSS",
        DEFAULT_DLSS);
    settings.dlssMode = readDlssModeEnvironment();
    settings.vsync = readBooleanEnvironment("BLACKHOLE_VSYNC", false);

    return settings;
}

Camera::Camera()
    : fovY(75.0f),
      azimuth(PI / 2.0f),
      elevation(PI / 3.0f),
      radius(4.5e11f)
{
    target = glm::vec3(8.0e10f, 0.0f, 0.0f);
    updateVectors();
}

void Camera::updateVectors()
{
    pos.x = target.x + radius * std::sin(elevation) * std::cos(azimuth);
    pos.y = target.y + radius * std::cos(elevation);
    pos.z = target.z + radius * std::sin(elevation) * std::sin(azimuth);
}

void Camera::processMouse(double xpos, double ypos)
{
    float dx = static_cast<float>(xpos - lastX);
    float dy = static_cast<float>(ypos - lastY);

    if(dragging && !panning)
    {
        azimuth -= dx * orbitSpeed;
        elevation -= dy * orbitSpeed;
        elevation = glm::clamp(elevation, 0.01f, PI - 0.01f);
    }
    else if(panning)
    {
        glm::vec3 forward = glm::normalize(target - pos);
        glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
        glm::vec3 up = glm::normalize(glm::cross(right, forward));
        target += (-dx * right + dy * up) * radius * panSpeed;
    }

    lastX = xpos;
    lastY = ypos;
    updateVectors();
}

BlackHole::BlackHole(glm::vec3 pos, double m)
    : position(pos), mass(m)
{
    r_s = (2.0 * GRAVITATIONAL_CONSTANT * mass) /
          (SPEED_OF_LIGHT * SPEED_OF_LIGHT);
}

bool BlackHole::Intercept(double px, double py, double pz) const
{
    double dx = px - position.x;
    double dy = py - position.y;
    double dz = pz - position.z;
    double distanceSquared = dx * dx + dy * dy + dz * dz;
    return distanceSquared < r_s * r_s;
}

Camera camera;
BlackHole SagA(glm::vec3(0.0f), 8.54e36);

std::vector<Object> objects =
{
    {
        glm::vec4(4.0e11f, 0.0f, 0.0f, 4.0e10f),
        glm::vec4(1.0f, 1.0f, 0.0f, 1.0f),
        0.0f,
        {0.0f, 0.0f, 0.0f},
        glm::vec3(0.0f),
        0.0f
    }
};

bool handleCameraMessage(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    switch(message)
    {
    case WM_LBUTTONDOWN:
        camera.dragging = true;
        camera.panning = (wParam & MK_SHIFT) != 0;
        camera.lastX = GET_X_LPARAM(lParam);
        camera.lastY = GET_Y_LPARAM(lParam);
        SetCapture(window);
        return true;

    case WM_MOUSEMOVE:
        if(camera.dragging)
        {
            camera.processMouse(
                static_cast<double>(GET_X_LPARAM(lParam)),
                static_cast<double>(GET_Y_LPARAM(lParam)));
            return true;
        }
        break;

    case WM_LBUTTONUP:
        camera.dragging = false;
        camera.panning = false;
        if(GetCapture() == window)
            ReleaseCapture();
        return true;

    case WM_MOUSEWHEEL:
    {
        const short wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        if(wheelDelta > 0)
            camera.radius /= camera.zoomSpeed;
        else if(wheelDelta < 0)
            camera.radius *= camera.zoomSpeed;

        camera.radius = glm::clamp(camera.radius, camera.minRadius, camera.maxRadius);
        camera.updateVectors();
        return true;
    }

    default:
        break;
    }

    return false;
}
