#ifndef WOLF_COMPAT_SDL1_H
#define WOLF_COMPAT_SDL1_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t  Uint8;
typedef int8_t   Sint8;
typedef uint16_t Uint16;
typedef int16_t  Sint16;
typedef uint32_t Uint32;
typedef int32_t  Sint32;

typedef Sint16 INT16;

#define SDL_INIT_TIMER      0x00000001u
#define SDL_INIT_AUDIO      0x00000010u
#define SDL_INIT_VIDEO      0x00000020u
#define SDL_INIT_JOYSTICK   0x00000200u
#define SDL_INIT_EVERYTHING 0x0000FFFFu

#define SDL_SWSURFACE   0x00000000u
#define SDL_HWSURFACE   0x00000001u
#define SDL_HWPALETTE   0x20000000u
#define SDL_DOUBLEBUF   0x40000000u
#define SDL_FULLSCREEN  0x80000000u
#define SDL_OPENGL      0x00000002u
#define SDL_OPENGLBLIT  0x0000000Au

#define SDL_LOGPAL  0x01
#define SDL_PHYSPAL 0x02

#define SDL_DISABLE 0
#define SDL_ENABLE  1
#define SDL_IGNORE  0

#define SDL_BUTTON(X)       (1u << ((X) - 1))
#define SDL_BUTTON_LEFT     1
#define SDL_BUTTON_MIDDLE   2
#define SDL_BUTTON_RIGHT    3

#define SDL_HAT_UP    0x01
#define SDL_HAT_RIGHT 0x02
#define SDL_HAT_DOWN  0x04
#define SDL_HAT_LEFT  0x08

#define SDL_MUSTLOCK(S) 0

typedef enum {
    SDL_GRAB_QUERY = -1,
    SDL_GRAB_OFF   = 0,
    SDL_GRAB_ON    = 1
} SDL_GrabMode;

typedef enum {
    SDL_NOEVENT = 0,
    SDL_KEYDOWN = 2,
    SDL_KEYUP = 3,
    SDL_MOUSEMOTION = 4,
    SDL_MOUSEBUTTONDOWN = 5,
    SDL_MOUSEBUTTONUP = 6,
    SDL_QUIT = 12
} SDL_EventType;

typedef enum {
    SDLK_UNKNOWN = 0,
    SDLK_BACKSPACE = 8,
    SDLK_TAB = 9,
    SDLK_RETURN = 13,
    SDLK_PAUSE = 19,
    SDLK_ESCAPE = 27,
    SDLK_SPACE = 32,
    SDLK_0 = 48, SDLK_1, SDLK_2, SDLK_3, SDLK_4,
    SDLK_5, SDLK_6, SDLK_7, SDLK_8, SDLK_9,
    SDLK_a = 97, SDLK_b, SDLK_c, SDLK_d, SDLK_e, SDLK_f, SDLK_g,
    SDLK_h, SDLK_i, SDLK_j, SDLK_k, SDLK_l, SDLK_m, SDLK_n,
    SDLK_o, SDLK_p, SDLK_q, SDLK_r, SDLK_s, SDLK_t, SDLK_u,
    SDLK_v, SDLK_w, SDLK_x, SDLK_y, SDLK_z,
    SDLK_DELETE = 127,
    SDLK_KP2 = 258,
    SDLK_KP4 = 260,
    SDLK_KP5 = 261,
    SDLK_KP6 = 262,
    SDLK_KP8 = 264,
    SDLK_KP_ENTER = 271,
    SDLK_UP = 273,
    SDLK_DOWN = 274,
    SDLK_RIGHT = 275,
    SDLK_LEFT = 276,
    SDLK_INSERT = 277,
    SDLK_HOME = 278,
    SDLK_END = 279,
    SDLK_PAGEUP = 280,
    SDLK_PAGEDOWN = 281,
    SDLK_F1 = 282, SDLK_F2, SDLK_F3, SDLK_F4, SDLK_F5, SDLK_F6,
    SDLK_F7, SDLK_F8, SDLK_F9, SDLK_F10, SDLK_F11, SDLK_F12,
    SDLK_NUMLOCK = 300,
    SDLK_CAPSLOCK = 301,
    SDLK_SCROLLOCK = 302,
    SDLK_RSHIFT = 303,
    SDLK_LSHIFT = 304,
    SDLK_RCTRL = 305,
    SDLK_LCTRL = 306,
    SDLK_RALT = 307,
    SDLK_LALT = 308,
    SDLK_PRINT = 316,
    SDLK_LAST = 323
} SDLKey;

typedef enum {
    KMOD_NONE   = 0x0000,
    KMOD_LSHIFT = 0x0001,
    KMOD_RSHIFT = 0x0002,
    KMOD_LCTRL  = 0x0040,
    KMOD_RCTRL  = 0x0080,
    KMOD_LALT   = 0x0100,
    KMOD_RALT   = 0x0200,
    KMOD_NUM    = 0x1000,
    KMOD_CAPS   = 0x2000,
    KMOD_SHIFT  = (KMOD_LSHIFT | KMOD_RSHIFT)
} SDLMod;

typedef struct SDL_Color {
    Uint8 r;
    Uint8 g;
    Uint8 b;
    Uint8 unused;
} SDL_Color;

typedef struct SDL_Palette {
    int ncolors;
    SDL_Color *colors;
} SDL_Palette;

