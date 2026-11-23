#ifndef TETRIS_COMPAT_SDL_H
#define TETRIS_COMPAT_SDL_H

#include <stddef.h>
#include <stdint.h>
#include <math.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t  Uint8;
typedef int8_t   Sint8;
typedef uint16_t Uint16;
typedef int16_t  Sint16;
typedef uint32_t Uint32;
typedef int32_t  Sint32;
typedef uint64_t Uint64;
typedef int64_t  Sint64;

typedef int SDL_bool;
#define SDL_FALSE 0
#define SDL_TRUE  1

#define SDL_ALPHA_OPAQUE      255
#define SDL_ALPHA_TRANSPARENT 0

#define SDL_INIT_TIMER          0x00000001u
#define SDL_INIT_AUDIO          0x00000010u
#define SDL_INIT_VIDEO          0x00000020u
#define SDL_INIT_JOYSTICK       0x00000200u
#define SDL_INIT_GAMECONTROLLER 0x00002000u
#define SDL_INIT_EVENTS         0x00004000u
#define SDL_INIT_EVERYTHING     0x0000FFFFu

#define SDL_HINT_VIDEO_HIGHDPI_DISABLED "SDL_VIDEO_HIGHDPI_DISABLED"

#define SDL_WINDOWPOS_CENTERED 0x2FFF0000
#define SDL_WINDOW_SHOWN       0x00000004u
#define SDL_WINDOW_RESIZABLE   0x00000020u
#define SDL_WINDOW_FULLSCREEN  0x00000001u

#define SDL_RENDERER_ACCELERATED  0x00000002u
#define SDL_RENDERER_PRESENTVSYNC 0x00000004u

#define SDL_PIXELFORMAT_RGBA8888 0x16462004u
#define SDL_TEXTUREACCESS_STATIC 0
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_TEXTUREACCESS_TARGET 2

typedef enum {
    SDL_BLENDMODE_NONE  = 0,
    SDL_BLENDMODE_BLEND = 1
} SDL_BlendMode;

typedef struct SDL_Rect {
    int x, y, w, h;
} SDL_Rect;

typedef struct SDL_Point {
    int x, y;
} SDL_Point;

typedef struct SDL_Color {
    Uint8 r, g, b, a;
} SDL_Color;

typedef struct SDL_Surface {
    int w;
    int h;
    int pitch;
    void *pixels;
    SDL_bool is_premultiplied;
    SDL_bool borrowed_pixels;
    SDL_bool has_alpha_mod;
    Uint8 alpha_mod;
    uint64_t cache_key;
} SDL_Surface;

typedef struct SDL_Window   SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture  SDL_Texture;

typedef struct SDL_RWops {
    const void *data;
    size_t size;
    size_t pos;
    SDL_bool own_mem;
} SDL_RWops;

typedef Sint32 SDL_Keycode;
typedef SDL_Keycode SDL_KeyCode;

#define SDLK_SCANCODE_MASK (1 << 30)
#define SDL_SCANCODE_TO_KEYCODE(X) ((X) | SDLK_SCANCODE_MASK)

enum {
    SDLK_UNKNOWN = 0,
    SDLK_RETURN = '\r',
    SDLK_ESCAPE = '\x1B',
    SDLK_BACKSPACE = '\b',
    SDLK_TAB = '\t',
    SDLK_SPACE = ' ',
    SDLK_QUOTE = '\'',
    SDLK_COMMA = ',',
    SDLK_MINUS = '-',
    SDLK_PERIOD = '.',
    SDLK_SLASH = '/',
    SDLK_0 = '0', SDLK_1 = '1', SDLK_2 = '2', SDLK_3 = '3', SDLK_4 = '4',
    SDLK_5 = '5', SDLK_6 = '6', SDLK_7 = '7', SDLK_8 = '8', SDLK_9 = '9',
    SDLK_SEMICOLON = ';',
    SDLK_EQUALS = '=',
    SDLK_LEFTBRACKET = '[',
    SDLK_BACKSLASH = '\\',
    SDLK_RIGHTBRACKET = ']',
    SDLK_BACKQUOTE = '`',
    SDLK_a = 'a', SDLK_b = 'b', SDLK_c = 'c', SDLK_d = 'd', SDLK_e = 'e',
    SDLK_f = 'f', SDLK_g = 'g', SDLK_h = 'h', SDLK_i = 'i', SDLK_j = 'j',
    SDLK_k = 'k', SDLK_l = 'l', SDLK_m = 'm', SDLK_n = 'n', SDLK_o = 'o',
    SDLK_p = 'p', SDLK_q = 'q', SDLK_r = 'r', SDLK_s = 's', SDLK_t = 't',
    SDLK_u = 'u', SDLK_v = 'v', SDLK_w = 'w', SDLK_x = 'x', SDLK_y = 'y',
    SDLK_z = 'z',
    SDLK_DELETE = '\x7F',

    SDLK_CAPSLOCK = SDL_SCANCODE_TO_KEYCODE(57),
    SDLK_RIGHT = SDL_SCANCODE_TO_KEYCODE(79),
    SDLK_LEFT  = SDL_SCANCODE_TO_KEYCODE(80),
    SDLK_DOWN  = SDL_SCANCODE_TO_KEYCODE(81),
    SDLK_UP    = SDL_SCANCODE_TO_KEYCODE(82),

