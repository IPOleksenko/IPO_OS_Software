/*
 * TrueType glyph rasterization shim (SDL_ttf) for SENSE: The Game on IPO_OS.
 */

#include <SDL.h>
#include <SDL_ttf.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLYPH_CACHE_CAP 512

typedef struct {
    float x0, y0, x1, y1;
    int dir; /* +1 downward, -1 upward */
} TT_Edge;

typedef struct {
    int32_t x, y;
    uint8_t on_curve;
} TT_Point;

typedef struct {
    uint32_t cp;
    int w, h;
    int xoff, yoff;
    int advance;
    uint8_t *alpha;
    uint32_t stamp;
} TT_CachedGlyph;

struct TTF_Font {
    uint8_t *buf;
    size_t size;
    bool own_buf;
    int ptsize;
    int hinting;

    uint32_t cmap_off;
    uint32_t loca_off;
    uint32_t glyf_off;
    uint32_t hmtx_off;

    uint16_t units_per_em;
    int16_t  index_to_loc_format;
    int16_t  ascent;
    int16_t  descent;
    int16_t  line_gap;
    uint16_t num_hmetrics;
    uint16_t num_glyphs;

    float scale;
    int line_height;
    int baseline;
    uint64_t content_hash;

    TT_CachedGlyph cache[GLYPH_CACHE_CAP];
    uint32_t stamp_seq;
};

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int16_t rd_s16(const uint8_t *p) {
    return (int16_t)rd_u16(p);
}

static uint32_t rd_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static uint32_t tt_find_table(const TTF_Font *f, const char tag[4]) {
    if (!f || f->size < 12) return 0;
    uint16_t ntables = rd_u16(f->buf + 4);
    if (12u + (size_t)ntables * 16u > f->size) return 0;
    for (uint16_t i = 0; i < ntables; i++) {
        const uint8_t *rec = f->buf + 12u + (size_t)i * 16u;
        if (memcmp(rec, tag, 4) == 0) {
            uint32_t off = rd_u32(rec + 8);
            uint32_t len = rd_u32(rec + 12);
            if (off < f->size && off + len <= f->size) return off;
        }
    }
    return 0;
}

static uint16_t tt_glyph_index(const TTF_Font *f, uint32_t cp) {
    if (!f->cmap_off || f->cmap_off + 4 > f->size) return 0;
    uint16_t nsub = rd_u16(f->buf + f->cmap_off + 2);
    uint32_t best_sub = 0;
    uint16_t best_fmt = 0;

    for (uint16_t i = 0; i < nsub; i++) {
        uint32_t rec_off = f->cmap_off + 4u + (uint32_t)i * 8u;
        if (rec_off + 8 > f->size) break;
        uint16_t plat = rd_u16(f->buf + rec_off);
        uint16_t enc  = rd_u16(f->buf + rec_off + 2);
        uint32_t sub  = f->cmap_off + rd_u32(f->buf + rec_off + 4);
        if (sub + 4 > f->size) continue;
        uint16_t fmt = rd_u16(f->buf + sub);
        if ((plat == 3 && enc == 10 && fmt == 12) ||
            (plat == 0 && fmt == 12)) {
            best_sub = sub;
            best_fmt = 12;
            break;
        }
        if (((plat == 3 && enc == 1) || plat == 0) && fmt == 4) {
            best_sub = sub;
            best_fmt = 4;
        }
    }

    if (!best_sub) return 0;

    if (best_fmt == 12) {
        if (best_sub + 16 > f->size) return 0;
        uint32_t ngroups = rd_u32(f->buf + best_sub + 12);
        for (uint32_t i = 0; i < ngroups; i++) {
            uint32_t goff = best_sub + 16u + i * 12u;
            if (goff + 12 > f->size) break;
            uint32_t s = rd_u32(f->buf + goff);
            uint32_t e = rd_u32(f->buf + goff + 4);
            uint32_t g = rd_u32(f->buf + goff + 8);
            if (cp >= s && cp <= e) return (uint16_t)(g + (cp - s));
        }
        return 0;
    }

    if (best_fmt == 4 && cp <= 0xFFFFu) {
        if (best_sub + 14 > f->size) return 0;
        uint16_t seg_cnt = (uint16_t)(rd_u16(f->buf + best_sub + 6) / 2u);
        uint32_t end_off   = best_sub + 14u;
        uint32_t start_off = end_off + (uint32_t)seg_cnt * 2u + 2u;
        uint32_t delta_off = start_off + (uint32_t)seg_cnt * 2u;
        uint32_t range_off = delta_off + (uint32_t)seg_cnt * 2u;
        if (range_off + (uint32_t)seg_cnt * 2u > f->size) return 0;

        for (uint16_t i = 0; i < seg_cnt; i++) {
            uint16_t e = rd_u16(f->buf + end_off + i * 2u);
            if (cp <= e) {
                uint16_t s = rd_u16(f->buf + start_off + i * 2u);
                if (cp < s) return 0;
                uint16_t r_off = rd_u16(f->buf + range_off + i * 2u);
                int16_t delta = rd_s16(f->buf + delta_off + i * 2u);
                if (r_off == 0) {
                    return (uint16_t)((cp + (uint32_t)delta) & 0xFFFFu);
                }
                uint32_t roff_pos = range_off + i * 2u;
                uint32_t gid_pos = roff_pos + r_off + (cp - s) * 2u;
                if (gid_pos + 2 > f->size) return 0;
                uint16_t gid = rd_u16(f->buf + gid_pos);
                return gid ? (uint16_t)((gid + (uint32_t)delta) & 0xFFFFu) : 0;
            }
        }
    }

    return 0;
}

