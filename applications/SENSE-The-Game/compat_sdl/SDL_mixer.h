#ifndef SENSE_COMPAT_SDL_MIXER_H
#define SENSE_COMPAT_SDL_MIXER_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIX_INIT_MP3 0x00000008u
#define MIX_DEFAULT_FORMAT 0x8010u

typedef struct Mix_Music Mix_Music;
typedef struct Mix_Chunk Mix_Chunk;

int Mix_Init(int flags);
void Mix_Quit(void);
int Mix_OpenAudio(int frequency, Uint16 format, int channels, int chunksize);
void Mix_CloseAudio(void);

Mix_Music *Mix_LoadMUS_RW(SDL_RWops *src, int freesrc);
void Mix_FreeMusic(Mix_Music *music);
int Mix_PlayMusic(Mix_Music *music, int loops);
int Mix_HaltMusic(void);
int Mix_PausedMusic(void);
void Mix_ResumeMusic(void);
int Mix_PlayingMusic(void);
void Mix_PauseMusic(void);

Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *src, int freesrc);
void Mix_FreeChunk(Mix_Chunk *chunk);
int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops);
int Mix_Playing(int channel);

#ifdef __cplusplus
}
#endif

#endif
