#ifndef ZELDA_COMPAT_SDL_IMAGE_H
#define ZELDA_COMPAT_SDL_IMAGE_H

#include "SDL.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IMG_INIT_PNG 0x00000002

int IMG_Init(int flags);
void IMG_Quit(void);
SDL_Surface *IMG_Load(const char *file);
const char *IMG_GetError(void);

#ifdef __cplusplus
}
#endif

#endif
