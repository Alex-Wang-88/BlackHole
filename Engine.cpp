#include "Engine.hpp"

#include "Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
GLuint compileShader(GLenum type, const char* source, const char* label)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);

    if(!success)
    {
        char infoLog[1024];
        glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
        std::cerr << label << " compilation failed:\n" << infoLog << "\n";
    }

    return shader;
}

GLuint linkProgram(GLuint vertexShader, GLuint fragmentShader, const char* label)
{
    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);

    GLint success = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &success);

    if(!success)
    {
        char infoLog[1024];
        glGetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
        std::cerr << label << " link failed:\n" << infoLog << "\n";
    }

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    return program;
}

void configureRayTexture(GLuint& texture, int width, int height)
{
    const std::vector<unsigned char> clearPixels(
        static_cast<std::size_t>(width) *
            static_cast<std::size_t>(height) *
            4u,
        0);

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA8,
        width,
        height,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        clearPixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}
}

Engine::Engine(const RenderSettings& settings)
    : WIDTH(settings.windowWidth),
      HEIGHT(settings.windowHeight),
      RENDER_WIDTH(settings.renderWidth),
      RENDER_HEIGHT(settings.renderHeight)
{
    if(!glfwInit())
    {
        std::cerr << "Failed to initialize GLFW\n";
        std::exit(EXIT_FAILURE);
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    window = glfwCreateWindow(WIDTH, HEIGHT, "Black Hole", nullptr, nullptr);

    if(!window)
    {
        std::cerr << "Window failed to create\n";
        glfwTerminate();
        std::exit(EXIT_FAILURE);
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(settings.vsync ? 1 : 0);

    if(!blackhole::loadOpenGLFunctions())
    {
        std::cerr << "Failed to load the required OpenGL functions\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        std::exit(EXIT_FAILURE);
    }

    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    std::cout << "OpenGL GPU: " << (renderer != nullptr ? renderer : "unknown")
              << "\n";
    std::cout << "OpenGL version: " << (version != nullptr ? version : "unknown")
              << "\n";

    shaderProgram = createShaderProgram();
    gridShaderProgram = createGridShaderProgram();
    createPerspectiveGrid();

    const float quadVertices[] =
    {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 1.0f
    };

    glGenVertexArrays(1, &quadVAO);
    glGenBuffers(1, &quadVBO);
    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
        1,
        2,
        GL_FLOAT,
        GL_FALSE,
        4 * sizeof(float),
        reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    configureRayTexture(texture, RENDER_WIDTH, RENDER_HEIGHT);
    configureRayTexture(materialTextureId, RENDER_WIDTH, RENDER_HEIGHT);
}

GLuint Engine::createShaderProgram()
{
    const char* vertexShaderSource = R"(
        #version 410 core

        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec2 aTexCoord;

        out vec2 TexCoord;

        void main()
        {
            gl_Position = vec4(aPos, 0.0, 1.0);
            TexCoord = aTexCoord;
        }
    )";

    const char* fragmentShaderSource = R"(
        #version 410 core

        in vec2 TexCoord;
        out vec4 FragColor;

        uniform sampler2D screenTexture;
        uniform sampler2D materialTexture;

        float edgeLuma(vec4 color)
        {
            return dot(color.rgb, vec3(0.299, 0.587, 0.114)) + color.a * 0.25;
        }

        void main()
        {
            vec2 inverseSize = 1.0 / vec2(textureSize(screenTexture, 0));

            vec4 center = texture(screenTexture, TexCoord);
            vec4 northWest = texture(screenTexture, TexCoord + vec2(-1.0,  1.0) * inverseSize);
            vec4 northEast = texture(screenTexture, TexCoord + vec2( 1.0,  1.0) * inverseSize);
            vec4 southWest = texture(screenTexture, TexCoord + vec2(-1.0, -1.0) * inverseSize);
            vec4 southEast = texture(screenTexture, TexCoord + vec2( 1.0, -1.0) * inverseSize);

            vec4 materialPresence = texture(materialTexture, TexCoord);
            materialPresence = max(
                materialPresence,
                texture(materialTexture, TexCoord + vec2(-1.0,  1.0) * inverseSize));
            materialPresence = max(
                materialPresence,
                texture(materialTexture, TexCoord + vec2( 1.0,  1.0) * inverseSize));
            materialPresence = max(
                materialPresence,
                texture(materialTexture, TexCoord + vec2(-1.0, -1.0) * inverseSize));
            materialPresence = max(
                materialPresence,
                texture(materialTexture, TexCoord + vec2( 1.0, -1.0) * inverseSize));

            float lumaCenter = edgeLuma(center);
            float lumaNorthWest = edgeLuma(northWest);
            float lumaNorthEast = edgeLuma(northEast);
            float lumaSouthWest = edgeLuma(southWest);
            float lumaSouthEast = edgeLuma(southEast);
            float lumaMinimum = min(
                lumaCenter,
                min(min(lumaNorthWest, lumaNorthEast), min(lumaSouthWest, lumaSouthEast)));
            float lumaMaximum = max(
                lumaCenter,
                max(max(lumaNorthWest, lumaNorthEast), max(lumaSouthWest, lumaSouthEast)));

            if(lumaMaximum - lumaMinimum < 0.035)
            {
                FragColor = center;
                return;
            }

            vec2 direction;
            direction.x = -((lumaNorthWest + lumaNorthEast) - (lumaSouthWest + lumaSouthEast));
            direction.y =  ((lumaNorthWest + lumaSouthWest) - (lumaNorthEast + lumaSouthEast));

            float directionReduce = max(
                (lumaNorthWest + lumaNorthEast + lumaSouthWest + lumaSouthEast) * 0.0078125,
                0.0009765625);
            float inverseDirectionMinimum =
                1.0 / (min(abs(direction.x), abs(direction.y)) + directionReduce);
            direction =
                clamp(direction * inverseDirectionMinimum, vec2(-1.5), vec2(1.5)) * inverseSize;

            vec4 resultA = 0.5 * (
                texture(screenTexture, TexCoord + direction * (1.0 / 3.0 - 0.5)) +
                texture(screenTexture, TexCoord + direction * (2.0 / 3.0 - 0.5)));
            vec4 resultB = resultA * 0.5 + 0.25 * (
                texture(screenTexture, TexCoord + direction * -0.5) +
                texture(screenTexture, TexCoord + direction *  0.5));

            float lumaResultB = edgeLuma(resultB);
            vec4 filteredColor =
                (lumaResultB < lumaMinimum || lumaResultB > lumaMaximum) ? resultA : resultB;

            float diskStrength = 0.35 * smoothstep(0.01, 0.25, materialPresence.g);
            float objectStrength = 0.08 * smoothstep(0.01, 0.25, materialPresence.b);
            float filterStrength = max(diskStrength, objectStrength);

            FragColor = mix(center, filteredColor, filterStrength);
        }
    )";

    GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertexShaderSource, "Screen vertex shader");
    GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentShaderSource, "Screen fragment shader");
    return linkProgram(vertexShader, fragmentShader, "Screen shader");
}

