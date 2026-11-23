#include "cxx_unwind.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

extern const uint8_t __eh_frame_start[] __attribute__((weak));
extern const uint8_t __eh_frame_end[] __attribute__((weak));

typedef struct {
    uint32_t regs[9]; /* 0:eax 1:ecx 2:edx 3:ebx 4:esp 5:ebp 6:esi 7:edi 8:eip */
    uint32_t cfa;
} unwind_regs_t;

struct _Unwind_Context {
    unwind_regs_t state_regs;
    uint32_t args_size;
    uintptr_t func_start;
    uintptr_t lsda;
    _Unwind_Personality_Fn personality;
    int ip_before_insn;
    int have_fde;
};

extern void _Unwind_InstallContext(const unwind_regs_t *regs) __attribute__((__noreturn__));

typedef struct {
    const uint8_t *begin;
    const uint8_t *end;
} eh_range_t;

static pthread_mutex_t s_eh_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static eh_range_t *s_dyn_ranges = NULL;
static size_t s_dyn_count = 0u;
static size_t s_dyn_cap = 0u;

void __register_frame_info(const void *begin, void *ob) {
    (void)ob;
    if (!begin) return;
    pthread_mutex_lock(&s_eh_reg_lock);
    for (size_t i = 0u; i < s_dyn_count; i++) {
        if (s_dyn_ranges[i].begin == (const uint8_t *)begin) {
            pthread_mutex_unlock(&s_eh_reg_lock);
            return;
        }
    }
    if (s_dyn_count == s_dyn_cap) {
        size_t ncap = s_dyn_cap ? (s_dyn_cap * 2u) : 8u;
        eh_range_t *nr = (eh_range_t *)realloc(s_dyn_ranges, ncap * sizeof(eh_range_t));
        if (!nr) {
            pthread_mutex_unlock(&s_eh_reg_lock);
            return;
        }
        s_dyn_ranges = nr;
        s_dyn_cap = ncap;
    }
    s_dyn_ranges[s_dyn_count].begin = (const uint8_t *)begin;
    s_dyn_ranges[s_dyn_count].end = NULL;
    s_dyn_count++;
    pthread_mutex_unlock(&s_eh_reg_lock);
}

void __deregister_frame_info(const void *begin) {
    if (!begin) return;
    pthread_mutex_lock(&s_eh_reg_lock);
    for (size_t i = 0u; i < s_dyn_count; i++) {
        if (s_dyn_ranges[i].begin == (const uint8_t *)begin) {
            s_dyn_ranges[i] = s_dyn_ranges[--s_dyn_count];
            break;
        }
    }
    pthread_mutex_unlock(&s_eh_reg_lock);
}

static uint32_t read_uleb128(const uint8_t **pp) {
    const uint8_t *p = *pp;
    uint32_t val = 0u;
    uint32_t shift = 0u;
    uint8_t b;
    do {
        b = *p++;
        val |= ((uint32_t)(b & 0x7Fu)) << shift;
        shift += 7u;
    } while (b & 0x80u);
    *pp = p;
    return val;
}

static int32_t read_sleb128(const uint8_t **pp) {
    const uint8_t *p = *pp;
    uint32_t val = 0u;
    uint32_t shift = 0u;
    uint8_t b;
    do {
        b = *p++;
        val |= ((uint32_t)(b & 0x7Fu)) << shift;
        shift += 7u;
    } while (b & 0x80u);
    if ((shift < 32u) && (b & 0x40u)) {
        val |= (~0u << shift);
    }
    *pp = p;
    return (int32_t)val;
}