typedef struct SDL_PixelFormat {
    SDL_Palette *palette;
    Uint8 BitsPerPixel;
    Uint8 BytesPerPixel;
    Uint8 Rloss, Gloss, Bloss, Aloss;
    Uint8 Rshift, Gshift, Bshift, Ashift;
    Uint32 Rmask, Gmask, Bmask, Amask;
    Uint32 colorkey;
    Uint8 alpha;
} SDL_PixelFormat;

typedef struct SDL_Rect {
    Sint16 x, y;
    Uint16 w, h;
} SDL_Rect;

typedef struct SDL_Surface {
    Uint32 flags;
    SDL_PixelFormat *format;
    int w, h;
    Uint16 pitch;
    void *pixels;
    int offset;
    SDL_Rect clip_rect;
    int refcount;
} SDL_Surface;

typedef struct SDL_VideoInfo {
    Uint32 hw_available : 1;
    Uint32 wm_available : 1;
    Uint32 blit_hw : 1;
    Uint32 blit_sw : 1;
    Uint32 video_mem;
    SDL_PixelFormat *vfmt;
} SDL_VideoInfo;

typedef struct SDL_keysym {
    Uint8 scancode;
    SDLKey sym;
    SDLMod mod;
    Uint16 unicode;
} SDL_keysym;

typedef struct SDL_KeyboardEvent {
    Uint8 type;
    Uint8 which;
    Uint8 state;
    SDL_keysym keysym;
} SDL_KeyboardEvent;

typedef struct SDL_MouseMotionEvent {
    Uint8 type;
    Uint8 which;
    Uint8 state;
    Uint16 x, y;
    Sint16 xrel, yrel;
} SDL_MouseMotionEvent;

typedef struct SDL_MouseButtonEvent {
    Uint8 type;
    Uint8 which;
    Uint8 button;
    Uint8 state;
    Uint16 x, y;
} SDL_MouseButtonEvent;

typedef union SDL_Event {
    Uint8 type;
    SDL_KeyboardEvent key;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
} SDL_Event;

typedef struct SDL_RWops {
    const void *mem;
    int size;
    int free_mem;
} SDL_RWops;

typedef struct SDL_Joystick {
    int index;
} SDL_Joystick;

typedef struct SDL_version {
    Uint8 major, minor, patch;
} SDL_version;

#define SDL_VERSION(x) do { (x)->major = 1; (x)->minor = 2; (x)->patch = 15; } while (0)

#define AUDIO_S16 0x8010u

int SDL_Init(Uint32 flags);
void SDL_Quit(void);
const char *SDL_GetError(void);

const SDL_VideoInfo *SDL_GetVideoInfo(void);
void SDL_WM_SetCaption(const char *title, const char *icon);
SDL_GrabMode SDL_WM_GrabInput(SDL_GrabMode mode);
int SDL_ShowCursor(int toggle);
SDL_Surface *SDL_SetVideoMode(int width, int height, int bpp, Uint32 flags);
SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height, int depth,
                                  Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask);
SDL_Surface *SDL_ConvertSurface(SDL_Surface *src, SDL_PixelFormat *fmt, Uint32 flags);
void SDL_FreeSurface(SDL_Surface *surface);
int SDL_LockSurface(SDL_Surface *surface);
void SDL_UnlockSurface(SDL_Surface *surface);
int SDL_SetColors(SDL_Surface *surface, SDL_Color *colors, int firstcolor, int ncolors);
int SDL_SetPalette(SDL_Surface *surface, int flags, SDL_Color *colors, int firstcolor, int ncolors);
Uint32 SDL_MapRGB(const SDL_PixelFormat *fmt, Uint8 r, Uint8 g, Uint8 b);
int SDL_FillRect(SDL_Surface *dst, SDL_Rect *dstrect, Uint32 color);
int SDL_BlitSurface(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect);
int SDL_Flip(SDL_Surface *screen);
int SDL_SaveBMP(SDL_Surface *surface, const char *file);

void SDL_GL_SwapBuffers(void);

Uint32 SDL_GetTicks(void);
void SDL_Delay(Uint32 ms);

int SDL_PollEvent(SDL_Event *event);
int SDL_WaitEvent(SDL_Event *event);
Uint8 SDL_EventState(Uint8 type, int state);
Uint8 *SDL_GetKeyState(int *numkeys);
SDLMod SDL_GetModState(void);
Uint8 SDL_GetMouseState(int *x, int *y);
void SDL_WarpMouse(Uint16 x, Uint16 y);

int SDL_NumJoysticks(void);
SDL_Joystick *SDL_JoystickOpen(int device_index);
void SDL_JoystickClose(SDL_Joystick *joystick);
void SDL_JoystickUpdate(void);
Sint16 SDL_JoystickGetAxis(SDL_Joystick *joystick, int axis);
Uint8 SDL_JoystickGetButton(SDL_Joystick *joystick, int button);
Uint8 SDL_JoystickGetHat(SDL_Joystick *joystick, int hat);
int SDL_JoystickNumButtons(SDL_Joystick *joystick);
int SDL_JoystickNumHats(SDL_Joystick *joystick);

SDL_RWops *SDL_RWFromMem(void *mem, int size);

/* IPO_OS Wolf3D direct presenter hook called from CRT_DAC */
void ipo_wolf_present(const uint8_t *pixels, int w, int h, int pitch, const SDL_Color *pal);

#ifdef __cplusplus
}
#endif

#endif
