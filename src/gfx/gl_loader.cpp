#include "gl_loader.hpp"

#include <cstring>

namespace xpbd::gfx::gl {
namespace {

template <typename T>
bool load(void* (*get_proc)(const char*), T& fn, const char* name) {
    fn = reinterpret_cast<T>(get_proc(name));
    return fn != nullptr;
}

}

void (*Clear)(GLbitfield) = nullptr;
void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
void (*Enable)(GLenum) = nullptr;
void (*Disable)(GLenum) = nullptr;
void (*BlendFunc)(GLenum, GLenum) = nullptr;
void (*BlendEquation)(GLenum) = nullptr;
void (*Viewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
void (*Scissor)(GLint, GLint, GLsizei, GLsizei) = nullptr;
void (*DepthFunc)(GLenum) = nullptr;
void (*DepthMask)(GLboolean) = nullptr;
void (*CullFace)(GLenum) = nullptr;
void (*FrontFace)(GLenum) = nullptr;
void (*LineWidth)(GLfloat) = nullptr;
void (*DrawArrays)(GLenum, GLint, GLsizei) = nullptr;
void (*DrawElements)(GLenum, GLsizei, GLenum, const void*) = nullptr;
void (*GenBuffers)(GLsizei, GLuint*) = nullptr;
void (*BindBuffer)(GLenum, GLuint) = nullptr;
void (*BufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
void (*BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void*) = nullptr;
void (*DeleteBuffers)(GLsizei, const GLuint*) = nullptr;
void (*GenVertexArrays)(GLsizei, GLuint*) = nullptr;
void (*BindVertexArray)(GLuint) = nullptr;
void (*DeleteVertexArrays)(GLsizei, const GLuint*) = nullptr;
void (*EnableVertexAttribArray)(GLuint) = nullptr;
void (*DisableVertexAttribArray)(GLuint) = nullptr;
void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) = nullptr;
GLuint (*CreateShader)(GLenum) = nullptr;
void (*ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
void (*CompileShader)(GLuint) = nullptr;
void (*GetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
void (*DeleteShader)(GLuint) = nullptr;
GLuint (*CreateProgram)() = nullptr;
void (*AttachShader)(GLuint, GLuint) = nullptr;
void (*LinkProgram)(GLuint) = nullptr;
void (*GetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
void (*DeleteProgram)(GLuint) = nullptr;
void (*UseProgram)(GLuint) = nullptr;
GLint (*GetUniformLocation)(GLuint, const GLchar*) = nullptr;
void (*Uniform1i)(GLint, GLint) = nullptr;
void (*Uniform1f)(GLint, GLfloat) = nullptr;
void (*Uniform2f)(GLint, GLfloat, GLfloat) = nullptr;
void (*Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
void (*UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*) = nullptr;
void (*GenTextures)(GLsizei, GLuint*) = nullptr;
void (*BindTexture)(GLenum, GLuint) = nullptr;
void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) =
    nullptr;
void (*TexParameteri)(GLenum, GLenum, GLint) = nullptr;
void (*DeleteTextures)(GLsizei, const GLuint*) = nullptr;
void (*ActiveTexture)(GLenum) = nullptr;
void (*PixelStorei)(GLenum, GLint) = nullptr;
void (*GenFramebuffers)(GLsizei, GLuint*) = nullptr;
void (*BindFramebuffer)(GLenum, GLuint) = nullptr;
void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
void (*DeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
void (*GenRenderbuffers)(GLsizei, GLuint*) = nullptr;
void (*BindRenderbuffer)(GLenum, GLuint) = nullptr;
void (*RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei) = nullptr;
void (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint) = nullptr;
void (*DeleteRenderbuffers)(GLsizei, const GLuint*) = nullptr;
GLenum (*CheckFramebufferStatus)(GLenum) = nullptr;
void (*BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield,
                        GLenum) = nullptr;
const GLubyte* (*GetString)(GLenum) = nullptr;
void (*GetIntegerv)(GLenum, GLint*) = nullptr;
GLenum (*GetError)() = nullptr;

bool loadGlProcs(void* (*get_proc)(const char*)) {
    if (!get_proc) {
        return false;
    }
    bool ok = true;
#define LOAD(name) ok = load(get_proc, name, "gl" #name) && ok
    LOAD(Clear);
    LOAD(ClearColor);
    LOAD(Enable);
    LOAD(Disable);
    LOAD(BlendFunc);
    LOAD(BlendEquation);
    LOAD(Viewport);
    LOAD(Scissor);
    LOAD(DepthFunc);
    LOAD(DepthMask);
    LOAD(CullFace);
    LOAD(FrontFace);
    LOAD(LineWidth);
    LOAD(DrawArrays);
    LOAD(DrawElements);
    LOAD(GenBuffers);
    LOAD(BindBuffer);
    LOAD(BufferData);
    LOAD(BufferSubData);
    LOAD(DeleteBuffers);
    LOAD(GenVertexArrays);
    LOAD(BindVertexArray);
    LOAD(DeleteVertexArrays);
    LOAD(EnableVertexAttribArray);
    LOAD(DisableVertexAttribArray);
    LOAD(VertexAttribPointer);
    LOAD(CreateShader);
    LOAD(ShaderSource);
    LOAD(CompileShader);
    LOAD(GetShaderiv);
    LOAD(GetShaderInfoLog);
    LOAD(DeleteShader);
    LOAD(CreateProgram);
    LOAD(AttachShader);
    LOAD(LinkProgram);
    LOAD(GetProgramiv);
    LOAD(GetProgramInfoLog);
    LOAD(DeleteProgram);
    LOAD(UseProgram);
    LOAD(GetUniformLocation);
    LOAD(Uniform1i);
    LOAD(Uniform1f);
    LOAD(Uniform2f);
    LOAD(Uniform4f);
    LOAD(UniformMatrix4fv);
    LOAD(GenTextures);
    LOAD(BindTexture);
    LOAD(TexImage2D);
    LOAD(TexParameteri);
    LOAD(DeleteTextures);
    LOAD(ActiveTexture);
    LOAD(PixelStorei);
    LOAD(GenFramebuffers);
    LOAD(BindFramebuffer);
    LOAD(FramebufferTexture2D);
    LOAD(DeleteFramebuffers);
    LOAD(GenRenderbuffers);
    LOAD(BindRenderbuffer);
    LOAD(RenderbufferStorage);
    LOAD(FramebufferRenderbuffer);
    LOAD(DeleteRenderbuffers);
    LOAD(CheckFramebufferStatus);
    LOAD(BlitFramebuffer);
    LOAD(GetString);
    LOAD(GetIntegerv);
    LOAD(GetError);
#undef LOAD
    return ok;
}

}
