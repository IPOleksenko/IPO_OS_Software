#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <syscall.h>
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include "ipo_libc_internal.h"

/* -------------------------------------------------------------
 * Memory allocator over sbrk
 * ------------------------------------------------------------- */

#define BLOCK_MAGIC 0x4D414C43 /* "MALC" */
#define NUM_BINS    32u

typedef struct block_hdr {
    size_t size;               /* Payload size in bytes (always ALIGN8) */
    uint32_t magic;
    int is_free;
    int bin_idx;
    struct block_hdr *phys_prev;
    struct block_hdr *phys_next;
    struct block_hdr *free_prev;
    struct block_hdr *free_next;
} block_hdr_t;

#define HDR_SIZE sizeof(block_hdr_t)
#define ALIGN8(s) (((s) + 7u) & ~7u)

static block_hdr_t *heap_head = NULL;
static block_hdr_t *heap_tail = NULL;
static block_hdr_t *free_bins[NUM_BINS];
static uint32_t bin_bitmap = 0;
static volatile uint8_t heap_lock = 0;

static uint32_t size_to_bin(size_t size) {
    if (size <= 128u) {
        if (size < 8u) return 0u;
        return (uint32_t)((size >> 3) - 1u);
    }
    if (size < 256u) return 16u;
    uint32_t msb = 31u - (uint32_t)__builtin_clz((uint32_t)size);
    uint32_t b = msb + 9u;
    if (b >= NUM_BINS) b = NUM_BINS - 1u;
    return b;
}

static void bin_insert(block_hdr_t *b) {
    uint32_t idx = size_to_bin(b->size);
    b->is_free = 1;
    b->bin_idx = (int)idx;
    b->free_prev = NULL;
    b->free_next = free_bins[idx];
    if (free_bins[idx]) {
        free_bins[idx]->free_prev = b;
    }
    free_bins[idx] = b;
    bin_bitmap |= (1u << idx);
}

static void bin_remove(block_hdr_t *b) {
    if (!b->is_free) return;
    uint32_t idx = (uint32_t)b->bin_idx;
    if (b->free_prev) {
        b->free_prev->free_next = b->free_next;
    } else if (idx < NUM_BINS) {
        free_bins[idx] = b->free_next;
    }
    if (b->free_next) {
        b->free_next->free_prev = b->free_prev;
    }
    if (idx < NUM_BINS && free_bins[idx] == NULL) {
        bin_bitmap &= ~(1u << idx);
    }
    b->free_prev = NULL;
    b->free_next = NULL;
    b->is_free = 0;
}

static void heap_lock_acquire(void) {
    while (__sync_lock_test_and_set(&heap_lock, 1u)) {
        __asm__ volatile("pause");
    }
}

static void heap_lock_release(void) {
    __sync_lock_release(&heap_lock);
}

static void *heap_sbrk_reserve(size_t size) {
    if (size == 0u) return sbrk(0);

    if (size <= 0x7FFFFFFFu) {
        void *p = sbrk((intptr_t)size);
        if (p == (void *)-1 || p == NULL) {
            errno = ENOMEM;
            return NULL;
        }
        return p;
    }

    /* size > 2 GiB: split into two sbrk calls; still single-chunk since the */
    void *base = sbrk((intptr_t)0x7FFFFFFF);
    if (base == (void *)-1 || base == NULL) { errno = ENOMEM; return NULL; }
    size_t remaining = size - 0x7FFFFFFFu;
    void *next = sbrk((intptr_t)(remaining <= 0x7FFFFFFFu ? remaining : 0x7FFFFFFFu));
    if (next == (void *)-1 || next == NULL) {
        sbrk(-(intptr_t)0x7FFFFFFF); /* try to rollback first chunk */
        errno = ENOMEM;
        return NULL;
    }
    return base;
}

static block_hdr_t *split_block_remainder(block_hdr_t *blk, size_t size) {
    if (blk->size >= size + HDR_SIZE + 16u) {
        size_t rem_size = blk->size - size - HDR_SIZE;
        block_hdr_t *rem = (block_hdr_t *)((uint8_t *)blk + HDR_SIZE + size);
        rem->magic = BLOCK_MAGIC;
        rem->size = rem_size;
        rem->is_free = 0;
        rem->bin_idx = 0;
        rem->free_prev = NULL;
        rem->free_next = NULL;
        rem->phys_next = blk->phys_next;
        rem->phys_prev = blk;
        if (blk->phys_next) blk->phys_next->phys_prev = rem;
        else heap_tail = rem;
        blk->phys_next = rem;
        blk->size = size;
        bin_insert(rem);
    }
    return blk;
}

