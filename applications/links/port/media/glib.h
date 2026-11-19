#ifndef IPO_LINKS_MEDIA_GLIB_H
#define IPO_LINKS_MEDIA_GLIB_H

#include <limits.h>
#include <math.h>
#include <stddef.h>

#ifndef exp2
static inline double ipo_glib_exp2(double x) { return pow(2.0, x); }
#define exp2(x) ipo_glib_exp2(x)
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef __cplusplus
#define G_BEGIN_DECLS extern "C" {
#define G_END_DECLS }
#else
#define G_BEGIN_DECLS
#define G_END_DECLS
#endif

typedef uint8_t guint8;
typedef int8_t gint8;
typedef uint16_t guint16;
typedef int16_t gint16;
typedef uint32_t guint32;
typedef int32_t gint32;
typedef uint64_t guint64;
typedef int64_t gint64;
typedef unsigned int guint;
typedef int gint;
typedef size_t gsize;
typedef long gssize;
typedef unsigned char guchar;
typedef char gchar;
typedef int gboolean;
typedef void *gpointer;
typedef const void *gconstpointer;
typedef void (*GDestroyNotify)(gpointer);

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#define G_MAXUINT32 0xffffffffU
#define G_MAXUINT64 0xffffffffffffffffULL
#define G_MAXINT 0x7fffffff
#define G_MAXINT64 0x7fffffffffffffffLL
#define G_MAXSIZE ((size_t)-1)
#define G_GUINT64_FORMAT "llu"
#define G_GINT64_FORMAT "lld"
#define G_GUINT64_CONSTANT(val) (val##ULL)
#define G_GINT64_CONSTANT(val) (val##LL)
#define G_PI 3.14159265358979323846
#define G_MAXDOUBLE 1.7976931348623157e+308

#define GUINT16_FROM_BE(x) ((guint16)((((guint16)(x)) >> 8) | (((guint16)(x)) << 8)))
#define GUINT32_FROM_BE(x) __builtin_bswap32(x)
#define GUINT64_FROM_BE(x) __builtin_bswap64(x)
#define g_ntohs(x) GUINT16_FROM_BE(x)

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef CLAMP
#define CLAMP(x, lo, hi) (((x) < (lo)) ? (lo) : (((x) > (hi)) ? (hi) : (x)))
#endif

#define G_N_ELEMENTS(arr) (sizeof(arr) / sizeof((arr)[0]))
#define G_LIKELY(x) (x)
#define G_UNLIKELY(x) (x)

#define g_return_if_fail(x) do { if (!(x)) return; } while (0)
#define g_return_val_if_fail(x, v) do { if (!(x)) return (v); } while (0)

#define g_malloc(n) malloc(n)
#define g_malloc0(n) calloc(1, (n))
#define g_try_malloc(n) malloc(n)
#define g_try_malloc0(n) calloc(1, (n))
#define g_try_malloc_n(n, s) calloc((size_t)(n), (size_t)(s))
#define g_realloc(p, n) realloc((p), (n))
#define g_free(p) free(p)
#define g_new(t, n) ((t *)malloc(sizeof(t) * (size_t)(n)))
#define g_new0(t, n) ((t *)calloc((size_t)(n), sizeof(t)))
#define g_renew(t, p, n) ((t *)realloc((p), sizeof(t) * (size_t)(n)))
#define g_clear_pointer(pp, fn) do { if (*(pp)) { (fn)(*(pp)); *(pp) = NULL; } } while (0)
#define g_clear_object(pp) do { *(pp) = NULL; } while (0)
#define g_steal_pointer(pp) ({ typeof(*(pp)) _p = *(pp); *(pp) = NULL; _p; })

static inline gpointer g_memdup2(gconstpointer p, gsize n) {
    void *d = malloc(n ? n : 1);
    if (d && p && n) memcpy(d, p, n);
    return d;
}

static inline gchar *g_strdup(const gchar *s) {
    if (!s) return NULL;
    size_t l = strlen(s) + 1;
    char *r = (char *)malloc(l);
    if (r) memcpy(r, s, l);
    return r;
}

static inline gchar *g_strndup(const gchar *s, gsize n) {
    char *r = (char *)malloc(n + 1);
    if (r) {
        if (s && n) memcpy(r, s, n);
        r[n] = '\0';
    }
    return r;
}

#define g_ascii_strcasecmp(a, b) strcasecmp((a), (b))
#define g_ascii_isprint(c) ((unsigned char)(c) >= 32 && (unsigned char)(c) < 127)

typedef struct _GBytes {
    guint8 *data;
    gsize len;
    int refcnt;
    int own;
} GBytes;

static inline GBytes *g_bytes_new(gconstpointer d, gsize n) {
    GBytes *b = (GBytes *)malloc(sizeof(GBytes));
    if (!b) return NULL;
    b->data = (guint8 *)malloc(n ? n : 1);
    if (b->data && d && n) memcpy(b->data, d, n);
    b->len = n;
    b->refcnt = 1;
    b->own = 1;
    return b;
}

