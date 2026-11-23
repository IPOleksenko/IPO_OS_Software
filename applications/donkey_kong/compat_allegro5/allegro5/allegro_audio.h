#pragma once
#include "allegro.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ALLEGRO_SAMPLE ALLEGRO_SAMPLE;

typedef struct {
    int _id;
    int _index;
} ALLEGRO_SAMPLE_ID;

typedef enum {
    ALLEGRO_PLAYMODE_ONCE = 0x100,
    ALLEGRO_PLAYMODE_LOOP = 0x101,
} ALLEGRO_PLAYMODE;

bool al_install_audio(void);
bool al_reserve_samples(int reserve_samples);
ALLEGRO_SAMPLE *al_load_sample(const char *filename);
void al_destroy_sample(ALLEGRO_SAMPLE *spl);
bool al_play_sample(ALLEGRO_SAMPLE *spl, float gain, float pan, float speed,
                    ALLEGRO_PLAYMODE loop, ALLEGRO_SAMPLE_ID *ret_id);
void al_stop_sample(ALLEGRO_SAMPLE_ID *spl_id);

#ifdef __cplusplus
}
#endif
