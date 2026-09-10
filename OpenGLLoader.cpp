#include "OpenGLLoader.hpp"

#include <iostream>

namespace blackhole
{
CreateShaderProc createShader = nullptr;
ShaderSourceProc shaderSource = nullptr;
CompileShaderProc compileShader = nullptr;
GetShaderivProc getShaderiv = nullptr;
GetShaderInfoLogProc getShaderInfoLog = nullptr;
CreateProgramProc createProgram = nullptr;
AttachShaderProc attachShader = nullptr;
LinkProgramProc linkProgram = nullptr;
GetProgramivProc getProgramiv = nullptr;
GetProgramInfoLogProc getProgramInfoLog = nullptr;
DeleteShaderProc deleteShader = nullptr;
DeleteProgramProc deleteProgram = nullptr;
UseProgramProc useProgram = nullptr;
GetUniformLocationProc getUniformLocation = nullptr;
Uniform1fProc uniform1f = nullptr;
Uniform1iProc uniform1i = nullptr;
Uniform1uiProc uniform1ui = nullptr;
Uniform2fProc uniform2f = nullptr;
Uniform3fProc uniform3f = nullptr;
Uniform4fProc uniform4f = nullptr;
UniformMatrix4fvProc uniformMatrix4fv = nullptr;
ActiveTextureProc activeTexture = nullptr;
GenVertexArraysProc genVertexArrays = nullptr;
BindVertexArrayProc bindVertexArray = nullptr;
DeleteVertexArraysProc deleteVertexArrays = nullptr;
GenBuffersProc genBuffers = nullptr;
BindBufferProc bindBuffer = nullptr;
BufferDataProc bufferData = nullptr;
DeleteBuffersProc deleteBuffers = nullptr;
BindBufferBaseProc bindBufferBase = nullptr;
DispatchComputeProc dispatchCompute = nullptr;
MemoryBarrierProc memoryBarrier = nullptr;
BindImageTextureProc bindImageTexture = nullptr;
EnableVertexAttribArrayProc enableVertexAttribArray = nullptr;
VertexAttribPointerProc vertexAttribPointer = nullptr;

namespace
{
template<typename Function>
bool load(Function& function, const char* name)
{
    function = reinterpret_cast<Function>(glfwGetProcAddress(name));
    if(function != nullptr) return true;

    std::cerr << "OpenGL function is unavailable: " << name << "\n";
    return false;
}
}

bool loadOpenGLFunctions()
{
    bool success = true;
    success = load(createShader, "glCreateShader") && success;
    success = load(shaderSource, "glShaderSource") && success;
    success = load(compileShader, "glCompileShader") && success;
    success = load(getShaderiv, "glGetShaderiv") && success;
    success = load(getShaderInfoLog, "glGetShaderInfoLog") && success;
    success = load(createProgram, "glCreateProgram") && success;
    success = load(attachShader, "glAttachShader") && success;
    success = load(linkProgram, "glLinkProgram") && success;
    success = load(getProgramiv, "glGetProgramiv") && success;
    success = load(getProgramInfoLog, "glGetProgramInfoLog") && success;
    success = load(deleteShader, "glDeleteShader") && success;
    success = load(deleteProgram, "glDeleteProgram") && success;
    success = load(useProgram, "glUseProgram") && success;
    success = load(getUniformLocation, "glGetUniformLocation") && success;
    success = load(uniform1f, "glUniform1f") && success;
    success = load(uniform1i, "glUniform1i") && success;
    success = load(uniform1ui, "glUniform1ui") && success;
    success = load(uniform2f, "glUniform2f") && success;
    success = load(uniform3f, "glUniform3f") && success;
    success = load(uniform4f, "glUniform4f") && success;
    success = load(uniformMatrix4fv, "glUniformMatrix4fv") && success;
    success = load(activeTexture, "glActiveTexture") && success;
    success = load(genVertexArrays, "glGenVertexArrays") && success;
    success = load(bindVertexArray, "glBindVertexArray") && success;
    success = load(deleteVertexArrays, "glDeleteVertexArrays") && success;
    success = load(genBuffers, "glGenBuffers") && success;
    success = load(bindBuffer, "glBindBuffer") && success;
    success = load(bufferData, "glBufferData") && success;
    success = load(deleteBuffers, "glDeleteBuffers") && success;
    success = load(bindBufferBase, "glBindBufferBase") && success;
    success = load(dispatchCompute, "glDispatchCompute") && success;
    success = load(memoryBarrier, "glMemoryBarrier") && success;
    success = load(bindImageTexture, "glBindImageTexture") && success;
    success = load(enableVertexAttribArray, "glEnableVertexAttribArray") && success;
    success = load(vertexAttribPointer, "glVertexAttribPointer") && success;
    return success;
}
}