static void tt_get_hmetrics(const TTF_Font *f, uint16_t gid, int *out_adv, int *out_lsb) {
    if (!f->hmtx_off || f->num_hmetrics == 0) {
        *out_adv = 0;
        *out_lsb = 0;
        return;
    }
    if (gid < f->num_hmetrics) {
        uint32_t off = f->hmtx_off + (uint32_t)gid * 4u;
        if (off + 4 <= f->size) {
            *out_adv = rd_u16(f->buf + off);
            *out_lsb = rd_s16(f->buf + off + 2);
            return;
        }
    }
    uint32_t last_adv_off = f->hmtx_off + (uint32_t)(f->num_hmetrics - 1u) * 4u;
    *out_adv = (last_adv_off + 2 <= f->size) ? rd_u16(f->buf + last_adv_off) : 0;
    uint32_t lsb_off = f->hmtx_off + (uint32_t)f->num_hmetrics * 4u +
                       (uint32_t)(gid - f->num_hmetrics) * 2u;
    *out_lsb = (lsb_off + 2 <= f->size) ? rd_s16(f->buf + lsb_off) : 0;
}

static uint32_t tt_glyph_offset(const TTF_Font *f, uint16_t gid, uint32_t *out_len) {
    if (!f->loca_off || !f->glyf_off || gid >= f->num_glyphs) return 0;
    uint32_t o0 = 0, o1 = 0;
    if (f->index_to_loc_format == 0) {
        uint32_t p = f->loca_off + (uint32_t)gid * 2u;
        if (p + 4 > f->size) return 0;
        o0 = (uint32_t)rd_u16(f->buf + p) * 2u;
        o1 = (uint32_t)rd_u16(f->buf + p + 2) * 2u;
    } else {
        uint32_t p = f->loca_off + (uint32_t)gid * 4u;
        if (p + 8 > f->size) return 0;
        o0 = rd_u32(f->buf + p);
        o1 = rd_u32(f->buf + p + 4);
    }
    if (o1 <= o0 || f->glyf_off + o1 > f->size) return 0;
    *out_len = o1 - o0;
    return f->glyf_off + o0;
}

typedef struct {
    TT_Edge *edges;
    size_t count;
    size_t cap;
} TT_EdgeList;

static void add_edge(TT_EdgeList *el, float x0, float y0, float x1, float y1) {
    if (fabsf(y1 - y0) < 1e-4f) return;
    if (el->count >= el->cap) {
        size_t ncap = el->cap ? el->cap * 2 : 128;
        TT_Edge *nb = (TT_Edge *)realloc(el->edges, ncap * sizeof(TT_Edge));
        if (!nb) return;
        el->edges = nb;
        el->cap = ncap;
    }
    if (y0 < y1) {
        el->edges[el->count++] = (TT_Edge){ x0, y0, x1, y1, +1 };
    } else {
        el->edges[el->count++] = (TT_Edge){ x1, y1, x0, y0, -1 };
    }
}