static inline GBytes *g_bytes_new_take(gpointer d, gsize n) {
    GBytes *b = (GBytes *)malloc(sizeof(GBytes));
    if (!b) return NULL;
    b->data = (guint8 *)d;
    b->len = n;
    b->refcnt = 1;
    b->own = 1;
    return b;
}

static inline GBytes *g_bytes_new_static(gconstpointer d, gsize n) {
    GBytes *b = (GBytes *)malloc(sizeof(GBytes));
    if (!b) return NULL;
    b->data = (guint8 *)d;
    b->len = n;
    b->refcnt = 1;
    b->own = 0;
    return b;
}

static inline GBytes *g_bytes_ref(GBytes *b) {
    if (b) b->refcnt++;
    return b;
}

static inline void g_bytes_unref(GBytes *b) {
    if (b && --b->refcnt <= 0) {
        if (b->own) free(b->data);
        free(b);
    }
}

static inline gconstpointer g_bytes_get_data(GBytes *b, gsize *sz) {
    if (sz) *sz = b ? b->len : 0;
    return b ? b->data : NULL;
}

static inline gsize g_bytes_get_size(GBytes *b) {
    return b ? b->len : 0;
}

static inline GBytes *g_bytes_new_from_bytes(GBytes *b, gsize off, gsize len) {
    if (!b || off > b->len) return g_bytes_new(NULL, 0);
    if (len > b->len - off) len = b->len - off;
    return g_bytes_new(b->data + off, len);
}

static inline gboolean g_bytes_equal(GBytes *a, GBytes *b) {
    return a && b && a->len == b->len && (a->len == 0 || memcmp(a->data, b->data, a->len) == 0);
}

typedef struct _GByteArray {
    guint8 *data;
    guint len;
    guint cap;
} GByteArray;

static inline GByteArray *g_byte_array_new(void) {
    return (GByteArray *)calloc(1, sizeof(GByteArray));
}

static inline GByteArray *g_byte_array_sized_new(guint r) {
    GByteArray *a = g_byte_array_new();
    if (a && r) {
        a->data = (guint8 *)malloc(r);
        a->cap = r;
    }
    return a;
}

static inline GByteArray *g_byte_array_append(GByteArray *a, const guint8 *d, guint l) {
    if (a->len + l > a->cap) {
        guint nc = a->cap ? a->cap * 2 : 64;
        while (nc < a->len + l) nc *= 2;
        a->data = (guint8 *)realloc(a->data, nc);
        a->cap = nc;
    }
    if (l && d) memcpy(a->data + a->len, d, l);
    a->len += l;
    return a;
}

static inline GByteArray *g_byte_array_set_size(GByteArray *a, guint l) {
    if (l > a->cap) {
        a->data = (guint8 *)realloc(a->data, l);
        a->cap = l;
    }
    a->len = l;
    return a;
}

static inline GByteArray *g_byte_array_remove_range(GByteArray *a, guint idx, guint l) {
    if (idx + l < a->len) memmove(a->data + idx, a->data + idx + l, a->len - (idx + l));
    a->len -= l;
    return a;
}

static inline GBytes *g_byte_array_free_to_bytes(GByteArray *a) {
    GBytes *b = g_bytes_new_take(a->data, a->len);
    free(a);
    return b;
}

static inline guint8 *g_byte_array_free(GByteArray *a, gboolean free_seg) {
    guint8 *r = free_seg ? NULL : a->data;
    if (free_seg) free(a->data);
    free(a);
    return r;
}

static inline void g_byte_array_unref(GByteArray *a) {
    if (a) g_byte_array_free(a, TRUE);
}

typedef struct _GArray {
    gchar *data;
    guint len;
    guint esz;
    guint cap;
    int zero;
} GArray;

static inline GArray *g_array_new(gboolean z, gboolean c, guint esz) {
    GArray *a = (GArray *)calloc(1, sizeof(GArray));
    if (a) {
        a->esz = esz;
        a->zero = z;
    }
    (void)c;
    return a;
}

static inline GArray *g_array_sized_new(gboolean z, gboolean c, guint esz, guint r) {
    GArray *a = g_array_new(z, c, esz);
    if (a && r) {
        a->data = (gchar *)calloc(r, esz);
        a->cap = r;
    }
    return a;
}

static inline GArray *g_array_append_vals(GArray *a, gconstpointer d, guint n) {
    if (a->len + n > a->cap) {
        guint nc = a->cap ? a->cap * 2 : 16;
        while (nc < a->len + n) nc *= 2;
        a->data = (gchar *)realloc(a->data, (size_t)nc * a->esz);
        a->cap = nc;
    }
    if (d && n) memcpy(a->data + (size_t)a->len * a->esz, d, (size_t)n * a->esz);
    a->len += n;
    return a;
}

