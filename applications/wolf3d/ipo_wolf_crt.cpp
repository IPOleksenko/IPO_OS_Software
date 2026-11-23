#include <SDL/SDL.h>

extern SDL_Surface *screenBuffer;
extern SDL_Color curpal[256];

void CRT_Init(int width) {
    (void)width;
}

void CRT_DAC(void) {
    if (!screenBuffer || !screenBuffer->pixels) return;
    ipo_wolf_present((const uint8_t *)screenBuffer->pixels,
                     screenBuffer->w, screenBuffer->h, screenBuffer->pitch,
                     curpal);
}

void CRT_Screenshot(void) {
}
