#pragma once




#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <cstddef>
#include <cstdint>

using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLint = int;
using GLuint = unsigned int;
using GLsizei = int;
using GLfloat = float;
using GLchar = char;
using GLubyte = unsigned char;
using GLintptr = ptrdiff_t;
using GLsizeiptr = ptrdiff_t;
using GLvoid = void;

#ifndef GL_FALSE
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_TRIANGLES 0x0004
#define GL_LINES 0x0001
#define GL_TRIANGLE_FAN 0x0006
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ONE 1
#define GL_ZERO 0
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_SCISSOR_TEST 0x0C11
#define GL_CULL_FACE 0x0B44
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_CCW 0x0901
#define GL_CW 0x0900
#define GL_LESS 0x0201
#define GL_LEQUAL 0x0203
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_RED 0x1903
#define GL_R8 0x8229
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_STREAM_DRAW 0x88E0
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_LINEAR 0x2601
#define GL_NEAREST 0x2600
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_DEPTH_COMPONENT 0x1902
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_NO_ERROR 0
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_FUNC_ADD 0x8006
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_TRIANGLE_STRIP 0x0005
#endif

namespace xpbd::gfx::gl {

bool loadGlProcs(void* (*get_proc)(const char*));

extern void (*Clear)(GLbitfield);
extern void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
extern void (*Enable)(GLenum);
extern void (*Disable)(GLenum);
extern void (*BlendFunc)(GLenum, GLenum);
extern void (*BlendEquation)(GLenum);
extern void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
extern void (*Scissor)(GLint, GLint, GLsizei, GLsizei);
extern void (*DepthFunc)(GLenum);
extern void (*DepthMask)(GLboolean);
extern void (*CullFace)(GLenum);
extern void (*FrontFace)(GLenum);
extern void (*LineWidth)(GLfloat);
extern void (*DrawArrays)(GLenum, GLint, GLsizei);
extern void (*DrawElements)(GLenum, GLsizei, GLenum, const void*);
extern void (*GenBuffers)(GLsizei, GLuint*);
extern void (*BindBuffer)(GLenum, GLuint);
extern void (*BufferData)(GLenum, GLsizeiptr, const void*, GLenum);
extern void (*BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void*);
extern void (*DeleteBuffers)(GLsizei, const GLuint*);
extern void (*GenVertexArrays)(GLsizei, GLuint*);
extern void (*BindVertexArray)(GLuint);
extern void (*DeleteVertexArrays)(GLsizei, const GLuint*);
extern void (*EnableVertexAttribArray)(GLuint);
extern void (*DisableVertexAttribArray)(GLuint);
extern void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
extern GLuint (*CreateShader)(GLenum);
extern void (*ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
extern void (*CompileShader)(GLuint);
extern void (*GetShaderiv)(GLuint, GLenum, GLint*);
extern void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
extern void (*DeleteShader)(GLuint);
extern GLuint (*CreateProgram)();
extern void (*AttachShader)(GLuint, GLuint);
extern void (*LinkProgram)(GLuint);
extern void (*GetProgramiv)(GLuint, GLenum, GLint*);
extern void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
extern void (*DeleteProgram)(GLuint);
extern void (*UseProgram)(GLuint);
extern GLint (*GetUniformLocation)(GLuint, const GLchar*);
extern void (*Uniform1i)(GLint, GLint);
extern void (*Uniform1f)(GLint, GLfloat);
extern void (*Uniform2f)(GLint, GLfloat, GLfloat);
extern void (*Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
extern void (*UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*);
extern void (*GenTextures)(GLsizei, GLuint*);
extern void (*BindTexture)(GLenum, GLuint);
extern void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                          const void*);
extern void (*TexParameteri)(GLenum, GLenum, GLint);
extern void (*DeleteTextures)(GLsizei, const GLuint*);
extern void (*ActiveTexture)(GLenum);
extern void (*PixelStorei)(GLenum, GLint);
extern void (*GenFramebuffers)(GLsizei, GLuint*);
extern void (*BindFramebuffer)(GLenum, GLuint);
extern void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
extern void (*DeleteFramebuffers)(GLsizei, const GLuint*);
extern void (*GenRenderbuffers)(GLsizei, GLuint*);
extern void (*BindRenderbuffer)(GLenum, GLuint);
extern void (*RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
extern void (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
extern void (*DeleteRenderbuffers)(GLsizei, const GLuint*);
extern GLenum (*CheckFramebufferStatus)(GLenum);
extern void (*BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield,
                               GLenum);
extern const GLubyte* (*GetString)(GLenum);
extern void (*GetIntegerv)(GLenum, GLint*);
extern GLenum (*GetError)();

}
