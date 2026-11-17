// OpenGL 4.3 core entry points for the engine.
//
// Linux: the functions come straight from glvnd's libOpenGL (link -lOpenGL), which dispatches to whichever vendor's
// context is current on the calling thread (GLX or EGL, Mesa or NVIDIA).
// Windows: opengl32.dll only exports GL 1.1, so ShoalLoadGL() fetches the rest with wglGetProcAddress once a
// context is current (see shoal_gl_win.cpp). The engine calls ShoalLoadGL() in init() on every platform.
#pragma once

#ifdef _WIN32
#  include "shoal_gl_win.h"
#else
#  define GL_GLEXT_PROTOTYPES 1
#  include <GL/glcorearb.h>
inline bool ShoalLoadGL() { return true; }
#endif
