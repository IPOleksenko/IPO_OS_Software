#ifndef SENSE_COMPAT_SDL_H
#define SENSE_COMPAT_SDL_H

#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif
#include <stdarg.h>
#include <ctype.h>

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

#define SDL_ALPHA_OPAQUE 255
#define SDL_ALPHA_TRANSPARENT 0

#ifndef SDL_min
#define SDL_min(x, y) (((x) < (y)) ? (x) : (y))
#endif

#define SDL_INIT_TIMER          0x00000001u
#define SDL_INIT_AUDIO          0x00000010u
#define SDL_INIT_VIDEO          0x00000020u
#define SDL_INIT_JOYSTICK       0x00000200u
#define SDL_INIT_GAMECONTROLLER 0x00002000u
#define SDL_INIT_EVENTS         0x00004000u
#define SDL_INIT_EVERYTHING     0x0000FFFFu

#define SDL_HINT_APP_NAME                       "SDL_APP_NAME"
#define SDL_HINT_AUDIO_DEVICE_APP_NAME          "SDL_AUDIO_DEVICE_APP_NAME"
#define SDL_HINT_AUDIO_RESAMPLING_MODE          "SDL_AUDIO_RESAMPLING_MODE"
#define SDL_HINT_ORIENTATIONS                   "SDL_ORIENTATIONS"
#define SDL_HINT_ANDROID_TRAP_BACK_BUTTON       "SDL_ANDROID_TRAP_BACK_BUTTON"
#define SDL_HINT_DIRECTINPUT_ENABLED            "SDL_DIRECTINPUT_ENABLED"
#define SDL_HINT_XINPUT_ENABLED                 "SDL_XINPUT_ENABLED"
#define SDL_HINT_ENABLE_STEAM_CONTROLLERS       "SDL_ENABLE_STEAM_CONTROLLERS"
#define SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS "SDL_JOYSTICK_HIDAPI_COMBINE_JOY_CONS"
#define SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE       "SDL_JOYSTICK_HIDAPI_GAMECUBE"
#define SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS       "SDL_JOYSTICK_HIDAPI_JOY_CONS"
#define SDL_HINT_JOYSTICK_HIDAPI_LUNA           "SDL_JOYSTICK_HIDAPI_LUNA"
#define SDL_HINT_JOYSTICK_HIDAPI_NINTENDO_CLASSIC "SDL_JOYSTICK_HIDAPI_NINTENDO_CLASSIC"
#define SDL_HINT_JOYSTICK_HIDAPI_PS3            "SDL_JOYSTICK_HIDAPI_PS3"
#define SDL_HINT_JOYSTICK_HIDAPI_PS4            "SDL_JOYSTICK_HIDAPI_PS4"
#define SDL_HINT_JOYSTICK_HIDAPI_PS5            "SDL_JOYSTICK_HIDAPI_PS5"
#define SDL_HINT_JOYSTICK_HIDAPI_SHIELD         "SDL_JOYSTICK_HIDAPI_SHIELD"
#define SDL_HINT_JOYSTICK_HIDAPI_STADIA         "SDL_JOYSTICK_HIDAPI_STADIA"
#define SDL_HINT_JOYSTICK_HIDAPI_STEAM          "SDL_JOYSTICK_HIDAPI_STEAM"
#define SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK      "SDL_JOYSTICK_HIDAPI_STEAMDECK"
#define SDL_HINT_JOYSTICK_HIDAPI_SWITCH         "SDL_JOYSTICK_HIDAPI_SWITCH"
#define SDL_HINT_JOYSTICK_HIDAPI_WII            "SDL_JOYSTICK_HIDAPI_WII"
#define SDL_HINT_JOYSTICK_HIDAPI_XBOX           "SDL_JOYSTICK_HIDAPI_XBOX"
#define SDL_HINT_JOYSTICK_HIDAPI_XBOX_360       "SDL_JOYSTICK_HIDAPI_XBOX_360"
#define SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_WIRELESS "SDL_JOYSTICK_HIDAPI_XBOX_360_WIRELESS"
#define SDL_HINT_JOYSTICK_HIDAPI_XBOX_ONE       "SDL_JOYSTICK_HIDAPI_XBOX_ONE"
#define SDL_HINT_JOYSTICK_IOKIT                 "SDL_JOYSTICK_IOKIT"
#define SDL_HINT_JOYSTICK_MFI                   "SDL_JOYSTICK_MFI"
#define SDL_HINT_JOYSTICK_RAWINPUT              "SDL_JOYSTICK_RAWINPUT"
#define SDL_HINT_JOYSTICK_RAWINPUT_CORRELATE_XINPUT "SDL_JOYSTICK_RAWINPUT_CORRELATE_XINPUT"
#define SDL_HINT_JOYSTICK_WGI                   "SDL_JOYSTICK_WGI"