static uintptr_t read_encoded_ptr(const uint8_t **pp, uint8_t enc, uintptr_t rel_base) {
    if (enc == 0xFFu) return 0u;
    const uint8_t *p = *pp;
    const uint8_t *field_addr = p;
    uint8_t fmt = enc & 0x0Fu;
    uint8_t app = enc & 0x70u;
    uint32_t raw = 0u;
    int signed_val = 0;

    switch (fmt) {
    case 0x00u: /* absptr */
    case 0x03u: /* udata4 */
        memcpy(&raw, p, 4u);
        p += 4;
        break;
    case 0x01u: /* uleb128 */
        raw = read_uleb128(&p);
        break;
    case 0x02u: { /* udata2 */
        uint16_t v = 0u;
        memcpy(&v, p, 2u);
        raw = v;
        p += 2;
        break;
    }
    case 0x04u: { /* udata8 */
        uint64_t v = 0ULL;
        memcpy(&v, p, 8u);
        raw = (uint32_t)v;
        p += 8;
        break;
    }
    case 0x09u: /* sleb128 */
        raw = (uint32_t)read_sleb128(&p);
        signed_val = 1;
        break;
    case 0x0Au: { /* sdata2 */
        int16_t v = 0;
        memcpy(&v, p, 2u);
        raw = (uint32_t)(int32_t)v;
        signed_val = 1;
        p += 2;
        break;
    }
    case 0x0Bu: { /* sdata4 */
        int32_t v = 0;
        memcpy(&v, p, 4u);
        raw = (uint32_t)v;
        signed_val = 1;
        p += 4;
        break;
    }
    case 0x0Cu: { /* sdata8 */
        int64_t v = 0;
        memcpy(&v, p, 8u);
        raw = (uint32_t)v;
        signed_val = 1;
        p += 8;
        break;
    }
    default:
        break;
    }
    *pp = p;

    if (raw == 0u && app != 0u) return 0u;
    (void)signed_val;

    uintptr_t res = 0u;
    switch (app) {
    case 0x00u: res = (uintptr_t)raw; break;
    case 0x10u: res = (uintptr_t)field_addr + (uintptr_t)raw; break; /* pcrel */
    case 0x20u: /* textrel */
    case 0x30u: /* datarel */
    case 0x40u: /* funcrel */
        res = rel_base + (uintptr_t)raw;
        break;
    case 0x50u: res = (uintptr_t)raw; break; /* aligned */
    default:    res = (uintptr_t)raw; break;
    }

    if ((enc & 0x80u) && res != 0u) {
        uintptr_t ind = 0u;
        memcpy(&ind, (const void *)res, sizeof(ind));
        res = ind;
    }
    return res;
}

typedef enum {
    CFA_REG_UNDEF = 0,
    CFA_REG_SAME,
    CFA_REG_OFFSET,
    CFA_REG_VAL_OFFSET,
    CFA_REG_REGISTER,
    CFA_REG_EXPRESSION,
    CFA_REG_VAL_EXPRESSION
} cfa_reg_rule_t;

#define UW_NUM_REGS 9u

typedef struct {
    uint32_t cfa_reg;
    int32_t cfa_off;
    const uint8_t *cfa_expr;
    uint32_t cfa_expr_len;
    int cfa_is_expr;

    cfa_reg_rule_t rules[UW_NUM_REGS];
    int32_t vals[UW_NUM_REGS];
    const uint8_t *exprs[UW_NUM_REGS];
    uint32_t expr_lens[UW_NUM_REGS];
    uint32_t args_size;
} cfa_state_t;

typedef struct {
    const uint8_t *fde_start;
    const uint8_t *fde_end;
    const uint8_t *cie_start;
    uintptr_t pc_begin;
    uintptr_t pc_range;
    uintptr_t lsda;
    _Unwind_Personality_Fn personality;
    uint32_t code_align;
    int32_t data_align;
    uint32_t ra_reg;
    int is_signal;
    const uint8_t *cie_insns;
    const uint8_t *cie_insns_end;
    const uint8_t *fde_insns;
    const uint8_t *fde_insns_end;
} fde_info_t;

static int parse_cie(const uint8_t *cie_start,
                     uint32_t *out_code_align, int32_t *out_data_align,
                     uint32_t *out_ra_reg, uint8_t *out_lsda_enc,
                     uint8_t *out_fde_enc, _Unwind_Personality_Fn *out_pers,
                     int *out_is_signal,
                     const uint8_t **out_insns, const uint8_t **out_insns_end) {
    const uint8_t *p = cie_start;
    uint32_t len = 0u;
    memcpy(&len, p, 4u);
    p += 4;
    if (len < 8u || len == 0xFFFFFFFFu) return -1;
    const uint8_t *end = p + len;
    uint32_t cie_id = 0u;
    memcpy(&cie_id, p, 4u);
    p += 4;
    if (cie_id != 0u) return -1;

    uint8_t version = *p++;
    const char *aug = (const char *)p;
    p += strlen(aug) + 1u;
    if (version >= 4u) {
        p += 2; /* address_size, segment_size */
    }
    *out_code_align = read_uleb128(&p);
    *out_data_align = read_sleb128(&p);
    *out_ra_reg = (version == 1u) ? (uint32_t)*p++ : read_uleb128(&p);

    *out_lsda_enc = 0xFFu;
    *out_fde_enc = 0x00u;
    *out_pers = NULL;
    *out_is_signal = 0;

    if (aug[0] == 'z') {
        uint32_t aug_len = read_uleb128(&p);
        const uint8_t *aug_end = p + aug_len;
        if (aug_end > end) return -1;
        for (const char *a = aug + 1; *a && p < aug_end; a++) {
            if (*a == 'L') {
                *out_lsda_enc = *p++;
            } else if (*a == 'P') {
                uint8_t penc = *p++;
                *out_pers = (_Unwind_Personality_Fn)read_encoded_ptr(&p, penc, 0u);
            } else if (*a == 'R') {
                *out_fde_enc = *p++;
            } else if (*a == 'S') {
                *out_is_signal = 1;
            } else {
                break;
            }
        }
        p = aug_end;
    }

    *out_insns = p;
    *out_insns_end = end;
    return 0;
}

