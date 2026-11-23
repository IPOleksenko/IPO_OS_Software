#ifndef WOLF_COMPAT_SDL_SYSWM_H
#define WOLF_COMPAT_SDL_SYSWM_H

#include "SDL.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SDL_SysWMinfo {
    SDL_version version;
    int dummy;
} SDL_SysWMinfo;

static inline int SDL_GetWMInfo(SDL_SysWMinfo *info) {
    if (info) {
        SDL_VERSION(&info->version);
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif
