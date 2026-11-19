/**
 * i_sound.c - Sound Blaster 16 audio driver for DOOM on IPO_OS
 *
 * Pre-converts 11025 Hz U8 DOOM SFX into 22050 Hz S16 PCM to hit
 * the high-performance integer Fast-Path 2 in IPO_OS audio_core.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "doomdef.h"
#include "doomstat.h"
#include "sounds.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#include <driver/audio_core.h>

#define NUM_CHANNELS 4

typedef struct {
    int16_t  *samples;
    uint32_t num_samples;
} cached_sfx_t;

typedef struct {
    audio_stream_t *stream;
    int handle;
    int sfx_id;
    int vol;
} doom_channel_t;

static cached_sfx_t   s_cached_sfx[NUMSFX];
static int16_t       *s_sfx_pool = NULL;
static doom_channel_t s_channels[NUM_CHANNELS];
static int            s_next_handle = 1;
static boolean        s_sound_inited = false;

typedef struct {
    int      lumpnum;
    uint32_t orig_rate;
    uint32_t orig_count;
    uint32_t out_samples;
    uint32_t pool_offset;
} sfx_lump_plan_t;

/*
 * Preload and up-convert every Doom SFX lump upfront into a single contiguous
 * PCM pool so I_StartSound never triggers lazy WAD reads or runtime sbrk()
 * heap expansions during gameplay.
 */
static void preload_all_sfx(void) {
    static sfx_lump_plan_t plans[NUMSFX];
    static int id_to_plan[NUMSFX];
    int unique_count = 0;
    uint32_t total_samples = 0;
    int i, id, u;
    uint32_t si;

    void *sp;
    __asm__ volatile("movl %%esp, %0" : "=r"(sp));
    printf("[doom_debug] preload_all_sfx entry: sp=%p\n", sp);

    for (i = 0; i < NUMSFX; i++) {
        id_to_plan[i] = -1;
    }

    for (id = 1; id < NUMSFX; id++) {
        printf("[doom_debug] loop id=%d/%d\n", id, NUMSFX);
        sfxinfo_t *sfx = &S_sfx[id];
        int found = -1;
        int lump;
        int lump_len;
        const unsigned char *raw;
        uint32_t orig_rate;
        uint32_t orig_count;
        uint32_t out_samples;

        if (!sfx->name || !sfx->name[0]) {
            continue;
        }
        /* S_Init() initializes sfx->lumpnum to -1 *after* I_InitSound() runs,
           so always resolve the lump directly from its sfx name/link here. */
        lump = I_GetSfxLumpNum(sfx->link ? sfx->link : sfx);
        if (lump < 0) {
            continue;
        }

        for (u = 0; u < unique_count; u++) {
            if (plans[u].lumpnum == lump) {
                found = u;
                break;
            }
        }
        if (found >= 0) {
            id_to_plan[id] = found;
            continue;
        }

        lump_len = W_LumpLength(lump);
        if (lump_len <= 8) {
            continue;
        }
        printf("[doom_debug] caching lump %d for sfx %d (%s)...\n", lump, id, sfx->name);
        raw = (const unsigned char *)W_CacheLumpNum(lump, PU_STATIC);
        printf("[doom_debug] cached lump %d ok\n", lump);
        if (!raw) {
            continue;
        }

        orig_rate = (uint32_t)raw[2] | ((uint32_t)raw[3] << 8);
        orig_count = (uint32_t)raw[4] | ((uint32_t)raw[5] << 8) |
                     ((uint32_t)raw[6] << 16) | ((uint32_t)raw[7] << 24);
        if (orig_count > (uint32_t)(lump_len - 8)) {
            orig_count = (uint32_t)(lump_len - 8);
        }
        if (orig_count == 0) {
            Z_ChangeTag((void *)raw, PU_CACHE);
            continue;
        }

        out_samples = (orig_rate <= 11025u) ? (orig_count * 2u) : orig_count;
        if (out_samples > UINT32_MAX - total_samples) {
            Z_ChangeTag((void *)raw, PU_CACHE);
            continue;
        }

        plans[unique_count].lumpnum = lump;
        plans[unique_count].orig_rate = orig_rate;
        plans[unique_count].orig_count = orig_count;
        plans[unique_count].out_samples = out_samples;
        plans[unique_count].pool_offset = total_samples;
        total_samples += out_samples;
        id_to_plan[id] = unique_count;
        unique_count++;
    }

    if (total_samples == 0) {
        return;
    }

    s_sfx_pool = (int16_t *)malloc((size_t)total_samples * sizeof(int16_t));
    for (u = 0; u < unique_count; u++) {
        const unsigned char *raw = (const unsigned char *)W_CacheLumpNum(plans[u].lumpnum, PU_STATIC);
        if (raw && s_sfx_pool) {
            int16_t *dst = s_sfx_pool + plans[u].pool_offset;
            uint32_t count = plans[u].orig_count;
            if (plans[u].orig_rate <= 11025u) {
                for (si = 0; si < count; si++) {
                    int16_t s16 = ((int16_t)raw[8 + si] - 128) << 8;
                    dst[si * 2u + 0u] = s16;
                    dst[si * 2u + 1u] = s16;
                }
            } else {
                for (si = 0; si < count; si++) {
                    dst[si] = ((int16_t)raw[8 + si] - 128) << 8;
                }
            }
        }
        if (raw) {
            Z_ChangeTag((void *)raw, PU_CACHE);
        }
    }

    if (!s_sfx_pool) {
        return;
    }

    for (id = 1; id < NUMSFX; id++) {
        int plan_idx = id_to_plan[id];
        if (plan_idx >= 0) {
            s_cached_sfx[id].samples = s_sfx_pool + plans[plan_idx].pool_offset;
            s_cached_sfx[id].num_samples = plans[plan_idx].out_samples;
        }
    }
}

