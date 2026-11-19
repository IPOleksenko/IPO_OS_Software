/**
 * i_main.c - Application entry point for DOOM on IPO_OS
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "doomdef.h"
#include "m_argv.h"
#include "d_main.h"

int main(int argc, char **argv) {
    /* Store DOOM config, saves, and screenshots inside /data/doom */
    extern int mkdir(const char *pathname, uint32_t mode);
    mkdir("/data", 0755);
    mkdir("/data/doom", 0755);
    setenv("HOME", "/data/doom", 1);

    if (access("/data/doom/doom1.wad", 0) == 0) {
        setenv("DOOMWADDIR", "/data/doom", 1);
    } else if (access("/doom1.wad", 0) == 0) {
        setenv("DOOMWADDIR", "", 1);
    } else {
        setenv("DOOMWADDIR", ".", 1);
    }

    myargc = argc;
    myargv = argv;

    D_DoomMain();

    return 0;
}