static void add_quad(TT_EdgeList *el, float x0, float y0,
                     float cx, float cy, float x1, float y1, int depth) {
    float mx = 0.5f * (x0 + x1);
    float my = 0.5f * (y0 + y1);
    float dx = cx - mx;
    float dy = cy - my;
    if (depth >= 5 || (dx * dx + dy * dy) <= 0.0625f) {
        add_edge(el, x0, y0, x1, y1);
        return;
    }
    float q0x = 0.5f * (x0 + cx), q0y = 0.5f * (y0 + cy);
    float q1x = 0.5f * (cx + x1), q1y = 0.5f * (cy + y1);
    float hx  = 0.5f * (q0x + q1x), hy = 0.5f * (q0y + q1y);
    add_quad(el, x0, y0, q0x, q0y, hx, hy, depth + 1);
    add_quad(el, hx, hy, q1x, q1y, x1, y1, depth + 1);
}

static inline void tt_xf(TT_Point p, float sx, float sy, float ox, float oy,
                         float m00, float m01, float m10, float m11,
                         float *rx, float *ry) {
    float px = (float)p.x * m00 + (float)p.y * m01 + ox;
    float py = (float)p.x * m10 + (float)p.y * m11 + oy;
    *rx = px * sx;
    *ry = -py * sy;
}

static void flatten_contour(TT_EdgeList *el, const TT_Point *pts, int count,
                            float sx, float sy, float ox, float oy,
                            float m00, float m01, float m10, float m11) {
    if (count < 2) return;

    int start = 0;
    while (start < count && !pts[start].on_curve) start++;

    float sx0, sy0;
    if (start < count) {
        tt_xf(pts[start], sx, sy, ox, oy, m00, m01, m10, m11, &sx0, &sy0);
    } else {
        TT_Point mid = {
            (pts[0].x + pts[count - 1].x) / 2,
            (pts[0].y + pts[count - 1].y) / 2,
            1
        };
        tt_xf(mid, sx, sy, ox, oy, m00, m01, m10, m11, &sx0, &sy0);
        start = 0;
    }

    float curx = sx0, cury = sy0;
    int i = 0;
    int idx = (pts[start].on_curve) ? ((start + 1) % count) : 0;
    int steps = count;

    while (i < steps) {
        TT_Point p = pts[idx];
        if (p.on_curve) {
            float nx, ny;
            tt_xf(p, sx, sy, ox, oy, m00, m01, m10, m11, &nx, &ny);
            add_edge(el, curx, cury, nx, ny);
            curx = nx;
            cury = ny;
            idx = (idx + 1) % count;
            i++;
        } else {
            int nxt_idx = (idx + 1) % count;
            TT_Point p2 = pts[nxt_idx];
            float cx, cy, nx, ny;
            tt_xf(p, sx, sy, ox, oy, m00, m01, m10, m11, &cx, &cy);
            if (p2.on_curve) {
                tt_xf(p2, sx, sy, ox, oy, m00, m01, m10, m11, &nx, &ny);
                add_quad(el, curx, cury, cx, cy, nx, ny, 0);
                curx = nx;
                cury = ny;
                idx = (nxt_idx + 1) % count;
                i += 2;
            } else {
                TT_Point mid = { (p.x + p2.x) / 2, (p.y + p2.y) / 2, 1 };
                tt_xf(mid, sx, sy, ox, oy, m00, m01, m10, m11, &nx, &ny);
                add_quad(el, curx, cury, cx, cy, nx, ny, 0);
                curx = nx;
                cury = ny;
                idx = nxt_idx;
                i += 1;
            }
        }
    }
    add_edge(el, curx, cury, sx0, sy0);
}

