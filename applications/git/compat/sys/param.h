#ifndef _GIT_COMPAT_SYS_PARAM_H
#define _GIT_COMPAT_SYS_PARAM_H

#include <limits.h>

#ifndef MAXPATHLEN
#define MAXPATHLEN PATH_MAX
#endif

#ifndef NOFILE
#define NOFILE 256
#endif

#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234
#define BIG_ENDIAN    4321
#define BYTE_ORDER    LITTLE_ENDIAN
#endif

#endif
