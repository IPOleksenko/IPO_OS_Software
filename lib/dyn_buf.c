#include <net/dyn_buf.h>
#include <memory/kmalloc.h>
#include <stdio.h>
#include <string.h>

#define DYN_BUF_DEFAULT_CAP 64

void dyn_buf_init(dyn_buf_t *buf, size_t initial_cap) {
    if (!buf) return;
    buf->size = 0;
    buf->capacity = initial_cap > 0 ? initial_cap : DYN_BUF_DEFAULT_CAP;
    buf->data = (uint8_t *)kmalloc(buf->capacity);
    if (buf->data) {
        memset(buf->data, 0, buf->capacity);
    } else {
        buf->capacity = 0;
    }
}

bool dyn_buf_reserve(dyn_buf_t *buf, size_t needed_cap) {
    if (!buf) return false;
    if (buf->capacity >= needed_cap) return true;

    size_t new_cap = buf->capacity ? buf->capacity : DYN_BUF_DEFAULT_CAP;
    while (new_cap < needed_cap) {
        /* Geometric growth: double size to guarantee amortized O(1) appending */
        size_t next = new_cap * 2;
        if (next < new_cap) {
            new_cap = needed_cap; /* overflow guard */
            break;
        }
        new_cap = next;
    }

    uint8_t *new_data = (uint8_t *)krealloc(buf->data, new_cap);
    if (!new_data) {
        return false;
    }

    /* Zero-fill freshly allocated margin */
    if (new_cap > buf->capacity) {
        memset(new_data + buf->capacity, 0, new_cap - buf->capacity);
    }

    buf->data = new_data;
    buf->capacity = new_cap;
    return true;
}

bool dyn_buf_append(dyn_buf_t *buf, const void *src, size_t len) {
    if (!buf || !src || len == 0) return true;
    if (!dyn_buf_reserve(buf, buf->size + len)) {
        return false;
    }
    memcpy(buf->data + buf->size, src, len);
    buf->size += len;
    return true;
}

bool dyn_buf_append_byte(dyn_buf_t *buf, uint8_t byte) {
    return dyn_buf_append(buf, &byte, 1);
}

bool dyn_buf_append_str(dyn_buf_t *buf, const char *str) {
    if (!str) return true;
    return dyn_buf_append(buf, str, strlen(str));
}

size_t dyn_buf_consume(dyn_buf_t *buf, void *dest, size_t len) {
    if (!buf || buf->size == 0 || len == 0) return 0;
    size_t to_read = len < buf->size ? len : buf->size;

    if (dest) {
        memcpy(dest, buf->data, to_read);
    }

    size_t remaining = buf->size - to_read;
    if (remaining > 0) {
        memmove(buf->data, buf->data + to_read, remaining);
    }
    buf->size = remaining;
    return to_read;
}

void dyn_buf_reset(dyn_buf_t *buf) {
    if (!buf) return;
    buf->size = 0;
}

void dyn_buf_free(dyn_buf_t *buf) {
    if (!buf) return;
    if (buf->data) {
        kfree(buf->data);
        buf->data = NULL;
    }
    buf->size = 0;
    buf->capacity = 0;
}

/* -------------------------------------------------------------
 * Dynamic String (arbitrary-length strings / IPs / hostnames)
 * ------------------------------------------------------------- */

void dyn_str_init(dyn_str_t *ds, const char *initial) {
    if (!ds) return;
    ds->len = 0;
    ds->capacity = 0;
    ds->cstr = NULL;

    if (initial) {
        size_t slen = strlen(initial);
        ds->capacity = slen + 16;
        ds->cstr = (char *)kmalloc(ds->capacity);
        if (ds->cstr) {
            memcpy(ds->cstr, initial, slen);
            ds->cstr[slen] = '\0';
            ds->len = slen;
        }
    } else {
        ds->capacity = 16;
        ds->cstr = (char *)kmalloc(ds->capacity);
        if (ds->cstr) {
            ds->cstr[0] = '\0';
            ds->len = 0;
        }
    }
}

