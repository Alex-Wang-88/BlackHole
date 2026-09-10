cmake_minimum_required(VERSION 3.20)

project(BlackHoleProject LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glfw)

set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glm)

find_package(OpenGL REQUIRED)

add_executable(
    BlackHole
    BlackHole.cpp
    Scene.cpp
    Engine.cpp
    GpuRayTracer.cpp
    OpenGLLoader.cpp)

target_include_directories(BlackHole PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
    BlackHole
    PRIVATE
    glfw
    glm::glm
    OpenGL::GL)

if(MSVC)
    target_compile_definitions(BlackHole PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(BlackHole PRIVATE /W4 /permissive-)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(BlackHole PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_custom_command(TARGET BlackHole POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/raytrace.comp
        $<TARGET_FILE_DIR:BlackHole>/raytrace.comp)
