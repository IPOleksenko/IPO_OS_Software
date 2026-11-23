#ifndef MARIO_COMPAT_SDL_IMAGE_H
#define MARIO_COMPAT_SDL_IMAGE_H

#include "SDL.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline SDL_Texture *IMG_LoadTexture(SDL_Renderer *renderer, const char *file) {
    SDL_Surface *s = SDL_LoadBMP(file);
    if (!s) return NULL;
    SDL_Texture *t = SDL_CreateTextureFromSurface(renderer, s);
    SDL_FreeSurface(s);
    return t;
}

#ifdef __cplusplus
}
#endif

#endif