static void *malloc_locked(size_t size) {
    if (size == 0) return NULL;
    if (size > (size_t)-1 - HDR_SIZE - 7u) {
        errno = ENOMEM;
        return NULL;
    }
    size = ALIGN8(size);

    /* O(1) segregated free-list lookup */
    uint32_t b = size_to_bin(size);
    block_hdr_t *fit = NULL;

    if (bin_bitmap & (1u << b)) {
        block_hdr_t *curr = free_bins[b];
        if (b < 16u) {
            /* Exact size bin: first block matches */
            if (curr && curr->magic == BLOCK_MAGIC && curr->is_free && curr->size >= size) {
                fit = curr;
            }
        } else {
            while (curr) {
                if (curr->magic == BLOCK_MAGIC && curr->is_free && curr->size >= size) {
                    fit = curr;
                    break;
                }
                curr = curr->free_next;
            }
        }
    }

    if (!fit && b + 1u < NUM_BINS) {
        uint32_t upper_mask = bin_bitmap & (~0u << (b + 1u));
        if (upper_mask != 0u) {
            uint32_t b2 = (uint32_t)__builtin_ctz(upper_mask);
            block_hdr_t *curr = free_bins[b2];
            if (b2 == NUM_BINS - 1u) {
                while (curr) {
                    if (curr->magic == BLOCK_MAGIC && curr->is_free && curr->size >= size) {
                        fit = curr;
                        break;
                    }
                    curr = curr->free_next;
                }
            } else if (curr && curr->magic == BLOCK_MAGIC && curr->is_free && curr->size >= size) {
                fit = curr;
            }
        }
    }

    if (fit) {
        bin_remove(fit);
        split_block_remainder(fit, size);
        return (void *)((uint8_t *)fit + HDR_SIZE);
    }

    /* Request memory from kernel */
    size_t alloc_size = ALIGN8(size + HDR_SIZE);
    if (alloc_size < 65536u) {
        alloc_size = 65536u; /* Allocate in 64KB chunks to reduce syscall overhead */
    }

    void *p = heap_sbrk_reserve(alloc_size);
    if (p == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    block_hdr_t *new_block = (block_hdr_t *)p;
    new_block->magic = BLOCK_MAGIC;
    new_block->size = alloc_size - HDR_SIZE;
    new_block->is_free = 0;
    new_block->bin_idx = 0;
    new_block->phys_next = NULL;
    new_block->free_prev = NULL;
    new_block->free_next = NULL;

    bool contiguous = heap_tail != NULL &&
        (uint8_t *)heap_tail + HDR_SIZE + heap_tail->size == (uint8_t *)new_block;

    new_block->phys_prev = contiguous ? heap_tail : NULL;

    if (!heap_head) {
        heap_head = new_block;
    } else if (heap_tail && contiguous) {
        heap_tail->phys_next = new_block;
    }
    heap_tail = new_block;

    split_block_remainder(new_block, size);
    return (void *)((uint8_t *)new_block + HDR_SIZE);
}

void *malloc(size_t size) {
    heap_lock_acquire();
    void *result = malloc_locked(size);
    heap_lock_release();
    return result;
}

static void free_locked(void *ptr) {
    if (!ptr) return;
    block_hdr_t *block = (block_hdr_t *)((uint8_t *)ptr - HDR_SIZE);
    if (block->magic != BLOCK_MAGIC || block->is_free) return;

    /* Coalesce with phys_next if free and physically contiguous */
    block_hdr_t *nxt = block->phys_next;
    if (nxt && nxt->magic == BLOCK_MAGIC && nxt->is_free &&
        (uint8_t *)block + HDR_SIZE + block->size == (uint8_t *)nxt) {
        bin_remove(nxt);
        block->size += HDR_SIZE + nxt->size;
        block->phys_next = nxt->phys_next;
        if (nxt->phys_next) nxt->phys_next->phys_prev = block;
        else heap_tail = block;
        nxt->magic = 0;
    }

    /* Coalesce with phys_prev if free and physically contiguous */
    block_hdr_t *prv = block->phys_prev;
    if (prv && prv->magic == BLOCK_MAGIC && prv->is_free &&
        (uint8_t *)prv + HDR_SIZE + prv->size == (uint8_t *)block) {
        bin_remove(prv);
        prv->size += HDR_SIZE + block->size;
        prv->phys_next = block->phys_next;
        if (block->phys_next) block->phys_next->phys_prev = prv;
        else heap_tail = prv;
        block->magic = 0;
        block = prv;
    }

    bin_insert(block);
}

typedef struct {
    void *aligned;
    void *raw;
    size_t user_size;
} std_aligned_entry_t;

static std_aligned_entry_t *s_aligned_tbl = NULL;
static size_t s_aligned_count = 0u;
static size_t s_aligned_cap = 0u;

static int find_aligned_locked(const void *ptr) {
    if (!ptr || s_aligned_count == 0u || !s_aligned_tbl) return -1;
    for (size_t i = 0u; i < s_aligned_cap; i++) {
        if (s_aligned_tbl[i].aligned == ptr) return (int)i;
    }
    return -1;
}

void free(void *ptr) {
    if (!ptr) return;
    heap_lock_acquire();
    int aidx = find_aligned_locked(ptr);
    if (aidx >= 0) {
        void *raw = s_aligned_tbl[aidx].raw;
        s_aligned_tbl[aidx].aligned = NULL;
        s_aligned_tbl[aidx].raw = NULL;
        s_aligned_tbl[aidx].user_size = 0u;
        s_aligned_count--;
        free_locked(raw);
        heap_lock_release();
        return;
    }
    free_locked(ptr);
    heap_lock_release();
}

void *calloc(size_t nmemb, size_t size) {
    if (nmemb && size > (size_t)-1 / nmemb) {
        errno = ENOMEM;
        return NULL;
    }
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t new_size) {
    if (!ptr) return malloc(new_size);
    if (new_size == 0) {
        free(ptr);
        return NULL;
    }
    if (new_size > (size_t)-1 - HDR_SIZE - 7u) {
        errno = ENOMEM;
        return NULL;
    }
    size_t aligned_new = ALIGN8(new_size);

    heap_lock_acquire();
    int aidx = find_aligned_locked(ptr);
    if (aidx >= 0) {
        size_t old_sz = s_aligned_tbl[aidx].user_size;
        void *new_ptr = malloc_locked(aligned_new);
        if (!new_ptr) {
            heap_lock_release();
            return NULL;
        }
        size_t copy_sz = old_sz < new_size ? old_sz : new_size;
        if (copy_sz > 0u) memcpy(new_ptr, ptr, copy_sz);
        void *raw = s_aligned_tbl[aidx].raw;
        s_aligned_tbl[aidx].aligned = NULL;
        s_aligned_tbl[aidx].raw = NULL;
        s_aligned_tbl[aidx].user_size = 0u;
        s_aligned_count--;
        free_locked(raw);
        heap_lock_release();
        return new_ptr;
    }

    block_hdr_t *block = (block_hdr_t *)((uint8_t *)ptr - HDR_SIZE);
    if (block->magic != BLOCK_MAGIC || block->is_free) {
        heap_lock_release();
        return NULL;
    }

    if (block->size >= aligned_new) {
        heap_lock_release();
        return ptr;
    }

    /* Try in-place expansion into contiguous free phys_next block */
    block_hdr_t *nxt = block->phys_next;
    if (nxt && nxt->magic == BLOCK_MAGIC && nxt->is_free &&
        (uint8_t *)block + HDR_SIZE + block->size == (uint8_t *)nxt &&
        block->size + HDR_SIZE + nxt->size >= aligned_new) {
        bin_remove(nxt);
        block->size += HDR_SIZE + nxt->size;
        block->phys_next = nxt->phys_next;
        if (nxt->phys_next) nxt->phys_next->phys_prev = block;
        else heap_tail = block;
        nxt->magic = 0;
        split_block_remainder(block, aligned_new);
        heap_lock_release();
        return ptr;
    }

    void *new_ptr = malloc_locked(aligned_new);
    if (!new_ptr) {
        heap_lock_release();
        return NULL;
    }
    memcpy(new_ptr, ptr, block->size);
    free_locked(ptr);
    heap_lock_release();
    return new_ptr;
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
    if (!memptr) return EINVAL;
    if (alignment < sizeof(void *) || (alignment & (alignment - 1u)) != 0u) {
        return EINVAL;
    }
    if (alignment <= 8u) {
        void *p = malloc(size ? size : 1u);
        if (!p) return ENOMEM;
        *memptr = p;
        return 0;
    }
    size_t alloc_sz = size ? size : 1u;
    if (alignment > SIZE_MAX - alloc_sz) return ENOMEM;

    heap_lock_acquire();
    void *raw = malloc_locked(alloc_sz + alignment);
    if (!raw) {
        heap_lock_release();
        return ENOMEM;
    }
    size_t mask = alignment - 1u;
    void *aligned = (void *)(((uintptr_t)raw + mask) & ~mask);
    int slot = -1;
    for (size_t i = 0u; i < s_aligned_cap; i++) {
        if (!s_aligned_tbl[i].aligned) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        size_t ncap = s_aligned_cap ? (s_aligned_cap * 2u) : 32u;
        std_aligned_entry_t *ntbl = (std_aligned_entry_t *)malloc_locked(ncap * sizeof(std_aligned_entry_t));
        if (!ntbl) {
            free_locked(raw);
            heap_lock_release();
            return ENOMEM;
        }
        if (s_aligned_tbl && s_aligned_cap > 0u) {
            memcpy(ntbl, s_aligned_tbl, s_aligned_cap * sizeof(std_aligned_entry_t));
            free_locked(s_aligned_tbl);
        }
        memset(ntbl + s_aligned_cap, 0, (ncap - s_aligned_cap) * sizeof(std_aligned_entry_t));
        s_aligned_tbl = ntbl;
        slot = (int)s_aligned_cap;
        s_aligned_cap = ncap;
    }
    s_aligned_tbl[slot].aligned = aligned;
    s_aligned_tbl[slot].raw = raw;
    s_aligned_tbl[slot].user_size = size;
    s_aligned_count++;
    heap_lock_release();
    *memptr = aligned;
    return 0;
}

void *aligned_alloc(size_t alignment, size_t size) {
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    void *p = NULL;
    int err = posix_memalign(&p, alignment, size);
    if (err != 0) {
        errno = err;
        return NULL;
    }
    return p;
}

/* -------------------------------------------------------------
 * Process Lifecycle & atexit
 * ------------------------------------------------------------- */

static void (**atexit_handlers)(void) = NULL;
static size_t atexit_count = 0;
static size_t atexit_cap = 0;
static pthread_mutex_t s_atexit_lock = PTHREAD_MUTEX_INITIALIZER;

int atexit(void (*func)(void)) {
    if (!func) return -1;
    pthread_mutex_lock(&s_atexit_lock);
    if (atexit_count >= atexit_cap) {
        size_t new_cap = (atexit_cap == 0) ? 16 : atexit_cap * 2;
        void (**new_handlers)(void) = (void (**)(void))realloc(atexit_handlers, new_cap * sizeof(void (*)(void)));
        if (!new_handlers) {
            pthread_mutex_unlock(&s_atexit_lock);
            return -1;
        }
        atexit_handlers = new_handlers;
        atexit_cap = new_cap;
    }
    atexit_handlers[atexit_count++] = func;
    pthread_mutex_unlock(&s_atexit_lock);
    return 0;
}

void __libc_fini_array(void);

void ipo_libc_run_atexit_and_fini(void) {
    /* Run atexit handlers in LIFO order safely even if handlers append more */
    for (;;) {
        pthread_mutex_lock(&s_atexit_lock);
        if (atexit_count == 0) {
            if (atexit_handlers) {
                free(atexit_handlers);
                atexit_handlers = NULL;
                atexit_cap = 0;
            }
            pthread_mutex_unlock(&s_atexit_lock);
            break;
        }
        void (*fn)(void) = atexit_handlers[--atexit_count];
        pthread_mutex_unlock(&s_atexit_lock);
        if (fn) fn();
    }
    /* Run global destructors and __cxa_finalize */
    __libc_fini_array();
}

void exit(int status) {
    ipo_libc_run_atexit_and_fini();
    /* Flush stdio streams */
    fflush(NULL);
    /* Set return value in kernel */
    ipo_set_return_int(status);
    /* Exit via kernel */
    ipo_exit(status);
    __builtin_unreachable();
}

void _exit(int status) {
    ipo_set_return_int(status);
    ipo_exit(status);
    __builtin_unreachable();
}

void abort(void) {
    fputs("Aborted\n", stderr);
    exit(134);
}

void __assert_fail(const char *assertion, const char *file, unsigned int line, const char *function) {
    fprintf(stderr, "Assertion failed: %s (%s: %s: %u)\n",
            assertion ? assertion : "unknown",
            file ? file : "unknown",
            function ? function : "unknown",
            line);
    abort();
}

/* -------------------------------------------------------------
 * Environment variables (Dynamically expandable, zero artificial limits)
 * ------------------------------------------------------------- */

static char *env[] = {
    NULL
};
__attribute__((weak)) char **environ = env;

static bool env_is_allocated = false;
static size_t env_cap = 0;
static pthread_mutex_t s_env_lock = PTHREAD_MUTEX_INITIALIZER;

static int ensure_heap_env_locked(void) {
    char **cur_env = environ ? environ : env;
    size_t count = 0;
    while (cur_env[count] != NULL) {
        count++;
    }
    if (env_is_allocated && environ == cur_env) {
        return 0;
    }
    size_t new_cap = count + 8;
    char **new_env = (char **)malloc(new_cap * sizeof(char *));
    if (!new_env) {
        errno = ENOMEM;
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        new_env[i] = strdup(cur_env[i]);
        if (!new_env[i]) {
            for (size_t k = 0; k < i; k++) free(new_env[k]);
            free(new_env);
            errno = ENOMEM;
            return -1;
        }
    }
    new_env[count] = NULL;
    environ = new_env;
    env_is_allocated = true;
    env_cap = new_cap;
    return 0;
}

char *getenv(const char *name) {
    if (!name || !*name) return NULL;
    size_t nlen = strlen(name);
    pthread_mutex_lock(&s_env_lock);
    char **cur_env = environ ? environ : env;
    char *res = NULL;
    for (char **ep = cur_env; *ep != NULL; ep++) {
        if (strncmp(*ep, name, nlen) == 0 && (*ep)[nlen] == '=') {
            res = *ep + nlen + 1;
            break;
        }
    }
    pthread_mutex_unlock(&s_env_lock);
    return res;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || strchr(name, '=') != NULL) {
        errno = EINVAL;
        return -1;
    }

    if (!value) value = "";
    size_t nlen = strlen(name);
    size_t vlen = strlen(value);
    size_t entry_len = nlen + 1 + vlen + 1;

    char *new_entry = (char *)malloc(entry_len);
    if (!new_entry) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(new_entry, name, nlen);
    new_entry[nlen] = '=';
    memcpy(new_entry + nlen + 1, value, vlen + 1);

    pthread_mutex_lock(&s_env_lock);
    if (ensure_heap_env_locked() != 0) {
        pthread_mutex_unlock(&s_env_lock);
        free(new_entry);
        return -1;
    }

    size_t count = 0;
    int found_idx = -1;
    while (environ[count] != NULL) {
        if (strncmp(environ[count], name, nlen) == 0 && environ[count][nlen] == '=') {
            found_idx = (int)count;
        }
        count++;
    }

    if (found_idx >= 0 && !overwrite) {
        pthread_mutex_unlock(&s_env_lock);
        free(new_entry);
        return 0;
    }

    if (found_idx >= 0) {
        free(environ[found_idx]);
        environ[found_idx] = new_entry;
        pthread_mutex_unlock(&s_env_lock);
        return 0;
    }

    if (count + 2 > env_cap) {
        size_t new_cap = env_cap ? env_cap * 2 : (count + 8);
        char **new_env = (char **)realloc(environ, new_cap * sizeof(char *));
        if (!new_env) {
            pthread_mutex_unlock(&s_env_lock);
            free(new_entry);
            errno = ENOMEM;
            return -1;
        }
        environ = new_env;
        env_cap = new_cap;
    }
    environ[count] = new_entry;
    environ[count + 1] = NULL;
    pthread_mutex_unlock(&s_env_lock);
    return 0;
}

