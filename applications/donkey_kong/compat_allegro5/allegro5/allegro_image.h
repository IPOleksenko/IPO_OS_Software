#pragma once
#include "allegro.h"

#ifdef __cplusplus
extern "C" {
#endif

bool al_init_image_addon(void);
void al_shutdown_image_addon(void);
ALLEGRO_BITMAP *al_load_bitmap(const char *filename);

#ifdef __cplusplus
}
#endif
