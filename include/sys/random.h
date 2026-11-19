/*
 * Entropy collection declaration for userspace cryptography and PRNG seeding.
 */

#ifndef _SYS_RANDOM_H
#define _SYS_RANDOM_H

#include <stddef.h>
#include <sys/types.h>

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags);

#endif