int putenv(char *string) {
    if (!string || !*string) {
        errno = EINVAL;
        return -1;
    }
    char *eq = strchr(string, '=');
    if (!eq) {
        return unsetenv(string);
    }
    size_t nlen = (size_t)(eq - string);
    char *name_buf = (char *)malloc(nlen + 1);
    if (!name_buf) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(name_buf, string, nlen);
    name_buf[nlen] = '\0';
    int res = setenv(name_buf, eq + 1, 1);
    free(name_buf);
    return res;
}

int unsetenv(const char *name) {
    if (!name || !*name || strchr(name, '=') != NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&s_env_lock);
    if (ensure_heap_env_locked() != 0) {
        pthread_mutex_unlock(&s_env_lock);
        return -1;
    }

    size_t nlen = strlen(name);
    size_t i = 0;

    while (environ[i] != NULL) {
        if (strncmp(environ[i], name, nlen) == 0 && environ[i][nlen] == '=') {
            free(environ[i]);
            size_t j = i;
            while (environ[j] != NULL) {
                environ[j] = environ[j + 1];
                j++;
            }
            continue;
        }
        i++;
    }
    pthread_mutex_unlock(&s_env_lock);
    return 0;
}

int clearenv(void) {
    pthread_mutex_lock(&s_env_lock);
    if (env_is_allocated && environ) {
        for (size_t i = 0; environ[i] != NULL; i++) {
            free(environ[i]);
            environ[i] = NULL;
        }
    } else if (ensure_heap_env_locked() == 0 && environ) {
        for (size_t i = 0; environ[i] != NULL; i++) {
            free(environ[i]);
            environ[i] = NULL;
        }
    }
    pthread_mutex_unlock(&s_env_lock);
    return 0;
}