typedef enum {
    SDL_LOG_CATEGORY_APPLICATION = 0,
    SDL_LOG_CATEGORY_ERROR,
    SDL_LOG_CATEGORY_ASSERT,
    SDL_LOG_CATEGORY_SYSTEM,
    SDL_LOG_CATEGORY_AUDIO,
    SDL_LOG_CATEGORY_VIDEO,
    SDL_LOG_CATEGORY_RENDER,
    SDL_LOG_CATEGORY_INPUT
} SDL_LogCategory;

#define SDL_WINDOWPOS_CENTERED 0x2FFF0000
#define SDL_WINDOW_SHOWN              0x00000004u
#define SDL_WINDOW_RESIZABLE          0x00000020u
#define SDL_WINDOW_FULLSCREEN         0x00000001u
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001u

#define SDL_RENDERER_ACCELERATED  0x00000002u
#define SDL_RENDERER_PRESENTVSYNC 0x00000004u

typedef enum {
    SDL_BLENDMODE_NONE = 0,
    SDL_BLENDMODE_BLEND = 1
} SDL_BlendMode;

typedef struct SDL_Rect {
    int x, y, w, h;
} SDL_Rect;

typedef struct SDL_Point {
    int x, y;
} SDL_Point;

typedef struct SDL_FPoint {
    float x, y;
} SDL_FPoint;

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

typedef struct SDL_RWops {
    const uint8_t *data;
    size_t size;
    size_t pos;
    SDL_bool own_mem;
} SDL_RWops;

typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;
typedef struct SDL_Joystick SDL_Joystick;
typedef struct SDL_GameController SDL_GameController;
typedef Sint32 SDL_JoystickID;
typedef Sint64 SDL_TouchID;
typedef Sint64 SDL_FingerID;
typedef Sint32 SDL_Keycode;

typedef enum {
    SDLK_UNKNOWN = 0,
    SDLK_ESCAPE  = 27,
    SDLK_SPACE   = ' ',
    SDLK_a       = 'a',
    SDLK_d       = 'd',
    SDLK_f       = 'f',
    SDLK_RIGHT   = 0x4000004Fu,
    SDLK_LEFT    = 0x40000050u
} SDL_KeyCode;

typedef enum {
    SDL_CONTROLLER_BUTTON_INVALID = -1,
    SDL_CONTROLLER_BUTTON_A = 0,
    SDL_CONTROLLER_BUTTON_B,
    SDL_CONTROLLER_BUTTON_X,
    SDL_CONTROLLER_BUTTON_Y,
    SDL_CONTROLLER_BUTTON_BACK,
    SDL_CONTROLLER_BUTTON_GUIDE,
    SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_LEFTSTICK,
    SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    SDL_CONTROLLER_BUTTON_DPAD_UP,
    SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT,
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    SDL_CONTROLLER_BUTTON_MAX
} SDL_GameControllerButton;

#define SDL_QUIT                    0x100u
#define SDL_WINDOWEVENT             0x200u
#define SDL_KEYDOWN                 0x300u
#define SDL_KEYUP                   0x301u
#define SDL_CONTROLLERBUTTONDOWN    0x651u
#define SDL_CONTROLLERBUTTONUP      0x652u
#define SDL_CONTROLLERDEVICEADDED   0x653u
#define SDL_CONTROLLERDEVICEREMOVED 0x654u
#define SDL_FINGERDOWN              0x700u
#define SDL_FINGERUP                0x701u
#define SDL_FINGERMOTION            0x702u

#define SDL_WINDOWEVENT_SIZE_CHANGED 6

typedef struct SDL_Keysym {
    int scancode;
    SDL_Keycode sym;
    Uint16 mod;
    Uint32 unused;
} SDL_Keysym;

typedef struct SDL_KeyboardEvent {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    Uint8 state;
    Uint8 repeat;
    Uint8 padding2;
    Uint8 padding3;
    SDL_Keysym keysym;
} SDL_KeyboardEvent;

