#ifndef MARIO_COMPAT_SDL_H
#define MARIO_COMPAT_SDL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDL_INIT_TIMER 0x00000001u
#define SDL_INIT_AUDIO 0x00000010u
#define SDL_INIT_VIDEO 0x00000020u

#define SDL_WINDOWPOS_CENTERED 0x2FFF0000
#define SDL_WINDOW_SHOWN       0x00000004u

#define SDL_RENDERER_ACCELERATED 0x00000002u

#define SDL_TRUE  1
#define SDL_FALSE 0

typedef uint8_t  Uint8;
typedef uint16_t Uint16;
typedef int16_t  Sint16;
typedef uint32_t Uint32;
typedef int32_t  Sint32;

typedef enum {
    SDL_BLENDMODE_NONE  = 0x00000000,
    SDL_BLENDMODE_BLEND = 0x00000001
} SDL_BlendMode;

typedef enum {
    SDL_FLIP_NONE       = 0x00000000,
    SDL_FLIP_HORIZONTAL = 0x00000001,
    SDL_FLIP_VERTICAL   = 0x00000002
} SDL_RendererFlip;

typedef enum {
    SDL_QUIT        = 0x100,
    SDL_WINDOWEVENT = 0x200,
    SDL_KEYDOWN     = 0x300,
    SDL_KEYUP       = 0x301,
    SDL_MOUSEMOTION = 0x400,
    SDL_MOUSEBUTTONDOWN = 0x401,
    SDL_MOUSEBUTTONUP   = 0x402,
    SDL_MOUSEWHEEL      = 0x403
} SDL_EventType;

enum {
    SDL_WINDOWEVENT_NONE       = 0,
    SDL_WINDOWEVENT_FOCUS_LOST = 13
};

#define SDL_BUTTON_LEFT  1
#define SDL_BUTTON_RIGHT 3

enum {
    SDLK_UNKNOWN   = 0,
    SDLK_BACKSPACE = 8,
    SDLK_TAB       = 9,
    SDLK_RETURN    = 13,
    SDLK_ESCAPE    = 27,
    SDLK_SPACE     = 32,
    SDLK_0 = 48, SDLK_1, SDLK_2, SDLK_3, SDLK_4,
    SDLK_5, SDLK_6, SDLK_7, SDLK_8, SDLK_9,
    SDLK_a = 97, SDLK_b, SDLK_c, SDLK_d, SDLK_e, SDLK_f, SDLK_g,
    SDLK_h, SDLK_i, SDLK_j, SDLK_k, SDLK_l, SDLK_m, SDLK_n,
    SDLK_o, SDLK_p, SDLK_q, SDLK_r, SDLK_s, SDLK_t, SDLK_u,
    SDLK_v, SDLK_w, SDLK_x, SDLK_y, SDLK_z,
    SDLK_RIGHT     = 0x4000004Fu,
    SDLK_LEFT      = 0x40000050u,
    SDLK_DOWN      = 0x40000051u,
    SDLK_UP        = 0x40000052u,
    SDLK_KP_ENTER  = 0x40000058u,
    SDLK_LCTRL     = 0x400000E0u,
    SDLK_LSHIFT    = 0x400000E1u,
    SDLK_RCTRL     = 0x400000E4u,
    SDLK_RSHIFT    = 0x400000E5u
};

typedef struct SDL_Rect {
    int x, y, w, h;
} SDL_Rect;

typedef struct SDL_Point {
    int x, y;
} SDL_Point;

typedef struct SDL_PixelFormat {
    uint32_t format;
} SDL_PixelFormat;

typedef struct SDL_Surface {
    int w, h;
    int pitch;
    const SDL_PixelFormat *format;
    void *pixels;
    bool has_colorkey;
    uint8_t ck_r, ck_g, ck_b;
} SDL_Surface;

typedef struct SDL_Texture SDL_Texture;
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;

typedef struct SDL_Keysym {
    int scancode;
    int sym;
    Uint16 mod;
} SDL_Keysym;

typedef struct SDL_KeyboardEvent {
    Uint32 type;
    Uint32 timestamp;
    SDL_Keysym keysym;
} SDL_KeyboardEvent;

typedef struct SDL_WindowEvent {
    Uint32 type;
    Uint32 timestamp;
    Uint8 event;
} SDL_WindowEvent;

typedef struct SDL_MouseButtonEvent {
    Uint32 type;
    Uint32 timestamp;
    Uint8 button;
    Sint32 x, y;
} SDL_MouseButtonEvent;

typedef struct SDL_MouseWheelEvent {
    Uint32 type;
    Uint32 timestamp;
    Sint32 x, y;
} SDL_MouseWheelEvent;

typedef union SDL_Event {
    Uint32 type;
    SDL_WindowEvent window;
    SDL_KeyboardEvent key;
    SDL_MouseButtonEvent button;
    SDL_MouseWheelEvent wheel;
} SDL_Event;

int SDL_Init(Uint32 flags);
void SDL_Quit(void);

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags);
void SDL_DestroyWindow(SDL_Window *window);
void SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *icon);

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags);
void SDL_DestroyRenderer(SDL_Renderer *renderer);

int SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
int SDL_SetRenderDrawBlendMode(SDL_Renderer *renderer, SDL_BlendMode blendMode);
int SDL_RenderClear(SDL_Renderer *renderer);
int SDL_RenderFillRect(SDL_Renderer *renderer, const SDL_Rect *rect);
int SDL_RenderDrawRect(SDL_Renderer *renderer, const SDL_Rect *rect);
int SDL_RenderDrawLine(SDL_Renderer *renderer, int x1, int y1, int x2, int y2);
void SDL_RenderPresent(SDL_Renderer *renderer);

SDL_Surface *SDL_LoadBMP(const char *file);
void SDL_FreeSurface(SDL_Surface *surface);
Uint32 SDL_MapRGB(const SDL_PixelFormat *format, Uint8 r, Uint8 g, Uint8 b);
int SDL_SetColorKey(SDL_Surface *surface, int flag, Uint32 key);

SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer *renderer, SDL_Surface *surface);
void SDL_DestroyTexture(SDL_Texture *texture);
int SDL_QueryTexture(SDL_Texture *texture, Uint32 *format, int *access, int *w, int *h);
int SDL_SetTextureColorMod(SDL_Texture *texture, Uint8 r, Uint8 g, Uint8 b);
int SDL_SetTextureAlphaMod(SDL_Texture *texture, Uint8 alpha);
int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *srcrect, const SDL_Rect *dstrect);
int SDL_RenderCopyEx(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *srcrect,
                     const SDL_Rect *dstrect, double angle, const SDL_Point *center, SDL_RendererFlip flip);

int SDL_PollEvent(SDL_Event *event);
Uint32 SDL_GetTicks(void);
void SDL_Delay(Uint32 ms);
Uint32 SDL_GetMouseState(int *x, int *y);

#ifdef __cplusplus
}
#endif

#endif
