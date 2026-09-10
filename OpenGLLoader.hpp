#ifndef BLACK_HOLE_OPENGL_LOADER_HPP
#define BLACK_HOLE_OPENGL_LOADER_HPP

#include <cstddef>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#define BLACKHOLE_UNDEFINE_GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
#ifdef BLACKHOLE_UNDEFINE_GLFW_INCLUDE_NONE
#undef GLFW_INCLUDE_NONE
#undef BLACKHOLE_UNDEFINE_GLFW_INCLUDE_NONE
#endif

#include <GL/gl.h>

// The Windows OpenGL 1.1 header does not declare modern entry points. Keep
// the declarations local and resolve them after GLFW creates the context.
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_INFO_LOG_LENGTH
#define GL_INFO_LOG_LENGTH 0x8B84
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_SHADER_STORAGE_BUFFER
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW 0x88E8
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_WRITE_ONLY
#define GL_WRITE_ONLY 0x88B9
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_SHADER_STORAGE_BARRIER_BIT
#define GL_SHADER_STORAGE_BARRIER_BIT 0x00002000
#endif
#ifndef GL_SHADER_IMAGE_ACCESS_BARRIER_BIT
#define GL_SHADER_IMAGE_ACCESS_BARRIER_BIT 0x00000020
#endif
#ifndef GL_TEXTURE_FETCH_BARRIER_BIT
#define GL_TEXTURE_FETCH_BARRIER_BIT 0x00000008
#endif

namespace blackhole
{
using CreateShaderProc = GLuint (APIENTRY*)(GLenum);
using ShaderSourceProc = void (APIENTRY*)(GLuint, GLsizei, const char* const*, const GLint*);
using CompileShaderProc = void (APIENTRY*)(GLuint);
using GetShaderivProc = void (APIENTRY*)(GLuint, GLenum, GLint*);
using GetShaderInfoLogProc = void (APIENTRY*)(GLuint, GLsizei, GLsizei*, char*);
using CreateProgramProc = GLuint (APIENTRY*)();
using AttachShaderProc = void (APIENTRY*)(GLuint, GLuint);
using LinkProgramProc = void (APIENTRY*)(GLuint);
using GetProgramivProc = void (APIENTRY*)(GLuint, GLenum, GLint*);
using GetProgramInfoLogProc = void (APIENTRY*)(GLuint, GLsizei, GLsizei*, char*);
using DeleteShaderProc = void (APIENTRY*)(GLuint);
using DeleteProgramProc = void (APIENTRY*)(GLuint);
using UseProgramProc = void (APIENTRY*)(GLuint);
using GetUniformLocationProc = GLint (APIENTRY*)(GLuint, const char*);
using Uniform1fProc = void (APIENTRY*)(GLint, GLfloat);
using Uniform1iProc = void (APIENTRY*)(GLint, GLint);
using Uniform1uiProc = void (APIENTRY*)(GLint, GLuint);
using Uniform2fProc = void (APIENTRY*)(GLint, GLfloat, GLfloat);
using Uniform3fProc = void (APIENTRY*)(GLint, GLfloat, GLfloat, GLfloat);
using Uniform4fProc = void (APIENTRY*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
using UniformMatrix4fvProc = void (APIENTRY*)(GLint, GLsizei, GLboolean, const GLfloat*);
using ActiveTextureProc = void (APIENTRY*)(GLenum);
using GenVertexArraysProc = void (APIENTRY*)(GLsizei, GLuint*);
using BindVertexArrayProc = void (APIENTRY*)(GLuint);
using DeleteVertexArraysProc = void (APIENTRY*)(GLsizei, const GLuint*);
using GenBuffersProc = void (APIENTRY*)(GLsizei, GLuint*);
using BindBufferProc = void (APIENTRY*)(GLenum, GLuint);
using BufferDataProc = void (APIENTRY*)(GLenum, std::ptrdiff_t, const void*, GLenum);
using DeleteBuffersProc = void (APIENTRY*)(GLsizei, const GLuint*);
using BindBufferBaseProc = void (APIENTRY*)(GLenum, GLuint, GLuint);
using DispatchComputeProc = void (APIENTRY*)(GLuint, GLuint, GLuint);
using MemoryBarrierProc = void (APIENTRY*)(GLbitfield);
using BindImageTextureProc = void (APIENTRY*)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);
using EnableVertexAttribArrayProc = void (APIENTRY*)(GLuint);
using VertexAttribPointerProc = void (APIENTRY*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);

extern CreateShaderProc createShader;
extern ShaderSourceProc shaderSource;
extern CompileShaderProc compileShader;
extern GetShaderivProc getShaderiv;
extern GetShaderInfoLogProc getShaderInfoLog;
extern CreateProgramProc createProgram;
extern AttachShaderProc attachShader;
extern LinkProgramProc linkProgram;
extern GetProgramivProc getProgramiv;
extern GetProgramInfoLogProc getProgramInfoLog;
extern DeleteShaderProc deleteShader;
extern DeleteProgramProc deleteProgram;
extern UseProgramProc useProgram;
extern GetUniformLocationProc getUniformLocation;
extern Uniform1fProc uniform1f;
extern Uniform1iProc uniform1i;
extern Uniform1uiProc uniform1ui;
extern Uniform2fProc uniform2f;
extern Uniform3fProc uniform3f;
extern Uniform4fProc uniform4f;
extern UniformMatrix4fvProc uniformMatrix4fv;
extern ActiveTextureProc activeTexture;
extern GenVertexArraysProc genVertexArrays;
extern BindVertexArrayProc bindVertexArray;
extern DeleteVertexArraysProc deleteVertexArrays;
extern GenBuffersProc genBuffers;
extern BindBufferProc bindBuffer;
extern BufferDataProc bufferData;
extern DeleteBuffersProc deleteBuffers;
extern BindBufferBaseProc bindBufferBase;
extern DispatchComputeProc dispatchCompute;
extern MemoryBarrierProc memoryBarrier;
extern BindImageTextureProc bindImageTexture;
extern EnableVertexAttribArrayProc enableVertexAttribArray;
extern VertexAttribPointerProc vertexAttribPointer;

bool loadOpenGLFunctions();
}

