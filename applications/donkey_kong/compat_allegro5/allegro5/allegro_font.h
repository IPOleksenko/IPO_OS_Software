#pragma once
#include "allegro.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ALLEGRO_FONT ALLEGRO_FONT;

enum {
    ALLEGRO_ALIGN_LEFT = 0,
    ALLEGRO_ALIGN_CENTRE = 1,
    ALLEGRO_ALIGN_RIGHT = 2,
};

bool al_init_font_addon(void);
void al_shutdown_font_addon(void);
ALLEGRO_FONT *al_create_builtin_font(void);
void al_destroy_font(ALLEGRO_FONT *f);
void al_draw_text(const ALLEGRO_FONT *font, ALLEGRO_COLOR color, float x, float y, int flags, const char *text);
void al_draw_multiline_text(const ALLEGRO_FONT *font, ALLEGRO_COLOR color,
                            float x, float y, float max_width, float line_height,
                            int flags, const char *text);

#ifdef __cplusplus
}
#endif
