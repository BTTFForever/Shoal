// Windows OpenGL loader, see shoal_gl_win.h.
#include "shoal_gl_win.h"

#define SHOAL_GL_DEFINE(type, name) type shoalgl_##name = nullptr;
SHOAL_GL_FUNCTIONS(SHOAL_GL_DEFINE)
#undef SHOAL_GL_DEFINE

static PROC ShoalGetProc(const char* name)
{
    // wglGetProcAddress returns null (or 1, 2, 3, -1 on some drivers) for GL 1.1 functions, which opengl32 exports.
    PROC p = wglGetProcAddress(name);
    if (p == nullptr || p == (PROC)1 || p == (PROC)2 || p == (PROC)3 || p == (PROC)-1)
    {
        static HMODULE gl = LoadLibraryA("opengl32.dll");
        p = gl ? GetProcAddress(gl, name) : nullptr;
    }
    return p;
}

bool ShoalLoadGL()
{
    bool ok = true;
#define SHOAL_GL_LOAD(type, name) shoalgl_##name = reinterpret_cast<type>(ShoalGetProc(#name)); if (!shoalgl_##name) ok = false;
    SHOAL_GL_FUNCTIONS(SHOAL_GL_LOAD)
#undef SHOAL_GL_LOAD
    return ok;
}