typedef struct SDL_WindowEvent {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    Uint8 event;
    Uint8 padding1;
    Uint8 padding2;
    Uint8 padding3;
    Sint32 data1;
    Sint32 data2;
} SDL_WindowEvent;

typedef struct SDL_ControllerDeviceEvent {
    Uint32 type;
    Uint32 timestamp;
    Sint32 which;
} SDL_ControllerDeviceEvent;

typedef struct SDL_ControllerButtonEvent {
    Uint32 type;
    Uint32 timestamp;
    SDL_JoystickID which;
    Uint8 button;
    Uint8 state;
    Uint8 padding1;
    Uint8 padding2;
} SDL_ControllerButtonEvent;

typedef struct SDL_TouchFingerEvent {
    Uint32 type;
    Uint32 timestamp;
    SDL_TouchID touchId;
    SDL_FingerID fingerId;
    float x;
    float y;
    float dx;
    float dy;
    float pressure;
    Uint32 windowID;
} SDL_TouchFingerEvent;

typedef union SDL_Event {
    Uint32 type;
    SDL_WindowEvent window;
    SDL_KeyboardEvent key;
    SDL_ControllerDeviceEvent cdevice;
    SDL_ControllerButtonEvent cbutton;
    SDL_TouchFingerEvent tfinger;
    uint8_t padding[64];
} SDL_Event;

int SDL_Init(Uint32 flags);
void SDL_Quit(void);
SDL_bool SDL_SetHint(const char *name, const char *value);
const char *SDL_GetError(void);

void SDL_Log(const char *fmt, ...);
void SDL_LogWarn(int category, const char *fmt, ...);
void SDL_LogError(int category, const char *fmt, ...);
void SDL_LogCritical(int category, const char *fmt, ...);
void SDL_LogDebug(int category, const char *fmt, ...);

int SDL_GetDisplayBounds(int displayIndex, SDL_Rect *rect);
SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags);
void SDL_DestroyWindow(SDL_Window *window);
int SDL_SetWindowFullscreen(SDL_Window *window, Uint32 flags);
void SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *icon);
void SDL_GetWindowSize(SDL_Window *window, int *w, int *h);

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags);
void SDL_DestroyRenderer(SDL_Renderer *renderer);
int SDL_RenderSetLogicalSize(SDL_Renderer *renderer, int w, int h);
int SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
int SDL_SetRenderDrawBlendMode(SDL_Renderer *renderer, SDL_BlendMode blendMode);
int SDL_RenderClear(SDL_Renderer *renderer);
int SDL_RenderFillRect(SDL_Renderer *renderer, const SDL_Rect *rect);
int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *srcrect, const SDL_Rect *dstrect);
void SDL_RenderPresent(SDL_Renderer *renderer);

SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer *renderer, SDL_Surface *surface);
void SDL_DestroyTexture(SDL_Texture *texture);
int SDL_QueryTexture(SDL_Texture *texture, Uint32 *format, int *access, int *w, int *h);
int SDL_SetTextureBlendMode(SDL_Texture *texture, SDL_BlendMode blendMode);
int SDL_SetTextureAlphaMod(SDL_Texture *texture, Uint8 alpha);

SDL_RWops *SDL_RWFromConstMem(const void *mem, int size);
void SDL_FreeRW(SDL_RWops *area);
SDL_Surface *SDL_LoadBMP_RW(SDL_RWops *src, int freesrc);
void SDL_FreeSurface(SDL_Surface *surface);

Uint32 SDL_GetTicks(void);
void SDL_Delay(Uint32 ms);
Uint64 SDL_GetPerformanceCounter(void);
Uint64 SDL_GetPerformanceFrequency(void);

int SDL_PollEvent(SDL_Event *event);

int SDL_NumJoysticks(void);
const char *SDL_JoystickNameForIndex(int device_index);
SDL_bool SDL_IsGameController(int joystick_index);
SDL_GameController *SDL_GameControllerOpen(int joystick_index);
void SDL_GameControllerClose(SDL_GameController *gamecontroller);
SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *gamecontroller);
const char *SDL_GameControllerName(SDL_GameController *gamecontroller);
SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *joystick);

/* Internal helpers shared with compat_sdl translation units */
SDL_Texture *sense_sdl_create_texture_from_bgra(SDL_Renderer *renderer, int w, int h,
                                                void *bgra_pixels, SDL_bool is_premultiplied);
void sense_sdl_set_error(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