static void collect_glyph_edges(const TTF_Font *f, uint16_t gid,
                                float sx, float sy,
                                float ox, float oy,
                                float m00, float m01, float m10, float m11,
                                TT_EdgeList *el, int depth) {
    if (depth > 8) return;
    uint32_t glen = 0;
    uint32_t goff = tt_glyph_offset(f, gid, &glen);
    if (!goff || glen < 10) return;

    const uint8_t *g = f->buf + goff;
    int16_t ncontours = rd_s16(g);

    if (ncontours > 0) {
        if (10u + (uint32_t)ncontours * 2u + 2u > glen) return;
        const uint8_t *end_pts_p = g + 10;
        int npts = (int)rd_u16(end_pts_p + (ncontours - 1) * 2) + 1;
        if (npts <= 0 || npts > 8192) return;

        uint16_t instr_len = rd_u16(end_pts_p + ncontours * 2);
        uint32_t pos = 10u + (uint32_t)ncontours * 2u + 2u + instr_len;
        if (pos >= glen) return;

        uint8_t *flags = (uint8_t *)malloc((size_t)npts);
        TT_Point *pts  = (TT_Point *)calloc((size_t)npts, sizeof(TT_Point));
        if (!flags || !pts) { free(flags); free(pts); return; }

        int fi = 0;
        while (fi < npts && pos < glen) {
            uint8_t fl = g[pos++];
            flags[fi++] = fl;
            if ((fl & 0x08u) && pos < glen) {
                uint8_t rep = g[pos++];
                while (rep-- > 0 && fi < npts) flags[fi++] = fl;
            }
        }

        int32_t cx = 0;
        for (int i = 0; i < npts; i++) {
            uint8_t fl = flags[i];
            int32_t dx = 0;
            if (fl & 0x02u) {
                if (pos < glen) dx = g[pos++];
                if (!(fl & 0x10u)) dx = -dx;
            } else if (!(fl & 0x10u)) {
                if (pos + 2 <= glen) { dx = rd_s16(g + pos); pos += 2; }
            }
            cx += dx;
            pts[i].x = cx;
            pts[i].on_curve = fl & 1u;
        }

        int32_t cy = 0;
        for (int i = 0; i < npts; i++) {
            uint8_t fl = flags[i];
            int32_t dy = 0;
            if (fl & 0x04u) {
                if (pos < glen) dy = g[pos++];
                if (!(fl & 0x20u)) dy = -dy;
            } else if (!(fl & 0x20u)) {
                if (pos + 2 <= glen) { dy = rd_s16(g + pos); pos += 2; }
            }
            cy += dy;
            pts[i].y = cy;
        }

        int c_start = 0;
        for (int c = 0; c < ncontours; c++) {
            int c_end = (int)rd_u16(end_pts_p + c * 2);
            if (c_end >= c_start && c_end < npts) {
                flatten_contour(el, pts + c_start, c_end - c_start + 1,
                                sx, sy, ox, oy, m00, m01, m10, m11);
            }
            c_start = c_end + 1;
        }

        free(flags);
        free(pts);
        return;
    }

    if (ncontours == -1) {
        uint32_t pos = 10;
        for (;;) {
            if (pos + 4 > glen) break;
            uint16_t flags = rd_u16(g + pos);
            uint16_t cgid  = rd_u16(g + pos + 2);
            pos += 4;

            float dx = 0.0f, dy = 0.0f;
            if (flags & 0x0001u) {
                if (pos + 4 > glen) break;
                if (flags & 0x0002u) {
                    dx = (float)rd_s16(g + pos);
                    dy = (float)rd_s16(g + pos + 2);
                }
                pos += 4;
            } else {
                if (pos + 2 > glen) break;
                if (flags & 0x0002u) {
                    dx = (float)(int8_t)g[pos];
                    dy = (float)(int8_t)g[pos + 1];
                }
                pos += 2;
            }

            float cm00 = 1.0f, cm01 = 0.0f, cm10 = 0.0f, cm11 = 1.0f;
            if (flags & 0x0008u) {
                if (pos + 2 > glen) break;
                float s = (float)rd_s16(g + pos) / 16384.0f;
                cm00 = cm11 = s;
                pos += 2;
            } else if (flags & 0x0040u) {
                if (pos + 4 > glen) break;
                cm00 = (float)rd_s16(g + pos) / 16384.0f;
                cm11 = (float)rd_s16(g + pos + 2) / 16384.0f;
                pos += 4;
            } else if (flags & 0x0080u) {
                if (pos + 8 > glen) break;
                cm00 = (float)rd_s16(g + pos) / 16384.0f;
                cm01 = (float)rd_s16(g + pos + 2) / 16384.0f;
                cm10 = (float)rd_s16(g + pos + 4) / 16384.0f;
                cm11 = (float)rd_s16(g + pos + 6) / 16384.0f;
                pos += 8;
            }

            float nox = m00 * dx + m01 * dy + ox;
            float noy = m10 * dx + m11 * dy + oy;
            float nm00 = m00 * cm00 + m01 * cm10;
            float nm01 = m00 * cm01 + m01 * cm11;
            float nm10 = m10 * cm00 + m11 * cm10;
            float nm11 = m10 * cm01 + m11 * cm11;

            collect_glyph_edges(f, cgid, sx, sy, nox, noy, nm00, nm01, nm10, nm11, el, depth + 1);
            if (!(flags & 0x0020u)) break;
        }
    }
}

typedef struct {
    float x;
    int dir;
} TT_Hit;

