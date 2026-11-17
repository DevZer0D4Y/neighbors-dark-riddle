// GLES2 for all platforms. Switch / Linux: the system GLES2 headers. Windows: the
// driver's OpenGL (ES 2 context, or desktop GL 2.1 as fallback) through function
// pointers loaded at runtime with SDL_GL_GetProcAddress.
#pragma once
#ifdef _WIN32
#define GL_GLES_PROTOTYPES 0
#include <SDL2/SDL_opengles2_khrplatform.h>
#include <SDL2/SDL_opengles2_gl2platform.h>
#include <SDL2/SDL_opengles2_gl2.h>
#define DR_GL_FUNCS(X) \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation) \
    X(PFNGLBINDBUFFERPROC, glBindBuffer) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) \
    X(PFNGLBINDTEXTUREPROC, glBindTexture) \
    X(PFNGLBLENDFUNCPROC, glBlendFunc) \
    X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    X(PFNGLCLEARPROC, glClear) \
    X(PFNGLCLEARCOLORPROC, glClearColor) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLCULLFACEPROC, glCullFace) \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc) \
    X(PFNGLDEPTHMASKPROC, glDepthMask) \
    X(PFNGLDISABLEPROC, glDisable) \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray) \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays) \
    X(PFNGLDRAWELEMENTSPROC, glDrawElements) \
    X(PFNGLENABLEPROC, glEnable) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
    X(PFNGLFRONTFACEPROC, glFrontFace) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) \
    X(PFNGLGENTEXTURESPROC, glGenTextures) \
    X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap) \
    X(PFNGLGETERRORPROC, glGetError) \
    X(PFNGLGETINTEGERVPROC, glGetIntegerv) \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSTRINGPROC, glGetString) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei) \
    X(PFNGLREADPIXELSPROC, glReadPixels) \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) \
    X(PFNGLSCISSORPROC, glScissor) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLTEXIMAGE2DPROC, glTexImage2D) \
    X(PFNGLTEXPARAMETERIPROC, glTexParameteri) \
    X(PFNGLUNIFORM1FPROC, glUniform1f) \
    X(PFNGLUNIFORM1IPROC, glUniform1i) \
    X(PFNGLUNIFORM3FPROC, glUniform3f) \
    X(PFNGLUNIFORM4FPROC, glUniform4f) \
    X(PFNGLUNIFORM4FVPROC, glUniform4fv) \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    X(PFNGLVIEWPORTPROC, glViewport) \

#define DR_GL_DECL(T, n) extern T dr_##n;
DR_GL_FUNCS(DR_GL_DECL)
#define glActiveTexture dr_glActiveTexture
#define glAttachShader dr_glAttachShader
#define glBindAttribLocation dr_glBindAttribLocation
#define glBindBuffer dr_glBindBuffer
#define glBindFramebuffer dr_glBindFramebuffer
#define glBindRenderbuffer dr_glBindRenderbuffer
#define glBindTexture dr_glBindTexture
#define glBlendFunc dr_glBlendFunc
#define glBufferData dr_glBufferData
#define glCheckFramebufferStatus dr_glCheckFramebufferStatus
#define glClear dr_glClear
#define glClearColor dr_glClearColor
#define glCompileShader dr_glCompileShader
#define glCreateProgram dr_glCreateProgram
#define glCreateShader dr_glCreateShader
#define glCullFace dr_glCullFace
#define glDepthFunc dr_glDepthFunc
#define glDepthMask dr_glDepthMask
#define glDisable dr_glDisable
#define glDisableVertexAttribArray dr_glDisableVertexAttribArray
#define glDrawArrays dr_glDrawArrays
#define glDrawElements dr_glDrawElements
#define glEnable dr_glEnable
#define glEnableVertexAttribArray dr_glEnableVertexAttribArray
#define glFramebufferRenderbuffer dr_glFramebufferRenderbuffer
#define glFramebufferTexture2D dr_glFramebufferTexture2D
#define glFrontFace dr_glFrontFace
#define glGenBuffers dr_glGenBuffers
#define glGenFramebuffers dr_glGenFramebuffers
#define glGenRenderbuffers dr_glGenRenderbuffers
#define glGenTextures dr_glGenTextures
#define glGenerateMipmap dr_glGenerateMipmap
#define glGetError dr_glGetError
#define glGetIntegerv dr_glGetIntegerv
#define glGetProgramInfoLog dr_glGetProgramInfoLog
#define glGetProgramiv dr_glGetProgramiv
#define glGetShaderInfoLog dr_glGetShaderInfoLog
#define glGetShaderiv dr_glGetShaderiv
#define glGetString dr_glGetString
#define glGetUniformLocation dr_glGetUniformLocation
#define glLinkProgram dr_glLinkProgram
#define glPixelStorei dr_glPixelStorei
#define glReadPixels dr_glReadPixels
#define glRenderbufferStorage dr_glRenderbufferStorage
#define glScissor dr_glScissor
#define glShaderSource dr_glShaderSource
#define glTexImage2D dr_glTexImage2D
#define glTexParameteri dr_glTexParameteri
#define glUniform1f dr_glUniform1f
#define glUniform1i dr_glUniform1i
#define glUniform3f dr_glUniform3f
#define glUniform4f dr_glUniform4f
#define glUniform4fv dr_glUniform4fv
#define glUniformMatrix4fv dr_glUniformMatrix4fv
#define glUseProgram dr_glUseProgram
#define glVertexAttribPointer dr_glVertexAttribPointer
#define glViewport dr_glViewport
bool drGLLoad();   // after the context is current; false if a function is missing
#else
#include <GLES2/gl2.h>
static inline bool drGLLoad() { return true; }
#endif