static int scan_eh_range(const uint8_t *start, const uint8_t *end, uintptr_t target_ip, fde_info_t *out) {
    if (!start) return 0;
    const uint8_t *p = start;
    while (!end || p + 4u <= end) {
        const uint8_t *rec_start = p;
        uint32_t len = 0u;
        memcpy(&len, p, 4u);
        p += 4;
        if (len == 0u || len == 0xFFFFFFFFu) break;
        const uint8_t *rec_end = p + len;
        if (end && rec_end > end) break;
        if (len < 4u) break;

        uint32_t cie_off = 0u;
        memcpy(&cie_off, p, 4u);
        if (cie_off == 0u) {
            p = rec_end;
            continue;
        }
        const uint8_t *cie_start = p - cie_off;
        if (cie_start < start || cie_start + 8u > rec_start) {
            p = rec_end;
            continue;
        }
        p += 4;

        uint32_t code_align = 1u;
        int32_t data_align = -4;
        uint32_t ra_reg = 8u;
        uint8_t lsda_enc = 0xFFu;
        uint8_t fde_enc = 0x00u;
        _Unwind_Personality_Fn pers = NULL;
        int is_signal = 0;
        const uint8_t *cie_insns = NULL;
        const uint8_t *cie_insns_end = NULL;

        if (parse_cie(cie_start, &code_align, &data_align, &ra_reg, &lsda_enc,
                      &fde_enc, &pers, &is_signal, &cie_insns, &cie_insns_end) != 0) {
            p = rec_end;
            continue;
        }

        uintptr_t pc_begin = read_encoded_ptr(&p, fde_enc, 0u);
        uintptr_t pc_range = read_encoded_ptr(&p, fde_enc & 0x0Fu, 0u);

        if (target_ip >= pc_begin && target_ip < pc_begin + pc_range) {
            uintptr_t lsda = 0u;
            const char *cie_aug = (const char *)(cie_start + 9u);
            if (cie_aug[0] == 'z') {
                uint32_t flen = read_uleb128(&p);
                const uint8_t *fend = p + flen;
                if (lsda_enc != 0xFFu && p < fend) {
                    lsda = read_encoded_ptr(&p, lsda_enc, pc_begin);
                }
                p = fend;
            }
            out->fde_start = rec_start;
            out->fde_end = rec_end;
            out->cie_start = cie_start;
            out->pc_begin = pc_begin;
            out->pc_range = pc_range;
            out->lsda = lsda;
            out->personality = pers;
            out->code_align = code_align;
            out->data_align = data_align;
            out->ra_reg = ra_reg;
            out->is_signal = is_signal;
            out->cie_insns = cie_insns;
            out->cie_insns_end = cie_insns_end;
            out->fde_insns = p;
            out->fde_insns_end = rec_end;
            return 1;
        }
        p = rec_end;
    }
    return 0;
}