int system(const char *command) {
    if (!command || !*command) return 0;
    while (*command == ' ' || *command == '\t') command++;
    if (!*command) return 0;

    size_t argv_cap = 16;
    char **argv = (char **)malloc(argv_cap * sizeof(char *));
    if (!argv) return -1;

    size_t arg_cap = 64;
    char *arg_buf = (char *)malloc(arg_cap);
    if (!arg_buf) {
        free(argv);
        return -1;
    }

    int argc = 0;
    size_t arg_pos = 0;
    bool in_arg = false;
    char in_quote = '\0';
    const char *p = command;

    while (*p) {
        char c = *p;

        if (in_quote) {
            if (c == in_quote) {
                in_quote = '\0';
            } else if (c == '\\' && (*(p + 1) == in_quote || *(p + 1) == '\\')) {
                p++;
                c = *p;
                if (arg_pos + 1 >= arg_cap) {
                    size_t new_cap = arg_cap * 2;
                    char *ext = (char *)realloc(arg_buf, new_cap);
                    if (ext) { arg_buf = ext; arg_cap = new_cap; }
                }
                arg_buf[arg_pos++] = c;
            } else {
                if (arg_pos + 1 >= arg_cap) {
                    size_t new_cap = arg_cap * 2;
                    char *ext = (char *)realloc(arg_buf, new_cap);
                    if (ext) { arg_buf = ext; arg_cap = new_cap; }
                }
                arg_buf[arg_pos++] = c;
            }
            p++;
            in_arg = true;
            continue;
        }

        if (c == '"' || c == '\'') {
            in_quote = c;
            in_arg = true;
            p++;
            continue;
        }

        if (c == '\\' && *(p + 1) != '\0') {
            char next = *(p + 1);
            if (next == ' ' || next == '\t' || next == '"' || next == '\'' || next == '\\') {
                p++;
                c = *p;
            }
            if (arg_pos + 1 >= arg_cap) {
                size_t new_cap = arg_cap * 2;
                char *ext = (char *)realloc(arg_buf, new_cap);
                if (ext) { arg_buf = ext; arg_cap = new_cap; }
            }
            arg_buf[arg_pos++] = c;
            p++;
            in_arg = true;
            continue;
        }

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (in_arg) {
                arg_buf[arg_pos] = '\0';
                char *arg_copy = strdup(arg_buf);
                if ((size_t)argc + 2 >= argv_cap) {
                    argv_cap *= 2;
                    char **new_argv = (char **)realloc(argv, argv_cap * sizeof(char *));
                    if (new_argv) argv = new_argv;
                }
                if (arg_copy) argv[argc++] = arg_copy;
                arg_pos = 0;
                in_arg = false;
            }
            p++;
            continue;
        }

        if (arg_pos + 1 >= arg_cap) {
            size_t new_cap = arg_cap * 2;
            char *ext = (char *)realloc(arg_buf, new_cap);
            if (ext) { arg_buf = ext; arg_cap = new_cap; }
        }
        arg_buf[arg_pos++] = c;
        in_arg = true;
        p++;
    }

    if (in_arg) {
        arg_buf[arg_pos] = '\0';
        char *arg_copy = strdup(arg_buf);
        if ((size_t)argc + 2 >= argv_cap) {
            argv_cap *= 2;
            char **new_argv = (char **)realloc(argv, argv_cap * sizeof(char *));
            if (new_argv) argv = new_argv;
        }
        if (arg_copy) argv[argc++] = arg_copy;
    }
    free(arg_buf);
    argv[argc] = NULL;

    if (argc == 0) {
        free(argv);
        return 0;
    }

    int pid = ipo_exec(argv[0], argc, argv);
    int code = (pid < 0) ? -1 : ipo_get_exit_code();
    for (int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
    return code;
}

