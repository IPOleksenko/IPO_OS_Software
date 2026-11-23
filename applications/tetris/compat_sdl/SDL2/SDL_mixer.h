#ifndef TETRIS_COMPAT_SDL_MIXER_H
#define TETRIS_COMPAT_SDL_MIXER_H
#include "SDL.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIX_INIT_MP3     0x00000008
#define MIX_INIT_WAVPACK 0x00000020
#define MIX_DEFAULT_FORMAT 0x8010u
#define AUDIO_S16LSB 0x8010u

typedef struct Mix_Chunk Mix_Chunk;
typedef struct Mix_Music Mix_Music;

int Mix_Init(int flags);
void Mix_Quit(void);
const char *Mix_GetError(void);
int Mix_OpenAudio(int frequency, Uint16 format, int channels, int chunksize);
void Mix_CloseAudio(void);
int Mix_AllocateChannels(int numchans);

Mix_Chunk *Mix_LoadWAV(const char *file);
void Mix_FreeChunk(Mix_Chunk *chunk);
int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops);
void Mix_Pause(int channel);
void Mix_Resume(int channel);
int Mix_HaltChannel(int channel);
int Mix_Playing(int channel);
int Mix_Paused(int channel);
int Mix_Volume(int channel, int volume);

Mix_Music *Mix_LoadMUS(const char *file);
void Mix_FreeMusic(Mix_Music *music);
int Mix_PlayMusic(Mix_Music *music, int loops);
void Mix_PauseMusic(void);
void Mix_ResumeMusic(void);
int Mix_HaltMusic(void);
int Mix_PlayingMusic(void);
int Mix_PausedMusic(void);
int Mix_VolumeMusic(int volume);

#ifdef __cplusplus
}
#endif

#endif