#define glCreateShader blackhole::createShader
#define glShaderSource blackhole::shaderSource
#define glCompileShader blackhole::compileShader
#define glGetShaderiv blackhole::getShaderiv
#define glGetShaderInfoLog blackhole::getShaderInfoLog
#define glCreateProgram blackhole::createProgram
#define glAttachShader blackhole::attachShader
#define glLinkProgram blackhole::linkProgram
#define glGetProgramiv blackhole::getProgramiv
#define glGetProgramInfoLog blackhole::getProgramInfoLog
#define glDeleteShader blackhole::deleteShader
#define glDeleteProgram blackhole::deleteProgram
#define glUseProgram blackhole::useProgram
#define glGetUniformLocation blackhole::getUniformLocation
#define glUniform1f blackhole::uniform1f
#define glUniform1i blackhole::uniform1i
#define glUniform1ui blackhole::uniform1ui
#define glUniform2f blackhole::uniform2f
#define glUniform3f blackhole::uniform3f
#define glUniform4f blackhole::uniform4f
#define glUniformMatrix4fv blackhole::uniformMatrix4fv
#define glActiveTexture blackhole::activeTexture
#define glGenVertexArrays blackhole::genVertexArrays
#define glBindVertexArray blackhole::bindVertexArray
#define glDeleteVertexArrays blackhole::deleteVertexArrays
#define glGenBuffers blackhole::genBuffers
#define glBindBuffer blackhole::bindBuffer
#define glBufferData blackhole::bufferData
#define glDeleteBuffers blackhole::deleteBuffers
#define glBindBufferBase blackhole::bindBufferBase
#define glDispatchCompute blackhole::dispatchCompute
#define glMemoryBarrier blackhole::memoryBarrier
#define glBindImageTexture blackhole::bindImageTexture
#define glEnableVertexAttribArray blackhole::enableVertexAttribArray
#define glVertexAttribPointer blackhole::vertexAttribPointer

#endif
