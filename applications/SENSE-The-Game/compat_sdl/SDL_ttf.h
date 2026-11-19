#ifndef SENSE_COMPAT_SDL_TTF_H
#define SENSE_COMPAT_SDL_TTF_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TTF_HINTING_NORMAL 0
#define TTF_HINTING_LIGHT  1

typedef struct TTF_Font TTF_Font;

int TTF_Init(void);
void TTF_Quit(void);
TTF_Font *TTF_OpenFont(const char *file, int ptsize);
TTF_Font *TTF_OpenFontRW(SDL_RWops *src, int freesrc, int ptsize);
void TTF_CloseFont(TTF_Font *font);
void TTF_SetFontHinting(TTF_Font *font, int hinting);
int TTF_FontHeight(const TTF_Font *font);
SDL_Surface *TTF_RenderUTF8_Blended(TTF_Font *font, const char *text, SDL_Color fg);

#ifdef __cplusplus
}
#endif

#endif