#define g_array_append_val(a, v) g_array_append_vals((a), &(v), 1)
#define g_array_index(a, t, i) (((t *)(void *)(a)->data)[i])

static inline GArray *g_array_set_size(GArray *a, guint l) {
    if (l > a->cap) {
        a->data = (gchar *)realloc(a->data, (size_t)l * a->esz);
        a->cap = l;
    }
    if (l > a->len) memset(a->data + (size_t)a->len * a->esz, 0, (size_t)(l - a->len) * a->esz);
    a->len = l;
    return a;
}

static inline GArray *g_array_remove_index(GArray *a, guint idx) {
    if (idx + 1 < a->len) memmove(a->data + idx * a->esz, a->data + (idx + 1) * a->esz, (a->len - idx - 1) * a->esz);
    a->len--;
    return a;
}

static inline GArray *g_array_remove_range(GArray *a, guint idx, guint n) {
    if (!a || idx + n > a->len) return a;
    if (idx + n < a->len) memmove(a->data + idx * a->esz, a->data + (idx + n) * a->esz, (a->len - idx - n) * a->esz);
    a->len -= n;
    return a;
}

static inline void g_array_set_clear_func(GArray *a, GDestroyNotify fn) {
    (void)a;
    (void)fn;
}

typedef int (*GCompareFunc)(gconstpointer a, gconstpointer b);
static inline void g_array_sort(GArray *a, GCompareFunc cmp) {
    if (a && a->len > 1) qsort(a->data, a->len, a->esz, cmp);
}

static inline gchar *g_array_free(GArray *a, gboolean fs) {
    gchar *r = fs ? NULL : a->data;
    if (fs) free(a->data);
    free(a);
    return r;
}

static inline void g_array_unref(GArray *a) {
    if (a) g_array_free(a, TRUE);
}

typedef struct _GPtrArray {
    gpointer *pdata;
    guint len;
    guint cap;
    GDestroyNotify fn;
} GPtrArray;

static inline GPtrArray *g_ptr_array_new_with_free_func(GDestroyNotify fn) {
    GPtrArray *p = (GPtrArray *)calloc(1, sizeof(GPtrArray));
    if (p) p->fn = fn;
    return p;
}

static inline void g_ptr_array_add(GPtrArray *p, gpointer d) {
    if (p->len + 1 > p->cap) {
        guint nc = p->cap ? p->cap * 2 : 16;
        p->pdata = (gpointer *)realloc(p->pdata, nc * sizeof(gpointer));
        p->cap = nc;
    }
    p->pdata[p->len++] = d;
}

static inline gpointer g_ptr_array_index(GPtrArray *p, guint i) {
    return p->pdata[i];
}

static inline void g_ptr_array_unref(GPtrArray *p) {
    if (!p) return;
    if (p->fn) {
        for (guint i = 0; i < p->len; i++) {
            if (p->pdata[i]) p->fn(p->pdata[i]);
        }
    }
    free(p->pdata);
    free(p);
}

typedef struct _GQueueNode {
    gpointer data;
    struct _GQueueNode *next;
} GQueueNode;

typedef struct _GQueue {
    GQueueNode *head, *tail;
    guint length;
} GQueue;

#define G_QUEUE_INIT { NULL, NULL, 0 }

static inline void g_queue_init(GQueue *q) { q->head = q->tail = NULL; q->length = 0; }
static inline void g_queue_push_tail(GQueue *q, gpointer d) {
    GQueueNode *n = (GQueueNode *)malloc(sizeof(GQueueNode));
    n->data = d; n->next = NULL;
    if (q->tail) q->tail->next = n; else q->head = n;
    q->tail = n; q->length++;
}
static inline gpointer g_queue_pop_head(GQueue *q) {
    GQueueNode *n = q->head;
    if (!n) return NULL;
    gpointer d = n->data;
    q->head = n->next;
    if (!q->head) q->tail = NULL;
    free(n); q->length--;
    return d;
}
static inline gpointer g_queue_peek_head(GQueue *q) { return q->head ? q->head->data : NULL; }
static inline gboolean g_queue_is_empty(GQueue *q) { return q->length == 0; }
static inline guint g_queue_get_length(GQueue *q) { return q->length; }
static inline void g_queue_clear_full(GQueue *q, GDestroyNotify fn) {
    while (q->head) { gpointer d = g_queue_pop_head(q); if (fn && d) fn(d); }
}
static inline GQueue *g_queue_new(void) { return (GQueue *)calloc(1, sizeof(GQueue)); }
static inline void g_queue_free(GQueue *q) { while (q && q->head) g_queue_pop_head(q); free(q); }
static inline void g_queue_free_full(GQueue *q, GDestroyNotify fn) { if (q) { g_queue_clear_full(q, fn); free(q); } }

static inline gboolean g_once_init_enter(volatile gsize *v) { return (*v == 0); }
static inline void g_once_init_leave(volatile gsize *v, gsize val) { *v = val; }

#endif
