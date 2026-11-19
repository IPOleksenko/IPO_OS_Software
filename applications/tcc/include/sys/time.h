#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <stdint.h>

struct timeval {
    int64_t tv_sec;
    long tv_usec;
};

int gettimeofday(struct timeval *tv, void *tz);

#endif