/* -------------------------------------------------------------
 * Conversion functions
 * ------------------------------------------------------------- */

int atoi(const char *nptr) {
    return (int)strtol(nptr, NULL, 10);
}

long atol(const char *nptr) {
    return strtol(nptr, NULL, 10);
}

long long atoll(const char *nptr) {
    return strtoll(nptr, NULL, 10);
}

double atof(const char *nptr) {
    return strtod(nptr, NULL);
}

long strtol(const char *nptr, char **endptr, int base) {
    if (!nptr) return 0;
    const char *s = nptr;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }

    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (base == 0) {
        base = (s[0] == '0') ? 8 : 10;
    }

    long val = 0;
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        val = val * base + d;
        s++;
    }
    if (endptr) *endptr = (char *)s;
    return neg ? -val : val;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    return (unsigned long)strtol(nptr, endptr, base);
}

long long strtoll(const char *nptr, char **endptr, int base) {
    return (long long)strtol(nptr, endptr, base);
}

unsigned long long strtoull(const char *nptr, char **endptr, int base) {
    return (unsigned long long)strtol(nptr, endptr, base);
}

double strtod(const char *nptr, char **endptr) {
    if (!nptr) return 0.0;
    const char *s = nptr;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }

    double val = 0.0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10.0 + (*s - '0');
        s++;
    }
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') {
            val += (*s - '0') * frac;
            frac *= 0.1;
            s++;
        }
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        int exp_neg = 0;
        if (*s == '-') { exp_neg = 1; s++; }
        else if (*s == '+') { s++; }
        int exp = 0;
        while (*s >= '0' && *s <= '9') {
            exp = exp * 10 + (*s - '0');
            s++;
        }
        double factor = 1.0;
        for (int i = 0; i < exp; i++) factor *= 10.0;
        if (exp_neg) val /= factor;
        else val *= factor;
    }
    if (endptr) *endptr = (char *)s;
    return neg ? -val : val;
}

