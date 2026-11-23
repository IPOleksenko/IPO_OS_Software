#ifndef ZELDA_COMPAT_SDL_MIXER_H
#define ZELDA_COMPAT_SDL_MIXER_H

#include "SDL.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIX_INIT_OGG       0x00000010
#define MIX_DEFAULT_FORMAT 0x8010u
#define MIX_MAX_VOLUME     128

typedef struct Mix_Music Mix_Music;
typedef struct Mix_Chunk Mix_Chunk;

int Mix_Init(int flags);
void Mix_Quit(void);
int Mix_OpenAudio(int frequency, Uint16 format, int channels, int chunksize);
void Mix_CloseAudio(void);

Mix_Music *Mix_LoadMUS(const char *file);
Mix_Chunk *Mix_LoadWAV(const char *file);
void Mix_FreeMusic(Mix_Music *music);
void Mix_FreeChunk(Mix_Chunk *chunk);

int Mix_PlayMusic(Mix_Music *music, int loops);
int Mix_HaltMusic(void);
int Mix_VolumeMusic(int volume);

int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops);
int Mix_VolumeChunk(Mix_Chunk *chunk, int volume);

const char *Mix_GetError(void);

#ifdef __cplusplus
}
#endif

#endif
