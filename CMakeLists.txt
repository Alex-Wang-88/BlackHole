cmake_minimum_required(VERSION 3.20)

project(BlackHoleProject LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The Direct3D 12 renderer requires Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BLACKHOLE_ENABLE_DLSS
    "Enable the optional NVIDIA Streamline DLSS integration"
    ON)
set(BLACKHOLE_STREAMLINE_SDK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/streamline"
    CACHE PATH
    "Streamline SDK root containing include/ and (optionally) bin/")
set(BLACKHOLE_STREAMLINE_RUNTIME_DIR
    ""
    CACHE PATH
    "Directory containing sl.interposer.dll, sl.common.dll, sl.dlss.dll and nvngx_dlss.dll")

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
    comctl32
    winmm)

if(BLACKHOLE_ENABLE_DLSS)
    if(EXISTS "${BLACKHOLE_STREAMLINE_SDK_ROOT}/include/sl.h")
        target_include_directories(
            BlackHole
            PRIVATE
            "${BLACKHOLE_STREAMLINE_SDK_ROOT}/include")
        target_compile_definitions(BlackHole PRIVATE BLACKHOLE_HAS_STREAMLINE=1)

        set(_blackhole_streamline_runtime "${BLACKHOLE_STREAMLINE_RUNTIME_DIR}")
        if(_blackhole_streamline_runtime STREQUAL "")
            if(EXISTS "${BLACKHOLE_STREAMLINE_SDK_ROOT}/bin/x64/sl.interposer.dll")
                set(_blackhole_streamline_runtime
                    "${BLACKHOLE_STREAMLINE_SDK_ROOT}/bin/x64")
            else()
                set(_blackhole_streamline_runtime
                    "${BLACKHOLE_STREAMLINE_SDK_ROOT}/bin")
            endif()
        endif()

        foreach(_dlss_runtime IN ITEMS
                sl.interposer.dll
                sl.common.dll
                sl.dlss.dll
                nvngx_dlss.dll)
            if(EXISTS "${_blackhole_streamline_runtime}/${_dlss_runtime}")
                add_custom_command(TARGET BlackHole POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_blackhole_streamline_runtime}/${_dlss_runtime}"
                        "$<TARGET_FILE_DIR:BlackHole>/${_dlss_runtime}")
            else()
                message(WARNING
                    "DLSS runtime file not found: ${_blackhole_streamline_runtime}/${_dlss_runtime}. "
                    "The executable will build, but it will use the native fallback until the file is installed.")
            endif()
        endforeach()
    else()
        message(WARNING
            "Streamline headers were not found under ${BLACKHOLE_STREAMLINE_SDK_ROOT}. "
            "Building without DLSS; set BLACKHOLE_STREAMLINE_SDK_ROOT to enable it.")
    endif()
endif()

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
