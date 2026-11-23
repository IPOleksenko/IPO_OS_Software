#ifndef _GIT_COMPAT_LIBGEN_H
#define _GIT_COMPAT_LIBGEN_H

#ifdef __cplusplus
extern "C" {
#endif

char *gitbasename(char *path);
char *gitdirname(char *path);

#ifdef __cplusplus
}
#endif

#endif
