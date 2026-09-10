#ifndef BLACK_HOLE_SCENE_HPP
#define BLACK_HOLE_SCENE_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

inline constexpr int DEFAULT_WINDOW_WIDTH = 800;
inline constexpr int DEFAULT_WINDOW_HEIGHT = 600;
inline constexpr float DEFAULT_RENDER_SCALE = 0.32f;
inline constexpr int DEFAULT_RENDER_WIDTH = 256;
inline constexpr int DEFAULT_RENDER_HEIGHT = 192;
inline constexpr std::uint32_t DEFAULT_MAX_STEPS = 6144;
inline constexpr std::uint32_t DEFAULT_TEMPORAL_SAMPLE_LIMIT = 1;
inline constexpr int DEFAULT_TARGET_FPS = 60;
inline constexpr bool DEFAULT_RAYTRACING = true;
// A larger integration step lets the reduced-step Windows preset traverse
// the camera-to-disk distance without running thousands of empty iterations.
inline constexpr double D_LAMBDA_METERS = 1.0e8;
inline constexpr double ESCAPE_R_METERS = 8.0e11;
inline constexpr float DISK_R1_RS = 3.0f;
inline constexpr float DISK_R2_RS = 4.5f;

struct RenderSettings
{
    int windowWidth = DEFAULT_WINDOW_WIDTH;
    int windowHeight = DEFAULT_WINDOW_HEIGHT;
    int renderWidth = DEFAULT_RENDER_WIDTH;
    int renderHeight = DEFAULT_RENDER_HEIGHT;
    std::uint32_t maxSteps = DEFAULT_MAX_STEPS;

    // Zero means keep accumulating instead of stopping after a fixed number
    // of temporal samples.
    std::uint32_t temporalSampleLimit = DEFAULT_TEMPORAL_SAMPLE_LIMIT;
    int targetFps = DEFAULT_TARGET_FPS;
    bool rayTracing = DEFAULT_RAYTRACING;
    bool vsync = false;
};

RenderSettings loadRenderSettings();

inline constexpr int GRID_HALF_CELLS = 32;
inline constexpr float GRID_STEP_RS = 1.5f;
inline constexpr float GRID_WELL_DEPTH_RS = 9.0f;
inline constexpr float GRID_WELL_RADIUS_RS = 10.0f;

struct Camera
{
    glm::vec3 pos;
    glm::vec3 target;
    float fovY;
    float azimuth;
    float elevation;
    float radius;

    float minRadius = 1.0e11f;
    float maxRadius = 1.2e12f;

    bool dragging = false;
    bool panning = false;
    double lastX = 0.0;
    double lastY = 0.0;

    float orbitSpeed = 0.008f;
    float panSpeed = 0.001f;
    float zoomSpeed = 1.08f;

    Camera();

    void updateVectors();
    void processMouse(double xpos, double ypos);
};

struct BlackHole
{
    glm::vec3 position;
    double mass;
    double r_s;

    BlackHole(glm::vec3 pos, double m);

    bool Intercept(double px, double py, double pz) const;
};

struct alignas(16) Object
{
    glm::vec4 posRadius;
    glm::vec4 color;
    float mass;
    float alignmentPadding[3];
    glm::vec3 velocity;
    float velocityPadding;
};

static_assert(sizeof(Object) == 64, "Object must match the GPU buffer layout");

extern Camera camera;
extern BlackHole SagA;
extern std::vector<Object> objects;

bool handleCameraMessage(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam);

#endif