static int cmp_hit(const void *a, const void *b) {
    float ax = ((const TT_Hit *)a)->x;
    float bx = ((const TT_Hit *)b)->x;
    return (ax < bx) ? -1 : (ax > bx ? 1 : 0);
}

static const TT_CachedGlyph *tt_get_glyph(TTF_Font *f, uint32_t cp) {
    uint32_t oldest_idx = 0;
    uint32_t oldest_stamp = 0xFFFFFFFFu;

    for (uint32_t i = 0; i < GLYPH_CACHE_CAP; i++) {
        if (f->cache[i].alpha && f->cache[i].cp == cp && f->cache[i].stamp != 0) {
            f->cache[i].stamp = ++f->stamp_seq;
            return &f->cache[i];
        }
        if (f->cache[i].stamp < oldest_stamp) {
            oldest_stamp = f->cache[i].stamp;
            oldest_idx = i;
        }
    }

    TT_CachedGlyph *slot = &f->cache[oldest_idx];
    free(slot->alpha);
    memset(slot, 0, sizeof(*slot));
    slot->cp = cp;
    slot->stamp = ++f->stamp_seq;

    uint16_t gid = tt_glyph_index(f, cp);
    if (gid == 0 && cp != ' ') gid = tt_glyph_index(f, '?');

    int adv_u = 0, lsb_u = 0;
    tt_get_hmetrics(f, gid, &adv_u, &lsb_u);
    slot->advance = (int)floorf((float)adv_u * f->scale + 0.5f);

    TT_EdgeList el = { NULL, 0, 0 };
    collect_glyph_edges(f, gid, f->scale, f->scale, 0.0f, 0.0f,
                        1.0f, 0.0f, 0.0f, 1.0f, &el, 0);

    if (el.count == 0) {
        free(el.edges);
        return slot;
    }

    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (size_t i = 0; i < el.count; i++) {
        const TT_Edge *e = &el.edges[i];
        if (e->x0 < minx) minx = e->x0;
        if (e->x1 < minx) minx = e->x1;
        if (e->x0 > maxx) maxx = e->x0;
        if (e->x1 > maxx) maxx = e->x1;
        if (e->y0 < miny) miny = e->y0;
        if (e->y1 < miny) miny = e->y1;
        if (e->y0 > maxy) maxy = e->y0;
        if (e->y1 > maxy) maxy = e->y1;
    }

    int ix0 = (int)floorf(minx) - 1;
    int iy0 = (int)floorf(miny) - 1;
    int ix1 = (int)ceilf(maxx) + 1;
    int iy1 = (int)ceilf(maxy) + 1;
    int w = ix1 - ix0;
    int h = iy1 - iy0;
    if (w <= 0 || h <= 0 || w > 512 || h > 512) {
        free(el.edges);
        return slot;
    }

    uint8_t *alpha = (uint8_t *)calloc((size_t)w * (size_t)h, 1);
    float *row_cov = (float *)malloc((size_t)w * sizeof(float));
    TT_Hit *hits   = (TT_Hit *)malloc(el.count * sizeof(TT_Hit));
    if (!alpha || !row_cov || !hits) {
        free(alpha); free(row_cov); free(hits); free(el.edges);
        return slot;
    }

    const int SUB = 4;
    for (int py = 0; py < h; py++) {
        memset(row_cov, 0, (size_t)w * sizeof(float));
        for (int s = 0; s < SUB; s++) {
            float y = (float)(iy0 + py) + ((float)s + 0.5f) / (float)SUB;
            size_t nhits = 0;
            for (size_t i = 0; i < el.count; i++) {
                const TT_Edge *e = &el.edges[i];
                if (e->y0 <= y && y < e->y1) {
                    float t = (y - e->y0) / (e->y1 - e->y0);
                    float x = e->x0 + t * (e->x1 - e->x0);
                    hits[nhits++] = (TT_Hit){ x - (float)ix0, e->dir };
                }
            }
            if (nhits < 2) continue;
            qsort(hits, nhits, sizeof(TT_Hit), cmp_hit);

            int wind = 0;
            for (size_t i = 0; i + 1 < nhits; i++) {
                wind += hits[i].dir;
                if (wind != 0) {
                    float xa = hits[i].x;
                    float xb = hits[i + 1].x;
                    if (xb <= xa) continue;
                    int xstart = (int)floorf(xa);
                    int xend   = (int)floorf(xb);
                    if (xstart < 0) xstart = 0;
                    if (xend >= w) xend = w - 1;
                    for (int px = xstart; px <= xend; px++) {
                        float l = (xa > (float)px) ? xa : (float)px;
                        float r = (xb < (float)(px + 1)) ? xb : (float)(px + 1);
                        if (r > l) row_cov[px] += (r - l) * (1.0f / (float)SUB);
                    }
                }
            }
        }
        uint8_t *dst = alpha + (size_t)py * (size_t)w;
        for (int px = 0; px < w; px++) {
            float c = row_cov[px];
            if (c <= 0.0f) dst[px] = 0;
            else if (c >= 1.0f) dst[px] = 255;
            else dst[px] = (uint8_t)(c * 255.0f + 0.5f);
        }
    }

    free(row_cov);
    free(hits);
    free(el.edges);

    slot->w = w;
    slot->h = h;
    slot->xoff = ix0;
    slot->yoff = iy0;
    slot->alpha = alpha;
    return slot;
}

