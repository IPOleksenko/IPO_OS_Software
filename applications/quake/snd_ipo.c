#include "quakedef.h"

#include <stdint.h>
#include <string.h>

#include <driver/audio_core.h>
#include <ipo/audio.h>
#include <driver/sound.h>
#include <system/timer.h>

#define QUAKE_AUDIO_FREQ    22050u
#define QUAKE_AUDIO_SAMPLES 16384u /* power of 2 */

static dma_t s_dma;
static int16_t s_ring[QUAKE_AUDIO_SAMPLES];
static audio_stream_t *s_astream = NULL;
static uint32_t s_start_ms = 0;
static qboolean s_snd_ready = false;

static uint64_t quake_played_frames(void) {
    if (s_snd_ready && s_astream) {
        ipo_audio_clock_t clk;
        memset(&clk, 0, sizeof(clk));
        clk.struct_size = (uint32_t)sizeof(clk);
        if (ipo_audio_get_stream_clock(s_astream, &clk) == IPO_AUDIO_OK) {
            return clk.played_frames;
        }
    }
    uint32_t elapsed = timer_millis() - s_start_ms;
    return ((uint64_t)elapsed * QUAKE_AUDIO_FREQ) / 1000ULL;
}

qboolean SNDDMA_Init(void) {
    memset(&s_dma, 0, sizeof(s_dma));
    memset(s_ring, 0, sizeof(s_ring));

    shm = &s_dma;
    shm->channels = 1;
    shm->samplebits = 16;
    shm->speed = (int)QUAKE_AUDIO_FREQ;
    shm->samples = (int)QUAKE_AUDIO_SAMPLES;
    shm->submission_chunk = 1;
    shm->samplepos = 0;
    shm->buffer = (unsigned char *)s_ring;

    if (audio_init()) {
        s_astream = audio_stream_create_ring(AUDIO_FORMAT_S16, 1u, QUAKE_AUDIO_FREQ,
                                             s_ring, QUAKE_AUDIO_SAMPLES, 0u);
        if (s_astream) {
            audio_stream_play(s_astream);
            s_snd_ready = true;
        }
    }
    s_start_ms = timer_millis();
    shm->soundalive = true;
    shm->gamealive = true;
    return true;
}

int SNDDMA_GetDMAPos(void) {
    if (!shm || !shm->soundalive) return 0;
    uint64_t total_samples = quake_played_frames();
    shm->samplepos = (int)(total_samples & (QUAKE_AUDIO_SAMPLES - 1u));
    return shm->samplepos;
}

void SNDDMA_Shutdown(void) {
    if (s_astream) {
        audio_stream_stop(s_astream);
        audio_stream_destroy(s_astream);
        s_astream = NULL;
    }
    s_snd_ready = false;
}

void SNDDMA_Submit(void) {
    if (!s_snd_ready || !s_astream) return;
    uint64_t play_samples = quake_played_frames();
    /* Keep hardware buffered slightly ahead of actual kernel DAC cursor */
    uint32_t write_pos = (uint32_t)play_samples + (QUAKE_AUDIO_FREQ / 12u);
    audio_stream_update_buffered(s_astream, write_pos, false);
    audio_tick();
}

void ipo_snd_pump(void) {
    if (s_snd_ready) {
        audio_tick();
    }
}
