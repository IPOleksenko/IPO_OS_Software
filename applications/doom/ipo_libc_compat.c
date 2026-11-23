/**
 * ipo_libc_compat.c - Minimal Libc compatibility layer for DOOM on IPO_OS
 *
 * Provides:
 * - Proper vsnprintf/snprintf with full precision padding (e.g. "%.3d" -> "033")
 * - Environment variable array stub
 * - Safe exit handler with text mode restoration
 */

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <syscall.h>

extern void I_ShutdownGraphics(void);

/* Default environment array for libc getenv() */
static char s_home_env[] = "HOME=/data/doom";
static char s_wad_env[] = "DOOMWADDIR=";
static char *s_default_environ[] = {
    s_home_env,
    s_wad_env,
    NULL
};
char **environ = s_default_environ;

char *sndserver_filename = "./sndserver";
int mb_used = 6;

extern void ipo_libc_run_atexit_and_fini(void);

void exit(int status) {
    ipo_libc_run_atexit_and_fini();
    I_ShutdownGraphics();
    if (status != 0) {
        printf("\n[DOOM] exit(%d) called!\n", status);
    }
    ipo_exit(status);
    for (;;) {}
}