int TTF_Init(void) {
    return 0;
}

void TTF_Quit(void) {
}

TTF_Font *TTF_OpenFontRW(SDL_RWops *src, int freesrc, int ptsize) {
    if (!src || !src->data || src->size < 64) {
        if (src && freesrc) SDL_FreeRW(src);
        return NULL;
    }

    TTF_Font *f = (TTF_Font *)calloc(1, sizeof(TTF_Font));
    if (!f) {
        if (freesrc) SDL_FreeRW(src);
        return NULL;
    }

    if (freesrc && src->own_mem) {
        f->buf = (uint8_t *)(uintptr_t)src->data;
        src->data = NULL;
        f->own_buf = true;
    } else if (!src->own_mem && !freesrc) {
        f->buf = (uint8_t *)malloc(src->size);
        if (f->buf) memcpy(f->buf, src->data, src->size);
        f->own_buf = true;
    } else {
        /* Safe to reference const static incbin buffer or own copy */
        if (src->own_mem) {
            f->buf = (uint8_t *)malloc(src->size);
            if (f->buf) memcpy(f->buf, src->data, src->size);
            f->own_buf = true;
        } else {
            f->buf = (uint8_t *)(uintptr_t)src->data;
            f->own_buf = false;
        }
    }
    f->size = src->size;
    f->ptsize = (ptsize > 1) ? ptsize : 12;

    {
        uint64_t h = 14695981039346656037ULL ^ (uint64_t)f->size ^ ((uint64_t)f->ptsize << 32);
        size_t n = (f->size < 2048u) ? f->size : 2048u;
        for (size_t i = 0; i < n; i++) {
            h ^= f->buf[i];
            h *= 1099511628211ULL;
        }
        f->content_hash = h ? h : 1ULL;
    }

    if (freesrc) SDL_FreeRW(src);
    if (!f->buf) { free(f); return NULL; }

    f->cmap_off = tt_find_table(f, "cmap");
    f->loca_off = tt_find_table(f, "loca");
    f->glyf_off = tt_find_table(f, "glyf");
    f->hmtx_off = tt_find_table(f, "hmtx");
    uint32_t head = tt_find_table(f, "head");
    uint32_t hhea = tt_find_table(f, "hhea");
    uint32_t maxp = tt_find_table(f, "maxp");

    if (!head || !hhea || !maxp || !f->cmap_off || !f->loca_off || !f->glyf_off || !f->hmtx_off) {
        TTF_CloseFont(f);
        return NULL;
    }

    f->units_per_em        = rd_u16(f->buf + head + 18);
    f->index_to_loc_format = rd_s16(f->buf + head + 50);
    f->ascent              = rd_s16(f->buf + hhea + 4);
    f->descent             = rd_s16(f->buf + hhea + 6);
    f->line_gap            = rd_s16(f->buf + hhea + 8);
    f->num_hmetrics        = rd_u16(f->buf + hhea + 34);
    f->num_glyphs          = rd_u16(f->buf + maxp + 4);

    if (f->units_per_em == 0) {
        TTF_CloseFont(f);
        return NULL;
    }

    f->scale = (float)f->ptsize / (float)f->units_per_em;
    int asc_px  = (int)ceilf((float)f->ascent * f->scale);
    int desc_px = (int)floorf((float)f->descent * f->scale);
    int gap_px  = (int)floorf((float)f->line_gap * f->scale + 0.5f);
    f->baseline = asc_px;
    f->line_height = asc_px - desc_px + gap_px;
    if (f->line_height < f->ptsize) f->line_height = f->ptsize;

    return f;
}

