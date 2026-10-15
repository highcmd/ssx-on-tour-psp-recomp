#define SSX_GL_NO_MACROS
#include "gl_funcs.h"

#include <SDL2/SDL.h>
#include <cstdio>

#define SSX_GL_DEFINE(type, name) type ssx_##name;
SSX_GL_FUNCS(SSX_GL_DEFINE)
#undef SSX_GL_DEFINE

bool gl_load_functions() {
	bool ok = true;
#define SSX_GL_LOAD(type, name)                                                  \
	ssx_##name = (type)SDL_GL_GetProcAddress(#name);                             \
	if (!ssx_##name) { fprintf(stderr, "GL: missing %s\n", #name); ok = false; }
	SSX_GL_FUNCS(SSX_GL_LOAD)
#undef SSX_GL_LOAD
	return ok;
}
