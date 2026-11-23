#ifndef IPO_QUAKE_COMPAT_H
#define IPO_QUAKE_COMPAT_H

#ifndef stricmp
#define stricmp strcasecmp
#endif

#ifndef strnicmp
#define strnicmp strncasecmp
#endif

/* Fallback intel long-double constants when -U__i386__ disables WinQuake asm */
#ifndef LDBL_MANT_DIG
#define LDBL_MANT_DIG   64
#define LDBL_DIG        18
#define LDBL_EPSILON    1.08420217248550443401e-19L
#define LDBL_MIN_EXP    (-16381)
#define LDBL_MIN        3.36210314311209350626e-4932L
#define LDBL_MIN_10_EXP (-4931)
#define LDBL_MAX_EXP    16384
#define LDBL_MAX        1.18973149535723176502e+4932L
#define LDBL_MAX_10_EXP 4932
#define DECIMAL_DIG     21
#endif

#endif