float strtof(const char *nptr, char **endptr) {
    return (float)strtod(nptr, endptr);
}

long double strtold(const char *nptr, char **endptr) {
    return (long double)strtod(nptr, endptr);
}

/* -------------------------------------------------------------
 * Math & utilities
 * ------------------------------------------------------------- */

int abs(int j) {
    return (j < 0) ? -j : j;
}

long labs(long j) {
    return (j < 0) ? -j : j;
}

div_t div(int numer, int denom) {
    div_t r;
    r.quot = numer / denom;
    r.rem = numer % denom;
    return r;
}

ldiv_t ldiv(long numer, long denom) {
    ldiv_t r;
    r.quot = numer / denom;
    r.rem = numer % denom;
    return r;
}

static uint32_t rand_next = 1u;

int rand_r(unsigned int *seedp) {
    if (!seedp) return 0;
    *seedp = *seedp * 1103515245u + 12345u;
    return (int)((*seedp / 65536u) % 32768u);
}

int rand(void) {
    uint32_t old_val, next_val;
    do {
        old_val = __atomic_load_n(&rand_next, __ATOMIC_RELAXED);
        next_val = old_val * 1103515245u + 12345u;
    } while (!__atomic_compare_exchange_n(&rand_next, &old_val, next_val, false,
                                          __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    return (int)((next_val / 65536u) % 32768u);
}

void srand(unsigned int seed) {
    __atomic_store_n(&rand_next, (uint32_t)seed, __ATOMIC_RELAXED);
}

static void qsort_swap(char *a, char *b, size_t size) {
    if (a == b || size == 0) return;
    char *tmp = (char *)malloc(size);
    if (tmp) {
        memcpy(tmp, a, size);
        memcpy(a, b, size);
        memcpy(b, tmp, size);
        free(tmp);
        return;
    }
    while (size-- > 0) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static void qsort_sift_down(char *arr, size_t start, size_t end, size_t size,
                            int (*compar)(const void *, const void *)) {
    size_t root = start;
    while (root * 2 + 1 <= end) {
        size_t child = root * 2 + 1;
        if (child + 1 <= end &&
            compar(arr + child * size, arr + (child + 1) * size) < 0) {
            child++;
        }
        if (compar(arr + root * size, arr + child * size) < 0) {
            qsort_swap(arr + root * size, arr + child * size, size);
            root = child;
        } else {
            return;
        }
    }
}

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    if (nmemb <= 1 || !base || size == 0 || !compar) return;
    char *arr = (char *)base;
    /* Iterative O(N log N) heapsort with O(1) stack avoids O(N^2) stalls on sorted inputs */
    for (size_t i = nmemb / 2; i > 0; i--) {
        qsort_sift_down(arr, i - 1, nmemb - 1, size, compar);
    }
    for (size_t end = nmemb - 1; end > 0; end--) {
        qsort_swap(arr, arr + end * size, size);
        qsort_sift_down(arr, 0, end - 1, size, compar);
    }
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    size_t l = 0;
    size_t r = nmemb;
    while (l < r) {
        size_t m = l + (r - l) / 2;
        const void *elem = (const char *)base + m * size;
        int cmp = compar(key, elem);
        if (cmp == 0) return (void *)elem;
        if (cmp < 0) r = m;
        else l = m + 1;
    }
    return NULL;
}

char *realpath(const char *path, char *resolved_path) {
    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    if (path[0] == '\0') {
        errno = ENOENT;
        return NULL;
    }

    char *abs_path = NULL;
    if (path[0] != '/') {
        uint32_t cwd_cap = 256;
        char *cwd_buf = (char *)malloc(cwd_cap);
        while (cwd_buf) {
            if (ipo_getcwd(cwd_buf, cwd_cap) != 0) {
                break;
            }
            cwd_cap *= 2;
            char *new_buf = (char *)realloc(cwd_buf, cwd_cap);
            if (!new_buf) {
                free(cwd_buf);
                cwd_buf = NULL;
                break;
            }
            cwd_buf = new_buf;
        }
        if (!cwd_buf || cwd_buf[0] == '\0') {
            if (cwd_buf) free(cwd_buf);
            cwd_buf = strdup("/");
        }

        size_t clen = strlen(cwd_buf);
        size_t plen = strlen(path);
        size_t needed = clen + 1 + plen + 1;
        abs_path = (char *)malloc(needed);
        if (!abs_path) {
            free(cwd_buf);
            errno = ENOMEM;
            return NULL;
        }
        if (clen == 1 && cwd_buf[0] == '/') {
            snprintf(abs_path, needed, "/%s", path);
        } else {
            snprintf(abs_path, needed, "%s/%s", cwd_buf, path);
        }
        free(cwd_buf);
    } else {
        abs_path = strdup(path);
        if (!abs_path) {
            errno = ENOMEM;
            return NULL;
        }
    }

    size_t cap = 16;
    size_t count = 0;
    char **tokens = (char **)malloc(cap * sizeof(char *));
    if (!tokens) {
        free(abs_path);
        errno = ENOMEM;
        return NULL;
    }

    char *curr = abs_path;
    while (*curr) {
        while (*curr == '/') curr++;
        if (!*curr) break;
        char *start = curr;
        while (*curr && *curr != '/') curr++;
        if (*curr) {
            *curr = '\0';
            curr++;
        }

        if (strcmp(start, ".") == 0) {
            continue;
        }
        if (strcmp(start, "..") == 0) {
            if (count > 0) count--;
            continue;
        }

        if (count >= cap) {
            size_t new_cap = cap * 2;
            char **new_tokens = (char **)realloc(tokens, new_cap * sizeof(char *));
            if (!new_tokens) {
                free(tokens);
                free(abs_path);
                errno = ENOMEM;
                return NULL;
            }
            tokens = new_tokens;
            cap = new_cap;
        }
        tokens[count++] = start;
    }

    size_t out_len = 0;
    if (count == 0) {
        out_len = 1; /* "/" */
    } else {
        for (size_t i = 0; i < count; i++) {
            out_len += 1 + strlen(tokens[i]);
        }
    }

    if (!resolved_path) {
        resolved_path = (char *)malloc(out_len + 1);
        if (!resolved_path) {
            free(tokens);
            free(abs_path);
            errno = ENOMEM;
            return NULL;
        }
    }

    if (count == 0) {
        resolved_path[0] = '/';
        resolved_path[1] = '\0';
    } else {
        char *p = resolved_path;
        for (size_t i = 0; i < count; i++) {
            *p++ = '/';
            size_t slen = strlen(tokens[i]);
            memcpy(p, tokens[i], slen);
            p += slen;
        }
        *p = '\0';
    }

    free(tokens);
    free(abs_path);
    return resolved_path;
}
