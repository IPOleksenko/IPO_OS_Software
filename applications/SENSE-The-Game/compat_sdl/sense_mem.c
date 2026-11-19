/*
 * Freestanding memory primitives used by SENSE: The Game compatibility layers.
 */

#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (n >= 16u) {
        size_t dwords = n >> 2;
        size_t tail = n & 3u;
        __asm__ volatile(
            "cld\n\t"
            "rep movsl\n\t"
            : "+D"(d), "+S"(s), "+c"(dwords)
            :
            : "memory");
        n = tail;
    }
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *d = (uint8_t *)s;
    uint8_t b = (uint8_t)c;
    if (n >= 16u) {
        uint32_t fill32 = (uint32_t)b * 0x01010101u;
        size_t dwords = n >> 2;
        size_t tail = n & 3u;
        __asm__ volatile(
            "cld\n\t"
            "rep stosl\n\t"
            : "+D"(d), "+c"(dwords)
            : "a"(fill32)
            : "memory");
        n = tail;
    }
    while (n--) {
        *d++ = b;
    }
    return s;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) return dest;
    if ((uintptr_t)d < (uintptr_t)s || (uintptr_t)d >= (uintptr_t)s + n) {
        return memcpy(dest, src, n);
    }
    d += n;
    s += n;
    while (n--) {
        *--d = *--s;
    }
    return dest;
}