void I_InitSound(void) {
    int i;
    printf("[doom_debug] I_InitSound: audio_init()...\n");
    audio_init();
    printf("[doom_debug] I_InitSound: audio_init() ok\n");
    memset(s_cached_sfx, 0, sizeof(s_cached_sfx));
    if (s_sfx_pool) {
        free(s_sfx_pool);
        s_sfx_pool = NULL;
    }
    for (i = 0; i < NUM_CHANNELS; i++) {
        s_channels[i].stream = NULL;
        s_channels[i].handle = 0;
        s_channels[i].sfx_id = 0;
        s_channels[i].vol = 0;
    }
    printf("[doom_debug] I_InitSound: preload_all_sfx()...\n");
    preload_all_sfx();
    printf("[doom_debug] I_InitSound: preload_all_sfx() ok\n");
    s_sound_inited = true;
}

void I_UpdateSound(void) {
    int i;
    if (!s_sound_inited) return;

    /* Clean up idle streams so audio_core's stream list never accumulates dead streams */
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (s_channels[i].stream && !s_channels[i].stream->is_playing) {
            audio_stream_destroy(s_channels[i].stream);
            s_channels[i].stream = NULL;
            s_channels[i].handle = 0;
        }
    }

}

static float doom_vol_to_frac(int vol) {
    float volume_frac = (float)vol / 15.0f;
    if (volume_frac > 1.0f) volume_frac = (float)vol / 127.0f;
    if (volume_frac > 1.0f) volume_frac = 1.0f;
    if (volume_frac < 0.0f) volume_frac = 0.0f;
    return volume_frac;
}

void I_SubmitSound(void) {
    /* Kernel IRQ0 drives audio_poll() at 1000 Hz; avoid extra per-frame syscall overhead. */
}

