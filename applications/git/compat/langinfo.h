#ifndef _GIT_COMPAT_LANGINFO_H
#define _GIT_COMPAT_LANGINFO_H

typedef int nl_item;
#define CODESET 1

static inline char *nl_langinfo(nl_item item) {
    (void)item;
    return (char *)"UTF-8";
}

#endif
