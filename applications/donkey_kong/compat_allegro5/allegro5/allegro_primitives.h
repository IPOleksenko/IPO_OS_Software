#pragma once
#include "allegro.h"

#ifdef __cplusplus
extern "C" {
#endif

bool al_init_primitives_addon(void);
void al_shutdown_primitives_addon(void);
void al_draw_line(float x1, float y1, float x2, float y2, ALLEGRO_COLOR color, float thickness);
void al_draw_rectangle(float x1, float y1, float x2, float y2, ALLEGRO_COLOR color, float thickness);
void al_draw_filled_rectangle(float x1, float y1, float x2, float y2, ALLEGRO_COLOR color);
void al_draw_filled_rounded_rectangle(float x1, float y1, float x2, float y2, float rx, float ry, ALLEGRO_COLOR color);
void al_draw_circle(float cx, float cy, float r, ALLEGRO_COLOR color, float thickness);

#ifdef __cplusplus
}
#endif
