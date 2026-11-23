#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "py/mpconfig.h"
#include "py/misc.h"
#include "py/mphal.h"

#define CHAR_CTRL_C (3)
#define CHAR_CTRL_D (4)

int readline(vstr_t *line, const char *prompt) {
    if (prompt) {
        fputs(prompt, stdout);
        fflush(stdout);
    }
    vstr_reset(line);
    size_t read_bytes = 0;
    for (;;) {
        int ch = fgetc(stdin);
        if (ch == EOF) {
            if (read_bytes == 0) {
                return CHAR_CTRL_D;
            }
            break;
        }
        read_bytes++;
        if (ch == '\n') {
            break;
        }
        if (ch != '\r') {
            vstr_add_byte(line, ch);
        }
    }
    return 0;
}

void mp_hal_stdio_mode_raw(void) {}
void mp_hal_stdio_mode_orig(void) {}
