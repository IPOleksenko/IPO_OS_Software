#ifndef SENSE_COMPAT_SDL_IMAGE_H
#define SENSE_COMPAT_SDL_IMAGE_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IMG_INIT_PNG 0x00000002u

int IMG_Init(int flags);
void IMG_Quit(void);
SDL_Texture *IMG_LoadTexture(SDL_Renderer *renderer, const char *file);
SDL_Texture *IMG_LoadTexture_RW(SDL_Renderer *renderer, SDL_RWops *src, int freesrc);

#ifdef __cplusplus
}
#endif

#endif
