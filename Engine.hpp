#ifndef BLACK_HOLE_ENGINE_HPP
#define BLACK_HOLE_ENGINE_HPP

#include "OpenGLLoader.hpp"
#include "Scene.hpp"

class Engine
{
public:
    explicit Engine(const RenderSettings& settings);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void renderScene(double schwarzschildRadius);

    GLuint colorTexture() const { return texture; }
    GLuint materialTexture() const { return materialTextureId; }

    GLFWwindow* window = nullptr;
    int WIDTH = DEFAULT_WINDOW_WIDTH;
    int HEIGHT = DEFAULT_WINDOW_HEIGHT;
    int RENDER_WIDTH = DEFAULT_RENDER_WIDTH;
    int RENDER_HEIGHT = DEFAULT_RENDER_HEIGHT;

private:
    GLuint quadVAO = 0;
    GLuint quadVBO = 0;
    GLuint texture = 0;
    GLuint materialTextureId = 0;
    GLuint shaderProgram = 0;

    GLuint gridVAO = 0;
    GLuint gridVBO = 0;
    GLuint gridEBO = 0;
    GLuint gridShaderProgram = 0;
    GLsizei gridIndexCount = 0;

    GLuint createShaderProgram();
    GLuint createGridShaderProgram();
    void createPerspectiveGrid();
    void drawPerspectiveGrid(double schwarzschildRadius, float aspect);
};

#endif
