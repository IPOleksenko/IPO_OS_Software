#ifndef _STDINT_H
#define _STDINT_H

typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef int32_t            intptr_t;
typedef uint32_t           uintptr_t;

typedef int64_t            intmax_t;
typedef uint64_t           uintmax_t;

typedef int8_t             int_least8_t;
typedef int16_t            int_least16_t;
typedef int32_t            int_least32_t;
typedef int64_t            int_least64_t;
typedef uint8_t            uint_least8_t;
typedef uint16_t           uint_least16_t;
typedef uint32_t           uint_least32_t;
typedef uint64_t           uint_least64_t;

typedef int8_t             int_fast8_t;
typedef int32_t            int_fast16_t;
typedef int32_t            int_fast32_t;
typedef int64_t            int_fast64_t;
typedef uint8_t            uint_fast8_t;
typedef uint32_t           uint_fast16_t;
typedef uint32_t           uint_fast32_t;
typedef uint64_t           uint_fast64_t;

#ifndef INT8_C
#define INT8_C(value) value
#endif
#ifndef UINT8_C
#define UINT8_C(value) value##U
#endif
#ifndef INT16_C
#define INT16_C(value) value
#endif
#ifndef UINT16_C
#define UINT16_C(value) value##U
#endif
#ifndef INT32_C
#define INT32_C(value) value
#endif
#ifndef UINT32_C
#define UINT32_C(value) value##U
#endif
#ifndef INT64_C
#define INT64_C(value) value##LL
#endif
#ifndef UINT64_C
#define UINT64_C(value) value##ULL
#endif

#define INT8_MIN   (-128)
#define INT8_MAX   127
#define UINT8_MAX  255

#define INT16_MIN  (-32768)
#define INT16_MAX  32767
#define UINT16_MAX 65535

#define INT32_MIN  (-2147483647 - 1)
#define INT32_MAX  2147483647
#define UINT32_MAX 4294967295U

#define INT64_MIN  (-9223372036854775807LL - 1)
#define INT64_MAX  9223372036854775807LL
#define UINT64_MAX 18446744073709551615ULL

#ifndef SIZE_MAX
#define SIZE_MAX   4294967295U
#endif
#ifndef UINTPTR_MAX
#define UINTPTR_MAX 4294967295U
#endif
#ifndef INTPTR_MAX
#define INTPTR_MAX  2147483647
#endif
#ifndef INTPTR_MIN
#define INTPTR_MIN  (-2147483647 - 1)
#endif
#ifndef INTMAX_MIN
#define INTMAX_MIN  INT64_MIN
#endif
#ifndef INTMAX_MAX
#define INTMAX_MAX  INT64_MAX
#endif
#ifndef UINTMAX_MAX
#define UINTMAX_MAX UINT64_MAX
#endif
#ifndef PTRDIFF_MIN
#define PTRDIFF_MIN INT32_MIN
#endif
#ifndef PTRDIFF_MAX
#define PTRDIFF_MAX INT32_MAX
#endif

#endif
