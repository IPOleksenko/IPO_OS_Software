#ifndef MARIO_COMPAT_SDL_MIXER_H
#define MARIO_COMPAT_SDL_MIXER_H

#include "SDL.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIX_DEFAULT_FORMAT 0x8010u

typedef struct Mix_Music Mix_Music;
typedef struct Mix_Chunk Mix_Chunk;

int Mix_OpenAudio(int frequency, Uint16 format, int channels, int chunksize);
Mix_Music *Mix_LoadMUS(const char *file);
Mix_Chunk *Mix_LoadWAV(const char *file);
void Mix_FreeMusic(Mix_Music *music);
void Mix_FreeChunk(Mix_Chunk *chunk);
int Mix_PlayMusic(Mix_Music *music, int loops);
int Mix_HaltMusic(void);
void Mix_PauseMusic(void);
void Mix_ResumeMusic(void);
int Mix_PausedMusic(void);
int Mix_VolumeMusic(int volume);
int Mix_VolumeChunk(Mix_Chunk *chunk, int volume);
int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops);

#ifdef __cplusplus
}
#endif

#endif