static int find_fde(uintptr_t target_ip, fde_info_t *out) {
    if (__eh_frame_start && scan_eh_range(__eh_frame_start, __eh_frame_end, target_ip, out)) {
        return 1;
    }
    pthread_mutex_lock(&s_eh_reg_lock);
    for (size_t i = 0u; i < s_dyn_count; i++) {
        if (scan_eh_range(s_dyn_ranges[i].begin, s_dyn_ranges[i].end, target_ip, out)) {
            pthread_mutex_unlock(&s_eh_reg_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&s_eh_reg_lock);
    return 0;
}

static uint32_t eval_dwarf_expr(const uint8_t *expr, uint32_t len,
                                const unwind_regs_t *regs, uint32_t initial_cfa,
                                int has_initial) {
    uint32_t stack[32];
    int sp = 0;
    if (has_initial) {
        stack[sp++] = initial_cfa;
    }
    const uint8_t *p = expr;
    const uint8_t *end = expr + len;
    while (p < end) {
        uint8_t op = *p++;
        if (op >= 0x30u && op <= 0x4Fu) {
            if (sp < 32) stack[sp++] = op - 0x30u;
        } else if (op >= 0x70u && op <= 0x8Fu) {
            uint32_t reg = op - 0x70u;
            int32_t off = read_sleb128(&p);
            uint32_t base = (reg < UW_NUM_REGS) ? regs->regs[reg] : 0u;
            if (sp < 32) stack[sp++] = (uint32_t)((int32_t)base + off);
        } else {
            switch (op) {
            case 0x06u: /* DW_OP_deref */
                if (sp > 0) {
                    uint32_t v = 0u;
                    memcpy(&v, (const void *)(uintptr_t)stack[sp - 1], 4u);
                    stack[sp - 1] = v;
                }
                break;
            case 0x08u: if (sp < 32) stack[sp++] = *p++; break; /* const1u */
            case 0x09u: if (sp < 32) stack[sp++] = (uint32_t)(int32_t)*(const int8_t *)p; p++; break;
            case 0x0Au: { uint16_t v; memcpy(&v, p, 2u); p += 2; if (sp < 32) stack[sp++] = v; break; }
            case 0x0Bu: { int16_t v; memcpy(&v, p, 2u); p += 2; if (sp < 32) stack[sp++] = (uint32_t)(int32_t)v; break; }
            case 0x0Cu:
            case 0x0Du: { uint32_t v; memcpy(&v, p, 4u); p += 4; if (sp < 32) stack[sp++] = v; break; }
            case 0x10u: if (sp < 32) stack[sp++] = read_uleb128(&p); break;
            case 0x11u: if (sp < 32) stack[sp++] = (uint32_t)read_sleb128(&p); break;
            case 0x22u: if (sp >= 2) { stack[sp - 2] += stack[sp - 1]; sp--; } break; /* plus */
            case 0x23u: { uint32_t u = read_uleb128(&p); if (sp > 0) stack[sp - 1] += u; break; } /* plus_uconst */
            case 0x1Cu: if (sp >= 2) { stack[sp - 2] -= stack[sp - 1]; sp--; } break; /* minus */
            case 0x92u: { /* bregx */
                uint32_t reg = read_uleb128(&p);
                int32_t off = read_sleb128(&p);
                uint32_t base = (reg < UW_NUM_REGS) ? regs->regs[reg] : 0u;
                if (sp < 32) stack[sp++] = (uint32_t)((int32_t)base + off);
                break;
            }
            default:
                p = end;
                break;
            }
        }
    }
    return (sp > 0) ? stack[sp - 1] : 0u;
}

static void exec_cfa_insns(const uint8_t *p, const uint8_t *end,
                           const fde_info_t *fde, uintptr_t stop_ip,
                           uintptr_t *loc_io, cfa_state_t *st,
                           const cfa_state_t *cie_default) {
    cfa_state_t *stack = NULL;
    size_t sp = 0u;
    size_t stack_cap = 0u;
    uintptr_t loc = *loc_io;

    while (p < end && loc <= stop_ip) {
        uint8_t op = *p++;
        uint8_t hi = op & 0xC0u;
        uint8_t lo = op & 0x3Fu;

        if (hi == 0x40u) { /* DW_CFA_advance_loc */
            loc += (uintptr_t)lo * fde->code_align;
        } else if (hi == 0x80u) { /* DW_CFA_offset */
            uint32_t off = read_uleb128(&p);
            if (lo < UW_NUM_REGS) {
                st->rules[lo] = CFA_REG_OFFSET;
                st->vals[lo] = (int32_t)off * fde->data_align;
            }
        } else if (hi == 0xC0u) { /* DW_CFA_restore */
            if (lo < UW_NUM_REGS && cie_default) {
                st->rules[lo] = cie_default->rules[lo];
                st->vals[lo] = cie_default->vals[lo];
                st->exprs[lo] = cie_default->exprs[lo];
                st->expr_lens[lo] = cie_default->expr_lens[lo];
            }
        } else {
            switch (op) {
            case 0x00u: /* nop */
                break;
            case 0x01u: /* set_loc */
                loc = read_encoded_ptr(&p, 0x00u, fde->pc_begin);
                break;
            case 0x02u: /* advance_loc1 */
                loc += (uintptr_t)(*p++) * fde->code_align;
                break;
            case 0x03u: { /* advance_loc2 */
                uint16_t d = 0u;
                memcpy(&d, p, 2u);
                p += 2;
                loc += (uintptr_t)d * fde->code_align;
                break;
            }
            case 0x04u: { /* advance_loc4 */
                uint32_t d = 0u;
                memcpy(&d, p, 4u);
                p += 4;
                loc += (uintptr_t)d * fde->code_align;
                break;
            }
            case 0x05u: { /* offset_extended */
                uint32_t r = read_uleb128(&p);
                uint32_t off = read_uleb128(&p);
                if (r < UW_NUM_REGS) {
                    st->rules[r] = CFA_REG_OFFSET;
                    st->vals[r] = (int32_t)off * fde->data_align;
                }
                break;
            }
            case 0x06u: { /* restore_extended */
                uint32_t r = read_uleb128(&p);
                if (r < UW_NUM_REGS && cie_default) {
                    st->rules[r] = cie_default->rules[r];
                    st->vals[r] = cie_default->vals[r];
                    st->exprs[r] = cie_default->exprs[r];
                    st->expr_lens[r] = cie_default->expr_lens[r];
                }
                break;
            }
            case 0x07u: { /* undefined */
                uint32_t r = read_uleb128(&p);
                if (r < UW_NUM_REGS) st->rules[r] = CFA_REG_UNDEF;
                break;
            }
            case 0x08u: { /* same_value */
                uint32_t r = read_uleb128(&p);
                if (r < UW_NUM_REGS) st->rules[r] = CFA_REG_SAME;
                break;
            }
            case 0x09u: { /* register */
                uint32_t r1 = read_uleb128(&p);
                uint32_t r2 = read_uleb128(&p);
                if (r1 < UW_NUM_REGS && r2 < UW_NUM_REGS) {
                    st->rules[r1] = CFA_REG_REGISTER;
                    st->vals[r1] = (int32_t)r2;
                }
                break;
            }
            case 0x0Au: { /* remember_state */
                if (sp == stack_cap) {
                    size_t ncap = stack_cap ? (stack_cap * 2u) : 8u;
                    cfa_state_t *ns = (cfa_state_t *)realloc(stack, ncap * sizeof(cfa_state_t));
                    if (!ns) { p = end; break; }
                    stack = ns;
                    stack_cap = ncap;
                }
                stack[sp++] = *st;
                break;
            }
            case 0x0Bu: /* restore_state */
                if (sp > 0u) {
                    *st = stack[--sp];
                }
                break;
            case 0x0Cu: /* def_cfa */
                st->cfa_reg = read_uleb128(&p);
                st->cfa_off = (int32_t)read_uleb128(&p);
                st->cfa_is_expr = 0;
                break;
            case 0x0Du: /* def_cfa_register */
                st->cfa_reg = read_uleb128(&p);
                st->cfa_is_expr = 0;
                break;
            case 0x0Eu: /* def_cfa_offset */
                st->cfa_off = (int32_t)read_uleb128(&p);
                st->cfa_is_expr = 0;
                break;
            case 0x0Fu: { /* def_cfa_expression */
                uint32_t elen = read_uleb128(&p);
                st->cfa_expr = p;
                st->cfa_expr_len = elen;
                st->cfa_is_expr = 1;
                p += elen;
                break;
            }
            case 0x10u: { /* expression */
                uint32_t r = read_uleb128(&p);
                uint32_t elen = read_uleb128(&p);
                if (r < UW_NUM_REGS) {
                    st->rules[r] = CFA_REG_EXPRESSION;
                    st->exprs[r] = p;
                    st->expr_lens[r] = elen;
                }
                p += elen;
                break;
            }
            case 0x11u: { /* offset_extended_sf */
                uint32_t r = read_uleb128(&p);
                int32_t off = read_sleb128(&p);
                if (r < UW_NUM_REGS) {
                    st->rules[r] = CFA_REG_OFFSET;
                    st->vals[r] = off * fde->data_align;
                }
                break;
            }
            case 0x12u: /* def_cfa_sf */
                st->cfa_reg = read_uleb128(&p);
                st->cfa_off = read_sleb128(&p) * fde->data_align;
                st->cfa_is_expr = 0;
                break;
            case 0x13u: /* def_cfa_offset_sf */
                st->cfa_off = read_sleb128(&p) * fde->data_align;
                st->cfa_is_expr = 0;
                break;
            case 0x14u: { /* val_offset */
                uint32_t r = read_uleb128(&p);
                uint32_t off = read_uleb128(&p);
                if (r < UW_NUM_REGS) {
                    st->rules[r] = CFA_REG_VAL_OFFSET;
                    st->vals[r] = (int32_t)off * fde->data_align;
                }
                break;
            }
            case 0x15u: { /* val_offset_sf */
                uint32_t r = read_uleb128(&p);
                int32_t off = read_sleb128(&p);
                if (r < UW_NUM_REGS) {
                    st->rules[r] = CFA_REG_VAL_OFFSET;
                    st->vals[r] = off * fde->data_align;
                }
                break;
            }
            case 0x16u: { /* val_expression */
                uint32_t r = read_uleb128(&p);
                uint32_t elen = read_uleb128(&p);
                if (r < UW_NUM_REGS) {
                    st->rules[r] = CFA_REG_VAL_EXPRESSION;
                    st->exprs[r] = p;
                    st->expr_lens[r] = elen;
                }
                p += elen;
                break;
            }
            case 0x2Eu: /* DW_CFA_GNU_args_size */
                st->args_size = read_uleb128(&p);
                break;
            case 0x2Fu: { /* DW_CFA_GNU_negative_offset_extended */
                uint32_t r = read_uleb128(&p);
                uint32_t off = read_uleb128(&p);
                if (r < UW_NUM_REGS) {
                    st->rules[r] = CFA_REG_OFFSET;
                    st->vals[r] = -(int32_t)off * fde->data_align;
                }
                break;
            }
            default:
                p = end;
                break;
            }
        }
    }

    *loc_io = loc;
    free(stack);
}

static int uw_eval_frame(const unwind_regs_t *cur, int ip_before,
                         struct _Unwind_Context *ctx_out, cfa_state_t *st_out) {
    uintptr_t cur_ip = cur->regs[8];
    if (cur_ip == 0u) return 0;
    uintptr_t lookup_ip = ip_before ? cur_ip : (cur_ip - 1u);

    fde_info_t fde;
    if (!find_fde(lookup_ip, &fde)) {
        return 0;
    }

    cfa_state_t cie_st;
    memset(&cie_st, 0, sizeof(cie_st));
    for (size_t i = 0u; i < UW_NUM_REGS; i++) cie_st.rules[i] = CFA_REG_SAME;
    cie_st.rules[4] = CFA_REG_UNDEF;
    cie_st.cfa_reg = 4u;
    cie_st.cfa_off = 4;

    uintptr_t dummy_loc = fde.pc_begin;
    exec_cfa_insns(fde.cie_insns, fde.cie_insns_end, &fde, (uintptr_t)-1, &dummy_loc, &cie_st, NULL);

    cfa_state_t st = cie_st;
    uintptr_t loc = fde.pc_begin;
    exec_cfa_insns(fde.fde_insns, fde.fde_insns_end, &fde, lookup_ip, &loc, &st, &cie_st);

    uint32_t cfa = 0u;
    if (st.cfa_is_expr) {
        cfa = eval_dwarf_expr(st.cfa_expr, st.cfa_expr_len, cur, 0u, 0);
    } else if (st.cfa_reg < UW_NUM_REGS) {
        cfa = (uint32_t)((int32_t)cur->regs[st.cfa_reg] + st.cfa_off);
    }

    memset(ctx_out, 0, sizeof(*ctx_out));
    ctx_out->state_regs = *cur;
    ctx_out->state_regs.cfa = cfa;
    ctx_out->args_size = st.args_size;
    ctx_out->func_start = fde.pc_begin;
    ctx_out->lsda = fde.lsda;
    ctx_out->personality = fde.personality;
    ctx_out->ip_before_insn = fde.is_signal ? 1 : ip_before;
    ctx_out->have_fde = (int)fde.ra_reg + 1;
    *st_out = st;
    return 1;
}

static void uw_step_frame(const unwind_regs_t *cur, const struct _Unwind_Context *ctx,
                          const cfa_state_t *st, unwind_regs_t *caller, int *caller_ip_before) {
    uint32_t cfa = ctx->state_regs.cfa;
    uint32_t ra_reg = (uint32_t)(ctx->have_fde - 1);
    *caller = *cur;
    caller->regs[4] = cfa; /* SysV i386 CFA equals caller's ESP */

    for (size_t i = 0u; i < UW_NUM_REGS; i++) {
        switch (st->rules[i]) {
        case CFA_REG_OFFSET: {
            uint32_t addr = (uint32_t)((int32_t)cfa + st->vals[i]);
            memcpy(&caller->regs[i], (const void *)(uintptr_t)addr, 4u);
            break;
        }
        case CFA_REG_VAL_OFFSET:
            caller->regs[i] = (uint32_t)((int32_t)cfa + st->vals[i]);
            break;
        case CFA_REG_REGISTER:
            if ((size_t)st->vals[i] < UW_NUM_REGS) {
                caller->regs[i] = cur->regs[st->vals[i]];
            }
            break;
        case CFA_REG_EXPRESSION: {
            uint32_t addr = eval_dwarf_expr(st->exprs[i], st->expr_lens[i], cur, cfa, 1);
            memcpy(&caller->regs[i], (const void *)(uintptr_t)addr, 4u);
            break;
        }
        case CFA_REG_VAL_EXPRESSION:
            caller->regs[i] = eval_dwarf_expr(st->exprs[i], st->expr_lens[i], cur, cfa, 1);
            break;
        case CFA_REG_SAME:
        case CFA_REG_UNDEF:
        default:
            break;
        }
    }
    if (ra_reg < UW_NUM_REGS) {
        caller->regs[8] = caller->regs[ra_reg];
    }
    caller->cfa = cfa;
    *caller_ip_before = ctx->ip_before_insn ? 1 : 0;
}

_Unwind_Reason_Code _Unwind_RaiseException_impl(struct _Unwind_Exception *exc,
                                                const unwind_regs_t *entry_regs) {
    if (!exc || !entry_regs) return _URC_FATAL_PHASE1_ERROR;

    /* Phase 1: Search upward for handler */
    unwind_regs_t cur = *entry_regs;
    int ip_before = 0;
    uint32_t handler_cfa = 0u;
    int found = 0;

    while (1) {
        struct _Unwind_Context ctx;
        cfa_state_t st;
        if (!uw_eval_frame(&cur, ip_before, &ctx, &st)) {
            return _URC_END_OF_STACK;
        }
        if (ctx.personality) {
            _Unwind_Reason_Code rc = ctx.personality(1, _UA_SEARCH_PHASE,
                                                     exc->exception_class, exc, &ctx);
            if (rc == _URC_HANDLER_FOUND) {
                handler_cfa = ctx.state_regs.cfa;
                found = 1;
                break;
            }
            if (rc != _URC_CONTINUE_UNWIND) {
                return _URC_FATAL_PHASE1_ERROR;
            }
        }
        unwind_regs_t next;
        int next_before = 0;
        uw_step_frame(&cur, &ctx, &st, &next, &next_before);
        cur = next;
        ip_before = next_before;
    }

    if (!found) {
        return _URC_END_OF_STACK;
    }

    exc->private_1 = handler_cfa;
    exc->private_2 = 0u;

    /* Phase 2: Run cleanups and install matching handler */
    cur = *entry_regs;
    ip_before = 0;
    while (1) {
        struct _Unwind_Context ctx;
        cfa_state_t st;
        if (!uw_eval_frame(&cur, ip_before, &ctx, &st)) {
            return _URC_FATAL_PHASE2_ERROR;
        }
        if (ctx.personality) {
            _Unwind_Action act = _UA_CLEANUP_PHASE;
            if (ctx.state_regs.cfa == handler_cfa) {
                act |= _UA_HANDLER_FRAME;
            }
            _Unwind_Reason_Code rc = ctx.personality(1, act, exc->exception_class, exc, &ctx);
            if (rc == _URC_INSTALL_CONTEXT) {
                if (act & _UA_HANDLER_FRAME) {
                    exc->private_1 = 0u;
                    exc->private_2 = 0u;
                }
                unwind_regs_t install = ctx.state_regs;
                install.regs[4] += ctx.args_size;
                _Unwind_InstallContext(&install);
            }
            if (rc != _URC_CONTINUE_UNWIND) {
                return _URC_FATAL_PHASE2_ERROR;
            }
        }
        unwind_regs_t next;
        int next_before = 0;
        uw_step_frame(&cur, &ctx, &st, &next, &next_before);
        cur = next;
        ip_before = next_before;
    }
}

void _Unwind_Resume_impl(struct _Unwind_Exception *exc, const unwind_regs_t *entry_regs) {
    if (!exc || !entry_regs) abort();
    unwind_regs_t cur = *entry_regs;
    int ip_before = 0;
    uint32_t handler_cfa = exc->private_1;

    while (1) {
        struct _Unwind_Context ctx;
        cfa_state_t st;
        if (!uw_eval_frame(&cur, ip_before, &ctx, &st)) {
            abort();
        }
        if (ctx.personality) {
            _Unwind_Action act = _UA_CLEANUP_PHASE;
            if (handler_cfa != 0u && ctx.state_regs.cfa == handler_cfa) {
                act |= _UA_HANDLER_FRAME;
            }
            _Unwind_Reason_Code rc = ctx.personality(1, act, exc->exception_class, exc, &ctx);
            if (rc == _URC_INSTALL_CONTEXT) {
                if (act & _UA_HANDLER_FRAME) {
                    exc->private_1 = 0u;
                    exc->private_2 = 0u;
                }
                unwind_regs_t install = ctx.state_regs;
                install.regs[4] += ctx.args_size;
                _Unwind_InstallContext(&install);
            }
            if (rc != _URC_CONTINUE_UNWIND) {
                abort();
            }
        }
        unwind_regs_t next;
        int next_before = 0;
        uw_step_frame(&cur, &ctx, &st, &next, &next_before);
        cur = next;
        ip_before = next_before;
    }
}

_Unwind_Reason_Code _Unwind_Resume_or_Rethrow_impl(struct _Unwind_Exception *exc,
                                                     const unwind_regs_t *entry_regs) {
    if (exc && exc->private_1 != 0u) {
        _Unwind_Resume_impl(exc, entry_regs);
    }
    return _Unwind_RaiseException_impl(exc, entry_regs);
}

_Unwind_Reason_Code _Unwind_Backtrace_impl(_Unwind_Trace_Fn trace, void *arg,
                                           const unwind_regs_t *entry_regs) {
    if (!trace || !entry_regs) return _URC_NO_REASON;
    unwind_regs_t cur = *entry_regs;
    int ip_before = 0;

    while (1) {
        struct _Unwind_Context ctx;
        cfa_state_t st;
        if (!uw_eval_frame(&cur, ip_before, &ctx, &st)) {
            return _URC_END_OF_STACK;
        }
        _Unwind_Reason_Code rc = trace(&ctx, arg);
        if (rc != _URC_NO_REASON) {
            return rc;
        }
        unwind_regs_t next;
        int next_before = 0;
        uw_step_frame(&cur, &ctx, &st, &next, &next_before);
        cur = next;
        ip_before = next_before;
    }
}

void _Unwind_DeleteException(struct _Unwind_Exception *exc) {
    if (exc && exc->exception_cleanup) {
        exc->exception_cleanup(_URC_FOREIGN_EXCEPTION_CAUGHT, exc);
    }
}

uintptr_t _Unwind_GetGR(struct _Unwind_Context *context, int index) {
    if (!context || index < 0 || (size_t)index >= UW_NUM_REGS) return 0u;
    return context->state_regs.regs[index];
}

void _Unwind_SetGR(struct _Unwind_Context *context, int index, uintptr_t new_value) {
    if (!context || index < 0 || (size_t)index >= UW_NUM_REGS) return;
    context->state_regs.regs[index] = new_value;
}

uintptr_t _Unwind_GetIP(struct _Unwind_Context *context) {
    return context ? context->state_regs.regs[8] : 0u;
}

uintptr_t _Unwind_GetIPInfo(struct _Unwind_Context *context, int *ip_before_insn) {
    if (!context) {
        if (ip_before_insn) *ip_before_insn = 0;
        return 0u;
    }
    if (ip_before_insn) *ip_before_insn = context->ip_before_insn;
    return context->state_regs.regs[8];
}

void _Unwind_SetIP(struct _Unwind_Context *context, uintptr_t new_value) {
    if (context) {
        context->state_regs.regs[8] = new_value;
    }
}

uintptr_t _Unwind_GetCFA(struct _Unwind_Context *context) {
    return context ? context->state_regs.cfa : 0u;
}

void *_Unwind_GetLanguageSpecificData(struct _Unwind_Context *context) {
    return context ? (void *)context->lsda : NULL;
}

uintptr_t _Unwind_GetRegionStart(struct _Unwind_Context *context) {
    return context ? context->func_start : 0u;
}

uintptr_t _Unwind_GetDataRelBase(struct _Unwind_Context *context) {
    (void)context;
    return 0u;
}

uintptr_t _Unwind_GetTextRelBase(struct _Unwind_Context *context) {
    return context ? context->func_start : 0u;
}