GLuint Engine::createGridShaderProgram()
{
    const char* vertexShaderSource = R"(
        #version 410 core

        layout(location = 0) in vec3 aPosition;
        uniform mat4 uMVP;

        void main()
        {
            gl_Position = uMVP * vec4(aPosition, 1.0);
        }
    )";

    const char* fragmentShaderSource = R"(
        #version 410 core

        out vec4 FragColor;
        uniform vec4 uGridColor;

        void main()
        {
            FragColor = uGridColor;
        }
    )";

    GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertexShaderSource, "Grid vertex shader");
    GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentShaderSource, "Grid fragment shader");
    return linkProgram(vertexShader, fragmentShader, "Grid shader");
}

void Engine::createPerspectiveGrid()
{
    const int sideVertexCount = GRID_HALF_CELLS * 2 + 1;
    std::vector<glm::vec3> vertices;
    std::vector<GLuint> indices;

    vertices.reserve(static_cast<std::size_t>(sideVertexCount * sideVertexCount));

    for(int zIndex = 0; zIndex < sideVertexCount; ++zIndex)
    {
        float z = static_cast<float>(zIndex - GRID_HALF_CELLS) * GRID_STEP_RS;

        for(int xIndex = 0; xIndex < sideVertexCount; ++xIndex)
        {
            float x = static_cast<float>(xIndex - GRID_HALF_CELLS) * GRID_STEP_RS;
            float radius = std::sqrt(x * x + z * z);
            float normalizedRadius = radius / GRID_WELL_RADIUS_RS;
            float y = -GRID_WELL_DEPTH_RS / (1.0f + normalizedRadius * normalizedRadius);
            vertices.emplace_back(x, y, z);
        }
    }

    auto vertexIndex = [](int zIndex, int xIndex)
    {
        return static_cast<GLuint>(zIndex * sideVertexCount + xIndex);
    };

    indices.reserve(static_cast<std::size_t>(sideVertexCount * (sideVertexCount - 1) * 4));

    for(int zIndex = 0; zIndex < sideVertexCount; ++zIndex)
    {
        for(int xIndex = 0; xIndex < sideVertexCount - 1; ++xIndex)
        {
            indices.push_back(vertexIndex(zIndex, xIndex));
            indices.push_back(vertexIndex(zIndex, xIndex + 1));
        }
    }

    for(int xIndex = 0; xIndex < sideVertexCount; ++xIndex)
    {
        for(int zIndex = 0; zIndex < sideVertexCount - 1; ++zIndex)
        {
            indices.push_back(vertexIndex(zIndex, xIndex));
            indices.push_back(vertexIndex(zIndex + 1, xIndex));
        }
    }

    gridIndexCount = static_cast<GLsizei>(indices.size());

    glGenVertexArrays(1, &gridVAO);
    glGenBuffers(1, &gridVBO);
    glGenBuffers(1, &gridEBO);
    glBindVertexArray(gridVAO);
    glBindBuffer(GL_ARRAY_BUFFER, gridVBO);
    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<std::ptrdiff_t>(vertices.size() * sizeof(glm::vec3)),
        vertices.data(),
        GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gridEBO);
    glBufferData(
        GL_ELEMENT_ARRAY_BUFFER,
        static_cast<std::ptrdiff_t>(indices.size() * sizeof(GLuint)),
        indices.data(),
        GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Engine::drawPerspectiveGrid(double schwarzschildRadius, float aspect)
{
    if(schwarzschildRadius <= 0.0 || gridIndexCount == 0) return;

    float inverseRadius = static_cast<float>(1.0 / schwarzschildRadius);
    glm::vec3 eye = camera.pos * inverseRadius;
    glm::vec3 target = camera.target * inverseRadius;

    glm::mat4 projection =
        glm::perspective(glm::radians(camera.fovY), aspect, 0.05f, 200.0f);
    glm::mat4 view = glm::lookAt(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 mvp = projection * view;

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(gridShaderProgram);
    glUniformMatrix4fv(
        glGetUniformLocation(gridShaderProgram, "uMVP"),
        1,
        GL_FALSE,
        glm::value_ptr(mvp));
    glUniform4f(
        glGetUniformLocation(gridShaderProgram, "uGridColor"),
        0.86f,
        0.53f,
        0.38f,
        0.78f);
    glBindVertexArray(gridVAO);
    glLineWidth(1.0f);
    glDrawElements(GL_LINES, gridIndexCount, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

void Engine::renderScene(
    double schwarzschildRadius)
{
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
    glViewport(0, 0, framebufferWidth, framebufferHeight);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if(framebufferWidth <= 0 || framebufferHeight <= 0) return;

    float aspect = static_cast<float>(framebufferWidth) /
                   static_cast<float>(framebufferHeight);
    drawPerspectiveGrid(schwarzschildRadius, aspect);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(shaderProgram);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(glGetUniformLocation(shaderProgram, "screenTexture"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, materialTextureId);
    glUniform1i(glGetUniformLocation(shaderProgram, "materialTexture"), 1);

    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);

    glfwSwapBuffers(window);
    glfwPollEvents();
}

Engine::~Engine()
{
    if(materialTextureId) glDeleteTextures(1, &materialTextureId);
    if(texture) glDeleteTextures(1, &texture);
    if(quadVBO) glDeleteBuffers(1, &quadVBO);
    if(quadVAO) glDeleteVertexArrays(1, &quadVAO);
    if(gridEBO) glDeleteBuffers(1, &gridEBO);
    if(gridVBO) glDeleteBuffers(1, &gridVBO);
    if(gridVAO) glDeleteVertexArrays(1, &gridVAO);
    if(gridShaderProgram) glDeleteProgram(gridShaderProgram);
    if(shaderProgram) glDeleteProgram(shaderProgram);
    if(window) glfwDestroyWindow(window);
    glfwTerminate();
}
