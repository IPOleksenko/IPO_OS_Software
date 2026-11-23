#ifndef ZELDA_COMPAT_SDL_TTF_H
#define ZELDA_COMPAT_SDL_TTF_H

#include "SDL.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TTF_Font TTF_Font;

int TTF_Init(void);
void TTF_Quit(void);
TTF_Font *TTF_OpenFont(const char *file, int ptsize);
void TTF_CloseFont(TTF_Font *font);
SDL_Surface *TTF_RenderText_Solid(TTF_Font *font, const char *text, SDL_Color fg);
const char *TTF_GetError(void);

#ifdef __cplusplus
}
#endif

#endif