bool dyn_str_set(dyn_str_t *ds, const char *val) {
    if (!ds) return false;
    if (!val) {
        if (ds->cstr) ds->cstr[0] = '\0';
        ds->len = 0;
        return true;
    }
    size_t slen = strlen(val);
    if (slen + 1 > ds->capacity) {
        size_t new_cap = slen + 16;
        char *new_cstr = (char *)krealloc(ds->cstr, new_cap);
        if (!new_cstr) return false;
        ds->cstr = new_cstr;
        ds->capacity = new_cap;
    }
    memcpy(ds->cstr, val, slen);
    ds->cstr[slen] = '\0';
    ds->len = slen;
    return true;
}

bool dyn_str_append(dyn_str_t *ds, const char *suffix) {
    if (!ds || !suffix) return true;
    size_t slen = strlen(suffix);
    if (slen == 0) return true;

    size_t needed = ds->len + slen + 1;
    if (needed > ds->capacity) {
        size_t new_cap = ds->capacity ? ds->capacity * 2 : 32;
        while (new_cap < needed) new_cap *= 2;
        char *new_cstr = (char *)krealloc(ds->cstr, new_cap);
        if (!new_cstr) return false;
        ds->cstr = new_cstr;
        ds->capacity = new_cap;
    }

    memcpy(ds->cstr + ds->len, suffix, slen);
    ds->len += slen;
    ds->cstr[ds->len] = '\0';
    return true;
}

bool dyn_str_append_char(dyn_str_t *ds, char c) {
    if (!ds) return false;
    size_t needed = ds->len + 2;
    if (needed > ds->capacity) {
        size_t new_cap = ds->capacity ? ds->capacity * 2 : 32;
        while (new_cap < needed) new_cap *= 2;
        char *new_cstr = (char *)krealloc(ds->cstr, new_cap);
        if (!new_cstr) return false;
        ds->cstr = new_cstr;
        ds->capacity = new_cap;
    }
    ds->cstr[ds->len++] = c;
    ds->cstr[ds->len] = '\0';
    return true;
}

static bool dyn_str_write_vprintf(dyn_str_t *ds, bool append_mode, const char *fmt, va_list ap) {
    if (!ds || !fmt) return false;
    va_list aq;
    va_copy(aq, ap);
    int n = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);
    if (n < 0) return false;

    size_t base = append_mode ? ds->len : 0;
    size_t needed = base + (size_t)n + 1;
    if (needed > ds->capacity) {
        size_t new_cap = ds->capacity ? ds->capacity * 2 : 32;
        while (new_cap < needed) new_cap *= 2;
        char *nb = (char *)krealloc(ds->cstr, new_cap);
        if (!nb) return false;
        ds->cstr = nb;
        ds->capacity = new_cap;
    }
    vsnprintf(ds->cstr + base, (size_t)n + 1, fmt, ap);
    ds->len = base + (size_t)n;
    return true;
}

bool dyn_str_vprintf(dyn_str_t *ds, const char *fmt, va_list ap) {
    return dyn_str_write_vprintf(ds, false, fmt, ap);
}

bool dyn_str_printf(dyn_str_t *ds, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    bool ok = dyn_str_vprintf(ds, fmt, ap);
    va_end(ap);
    return ok;
}

bool dyn_str_append_vprintf(dyn_str_t *ds, const char *fmt, va_list ap) {
    return dyn_str_write_vprintf(ds, true, fmt, ap);
}

bool dyn_str_append_printf(dyn_str_t *ds, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    bool ok = dyn_str_append_vprintf(ds, fmt, ap);
    va_end(ap);
    return ok;
}

bool dyn_buf_append_printf(dyn_buf_t *buf, const char *fmt, ...) {
    if (!buf || !fmt) return false;
    va_list ap, aq;
    va_start(ap, fmt);
    va_copy(aq, ap);
    int n = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);
    if (n < 0) {
        va_end(ap);
        return false;
    }
    if (!dyn_buf_reserve(buf, buf->size + (size_t)n + 1)) {
        va_end(ap);
        return false;
    }
    vsnprintf((char *)(buf->data + buf->size), (size_t)n + 1, fmt, ap);
    va_end(ap);
    buf->size += (size_t)n;
    return true;
}

const char *dyn_str_c(const dyn_str_t *ds) {
    return (ds && ds->cstr) ? ds->cstr : "";
}

void dyn_str_free(dyn_str_t *ds) {
    if (!ds) return;
    if (ds->cstr) {
        kfree(ds->cstr);
        ds->cstr = NULL;
    }
    ds->len = 0;
    ds->capacity = 0;
}