TTF_Font *TTF_OpenFont(const char *file, int ptsize) {
    if (!file || !file[0]) return NULL;
    FILE *fp = fopen(file, "rb");
    if (!fp && file[0] != '/') {
        const char *rel = (file[0] == '.' && file[1] == '/') ? (file + 2) : file;
        static const char prefix[] = "/data/SENSE: The Game/";
        size_t plen = sizeof(prefix) - 1u;
        size_t flen = strlen(rel);
        char *alt = (char *)malloc(plen + flen + 1u);
        if (alt) {
            memcpy(alt, prefix, plen);
            memcpy(alt + plen, rel, flen + 1u);
            fp = fopen(alt, "rb");
            free(alt);
        }
    }
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz <= 0 || sz > 32 * 1024 * 1024) { fclose(fp); return NULL; }
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);

    SDL_RWops *rw = (SDL_RWops *)calloc(1, sizeof(SDL_RWops));
    if (!rw) { free(buf); return NULL; }
    rw->data = buf;
    rw->size = (size_t)sz;
    rw->own_mem = SDL_TRUE;
    return TTF_OpenFontRW(rw, 1, ptsize);
}

void TTF_CloseFont(TTF_Font *font) {
    if (!font) return;
    for (int i = 0; i < GLYPH_CACHE_CAP; i++) {
        free(font->cache[i].alpha);
    }
    if (font->own_buf) free(font->buf);
    free(font);
}

void TTF_SetFontHinting(TTF_Font *font, int hinting) {
    if (font) font->hinting = hinting;
}

int TTF_FontHeight(const TTF_Font *font) {
    return font ? font->line_height : 0;
}

static uint32_t decode_utf8(const char **p) {
    const uint8_t *s = (const uint8_t *)(*p);
    uint8_t c0 = s[0];
    if (c0 < 0x80u) {
        (*p) += 1;
        return c0;
    }
    if ((c0 & 0xE0u) == 0xC0u && (s[1] & 0xC0u) == 0x80u) {
        (*p) += 2;
        return ((uint32_t)(c0 & 0x1Fu) << 6) | (uint32_t)(s[1] & 0x3Fu);
    }
    if ((c0 & 0xF0u) == 0xE0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u) {
        (*p) += 3;
        return ((uint32_t)(c0 & 0x0Fu) << 12) | ((uint32_t)(s[1] & 0x3Fu) << 6) |
               (uint32_t)(s[2] & 0x3Fu);
    }
    if ((c0 & 0xF8u) == 0xF0u && (s[1] & 0xC0u) == 0x80u &&
        (s[2] & 0xC0u) == 0x80u && (s[3] & 0xC0u) == 0x80u) {
        (*p) += 4;
        return ((uint32_t)(c0 & 0x07u) << 18) | ((uint32_t)(s[1] & 0x3Fu) << 12) |
               ((uint32_t)(s[2] & 0x3Fu) << 6) | (uint32_t)(s[3] & 0x3Fu);
    }
    (*p) += 1;
    return '?';
}

#define TTF_TEXT_CACHE_CAP 64

typedef struct {
    uint64_t font_hash;
    uint64_t text_hash;
    uint32_t rgba;
    size_t   text_len;
    char    *text;
    int      w, h;
    uint32_t *pixels;
    uint64_t cache_key;
    uint32_t stamp;
} TTFTextCacheEntry;

static TTFTextCacheEntry s_ttf_cache[TTF_TEXT_CACHE_CAP];
static uint32_t s_ttf_cache_stamp = 0;
static uint64_t s_ttf_cache_ver = 1;

