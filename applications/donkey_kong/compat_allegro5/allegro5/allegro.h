#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ALLEGRO_DISPLAY ALLEGRO_DISPLAY;
typedef struct ALLEGRO_BITMAP ALLEGRO_BITMAP;
typedef struct ALLEGRO_EVENT_QUEUE ALLEGRO_EVENT_QUEUE;
typedef struct ALLEGRO_EVENT_SOURCE ALLEGRO_EVENT_SOURCE;
typedef struct ALLEGRO_TIMER ALLEGRO_TIMER;
typedef struct ALLEGRO_CONFIG ALLEGRO_CONFIG;

typedef struct {
    float r, g, b, a;
} ALLEGRO_COLOR;

enum {
    ALLEGRO_WINDOWED          = 1 << 0,
    ALLEGRO_FULLSCREEN        = 1 << 1,
    ALLEGRO_RESIZABLE         = 1 << 4,
    ALLEGRO_FULLSCREEN_WINDOW = 1 << 9,
};

enum {
    ALLEGRO_NO_PRESERVE_TEXTURE = 1 << 8,
};

enum {
    ALLEGRO_FLIP_HORIZONTAL = 1 << 0,
    ALLEGRO_FLIP_VERTICAL   = 1 << 1,
};

enum {
    ALLEGRO_KEY_A = 1, ALLEGRO_KEY_B, ALLEGRO_KEY_C, ALLEGRO_KEY_D,
    ALLEGRO_KEY_E, ALLEGRO_KEY_F, ALLEGRO_KEY_G, ALLEGRO_KEY_H,
    ALLEGRO_KEY_I, ALLEGRO_KEY_J, ALLEGRO_KEY_K, ALLEGRO_KEY_L,
    ALLEGRO_KEY_M, ALLEGRO_KEY_N, ALLEGRO_KEY_O, ALLEGRO_KEY_P,
    ALLEGRO_KEY_Q, ALLEGRO_KEY_R, ALLEGRO_KEY_S, ALLEGRO_KEY_T,
    ALLEGRO_KEY_U, ALLEGRO_KEY_V, ALLEGRO_KEY_W, ALLEGRO_KEY_X,
    ALLEGRO_KEY_Y, ALLEGRO_KEY_Z,
    ALLEGRO_KEY_0, ALLEGRO_KEY_1, ALLEGRO_KEY_2, ALLEGRO_KEY_3,
    ALLEGRO_KEY_4, ALLEGRO_KEY_5, ALLEGRO_KEY_6, ALLEGRO_KEY_7,
    ALLEGRO_KEY_8, ALLEGRO_KEY_9,
    ALLEGRO_KEY_ESCAPE = 59,
    ALLEGRO_KEY_TAB = 64,
    ALLEGRO_KEY_ENTER = 67,
    ALLEGRO_KEY_SPACE = 75,
    ALLEGRO_KEY_LEFT = 82,
    ALLEGRO_KEY_RIGHT = 83,
    ALLEGRO_KEY_UP = 84,
    ALLEGRO_KEY_DOWN = 85,
    ALLEGRO_KEY_MAX = 227
};

enum {
    ALLEGRO_EVENT_KEY_DOWN = 10,
    ALLEGRO_EVENT_KEY_UP = 12,
    ALLEGRO_EVENT_TIMER = 30,
    ALLEGRO_EVENT_DISPLAY_CLOSE = 42,
    ALLEGRO_EVENT_DISPLAY_RESIZE = 41,
};

typedef struct {
    unsigned int type;
    ALLEGRO_EVENT_SOURCE *source;
    double timestamp;
} ALLEGRO_ANY_EVENT;

typedef struct {
    unsigned int type;
    ALLEGRO_DISPLAY *source;
    double timestamp;
    int x, y, width, height;
} ALLEGRO_DISPLAY_EVENT;

