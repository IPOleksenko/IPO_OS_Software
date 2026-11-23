#ifndef TETRIS_COMPAT_SDL_IMAGE_H
#define TETRIS_COMPAT_SDL_IMAGE_H
#include "SDL.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IMG_INIT_JPG 0x00000001
#define IMG_INIT_PNG 0x00000002

int IMG_Init(int flags);
void IMG_Quit(void);
const char *IMG_GetError(void);
SDL_Texture *IMG_LoadTexture(SDL_Renderer *renderer, const char *file);

#ifdef __cplusplus
}
#endif

#endif