    SDLK_KP_DIVIDE   = SDL_SCANCODE_TO_KEYCODE(84),
    SDLK_KP_MULTIPLY = SDL_SCANCODE_TO_KEYCODE(85),
    SDLK_KP_MINUS    = SDL_SCANCODE_TO_KEYCODE(86),
    SDLK_KP_PLUS     = SDL_SCANCODE_TO_KEYCODE(87),
    SDLK_KP_ENTER    = SDL_SCANCODE_TO_KEYCODE(88),
    SDLK_KP_1 = SDL_SCANCODE_TO_KEYCODE(89),
    SDLK_KP_2 = SDL_SCANCODE_TO_KEYCODE(90),
    SDLK_KP_3 = SDL_SCANCODE_TO_KEYCODE(91),
    SDLK_KP_4 = SDL_SCANCODE_TO_KEYCODE(92),
    SDLK_KP_5 = SDL_SCANCODE_TO_KEYCODE(93),
    SDLK_KP_6 = SDL_SCANCODE_TO_KEYCODE(94),
    SDLK_KP_7 = SDL_SCANCODE_TO_KEYCODE(95),
    SDLK_KP_8 = SDL_SCANCODE_TO_KEYCODE(96),
    SDLK_KP_9 = SDL_SCANCODE_TO_KEYCODE(97),
    SDLK_KP_0 = SDL_SCANCODE_TO_KEYCODE(98),
    SDLK_KP_PERIOD = SDL_SCANCODE_TO_KEYCODE(99),

    SDLK_LCTRL  = SDL_SCANCODE_TO_KEYCODE(224),
    SDLK_LSHIFT = SDL_SCANCODE_TO_KEYCODE(225),
    SDLK_LALT   = SDL_SCANCODE_TO_KEYCODE(226),
    SDLK_RCTRL  = SDL_SCANCODE_TO_KEYCODE(228),
    SDLK_RSHIFT = SDL_SCANCODE_TO_KEYCODE(229),
    SDLK_RALT   = SDL_SCANCODE_TO_KEYCODE(230)
};

#define SDL_BUTTON_LEFT   1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT  3
#define SDL_BUTTON(X) (1u << ((X) - 1))
#define SDL_BUTTON_LMASK SDL_BUTTON(SDL_BUTTON_LEFT)

typedef enum {
    SDL_QUIT = 0x100,
    SDL_KEYDOWN = 0x300,
    SDL_KEYUP = 0x301,
    SDL_MOUSEMOTION = 0x400,
    SDL_MOUSEBUTTONDOWN = 0x401,
    SDL_MOUSEBUTTONUP = 0x402,
    SDL_MOUSEWHEEL = 0x403
} SDL_EventType;

typedef struct SDL_Keysym {
    Uint32 scancode;
    SDL_Keycode sym;
    Uint16 mod;
} SDL_Keysym;

typedef struct SDL_KeyboardEvent {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    Uint8 state;
    Uint8 repeat;
    SDL_Keysym keysym;
} SDL_KeyboardEvent;

typedef struct SDL_MouseMotionEvent {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    Uint32 which;
    Uint32 state;
    Sint32 x, y;
    Sint32 xrel, yrel;
} SDL_MouseMotionEvent;

typedef struct SDL_MouseButtonEvent {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    Uint32 which;
    Uint8 button;
    Uint8 state;
    Uint8 clicks;
    Sint32 x, y;
} SDL_MouseButtonEvent;

typedef struct SDL_MouseWheelEvent {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    Uint32 which;
    Sint32 x, y;
    Uint32 direction;
} SDL_MouseWheelEvent;

typedef union SDL_Event {
    Uint32 type;
    SDL_KeyboardEvent key;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_MouseWheelEvent wheel;
    uint8_t padding[64];
} SDL_Event;

int SDL_Init(Uint32 flags);
void SDL_Quit(void);
SDL_bool SDL_SetHint(const char *name, const char *value);
const char *SDL_GetError(void);
Uint32 SDL_GetTicks(void);
void SDL_Delay(Uint32 ms);

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags);
void SDL_DestroyWindow(SDL_Window *window);
SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags);
void SDL_DestroyRenderer(SDL_Renderer *renderer);

SDL_Texture *SDL_CreateTexture(SDL_Renderer *renderer, Uint32 format, int access, int w, int h);
SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer *renderer, SDL_Surface *surface);
void SDL_DestroyTexture(SDL_Texture *texture);
void SDL_FreeSurface(SDL_Surface *surface);
SDL_Surface *SDL_CreateRGBSurfaceWithFormat(Uint32 flags, int w, int h, int depth, Uint32 format);

int SDL_SetRenderTarget(SDL_Renderer *renderer, SDL_Texture *texture);
int SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
int SDL_SetRenderDrawBlendMode(SDL_Renderer *renderer, SDL_BlendMode blendMode);
int SDL_SetTextureBlendMode(SDL_Texture *texture, SDL_BlendMode blendMode);
int SDL_SetTextureAlphaMod(SDL_Texture *texture, Uint8 alpha);

int SDL_RenderClear(SDL_Renderer *renderer);
int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *srcrect, const SDL_Rect *dstrect);
int SDL_RenderDrawPoint(SDL_Renderer *renderer, int x, int y);
int SDL_RenderDrawLine(SDL_Renderer *renderer, int x1, int y1, int x2, int y2);
int SDL_RenderDrawRect(SDL_Renderer *renderer, const SDL_Rect *rect);
int SDL_RenderFillRect(SDL_Renderer *renderer, const SDL_Rect *rect);
void SDL_RenderPresent(SDL_Renderer *renderer);

int SDL_PollEvent(SDL_Event *event);
int SDL_PushEvent(SDL_Event *event);
Uint32 SDL_GetMouseState(int *x, int *y);

SDL_RWops *SDL_RWFromFile(const char *file, const char *mode);
SDL_RWops *SDL_RWFromConstMem(const void *mem, int size);
size_t SDL_RWread(SDL_RWops *context, void *ptr, size_t size, size_t maxnum);
int SDL_RWclose(SDL_RWops *context);
void SDL_FreeRW(SDL_RWops *area);

#ifdef __cplusplus
}
#endif

#endif