typedef struct {
    unsigned int type;
    ALLEGRO_EVENT_SOURCE *source;
    double timestamp;
    ALLEGRO_DISPLAY *display;
    int keycode;
    unsigned int unichar;
    unsigned int modifiers;
    bool repeat;
} ALLEGRO_KEYBOARD_EVENT;

typedef struct {
    unsigned int type;
    ALLEGRO_TIMER *source;
    double timestamp;
    int64_t count;
} ALLEGRO_TIMER_EVENT;

typedef union {
    unsigned int type;
    ALLEGRO_ANY_EVENT any;
    ALLEGRO_DISPLAY_EVENT display;
    ALLEGRO_KEYBOARD_EVENT keyboard;
    ALLEGRO_TIMER_EVENT timer;
} ALLEGRO_EVENT;

bool al_init(void);
bool al_install_keyboard(void);
void al_uninstall_keyboard(void);

void al_set_new_display_flags(int flags);
ALLEGRO_DISPLAY *al_create_display(int w, int h);
void al_destroy_display(ALLEGRO_DISPLAY *display);
int al_get_display_width(ALLEGRO_DISPLAY *display);
int al_get_display_height(ALLEGRO_DISPLAY *display);
bool al_acknowledge_resize(ALLEGRO_DISPLAY *display);
void al_flip_display(void);
void al_set_target_backbuffer(ALLEGRO_DISPLAY *display);
ALLEGRO_EVENT_SOURCE *al_get_display_event_source(ALLEGRO_DISPLAY *display);

int al_get_new_bitmap_flags(void);
void al_set_new_bitmap_flags(int flags);
ALLEGRO_BITMAP *al_create_bitmap(int w, int h);
ALLEGRO_BITMAP *al_load_bitmap(const char *filename);
void al_destroy_bitmap(ALLEGRO_BITMAP *bitmap);
void al_set_target_bitmap(ALLEGRO_BITMAP *bitmap);
void al_clear_to_color(ALLEGRO_COLOR color);
ALLEGRO_COLOR al_map_rgb(unsigned char r, unsigned char g, unsigned char b);
void al_draw_bitmap_region(ALLEGRO_BITMAP *bitmap, float sx, float sy, float sw, float sh,
                           float dx, float dy, int flags);
void al_draw_scaled_bitmap(ALLEGRO_BITMAP *bitmap,
                           float sx, float sy, float sw, float sh,
                           float dx, float dy, float dw, float dh, int flags);

ALLEGRO_EVENT_QUEUE *al_create_event_queue(void);
void al_destroy_event_queue(ALLEGRO_EVENT_QUEUE *queue);
void al_register_event_source(ALLEGRO_EVENT_QUEUE *queue, ALLEGRO_EVENT_SOURCE *source);
bool al_event_queue_is_empty(ALLEGRO_EVENT_QUEUE *queue);
void al_wait_for_event(ALLEGRO_EVENT_QUEUE *queue, ALLEGRO_EVENT *ret_event);
ALLEGRO_EVENT_SOURCE *al_get_keyboard_event_source(void);

ALLEGRO_TIMER *al_create_timer(double speed_secs);
void al_destroy_timer(ALLEGRO_TIMER *timer);
void al_start_timer(ALLEGRO_TIMER *timer);
ALLEGRO_EVENT_SOURCE *al_get_timer_event_source(ALLEGRO_TIMER *timer);

ALLEGRO_CONFIG *al_create_config(void);
void al_destroy_config(ALLEGRO_CONFIG *config);
ALLEGRO_CONFIG *al_load_config_file(const char *filename);
bool al_save_config_file(const char *filename, const ALLEGRO_CONFIG *config);
const char *al_get_config_value(const ALLEGRO_CONFIG *config, const char *section, const char *key);
void al_set_config_value(ALLEGRO_CONFIG *config, const char *section, const char *key, const char *value);

#ifdef __cplusplus
}
#endif
