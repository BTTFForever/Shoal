// GL 4.3 entry points used by the engine, loaded at run time on Windows (opengl32.dll exports only GL 1.1).
// Generated from the gl* calls in shoal_engine.cpp, plus the framebuffer functions the Windows host uses to present
// through DXGI (ShoalWindows/shoal_win.cpp). If either starts using a new GL function, add it here.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/glcorearb.h> // ShoalCore/include (Khronos), no prototypes

#define SHOAL_GL_FUNCTIONS(X) \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
    X(PFNGLFLUSHPROC, glFlush) \
    X(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers) \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer) \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLBINDBUFFERPROC, glBindBuffer) \
    X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase) \
    X(PFNGLBINDTEXTUREPROC, glBindTexture) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLBLENDCOLORPROC, glBlendColor) \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate) \
    X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData) \
    X(PFNGLCLEARPROC, glClear) \
    X(PFNGLCLEARCOLORPROC, glClearColor) \
    X(PFNGLCLEARDEPTHFPROC, glClearDepthf) \
    X(PFNGLCLEARSTENCILPROC, glClearStencil) \
    X(PFNGLCOLORMASKPROC, glColorMask) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers) \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) \
    X(PFNGLDELETESHADERPROC, glDeleteShader) \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures) \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays) \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc) \
    X(PFNGLDEPTHMASKPROC, glDepthMask) \
    X(PFNGLDISABLEPROC, glDisable) \
    X(PFNGLDISPATCHCOMPUTEPROC, glDispatchCompute) \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays) \
    X(PFNGLDRAWELEMENTSINSTANCEDPROC, glDrawElementsInstanced) \
    X(PFNGLENABLEPROC, glEnable) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) \
    X(PFNGLGENTEXTURESPROC, glGenTextures) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
    X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap) \
    X(PFNGLGETBUFFERSUBDATAPROC, glGetBufferSubData) \
    X(PFNGLGETINTEGERVPROC, glGetIntegerv) \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSTRINGPROC, glGetString) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLMEMORYBARRIERPROC, glMemoryBarrier) \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei) \
    X(PFNGLREADPIXELSPROC, glReadPixels) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLSTENCILFUNCPROC, glStencilFunc) \
    X(PFNGLSTENCILMASKPROC, glStencilMask) \
    X(PFNGLSTENCILOPPROC, glStencilOp) \
    X(PFNGLTEXIMAGE2DPROC, glTexImage2D) \
    X(PFNGLTEXPARAMETERIPROC, glTexParameteri) \
    X(PFNGLUNIFORM1FPROC, glUniform1f) \
    X(PFNGLUNIFORM1IPROC, glUniform1i) \
    X(PFNGLUNIFORM4FPROC, glUniform4f) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    X(PFNGLVIEWPORTPROC, glViewport) \

#define SHOAL_GL_DECLARE(type, name) extern type shoalgl_##name;
SHOAL_GL_FUNCTIONS(SHOAL_GL_DECLARE)
#undef SHOAL_GL_DECLARE
#define glRenderbufferStorage shoalgl_glRenderbufferStorage
#define glGenRenderbuffers shoalgl_glGenRenderbuffers
#define glGenFramebuffers shoalgl_glGenFramebuffers
#define glFramebufferTexture2D shoalgl_glFramebufferTexture2D
#define glFramebufferRenderbuffer shoalgl_glFramebufferRenderbuffer
#define glFlush shoalgl_glFlush
#define glDeleteRenderbuffers shoalgl_glDeleteRenderbuffers
#define glDeleteFramebuffers shoalgl_glDeleteFramebuffers
#define glCheckFramebufferStatus shoalgl_glCheckFramebufferStatus
#define glBlitFramebuffer shoalgl_glBlitFramebuffer
#define glBindRenderbuffer shoalgl_glBindRenderbuffer
#define glBindFramebuffer shoalgl_glBindFramebuffer
#define glActiveTexture shoalgl_glActiveTexture
#define glAttachShader shoalgl_glAttachShader
#define glBindBuffer shoalgl_glBindBuffer
#define glBindBufferBase shoalgl_glBindBufferBase
#define glBindTexture shoalgl_glBindTexture
#define glBindVertexArray shoalgl_glBindVertexArray
#define glBlendColor shoalgl_glBlendColor
#define glBlendFuncSeparate shoalgl_glBlendFuncSeparate
#define glBufferData shoalgl_glBufferData
#define glBufferSubData shoalgl_glBufferSubData
#define glClear shoalgl_glClear
#define glClearColor shoalgl_glClearColor
#define glClearDepthf shoalgl_glClearDepthf
#define glClearStencil shoalgl_glClearStencil
#define glColorMask shoalgl_glColorMask
#define glCompileShader shoalgl_glCompileShader
#define glCreateProgram shoalgl_glCreateProgram
#define glCreateShader shoalgl_glCreateShader
#define glDeleteBuffers shoalgl_glDeleteBuffers
#define glDeleteProgram shoalgl_glDeleteProgram
#define glDeleteShader shoalgl_glDeleteShader
#define glDeleteTextures shoalgl_glDeleteTextures
#define glDeleteVertexArrays shoalgl_glDeleteVertexArrays
#define glDepthFunc shoalgl_glDepthFunc
#define glDepthMask shoalgl_glDepthMask
#define glDisable shoalgl_glDisable
#define glDispatchCompute shoalgl_glDispatchCompute
#define glDrawArrays shoalgl_glDrawArrays
#define glDrawElementsInstanced shoalgl_glDrawElementsInstanced
#define glEnable shoalgl_glEnable
#define glEnableVertexAttribArray shoalgl_glEnableVertexAttribArray
#define glGenBuffers shoalgl_glGenBuffers
#define glGenTextures shoalgl_glGenTextures
#define glGenVertexArrays shoalgl_glGenVertexArrays
#define glGenerateMipmap shoalgl_glGenerateMipmap
#define glGetBufferSubData shoalgl_glGetBufferSubData
#define glGetIntegerv shoalgl_glGetIntegerv
#define glGetProgramInfoLog shoalgl_glGetProgramInfoLog
#define glGetProgramiv shoalgl_glGetProgramiv
#define glGetShaderInfoLog shoalgl_glGetShaderInfoLog
#define glGetShaderiv shoalgl_glGetShaderiv
#define glGetString shoalgl_glGetString
#define glGetUniformLocation shoalgl_glGetUniformLocation
#define glLinkProgram shoalgl_glLinkProgram
#define glMemoryBarrier shoalgl_glMemoryBarrier
#define glPixelStorei shoalgl_glPixelStorei
#define glReadPixels shoalgl_glReadPixels
#define glShaderSource shoalgl_glShaderSource
#define glStencilFunc shoalgl_glStencilFunc
#define glStencilMask shoalgl_glStencilMask
#define glStencilOp shoalgl_glStencilOp
#define glTexImage2D shoalgl_glTexImage2D
#define glTexParameteri shoalgl_glTexParameteri
#define glUniform1f shoalgl_glUniform1f
#define glUniform1i shoalgl_glUniform1i
#define glUniform4f shoalgl_glUniform4f
#define glUseProgram shoalgl_glUseProgram
#define glVertexAttribPointer shoalgl_glVertexAttribPointer
#define glViewport shoalgl_glViewport

// Loads every function above; needs a current context. False if any is missing.
bool ShoalLoadGL();
