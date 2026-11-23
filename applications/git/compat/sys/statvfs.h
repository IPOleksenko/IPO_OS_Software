#ifndef _GIT_COMPAT_SYS_STATVFS_H
#define _GIT_COMPAT_SYS_STATVFS_H

#include <stdint.h>

struct statvfs {
    unsigned long f_bsize;
    unsigned long f_frsize;
    uint64_t      f_blocks;
    uint64_t      f_bfree;
    uint64_t      f_bavail;
    uint64_t      f_files;
    uint64_t      f_ffree;
    uint64_t      f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    unsigned long f_namemax;
};

#ifdef __cplusplus
extern "C" {
#endif

int statvfs(const char *path, struct statvfs *buf);

#ifdef __cplusplus
}
#endif

#endif
