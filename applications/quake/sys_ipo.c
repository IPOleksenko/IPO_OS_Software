#include "quakedef.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <system/state.h>
#include <system/timer.h>

extern void ipo_in_pump_events(void);
extern void ipo_snd_pump(void);
extern int ipo_snd_shutdown_pending(void);

qboolean isDedicated = false;

static FILE **sys_handles = NULL;
static int    sys_handles_cap = 0;
static qboolean s_time_init = false;
static uint32_t s_time_base_ms = 0;

static int findhandle(void) {
    for (int i = 1; i < sys_handles_cap; i++) {
        if (!sys_handles[i]) return i;
    }
    int new_cap = sys_handles_cap > 0 ? sys_handles_cap * 2 : 16;
    if (new_cap <= sys_handles_cap || (size_t)new_cap > SIZE_MAX / sizeof(FILE *)) {
        Sys_Error("out of handles");
        return -1;
    }
    FILE **grown = (FILE **)realloc(sys_handles, (size_t)new_cap * sizeof(FILE *));
    if (!grown) {
        Sys_Error("out of handles");
        return -1;
    }
    for (int i = sys_handles_cap; i < new_cap; i++) {
        grown[i] = NULL;
    }
    int slot = (sys_handles_cap > 1) ? sys_handles_cap : 1;
    sys_handles = grown;
    sys_handles_cap = new_cap;
    return slot;
}

static int filelength(FILE *f) {
    int pos = ftell(f);
    fseek(f, 0, SEEK_END);
    int end = ftell(f);
    fseek(f, pos, SEEK_SET);
    return end;
}

static char *resolve_quake_path_alloc(const char *in) {
    if (!in) return NULL;

    const char *rel = in;
    if (strncmp(rel, "/data/quake/", 12) == 0) {
        rel += 12;
    } else if (strncmp(rel, "./", 2) == 0) {
        rel += 2;
    } else if (rel[0] == '/') {
        size_t len = strlen(rel);
        char *out = (char *)malloc(len + 1u);
        if (out) memcpy(out, rel, len + 1u);
        return out;
    }

    size_t rlen = strlen(rel);
    const char *prefix = "/data/quake/";
    size_t plen = 12u;
    if (rlen > SIZE_MAX - plen - 1u) return NULL;
    char *out = (char *)malloc(plen + rlen + 1u);
    if (!out) return NULL;
    memcpy(out, prefix, plen);
    for (size_t i = 0; i < rlen; i++) {
        out[plen + i] = (char)tolower((unsigned char)rel[i]);
    }
    out[plen + rlen] = '\0';
    return out;
}

int Sys_FileOpenRead(char *path, int *hndl) {
    int i = findhandle();
    FILE *f = fopen(path, "rb");
    if (!f) {
        char *resolved = resolve_quake_path_alloc(path);
        if (resolved) {
            f = fopen(resolved, "rb");
            free(resolved);
        }
    }
    if (!f) {
        *hndl = -1;
        return -1;
    }
    sys_handles[i] = f;
    *hndl = i;
    return filelength(f);
}

int Sys_FileOpenWrite(char *path) {
    int i = findhandle();
    char *resolved = resolve_quake_path_alloc(path);
    FILE *f = resolved ? fopen(resolved, "wb") : NULL;
    if (resolved) free(resolved);
    if (!f) f = fopen(path, "wb");
    if (!f) {
        Sys_Error("Error opening %s", path);
    }
    sys_handles[i] = f;
    return i;
}

void Sys_FileClose(int handle) {
    if (handle > 0 && handle < sys_handles_cap && sys_handles[handle]) {
        fclose(sys_handles[handle]);
        sys_handles[handle] = NULL;
    }
}

void Sys_FileSeek(int handle, int position) {
    if (handle > 0 && handle < sys_handles_cap && sys_handles[handle]) {
        fseek(sys_handles[handle], position, SEEK_SET);
    }
}

int Sys_FileRead(int handle, void *dest, int count) {
    if (handle > 0 && handle < sys_handles_cap && sys_handles[handle]) {
        return (int)fread(dest, 1, (size_t)count, sys_handles[handle]);
    }
    return 0;
}

