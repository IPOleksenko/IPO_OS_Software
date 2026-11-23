#pragma once
#include "allegro_font.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ALLEGRO_TTF_NO_KERNING = 1,
    ALLEGRO_TTF_MONOCHROME = 2,
};

bool al_init_ttf_addon(void);
void al_shutdown_ttf_addon(void);
ALLEGRO_FONT *al_load_ttf_font(const char *filename, int size, int flags);

#ifdef __cplusplus
}
#endif
