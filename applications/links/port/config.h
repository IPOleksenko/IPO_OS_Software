#ifndef LINKS_IPO_CONFIG_H
#define LINKS_IPO_CONFIG_H

#define DEBUGLEVEL 0
#define NO_SIGNAL_HANDLERS 1
#define HAVE_DIRENT_H 1
#define HAVE_FCNTL_H 1
#define HAVE_GETCWD 1
#define HAVE_GETHOSTBYNAME 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_MEMCHR 1
#define HAVE_MEMCMP 1
#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMSET 1
#define HAVE_NETDB_H 1
#define HAVE_RAISE 1
#define HAVE_SNPRINTF 1
#define HAVE_STDLIB_H_X 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR 1
#define HAVE_STRTOD 1
#define HAVE_STRTOL 1
#define HAVE_STRTOUL 1
#define HAVE_SYS_SELECT_H 1
#define HAVE_SOCKLEN_T 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_VSNPRINTF 1
#define TIME_WITH_SYS_TIME 1
#define VERSION "2.22"

/* Graphics & Image Decoding Support */
#define G 1
#define GRDRV_IPO 1
#define HAVE_VOLATILE 1
#define HAVE_SETJMP_H 1
#define HAVE_MATH_H 1
#define HAVE_POW 1
#define HAVE_EXP 1
#define HAVE_LOG 1
#define HAVE_SQRT 1
#define HAVE_ZLIB 1
#define HAVE_ZLIB_H 1
#define HAVE_PNG_H 1
#define HAVE_LIBPNG 1
#define HAVE_PNG_SET_RGB_TO_GRAY 1
#define HAVE_PNG_GET_LIBPNG_VER 1
#define C_LITTLE_ENDIAN 1
#define SIZEOF_UNSIGNED_SHORT 2
#define SIZEOF_INT 4
#define SIZEOF_LONG 4
#define SIZEOF_VOID_P 4
#define HAVE_LONG_LONG 1
#define SIZEOF_UNSIGNED_LONG_LONG 8

/* SSL / TLS Support */
#define HAVE_SSL 1
#define HAVE_OPENSSL 1
#define HAVE_OPENSSL_INIT_SSL 1
#define OPENSSL_NO_STDIO 1
#define SSL_ACCEPT_INVALID_CERTIFICATE 0
#define SSL_WARN_ON_INVALID_CERTIFICATE 1
#define SSL_REJECT_INVALID_CERTIFICATE 2

/* Modern image decoding via Wuffs (JPEG, WebP, BMP, PNG, GIF) */
#define HAVE_WUFFS 1

/* JavaScript Support */
#define JS 1

#endif
