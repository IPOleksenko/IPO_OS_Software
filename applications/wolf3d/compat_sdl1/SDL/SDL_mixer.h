#ifndef WOLF_COMPAT_SDL_MIXER_H
#define WOLF_COMPAT_SDL_MIXER_H

#include "SDL.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIX_CHANNELS 8

typedef struct Mix_Chunk {
    int16_t *samples;
    uint32_t count;
    int volume;
    Uint8 *abuf;
    Uint32 alen;
} Mix_Chunk;

int Mix_OpenAudio(int frequency, Uint16 format, int channels, int chunksize);
void Mix_CloseAudio(void);
const char *Mix_GetError(void);

int Mix_ReserveChannels(int num);
int Mix_GroupChannels(int from, int to, int tag);
int Mix_GroupAvailable(int tag);
int Mix_GroupOldest(int tag);

Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *src, int freesrc);
void Mix_FreeChunk(Mix_Chunk *chunk);

int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops);
int Mix_HaltChannel(int channel);
int Mix_SetPanning(int channel, Uint8 left, Uint8 right);

void Mix_HookMusic(void (*mix_func)(void *udata, Uint8 *stream, int len), void *arg);
void Mix_ChannelFinished(void (*channel_finished)(int channel));

#ifdef __cplusplus
}
#endif

#endif
