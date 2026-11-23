#ifndef _GIT_COMPAT_GRP_H
#define _GIT_COMPAT_GRP_H

#include <sys/types.h>

struct group {
    char  *gr_name;
    char  *gr_passwd;
    gid_t  gr_gid;
    char **gr_mem;
};

#ifdef __cplusplus
extern "C" {
#endif

struct group *getgrgid(gid_t gid);
struct group *getgrnam(const char *name);

#ifdef __cplusplus
}
#endif

#endif