int Sys_FileWrite(int handle, void *data, int count) {
    if (handle > 0 && handle < sys_handles_cap && sys_handles[handle]) {
        return (int)fwrite(data, 1, (size_t)count, sys_handles[handle]);
    }
    return 0;
}

int Sys_FileTime(char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        char *resolved = resolve_quake_path_alloc(path);
        if (resolved) {
            f = fopen(resolved, "rb");
            free(resolved);
        }
    }
    if (f) {
        fclose(f);
        return 1;
    }
    return -1;
}

void Sys_mkdir(char *path) {
    char *resolved = resolve_quake_path_alloc(path);
    if (resolved) {
        mkdir(resolved, 0755);
        free(resolved);
    }
}

void Sys_MakeCodeWriteable(unsigned long startaddr, unsigned long length) {
    (void)startaddr;
    (void)length;
}

void Sys_DebugLog(char *file, char *fmt, ...) {
    (void)file; (void)fmt;
}

static char *sys_vformat_alloc(const char *fmt, va_list ap) {
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int need = vsnprintf(NULL, 0, fmt, ap_copy);
    va_end(ap_copy);
    if (need < 0) return NULL;
    size_t sz = (size_t)need + 1u;
    char *buf = (char *)malloc(sz);
    if (!buf) return NULL;
    vsnprintf(buf, sz, fmt, ap);
    return buf;
}

void Sys_Error(char *error, ...) {
    va_list argptr;
    va_start(argptr, error);
    char *msg = sys_vformat_alloc(error, argptr);
    va_end(argptr);

    serial_printf("[quake] Sys_Error: %s\n", msg ? msg : error);
    VID_Shutdown();
    printf("Sys_Error: %s\n", msg ? msg : error);
    free(msg);
    exit(1);
}

void Sys_Printf(char *fmt, ...) {
    va_list argptr;
    va_start(argptr, fmt);
    char *msg = sys_vformat_alloc(fmt, argptr);
    va_end(argptr);
    if (msg) {
        serial_printf("%s", msg);
        free(msg);
    }
}

void Sys_Quit(void) {
    Host_Shutdown();
    exit(0);
}

double Sys_FloatTime(void) {
    uint32_t now = timer_millis();
    if (!s_time_init) {
        s_time_base_ms = now;
        s_time_init = true;
    }
    return (double)(now - s_time_base_ms) / 1000.0;
}

char *Sys_ConsoleInput(void) {
    return NULL;
}

void Sys_Sleep(void) {
    ipo_snd_pump();
    usleep(1000u);
}

void Sys_SendKeyEvents(void) {
    ipo_in_pump_events();
    if (ipo_snd_shutdown_pending()) {
        Sys_Quit();
    }
}

void Sys_LowFPPrecision(void) {}
void Sys_HighFPPrecision(void) {}
void Sys_SetFPCW(void) {}

int main(int argc, char **argv) {
    static quakeparms_t parms;

    memset(&parms, 0, sizeof(parms));
    parms.memsize = 16 * 1024 * 1024;
    parms.membase = malloc((size_t)parms.memsize);
    if (!parms.membase) {
        parms.memsize = 8 * 1024 * 1024;
        parms.membase = malloc((size_t)parms.memsize);
    }
    if (!parms.membase) {
        printf("Quake: out of memory\n");
        return 1;
    }
    parms.basedir = "/data/quake";
    parms.cachedir = "";

    COM_InitArgv(argc, argv);
    parms.argc = com_argc;
    parms.argv = com_argv;

    serial_printf("[quake] Host_Init mem=%d\n", parms.memsize);
    Host_Init(&parms);

    double oldtime = Sys_FloatTime() - 0.1;
    while (1) {
        double newtime = Sys_FloatTime();
        double dt = newtime - oldtime;
        if (dt < 0.0) dt = 0.0;
        if (dt > 0.1) dt = 0.1;
        oldtime = newtime;

        Host_Frame((float)dt);
        ipo_snd_pump();
    }
    return 0;
}
