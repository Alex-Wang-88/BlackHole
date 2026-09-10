cmake_minimum_required(VERSION 3.20)

project(BlackHoleProject LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The Direct3D 12 renderer requires Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glm)

add_executable(
    BlackHole
    BlackHole.cpp
    Scene.cpp
    D3D12Engine.cpp)

target_include_directories(BlackHole PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(
    BlackHole
    PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE)
target_link_libraries(
    BlackHole
    PRIVATE
    glm::glm
    d3d12
    dxgi
    d3dcompiler
    user32
    gdi32
    winmm)

if(MSVC)
    target_compile_definitions(BlackHole PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(BlackHole PRIVATE /W4 /permissive-)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(BlackHole PRIVATE -Wall -Wextra -Wpedantic)
endif()

foreach(shader IN ITEMS raytrace.hlsl scene.hlsl composite.hlsl)
    add_custom_command(TARGET BlackHole POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}
            $<TARGET_FILE_DIR:BlackHole>/${shader})
endforeach()