void I_ShutdownSound(void) {
    int i;
    if (!s_sound_inited) return;

    for (i = 0; i < NUM_CHANNELS; i++) {
        if (s_channels[i].stream) {
            audio_stream_destroy(s_channels[i].stream);
            s_channels[i].stream = NULL;
            s_channels[i].handle = 0;
        }
    }

    if (s_sfx_pool) {
        free(s_sfx_pool);
        s_sfx_pool = NULL;
    }
    memset(s_cached_sfx, 0, sizeof(s_cached_sfx));

    audio_stop_all();
    sb16_stop();
    s_sound_inited = false;
}

void I_SetChannels(void) {
}

int I_GetSfxLumpNum(sfxinfo_t *sfx) {
    char namebuf[16];
    int lump;

    sprintf(namebuf, "ds%s", sfx->name);
    lump = W_CheckNumForName(namebuf);
    if (lump < 0) {
        lump = W_CheckNumForName("dspistol");
    }
    return lump;
}

int I_StartSound(int id, int vol, int sep, int pitch, int priority) {
    int i;
    int free_chan;
    float volume_frac;
    audio_stream_t *stream;
    int handle;

    (void)sep;
    (void)pitch;
    (void)priority;

    if (!s_sound_inited || id <= 0 || id >= NUMSFX) {
        return -1;
    }

    if (!s_cached_sfx[id].samples || s_cached_sfx[id].num_samples == 0) {
        return -1;
    }

    /* Find an idle channel or reuse the oldest */
    free_chan = -1;
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (!s_channels[i].stream || !s_channels[i].stream->is_playing) {
            free_chan = i;
            break;
        }
    }
    if (free_chan < 0) {
        free_chan = 0;
    }

    if (s_channels[free_chan].stream) {
        audio_stream_destroy(s_channels[free_chan].stream);
        s_channels[free_chan].stream = NULL;
    }

    /* Create stream using Fast-Path 2: AUDIO_FORMAT_S16, 1 channel, 22050 Hz */
    stream = audio_stream_create(AUDIO_FORMAT_S16, 1, 22050,
                                 s_cached_sfx[id].samples,
                                 s_cached_sfx[id].num_samples * sizeof(int16_t));
    if (!stream) {
        return -1;
    }

    volume_frac = doom_vol_to_frac(vol);

    audio_stream_set_volume(stream, volume_frac);
    audio_stream_play(stream);

    handle = s_next_handle++;
    if (s_next_handle <= 0) s_next_handle = 1;

    s_channels[free_chan].stream = stream;
    s_channels[free_chan].handle = handle;
    s_channels[free_chan].sfx_id = id;
    s_channels[free_chan].vol = vol;

    return handle;
}

void I_StopSound(int handle) {
    int i;
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (s_channels[i].handle == handle && s_channels[i].stream) {
            audio_stream_stop(s_channels[i].stream);
            audio_stream_destroy(s_channels[i].stream);
            s_channels[i].stream = NULL;
            s_channels[i].handle = 0;
            break;
        }
    }
}

int I_SoundIsPlaying(int handle) {
    int i;
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (s_channels[i].handle == handle && s_channels[i].stream) {
            return s_channels[i].stream->is_playing ? 1 : 0;
        }
    }
    return 0;
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch) {
    int i;
    (void)sep;
    (void)pitch;
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (s_channels[i].handle == handle && s_channels[i].stream) {
            if (s_channels[i].vol != vol) {
                s_channels[i].vol = vol;
                audio_stream_set_volume(s_channels[i].stream, doom_vol_to_frac(vol));
            }
            break;
        }
    }
}

/* Music Stubs */
void I_InitMusic(void) {}
void I_ShutdownMusic(void) {}
void I_SetMusicVolume(int volume) { (void)volume; }
void I_PauseSong(int handle) { (void)handle; }
void I_ResumeSong(int handle) { (void)handle; }
int  I_RegisterSong(void *data) { (void)data; return 1; }
void I_PlaySong(int handle, int looping) { (void)handle; (void)looping; }
void I_StopSong(int handle) { (void)handle; }
void I_UnRegisterSong(int handle) { (void)handle; }