static uint64_t fnv1a_text(const char *s, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static SDL_Surface *make_cached_surface_handle(const TTFTextCacheEntry *e, Uint8 alpha_mod) {
    SDL_Surface *s = (SDL_Surface *)calloc(1, sizeof(SDL_Surface));
    if (!s) return NULL;
    s->w = e->w;
    s->h = e->h;
    s->pitch = e->w * 4;
    s->pixels = e->pixels;
    s->is_premultiplied = SDL_FALSE;
    s->borrowed_pixels = SDL_TRUE;
    s->has_alpha_mod = SDL_TRUE;
    s->alpha_mod = alpha_mod;
    s->cache_key = e->cache_key;
    return s;
}

SDL_Surface *TTF_RenderUTF8_Blended(TTF_Font *font, const char *text, SDL_Color fg) {
    if (!font) return NULL;
    if (!text) text = "";

    size_t text_len = strlen(text);
    uint64_t text_hash = fnv1a_text(text, text_len);
    /* Key cache strictly on RGB and apply fg.a at texture blit time so fades never bust cache. */
    uint32_t rgb = ((uint32_t)fg.r << 16) | ((uint32_t)fg.g << 8) | (uint32_t)fg.b;

    int lru_idx = 0;
    uint32_t lru_stamp = 0xFFFFFFFFu;
    for (int i = 0; i < TTF_TEXT_CACHE_CAP; i++) {
        TTFTextCacheEntry *e = &s_ttf_cache[i];
        if (e->pixels && e->font_hash == font->content_hash &&
            e->text_hash == text_hash && e->rgba == rgb &&
            e->text_len == text_len && memcmp(e->text, text, text_len) == 0) {
            e->stamp = ++s_ttf_cache_stamp;
            return make_cached_surface_handle(e, fg.a);
        }
        if (!e->pixels) {
            lru_idx = i;
            lru_stamp = 0;
            break;
        }
        if (e->stamp < lru_stamp) {
            lru_stamp = e->stamp;
            lru_idx = i;
        }
    }

    int total_w = 0;
    int max_x = 0;
    const char *p = text;
    while (*p) {
        uint32_t cp = decode_utf8(&p);
        const TT_CachedGlyph *g = tt_get_glyph(font, cp);
        if (g->alpha) {
            int gx1 = total_w + g->xoff + g->w;
            if (gx1 > max_x) max_x = gx1;
        }
        total_w += g->advance;
        if (total_w > max_x) max_x = total_w;
    }

    int w = (max_x > 0) ? max_x + 2 : 1;
    int h = (font->line_height > 0) ? font->line_height + 2 : 1;

    uint32_t *pix = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (!pix) return NULL;

    uint32_t base_rgb = ((uint32_t)fg.r << 16) | ((uint32_t)fg.g << 8) | (uint32_t)fg.b;
    int pen_x = 1;
    int base_y = font->baseline;
    p = text;

    while (*p) {
        uint32_t cp = decode_utf8(&p);
        const TT_CachedGlyph *g = tt_get_glyph(font, cp);
        if (g->alpha && g->w > 0 && g->h > 0) {
            int dx0 = pen_x + g->xoff;
            int dy0 = base_y + g->yoff;
            for (int row = 0; row < g->h; row++) {
                int dy = dy0 + row;
                if (dy < 0 || dy >= h) continue;
                const uint8_t *src = g->alpha + (size_t)row * (size_t)g->w;
                uint32_t *dst = pix + (size_t)dy * (size_t)w;
                for (int col = 0; col < g->w; col++) {
                    int dx = dx0 + col;
                    if (dx < 0 || dx >= w) continue;
                    uint8_t a = src[col];
                    if (a == 0) continue;
                    uint8_t old_a = (uint8_t)(dst[dx] >> 24);
                    if (a > old_a) dst[dx] = ((uint32_t)a << 24) | base_rgb;
                }
            }
        }
        pen_x += g->advance;
    }

    char *tcopy = (char *)malloc(text_len + 1u);
    if (tcopy && text_len <= 1024u && (size_t)w * (size_t)h <= 512u * 1024u) {
        memcpy(tcopy, text, text_len + 1u);
        TTFTextCacheEntry *slot = &s_ttf_cache[lru_idx];
        free(slot->text);
        free(slot->pixels);
        slot->font_hash = font->content_hash;
        slot->text_hash = text_hash;
        slot->rgba = rgb;
        slot->text_len = text_len;
        slot->text = tcopy;
        slot->w = w;
        slot->h = h;
        slot->pixels = pix;
        slot->cache_key = ++s_ttf_cache_ver;
        slot->stamp = ++s_ttf_cache_stamp;
        SDL_Surface *s = make_cached_surface_handle(slot, fg.a);
        if (s) return s;
    } else {
        free(tcopy);
    }

    SDL_Surface *s = (SDL_Surface *)calloc(1, sizeof(SDL_Surface));
    if (!s) { free(pix); return NULL; }
    s->w = w;
    s->h = h;
    s->pitch = w * 4;
    s->pixels = pix;
    s->is_premultiplied = SDL_FALSE;
    s->has_alpha_mod = SDL_TRUE;
    s->alpha_mod = fg.a;
    return s;
}
