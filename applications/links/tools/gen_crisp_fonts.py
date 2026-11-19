#!/usr/bin/env python3
"""
Generate crisp 4bpp DejaVu Sans font tables + chunk-zlib world script packs
(Pan-European/Turkish/Vietnamese, Pan-Cyrillic, Greek, Armenian, Georgian,
Hebrew, Arabic + Presentation Forms, Lao, Thai, Devanagari, Tibetan, Ethiopic,
Cherokee, Chinese, Japanese, Korean, Symbols) for Links in IPO_OS_Software.
"""

import gzip
import os
import struct
import subprocess
import unicodedata
import zlib
from PIL import Image, ImageDraw, ImageFont

# Tier 1: Zero-decompression fast hot core (UI chrome + ASCII + Russian Cyrillic + symbols)
TIER1_FONTS = [
    ("IPO_FONT_REGULAR", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
     [10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32], False),
    ("IPO_FONT_BOLD", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
     [10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32], False),
    ("IPO_FONT_MONO", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
     [10, 11, 12, 13, 14, 15, 16, 18, 20], True),
]

TIER1_CPS = sorted(set(
    list(range(0x0020, 0x007F))
    + [
        0x00A0, 0x00A9, 0x00AB, 0x00AD, 0x00AE, 0x00B0, 0x00B7, 0x00BB,
        0x0401, 0x0451,
        0x2013, 0x2014, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E,
        0x2022, 0x2026, 0x2039, 0x203A, 0x20AC, 0x20BD, 0x2122,
        0x2190, 0x2191, 0x2192, 0x2193,
        0x25B2, 0x25B6, 0x25BC, 0x25C0, 0x25CF,
        0xFFFD,
    ]
    + list(range(0x0410, 0x0450))
))
TIER1_SET = set(TIER1_CPS)

# Tier 2: Chunk-zlib 4bpp DejaVu World pack (all non-Tier1 DejaVuSans scripts)
TIER2_RANGES = [
    (0x00A0, 0x02FF),  # Latin-1, Ext-A, Ext-B, IPA/Spacing (Polish, Czech, French, Turkish, Romanian, etc.)
    (0x0300, 0x036F),  # Combining Diacritical Marks
    (0x0370, 0x03FF),  # Greek & Coptic
    (0x0400, 0x052F),  # Cyrillic + Supplement (Ukrainian, Belarusian, Kazakh, Uzbek, Serbian, etc.)
    (0x0530, 0x058F),  # Armenian
    (0x0590, 0x05FF),  # Hebrew + Niqqud
    (0x0600, 0x06FF),  # Arabic + Harakat + Extended
    (0x0E80, 0x0EFF),  # Lao
    (0x10A0, 0x10FF),  # Georgian
    (0x1400, 0x167F),  # Canadian Aboriginal Syllabics
    (0x1E00, 0x1EFF),  # Latin Extended Additional (Vietnamese)
    (0x1F00, 0x1FFF),  # Greek Extended
    (0x2000, 0x27BF),  # Punctuation, Currency, Arrows, Math, Box, Geometric, Dingbats
    (0x2D00, 0x2D2F),  # Georgian Supplement
    (0xFB00, 0xFB06),  # Latin Ligatures
    (0xFB50, 0xFDFF),  # Arabic Presentation Forms-A (Persian/Kazakh)
    (0xFE70, 0xFEFF),  # Arabic Presentation Forms-B (Contextual forms + Lam-Alef)
]

TIER2_FACES = [
    ("R14", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 14),
    ("R18", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 18),
    ("B16", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 16),
]

CHUNK_T2 = 64
CHUNK_T3 = 128


def fc_charset(ttf_path):
    s = subprocess.check_output(["fc-query", "--format=%{charset}", ttf_path], text=True).strip()
    cps = set()
    for tok in s.split():
        if "-" in tok:
            a, b = tok.split("-")
            cps.update(range(int(a, 16), int(b, 16) + 1))
        else:
            cps.add(int(tok, 16))
    return cps


def fit_font_size(ttf_path, target_h):
    best = None
    for pt in range(max(6, target_h - 4), target_h + 6):
        f = ImageFont.truetype(ttf_path, pt)
        asc, desc = f.getmetrics()
        if asc + desc <= target_h:
            best = (f, asc, desc)
        elif best is None:
            best = (f, asc, desc)
    return best


def is_combining_cp(cp):
    if cp in (0x0E31, 0x0E47, 0x0E48, 0x0E49, 0x0E4A, 0x0E4B, 0x0E4C, 0x0E4D, 0x0E4E):
        return True
    if 0x0E34 <= cp <= 0x0E3A:
        return True
    if cp in (0x0900, 0x0901, 0x0902, 0x093C, 0x094D) or (0x0941 <= cp <= 0x0948) or (0x0951 <= cp <= 0x0954):
        return True
    if 0x064B <= cp <= 0x065F or cp == 0x0670:
        return True
    if 0x0591 <= cp <= 0x05C7:
        return True
    if 0x0300 <= cp <= 0x036F:
        return True
    return unicodedata.combining(chr(cp)) > 0


def is_wide_cp(cp):
    eaw = unicodedata.east_asian_width(chr(cp))
    return eaw in ("W", "F")


def rasterize_ttf_glyph(use_f, asc, baseline, target_h, cp, is_mono=False, mono_adv=0):
    ch = chr(cp)
    comb = is_combining_cp(cp)
    pad_top = baseline - asc
    sp_adv = use_f.getlength(" ")

    # Rasterize combining marks preceded by a space so libraqm never injects a dotted circle.
    if comb:
        cw = max(24, target_h * 3)
        ch_h = max(24, target_h + 12)
        ox, oy = 6, 4
        img = Image.new("L", (cw, ch_h), 0)
        drw = ImageDraw.Draw(img)
        drw.text((ox, oy), " " + ch, fill=255, font=use_f)
        bb = img.getbbox()
        if not bb:
            return 0, 0, 0, 0, 0, b""
        bx0, by0, bx1, by1 = bb
        src = img.crop(bb).tobytes()
        mw, mh = bx1 - bx0, by1 - by0
        y0 = pad_top + (by0 - oy)
        if y0 < 0:
            sy_start = -y0
            y0 = 0
        else:
            sy_start = 0
        bh = min(mh - sy_start, target_h - y0)
        if bh <= 0 or mw <= 0:
            return 0, 0, 0, 0, 0, b""
        xoff = max(-64, min(64, -((mw + 1) // 2)))
        adv = 0
        bw = mw
        sx_start = 0
    else:
        cw = max(24, target_h * 3)
        ch_h = max(24, target_h + 12)
        ox, oy = 4, 4
        img = Image.new("L", (cw, ch_h), 0)
        drw = ImageDraw.Draw(img)
        drw.text((ox, oy), ch, fill=255, font=use_f)
        bb = img.getbbox()
        if not bb:
            adv = mono_adv if is_mono else max(1, round(use_f.getlength(ch)))
            if cp in (0x200B, 0x200C, 0x200D, 0x200E, 0x200F, 0x2060, 0xFEFF):
                adv = 0
            return adv, 0, 0, 0, 0, b""

        bx0, by0, bx1, by1 = bb
        src = img.crop(bb).tobytes()
        mw, mh = bx1 - bx0, by1 - by0
        y0 = pad_top + (by0 - oy)
        sy_start = 0
        if y0 < 0:
            sy_start = -y0
            y0 = 0
        bh = mh - sy_start
        if y0 + bh > target_h:
            bh = target_h - y0
        if bh <= 0:
            adv = mono_adv if is_mono else max(1, round(use_f.getlength(ch)))
            return adv, 0, 0, 0, 0, b""

        rel_bx0 = bx0 - ox
        if is_mono:
            adv = mono_adv
            xoff = rel_bx0
            sx_start = 0
            bw = mw
            if xoff < 0:
                if mw <= adv:
                    xoff = 0
                else:
                    sx_start = -xoff
                    bw = mw - sx_start
                    xoff = 0
            if xoff + bw > adv:
                if bw <= adv:
                    xoff = adv - bw
                else:
                    bw = adv - xoff
        else:
            adv = max(1, round(use_f.getlength(ch)))
            xoff = max(0, rel_bx0)
            sx_start = 0
            bw = mw
            if xoff + bw > adv:
                adv = xoff + bw

    # Pack 4bpp alpha nibbles
    alpha_bytes = bytearray()
    for row in range(bh):
        srow = src[(sy_start + row) * mw + sx_start : (sy_start + row) * mw + sx_start + bw]
        for i in range(0, bw, 2):
            a0 = min(15, (srow[i] + 8) // 17)
            a1 = min(15, (srow[i + 1] + 8) // 17) if (i + 1 < bw) else 0
            alpha_bytes.append((a0 << 4) | a1)

    return min(255, adv), max(-128, min(127, xoff)), max(-128, min(127, y0)), bw, bh, bytes(alpha_bytes)


def rasterize_tier1_face(ttf_path, target_h, is_mono):
    use_f, asc, desc = fit_font_size(ttf_path, target_h)
    pad_top = max(0, (target_h - (asc + desc)) // 2)
    baseline = asc + pad_top
    mono_adv = max(4, round(use_f.getlength("M"))) if is_mono else 0

    glyphs = []
    alpha_blob = bytearray()
    for cp in TIER1_CPS:
        adv, xoff, y0, bw, bh, ab = rasterize_ttf_glyph(use_f, asc, baseline, target_h, cp, is_mono, mono_adv)
        off = len(alpha_blob)
        alpha_blob.extend(ab)
        glyphs.append((off, adv, xoff, max(0, y0), bw, bh))
    return baseline, mono_adv, glyphs, bytes(alpha_blob)


def load_pcf_glyphs(path, mapper=None, norm_h=None, norm_base=None):
    PCF_METRICS = 1 << 2
    PCF_BITMAPS = 1 << 3
    PCF_BDF_ENCODINGS = 1 << 5
    PCF_PROPERTIES = 1 << 0

    raw = gzip.open(path, "rb").read()
    tc = struct.unpack_from("<I", raw, 4)[0]
    tables = {}
    for i in range(tc):
        t, fmt, sz, off = struct.unpack_from("<IIII", raw, 8 + i * 16)
        tables[t] = (fmt, sz, off)

    line_asc = 14
    line_desc = 4
    if PCF_PROPERTIES in tables:
        fmt, sz, off = tables[PCF_PROPERTIES]
        end = "<" if not (fmt & 4) else ">"
        _, nprops = struct.unpack_from(end + "II", raw, off)
        poff = off + 8
        sbase = (poff + nprops * 9 + 3) & ~3
        slen = struct.unpack_from(end + "I", raw, sbase)[0]
        sdata = raw[sbase + 4 : sbase + 4 + slen]
        for i in range(nprops):
            noff, is_str, val = struct.unpack_from(end + "IBi", raw, poff + i * 9)
            n = sdata[noff : sdata.find(b"\x00", noff)].decode("latin1")
            if not is_str:
                if n == "FONT_ASCENT":
                    line_asc = val
                elif n == "FONT_DESCENT":
                    line_desc = val

    src_h = norm_h if norm_h is not None else max(8, line_asc + line_desc)
    src_base = norm_base if norm_base is not None else line_asc

    fmt, sz, off = tables[PCF_BDF_ENCODINGS]
    end = "<" if not (fmt & 4) else ">"
    _, min_b2, max_b2, min_b1, max_b1, _ = struct.unpack_from(end + "IHHHHH", raw, off)
    cols = max_b2 - min_b2 + 1
    rows = max_b1 - min_b1 + 1
    gidxs = struct.unpack_from(end + f"{cols * rows}H", raw, off + 14)

    fmt_m, _, off_m = tables[PCF_METRICS]
    end_m = "<" if not (fmt_m & 4) else ">"
    metrics = []
    if fmt_m & 0x100:
        nm = struct.unpack_from(end_m + "H", raw, off_m + 4)[0]
        for k in range(nm):
            lb, rb, mw, asc, desc = struct.unpack_from("BBBBB", raw, off_m + 6 + k * 5)
            metrics.append((lb - 0x80, rb - 0x80, mw - 0x80, asc - 0x80, desc - 0x80))
    else:
        nm = struct.unpack_from(end_m + "I", raw, off_m + 4)[0]
        for k in range(nm):
            lb, rb, mw, asc, desc, _ = struct.unpack_from(end_m + "hhhhhH", raw, off_m + 8 + k * 12)
            metrics.append((lb, rb, mw, asc, desc))

    fmt_b, _, off_b = tables[PCF_BITMAPS]
    end_b = "<" if not (fmt_b & 4) else ">"
    bit_msb = (fmt_b & 8) != 0
    byte_msb = (fmt_b & 4) != 0
    pad = 1 << (fmt_b & 3)
    ng = struct.unpack_from(end_b + "I", raw, off_b + 4)[0]
    offs = struct.unpack_from(end_b + f"{ng}I", raw, off_b + 8)
    bstart = off_b + 8 + ng * 4 + 16

    out = {}
    for r in range(rows):
        b1 = min_b1 + r
        for c in range(cols):
            b2 = min_b2 + c
            gi = gidxs[r * cols + c]
            if gi == 0xFFFF:
                continue
            code = (b1 << 8) | b2
            cp = mapper(code) if mapper else code
            if cp is None or cp < 0x20 or cp > 0xFFFF:
                continue
            # Skip PUA or surrogates
            if 0xD800 <= cp <= 0xF8FF:
                continue

            lb, rb, mw, asc, desc = metrics[gi]
            bw = max(0, rb - lb)
            bh = max(0, asc + desc)
            goff = bstart + offs[gi]
            rs = ((bw + pad * 8 - 1) // (pad * 8)) * pad if bw > 0 else pad
            row_stride_out = (bw + 7) // 8

            # Decode rows into MSB-first bytes per (bw+7)//8 and trim empty top/bottom rows
            rows_bits = []
            first_nz = bh
            last_nz = -1
            for y in range(bh):
                rbytes = raw[goff + y * rs : goff + (y + 1) * rs]
                orow = bytearray(row_stride_out)
                nz = False
                for x in range(bw):
                    bi = x // 8
                    if pad > 1 and byte_msb != bit_msb:
                        wi = x // (pad * 8)
                        within = (x % (pad * 8)) // 8
                        bi = wi * pad + (pad - 1 - within)
                    bv = rbytes[bi]
                    bit = (bv >> (7 - (x % 8))) & 1 if bit_msb else (bv >> (x % 8)) & 1
                    if bit:
                        orow[x // 8] |= (1 << (7 - (x % 8)))
                        nz = True
                if nz:
                    if first_nz == bh:
                        first_nz = y
                    last_nz = y
                rows_bits.append(bytes(orow))

            yoff = src_base - asc
            xoff = lb
            if last_nz < 0:
                bw_t, bh_t, packed = 0, 0, b""
            else:
                yoff += first_nz
                bh_t = last_nz - first_nz + 1
                bw_t = bw
                packed = b"".join(rows_bits[first_nz : last_nz + 1])

            comb = is_combining_cp(cp)
            adv = 0 if comb else max(1 if bw_t > 0 else mw, mw)
            flags = (1 if comb else 0) | (2 if is_wide_cp(cp) else 0)
            if comb and mw > 0 and xoff >= 0:
                # Fixed-cell Thai vowel/tone mark in 10x20: align around previous consonant center
                xoff = -((mw + 1) // 2) + lb
            out[cp] = (
                src_h,
                src_base,
                min(255, max(0, adv)),
                max(-128, min(127, xoff)),
                max(-128, min(127, yoff)),
                bw_t,
                bh_t,
                flags,
                packed,
            )
    return out


def main():
    port_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "port")
    h_path = os.path.join(port_dir, "ipo_crisp_fonts.h")
    world_c_path = os.path.join(port_dir, "ipo_world_fonts.c")
    c_path = os.path.join(port_dir, "ipo_crisp_fonts.c")

    # 1. Generate Tier 1 uncompressed hot atlas
    faces_meta = []
    blob = bytearray()
    for style_id, (sname, ttf_path, sizes, is_mono) in enumerate(TIER1_FONTS):
        for h in sizes:
            baseline, mono_adv, glyphs, ab = rasterize_tier1_face(ttf_path, h, is_mono)
            blob_base = len(blob)
            blob.extend(ab)
            adj_glyphs = [(blob_base + off, adv, xo, yo, bw, bh) for (off, adv, xo, yo, bw, bh) in glyphs]
            faces_meta.append((style_id, h, baseline, is_mono, adj_glyphs))

    # 2. Generate Tier 2 chunk-zlib 4bpp DejaVu world glyphs
    dj_reg = fc_charset("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")
    dj_bold = fc_charset("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf")
    t2_set = set()
    for lo, hi in TIER2_RANGES:
        for cp in range(lo, hi + 1):
            if cp in dj_reg and cp not in TIER1_SET:
                t2_set.add(cp)
    t2_cps = sorted(t2_set)

    t2_face_objs = []
    for _, ttf_path, target_h in TIER2_FACES:
        use_f, asc, desc = fit_font_size(ttf_path, target_h)
        pad_top = max(0, (target_h - (asc + desc)) // 2)
        baseline = asc + pad_top
        t2_face_objs.append((ttf_path, target_h, use_f, asc, baseline))

    t2_zblob = bytearray()
    t2_chunks = []  # (zoff, zlen, raw_len)
    for start in range(0, len(t2_cps), CHUNK_T2):
        chunk_cps = t2_cps[start : start + CHUNK_T2]
        n_g = len(chunk_cps)
        dir_bytes = n_g * 3 * 2
        payload = bytearray()
        rel_offs = []
        for cp in chunk_cps:
            comb = is_combining_cp(cp)
            flags = (1 if comb else 0) | (2 if is_wide_cp(cp) else 0)
            for face_i, (ttf_path, target_h, use_f, asc, baseline) in enumerate(t2_face_objs):
                # For bold face, verify bold glyph exists, else fall back to regular font
                if face_i == 2 and cp not in dj_bold:
                    use_r, asc_r, base_r = t2_face_objs[0][2], t2_face_objs[0][3], t2_face_objs[0][4]
                    th_r = t2_face_objs[0][1]
                    adv, xoff, y0, bw, bh, ab = rasterize_ttf_glyph(use_r, asc_r, base_r, th_r, cp)
                else:
                    adv, xoff, y0, bw, bh, ab = rasterize_ttf_glyph(use_f, asc, baseline, target_h, cp)
                rel = dir_bytes + len(payload)
                assert rel < 65535, f"Tier 2 chunk overflow: {rel}"
                rel_offs.append(rel)
                payload.extend(struct.pack("<BbbBBB", adv, xoff, y0, bw, bh, flags))
                payload.extend(ab)
        raw_chunk = struct.pack(f"<{len(rel_offs)}H", *rel_offs) + bytes(payload)
        z = zlib.compress(raw_chunk, 9)
        t2_chunks.append((len(t2_zblob), len(z), len(raw_chunk)))
        t2_zblob.extend(z)

    # 3. Generate Tier 3 chunk-zlib 1bpp PCF world BMP glyphs
    p_ja = load_pcf_glyphs("/usr/share/fonts/X11/misc/18x18ja.pcf.gz", norm_h=18, norm_base=14)
    p_ko = load_pcf_glyphs("/usr/share/fonts/X11/misc/18x18ko.pcf.gz", norm_h=18, norm_base=14)
    p_th = load_pcf_glyphs(
        "/usr/share/fonts/X11/misc/10x20-ISO8859-11.pcf.gz",
        mapper=lambda c: ord(bytes([c]).decode("tis_620")) if c >= 0xA1 else None,
        norm_h=20,
        norm_base=16,
    )
    p_u20 = load_pcf_glyphs("/usr/share/fonts/X11/misc/10x20.pcf.gz", norm_h=20, norm_base=16)
    p_u18 = load_pcf_glyphs("/usr/share/fonts/X11/misc/9x18.pcf.gz", norm_h=18, norm_base=14)
    p_cu = load_pcf_glyphs("/usr/share/fonts/X11/misc/cu12.pcf.gz", norm_h=20, norm_base=15)

    t3_map = {}
    # Reverse priority insert so higher priority wins cleanly on overlaps
    for src in (p_cu, p_u18, p_u20, p_th, p_ko, p_ja):
        t3_map.update(src)

    covered = TIER1_SET | t2_set
    t3_cps = sorted(cp for cp in t3_map.keys() if cp not in covered)
    t3_zblob = bytearray()
    t3_chunks = []
    for start in range(0, len(t3_cps), CHUNK_T3):
        chunk_cps = t3_cps[start : start + CHUNK_T3]
        n_g = len(chunk_cps)
        dir_bytes = n_g * 2
        payload = bytearray()
        rel_offs = []
        for cp in chunk_cps:
            src_h, src_base, adv, xoff, yoff, bw, bh, flags, packed = t3_map[cp]
            rel = dir_bytes + len(payload)
            assert rel < 65535, f"Tier 3 chunk overflow: {rel}"
            rel_offs.append(rel)
            payload.extend(struct.pack("<BBBbbBBB", src_h, src_base, adv, xoff, yoff, bw, bh, flags))
            payload.extend(packed)
        raw_chunk = struct.pack(f"<{len(rel_offs)}H", *rel_offs) + bytes(payload)
        z = zlib.compress(raw_chunk, 9)
        t3_chunks.append((len(t3_zblob), len(z), len(raw_chunk)))
        t3_zblob.extend(z)

    # 4. Write header ipo_crisp_fonts.h
    with open(h_path, "w") as hf:
        hf.write("""#ifndef LINKS_IPO_CRISP_FONTS_H
#define LINKS_IPO_CRISP_FONTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPO_FONT_REGULAR 0
#define IPO_FONT_BOLD    1
#define IPO_FONT_MONO    2

#define IPO_SHAPED_OVERSTRIKE 0x01u
#define IPO_SHAPED_RTL        0x02u
#define IPO_SHAPED_ZEROWIDTH  0x04u

typedef struct {
    uint32_t cp;
    uint8_t  flags;
} ipo_shaped_glyph_t;

int ipo_font_has_glyph(uint32_t cp);
int ipo_font_is_wide(uint32_t cp);
int ipo_font_is_combining(uint32_t cp);
int ipo_font_is_zero_width(uint32_t cp);
uint32_t ipo_font_compose_pair(uint32_t base, uint32_t mark);

/* Shape UTF-8 string into visual glyph run (NFD->NFC, Arabic contextual + Lam-Alef, RTL runs).
 * Returns total shaped length. Caller supplies stack buf out[out_cap]; if len > out_cap,
 * remaining glyphs are not written beyond out_cap (caller may retry with heap buffer). */
int ipo_font_shape_utf8(const char *utf8, ipo_shaped_glyph_t *out, int out_cap);

int ipo_font_measure_glyph(int style_idx, int height, uint32_t cp);
int ipo_font_text_width(int style_idx, int height, const char *utf8);
int ipo_font_draw_glyph_rgb32(uint32_t *dst, int dst_stride,
                              int clip_x1, int clip_y1, int clip_x2, int clip_y2,
                              int x, int y, int style_idx, int height,
                              uint32_t cp, uint32_t fg_rgb);
int ipo_font_raster_cell_bgr0(uint32_t *out_bgr0, int out_w, int out_h,
                              int style_idx, int height, int italic,
                              uint32_t cp, uint32_t fg_rgb, uint32_t bg_rgb);

/* Crisp tofu replacement box when no glyph table has a codepoint */
int ipo_font_measure_fallback(int height);
int ipo_font_raster_fallback_bgr0(uint32_t *out_bgr0, int out_w, int out_h,
                                  int height, uint32_t fg_rgb, uint32_t bg_rgb);

#ifdef __cplusplus
}
#endif

#endif
""")

    # 5. Write compressed world glyph data file ipo_world_fonts.c
    with open(world_c_path, "w") as wf:
        wf.write('#include <stdint.h>\n\n')
        wf.write("typedef struct {\n"
                 "    uint32_t zoff;\n"
                 "    uint16_t zlen;\n"
                 "    uint16_t raw_len;\n"
                 "} ipo_zchunk_t;\n\n")
        wf.write(f"const uint32_t ipo_t2_count = {len(t2_cps)}u;\n")
        wf.write(f"const uint32_t ipo_t2_chunk_count = {len(t2_chunks)}u;\n")
        wf.write(f"const uint32_t ipo_t2_cps[{len(t2_cps)}] = {{\n")
        for i in range(0, len(t2_cps), 12):
            wf.write("    " + ",".join(f"0x{cp:04X}u" for cp in t2_cps[i : i + 12]) + ",\n")
        wf.write("};\n\n")
        wf.write(f"const ipo_zchunk_t ipo_t2_chunks[{len(t2_chunks)}] = {{\n")
        for zoff, zlen, rlen in t2_chunks:
            wf.write(f"    {{{zoff}u,{zlen}u,{rlen}u}},\n")
        wf.write("};\n\n")
        wf.write(f"const uint8_t ipo_t2_zblob[{len(t2_zblob)}] = {{\n")
        for i in range(0, len(t2_zblob), 24):
            wf.write("    " + ",".join(str(b) for b in t2_zblob[i : i + 24]) + ",\n")
        wf.write("};\n\n")

        wf.write(f"const uint32_t ipo_t3_count = {len(t3_cps)}u;\n")
        wf.write(f"const uint32_t ipo_t3_chunk_count = {len(t3_chunks)}u;\n")
        wf.write(f"const uint16_t ipo_t3_cps[{len(t3_cps)}] = {{\n")
        for i in range(0, len(t3_cps), 12):
            wf.write("    " + ",".join(f"0x{cp:04X}u" for cp in t3_cps[i : i + 12]) + ",\n")
        wf.write("};\n\n")
        wf.write(f"const ipo_zchunk_t ipo_t3_chunks[{len(t3_chunks)}] = {{\n")
        for zoff, zlen, rlen in t3_chunks:
            wf.write(f"    {{{zoff}u,{zlen}u,{rlen}u}},\n")
        wf.write("};\n\n")
        wf.write(f"const uint8_t ipo_t3_zblob[{len(t3_zblob)}] = {{\n")
        for i in range(0, len(t3_zblob), 24):
            wf.write("    " + ",".join(str(b) for b in t3_zblob[i : i + 24]) + ",\n")
        wf.write("};\n")

    # 6. Write ipo_crisp_fonts.c (Tier 1 hot atlas + Tier 2/3 LRU chunk decoder + area resampler + shaping)
    with open(c_path, "w") as cf:
        cf.write('#include "ipo_crisp_fonts.h"\n#include "zlib.h"\n#include <stddef.h>\n#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n\n')
        cf.write("typedef struct {\n"
                 "    uint32_t data_off;\n"
                 "    uint8_t  adv;\n"
                 "    int8_t   xoff;\n"
                 "    uint8_t  yoff;\n"
                 "    uint8_t  bw;\n"
                 "    uint8_t  bh;\n"
                 "} ipo_glyph_t;\n\n")
        cf.write("typedef struct {\n"
                 "    uint8_t style_idx;\n"
                 "    uint8_t height;\n"
                 "    uint8_t baseline;\n"
                 "    const ipo_glyph_t *glyphs;\n"
                 "} ipo_font_face_t;\n\n")
        cf.write("typedef struct {\n"
                 "    uint32_t zoff;\n"
                 "    uint16_t zlen;\n"
                 "    uint16_t raw_len;\n"
                 "} ipo_zchunk_t;\n\n")
        cf.write("extern const uint32_t ipo_t2_count;\n"
                 "extern const uint32_t ipo_t2_chunk_count;\n"
                 "extern const uint32_t ipo_t2_cps[];\n"
                 "extern const ipo_zchunk_t ipo_t2_chunks[];\n"
                 "extern const uint8_t ipo_t2_zblob[];\n"
                 "extern const uint32_t ipo_t3_count;\n"
                 "extern const uint32_t ipo_t3_chunk_count;\n"
                 "extern const uint16_t ipo_t3_cps[];\n"
                 "extern const ipo_zchunk_t ipo_t3_chunks[];\n"
                 "extern const uint8_t ipo_t3_zblob[];\n\n")

        cf.write(f"#define IPO_NUM_CODEPOINTS {len(TIER1_CPS)}\n")
        cf.write("static const uint32_t s_codepoints[IPO_NUM_CODEPOINTS] = {\n")
        for i in range(0, len(TIER1_CPS), 12):
            cf.write("    " + ", ".join(f"0x{cp:04X}u" for cp in TIER1_CPS[i : i + 12]) + ",\n")
        cf.write("};\n\n")

        cf.write(f"static const uint8_t s_alpha_blob[{len(blob)}] = {{\n")
        for i in range(0, len(blob), 20):
            cf.write("    " + ",".join(f"{b}" for b in blob[i : i + 20]) + ",\n")
        cf.write("};\n\n")

        for idx, (st, h, base, is_mono, g_list) in enumerate(faces_meta):
            cf.write(f"static const ipo_glyph_t s_glyphs_{idx}[IPO_NUM_CODEPOINTS] = {{\n")
            for off, adv, xo, yo, bw, bh in g_list:
                cf.write(f"    {{{off}u,{adv},{xo},{yo},{bw},{bh}}},\n")
            cf.write("};\n\n")

        cf.write(f"#define IPO_NUM_FACES {len(faces_meta)}\n")
        cf.write("static const ipo_font_face_t s_faces[IPO_NUM_FACES] = {\n")
        for idx, (st, h, base, is_mono, _) in enumerate(faces_meta):
            cf.write(f"    {{{st}, {h}, {base}, s_glyphs_{idx}}},\n")
        cf.write("};\n\n")

        cf.write(C_RUNTIME_CODE)

    t2_kb = len(t2_zblob) / 1024.0
    t3_kb = len(t3_zblob) / 1024.0
    t1_kb = len(blob) / 1024.0
    print(f"Generated crisp world fonts: T1={len(TIER1_CPS)} cps ({t1_kb:.1f} KiB), "
          f"T2={len(t2_cps)} cps ({t2_kb:.1f} KiB z), "
          f"T3={len(t3_cps)} cps ({t3_kb:.1f} KiB z), total={len(TIER1_CPS)+len(t2_cps)+len(t3_cps)} cps!")


C_RUNTIME_CODE = r"""
#define T2_CHUNK_SZ 64u
#define T3_CHUNK_SZ 128u
#define T2_CACHE_SLOTS 16
#define T3_CACHE_SLOTS 24

typedef struct {
    int32_t  cid;
    uint32_t stamp;
    uint8_t *data;
} zcache_slot_t;

static zcache_slot_t s_t2_cache[T2_CACHE_SLOTS];
static zcache_slot_t s_t3_cache[T3_CACHE_SLOTS];
static uint32_t s_cache_tick = 1u;

static void init_zcache_once(void) {
    static int inited = 0;
    if (inited) return;
    inited = 1;
    for (int i = 0; i < T2_CACHE_SLOTS; i++) s_t2_cache[i].cid = -1;
    for (int i = 0; i < T3_CACHE_SLOTS; i++) s_t3_cache[i].cid = -1;
}

static const uint8_t *get_chunk_data(zcache_slot_t *slots, int nslots,
                                     const ipo_zchunk_t *meta, const uint8_t *zblob,
                                     uint32_t cid) {
    init_zcache_once();
    s_cache_tick++;
    int empty_i = -1;
    int lru_i = 0;
    uint32_t min_stamp = 0xFFFFFFFFu;
    for (int i = 0; i < nslots; i++) {
        if (slots[i].cid == (int32_t)cid) {
            slots[i].stamp = s_cache_tick;
            return slots[i].data;
        }
        if (slots[i].cid < 0 && empty_i < 0) empty_i = i;
        if (slots[i].stamp < min_stamp) {
            min_stamp = slots[i].stamp;
            lru_i = i;
        }
    }
    int slot_i = (empty_i >= 0) ? empty_i : lru_i;
    if (slots[slot_i].data) {
        free(slots[slot_i].data);
        slots[slot_i].data = NULL;
    }
    slots[slot_i].cid = -1;

    uint32_t raw_len = meta[cid].raw_len;
    uint8_t *buf = (uint8_t *)malloc(raw_len ? raw_len : 1u);
    if (!buf) return NULL;
    uLongf dlen = (uLongf)raw_len;
    if (uncompress(buf, &dlen, zblob + meta[cid].zoff, (uLong)meta[cid].zlen) != Z_OK) {
        free(buf);
        return NULL;
    }
    slots[slot_i].cid = (int32_t)cid;
    slots[slot_i].stamp = s_cache_tick;
    slots[slot_i].data = buf;
    return buf;
}

/* 4-bit coverage (0..15) -> 8-bit blend alpha (0..255). */
static const uint8_t s_cov_dark_on_light[16] = {
    0, 21, 39, 57, 75, 92, 109, 126, 143, 160, 176, 192, 208, 224, 240, 255
};
static const uint8_t s_cov_light_on_dark[16] = {
    0, 26, 47, 67, 85, 103, 120, 136, 152, 168, 183, 198, 213, 227, 241, 255
};

static int luma255(uint32_t rgb) {
    int r = (int)((rgb >> 16) & 0xFFu);
    int g = (int)((rgb >> 8) & 0xFFu);
    int b = (int)(rgb & 0xFFu);
    return (r * 77 + g * 150 + b * 29) >> 8;
}

static uint32_t blend_rgb_alpha(uint32_t bg, uint32_t fg, uint8_t a4, int dark_on_light) {
    if (a4 == 0) return bg & 0x00FFFFFFu;
    if (a4 >= 15) return fg & 0x00FFFFFFu;
    uint32_t a = dark_on_light ? s_cov_dark_on_light[a4] : s_cov_light_on_dark[a4];
    int br = (int)((bg >> 16) & 0xFFu), bgr = (int)((bg >> 8) & 0xFFu), bb = (int)(bg & 0xFFu);
    int fr = (int)((fg >> 16) & 0xFFu), fgr = (int)((fg >> 8) & 0xFFu), fb = (int)(fg & 0xFFu);
    int rr = br + (((fr - br) * (int)a + 127) / 255);
    int rg = bgr + (((fgr - bgr) * (int)a + 127) / 255);
    int rb = bb + (((fb - bb) * (int)a + 127) / 255);
    if (rr < 0) rr = 0; else if (rr > 255) rr = 255;
    if (rg < 0) rg = 0; else if (rg > 255) rg = 255;
    if (rb < 0) rb = 0; else if (rb > 255) rb = 255;
    return ((uint32_t)rr << 16) | ((uint32_t)rg << 8) | (uint32_t)rb;
}

static int find_t1_index(uint32_t cp) {
    if (cp >= 0x0020u && cp <= 0x007Eu) return (int)(cp - 0x0020u);
    int lo = 0x007Fu - 0x0020u;
    int hi = IPO_NUM_CODEPOINTS - 1;
    while (lo <= hi) {
        int mid = lo + ((hi - lo) >> 1);
        uint32_t v = s_codepoints[mid];
        if (v == cp) return mid;
        if (v < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static int find_t2_index(uint32_t cp) {
    int lo = 0, hi = (int)ipo_t2_count - 1;
    while (lo <= hi) {
        int mid = lo + ((hi - lo) >> 1);
        uint32_t v = ipo_t2_cps[mid];
        if (v == cp) return mid;
        if (v < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static int find_t3_index(uint32_t cp) {
    if (cp > 0xFFFFu) return -1;
    int lo = 0, hi = (int)ipo_t3_count - 1;
    while (lo <= hi) {
        int mid = lo + ((hi - lo) >> 1);
        uint32_t v = ipo_t3_cps[mid];
        if (v == cp) return mid;
        if (v < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

typedef struct {
    const uint8_t *alpha4; /* non-NULL for 4bpp glyphs (Tier 1 & 2) */
    const uint8_t *bits1;  /* non-NULL for 1bpp glyphs (Tier 3) */
    int src_h;
    int src_base;
    int adv;
    int xoff;
    int yoff;
    int bw;
    int bh;
    uint8_t flags;
} resolved_glyph_t;

static const ipo_font_face_t *select_t1_face(int style_idx, int height) {
    if (style_idx < 0 || style_idx > 2) style_idx = 0;
    if (height < 6) height = 6;
    const ipo_font_face_t *best = NULL;
    int best_diff = 0x7fffffff;
    for (int i = 0; i < IPO_NUM_FACES; i++) {
        if (s_faces[i].style_idx != style_idx) continue;
        if (s_faces[i].height == height) return &s_faces[i];
        int d = (int)s_faces[i].height - height;
        int ad = d < 0 ? -d : d;
        if (ad < best_diff || (ad == best_diff && d < 0)) {
            best_diff = ad;
            best = &s_faces[i];
        }
    }
    if (!best && style_idx != 0) return select_t1_face(0, height);
    return best;
}

static int resolve_glyph(int style_idx, int height, uint32_t cp, resolved_glyph_t *rg) {
    if (height < 6) height = 6;
    int t1 = find_t1_index(cp);
    if (t1 >= 0) {
        const ipo_font_face_t *f = select_t1_face(style_idx, height);
        if (!f) return 0;
        const ipo_glyph_t *g = &f->glyphs[t1];
        rg->alpha4 = s_alpha_blob + g->data_off;
        rg->bits1 = NULL;
        rg->src_h = f->height;
        rg->src_base = f->baseline;
        rg->adv = (int)g->adv;
        rg->xoff = (int)g->xoff;
        rg->yoff = (int)g->yoff;
        rg->bw = (int)g->bw;
        rg->bh = (int)g->bh;
        rg->flags = 0;
        return 1;
    }

    int t2 = find_t2_index(cp);
    if (t2 >= 0) {
        uint32_t cid = (uint32_t)t2 / T2_CHUNK_SZ;
        uint32_t in_c = (uint32_t)t2 % T2_CHUNK_SZ;
        const uint8_t *cd = get_chunk_data(s_t2_cache, T2_CACHE_SLOTS, ipo_t2_chunks, ipo_t2_zblob, cid);
        if (!cd) return 0;
        int face_slot = 0;
        int src_h = 14, src_base = 11;
        if (style_idx == IPO_FONT_BOLD && height >= 13) {
            face_slot = 2; src_h = 16; src_base = 12;
        } else if (height >= 16) {
            face_slot = 1; src_h = 18; src_base = 14;
        } else {
            face_slot = 0; src_h = 14; src_base = 11;
        }
        uint32_t slot = in_c * 3u + (uint32_t)face_slot;
        uint16_t off = (uint16_t)cd[slot * 2u] | ((uint16_t)cd[slot * 2u + 1u] << 8);
        const uint8_t *e = cd + off;
        rg->adv = (int)e[0];
        rg->xoff = (int)(int8_t)e[1];
        rg->yoff = (int)(int8_t)e[2];
        rg->bw = (int)e[3];
        rg->bh = (int)e[4];
        rg->flags = e[5];
        rg->alpha4 = e + 6;
        rg->bits1 = NULL;
        rg->src_h = src_h;
        rg->src_base = src_base;
        return 1;
    }

    int t3 = find_t3_index(cp);
    if (t3 >= 0) {
        uint32_t cid = (uint32_t)t3 / T3_CHUNK_SZ;
        uint32_t in_c = (uint32_t)t3 % T3_CHUNK_SZ;
        const uint8_t *cd = get_chunk_data(s_t3_cache, T3_CACHE_SLOTS, ipo_t3_chunks, ipo_t3_zblob, cid);
        if (!cd) return 0;
        uint16_t off = (uint16_t)cd[in_c * 2u] | ((uint16_t)cd[in_c * 2u + 1u] << 8);
        const uint8_t *e = cd + off;
        rg->src_h = (int)e[0];
        rg->src_base = (int)e[1];
        rg->adv = (int)e[2];
        rg->xoff = (int)(int8_t)e[3];
        rg->yoff = (int)(int8_t)e[4];
        rg->bw = (int)e[5];
        rg->bh = (int)e[6];
        rg->flags = e[7];
        rg->alpha4 = NULL;
        rg->bits1 = e + 8;
        return 1;
    }
    return 0;
}

/* Sample 0..15 coverage at source integer coordinate (sx, sy) */
static inline uint8_t raw_cov15(const resolved_glyph_t *rg, int sx, int sy) {
    if (sx < 0 || sy < 0 || sx >= rg->bw || sy >= rg->bh) return 0;
    if (rg->alpha4) {
        int stride4 = (rg->bw + 1) >> 1;
        uint8_t b = rg->alpha4[sy * stride4 + (sx >> 1)];
        return (sx & 1) ? (b & 0x0Fu) : (b >> 4);
    } else {
        int stride1 = (rg->bw + 7) >> 3;
        uint8_t b = rg->bits1[sy * stride1 + (sx >> 3)];
        return (b & (0x80u >> (sx & 7))) ? 15u : 0u;
    }
}

/* High-quality fixed-point box/bilinear resampled alpha (0..15) */
static uint8_t resampled_cov15(const resolved_glyph_t *rg, int dw, int dh, int dx, int dy, int synth_bold) {
    int sw = rg->bw, sh = rg->bh;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return 0;
    uint32_t c;
    if (dw == sw && dh == sh) {
        c = raw_cov15(rg, dx, dy);
    } else if (dw >= sw && dh >= sh) {
        /* Centered bilinear interpolation for crisp smooth upscaling */
        int fx = ((dx * 2 + 1) * sw * 128) / dw - 128;
        int fy = ((dy * 2 + 1) * sh * 128) / dh - 128;
        if (fx < 0) fx = 0;
        if (fy < 0) fy = 0;
        int x0 = fx >> 8, y0 = fy >> 8;
        int wx = fx & 255, wy = fy & 255;
        int x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
        int y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
        uint32_t c00 = raw_cov15(rg, x0, y0);
        uint32_t c10 = raw_cov15(rg, x1, y0);
        uint32_t c01 = raw_cov15(rg, x0, y1);
        uint32_t c11 = raw_cov15(rg, x1, y1);
        uint32_t top = c00 * (uint32_t)(256 - wx) + c10 * (uint32_t)wx;
        uint32_t bot = c01 * (uint32_t)(256 - wx) + c11 * (uint32_t)wx;
        c = (top * (uint32_t)(256 - wy) + bot * (uint32_t)wy + 32768u) >> 16;
    } else {
        /* Exact box-filter area coverage for crisp downscaling (prevents dropped CJK strokes!) */
        int sx0 = (dx * sw * 256) / dw;
        int sx1 = ((dx + 1) * sw * 256 + dw - 1) / dw;
        int sy0 = (dy * sh * 256) / dh;
        int sy1 = ((dy + 1) * sh * 256 + dh - 1) / dh;
        if (sx1 <= sx0) sx1 = sx0 + 1;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        int ix0 = sx0 >> 8, ix1 = (sx1 - 1) >> 8;
        int iy0 = sy0 >> 8, iy1 = (sy1 - 1) >> 8;
        uint64_t acc = 0;
        uint64_t area = (uint64_t)(sx1 - sx0) * (uint64_t)(sy1 - sy0);
        for (int iy = iy0; iy <= iy1; iy++) {
            int oy0 = (iy << 8) < sy0 ? sy0 : (iy << 8);
            int oy1 = ((iy + 1) << 8) > sy1 ? sy1 : ((iy + 1) << 8);
            int wy = oy1 - oy0;
            for (int ix = ix0; ix <= ix1; ix++) {
                int ox0 = (ix << 8) < sx0 ? sx0 : (ix << 8);
                int ox1 = ((ix + 1) << 8) > sx1 ? sx1 : ((ix + 1) << 8);
                uint32_t px = raw_cov15(rg, ix, iy);
                if (px) acc += (uint64_t)px * (uint64_t)(ox1 - ox0) * (uint64_t)wy;
            }
        }
        c = (uint32_t)((acc + (area >> 1)) / area);
        /* Gentle stroke reinforcement on 1bpp downscales so thin Kanji bars stay crisp */
        if (rg->bits1 && c > 0) {
            c = (c * 19u + 8u) >> 4;
            if (c > 15u) c = 15u;
        }
    }
    if (synth_bold && rg->bits1 && dx > 0) {
        /* 1px right stem thickening without rescaling whole bitmap */
        int sx_prev = ((dx - 1) * sw) / dw;
        int sy_cur = (dy * sh) / dh;
        uint32_t pc = raw_cov15(rg, sx_prev, sy_cur);
        uint32_t b = (pc * 11u) >> 4;
        if (b > c) c = b;
    }
    return (uint8_t)(c > 15u ? 15u : c);
}

int ipo_font_is_zero_width(uint32_t cp) {
    return (cp >= 0x200Bu && cp <= 0x200Fu) ||
           (cp >= 0x202Au && cp <= 0x202Eu) ||
           (cp >= 0x2060u && cp <= 0x206Fu) ||
           cp == 0xFEFFu || cp == 0x034Fu || cp == 0x061Cu || cp == 0x00ADu;
}

int ipo_font_is_combining(uint32_t cp) {
    if ((cp >= 0x0300u && cp <= 0x036Fu) ||
        (cp >= 0x0591u && cp <= 0x05C7u) ||
        (cp >= 0x064Bu && cp <= 0x065Fu) || cp == 0x0670u ||
        cp == 0x0E31u || (cp >= 0x0E34u && cp <= 0x0E3Au) || (cp >= 0x0E47u && cp <= 0x0E4Eu) ||
        cp == 0x0900u || cp == 0x0901u || cp == 0x0902u || cp == 0x093Cu ||
        (cp >= 0x0941u && cp <= 0x0948u) || cp == 0x094Du || (cp >= 0x0951u && cp <= 0x0954u)) {
        return 1;
    }
    resolved_glyph_t rg;
    if (resolve_glyph(0, 16, cp, &rg)) return (rg.flags & 1u) ? 1 : 0;
    return 0;
}

int ipo_font_is_wide(uint32_t cp) {
    if ((cp >= 0x1100u && cp <= 0x11FFu) ||
        (cp >= 0x2E80u && cp <= 0x9FFFu) ||
        (cp >= 0xAC00u && cp <= 0xD7AFu) ||
        (cp >= 0xF900u && cp <= 0xFAFFu) ||
        (cp >= 0xFE30u && cp <= 0xFE6Fu) ||
        (cp >= 0xFF01u && cp <= 0xFF60u)) {
        return 1;
    }
    return 0;
}

int ipo_font_has_glyph(uint32_t cp) {
    return find_t1_index(cp) >= 0 || find_t2_index(cp) >= 0 || find_t3_index(cp) >= 0;
}

/* Canonical NFD->NFC pair table for common accented Latin & Cyrillic */
uint32_t ipo_font_compose_pair(uint32_t b, uint32_t m) {
    switch (m) {
    case 0x0300u: /* grave */
        switch (b) {
        case 'A': return 0x00C0u; case 'E': return 0x00C8u; case 'I': return 0x00CCu;
        case 'O': return 0x00D2u; case 'U': return 0x00D9u;
        case 'a': return 0x00E0u; case 'e': return 0x00E8u; case 'i': return 0x00ECu;
        case 'o': return 0x00F2u; case 'u': return 0x00F9u;
        default: break;
        } break;
    case 0x0301u: /* acute */
        switch (b) {
        case 'A': return 0x00C1u; case 'E': return 0x00C9u; case 'I': return 0x00CDu;
        case 'O': return 0x00D3u; case 'U': return 0x00DAu; case 'Y': return 0x00DDu;
        case 'C': return 0x0106u; case 'N': return 0x0143u; case 'S': return 0x015Au; case 'Z': return 0x0179u;
        case 'a': return 0x00E1u; case 'e': return 0x00E9u; case 'i': return 0x00EDu;
        case 'o': return 0x00F3u; case 'u': return 0x00FAu; case 'y': return 0x00FDu;
        case 'c': return 0x0107u; case 'n': return 0x0144u; case 's': return 0x015Bu; case 'z': return 0x017Au;
        default: break;
        } break;
    case 0x0302u: /* circumflex */
        switch (b) {
        case 'A': return 0x00C2u; case 'E': return 0x00CAu; case 'I': return 0x00CEu;
        case 'O': return 0x00D4u; case 'U': return 0x00DBu;
        case 'a': return 0x00E2u; case 'e': return 0x00EAu; case 'i': return 0x00EEu;
        case 'o': return 0x00F4u; case 'u': return 0x00FBu;
        default: break;
        } break;
    case 0x0303u: /* tilde */
        switch (b) {
        case 'A': return 0x00C3u; case 'N': return 0x00D1u; case 'O': return 0x00D5u;
        case 'a': return 0x00E3u; case 'n': return 0x00F1u; case 'o': return 0x00F5u;
        default: break;
        } break;
    case 0x0306u: /* breve */
        if (b == 0x0418u) return 0x0419u;
        if (b == 0x0438u) return 0x0439u;
        if (b == 0x0423u) return 0x040Eu;
        if (b == 0x0443u) return 0x045Eu;
        if (b == 'A') return 0x0102u; if (b == 'a') return 0x0103u;
        if (b == 'G') return 0x011Eu; if (b == 'g') return 0x011Fu;
        break;
    case 0x0307u: /* dot above */
        if (b == 'I') return 0x0130u;
        if (b == 'Z') return 0x017Bu; if (b == 'z') return 0x017Cu;
        break;
    case 0x0308u: /* diaeresis */
        switch (b) {
        case 'A': return 0x00C4u; case 'E': return 0x00CBu; case 'I': return 0x00CFu;
        case 'O': return 0x00D6u; case 'U': return 0x00DCu; case 'Y': return 0x0178u;
        case 'a': return 0x00E4u; case 'e': return 0x00EBu; case 'i': return 0x00EFu;
        case 'o': return 0x00F6u; case 'u': return 0x00FCu; case 'y': return 0x00FFu;
        case 0x0415u: return 0x0401u; case 0x0435u: return 0x0451u;
        case 0x0406u: return 0x0407u; case 0x0456u: return 0x0457u;
        default: break;
        } break;
    case 0x030Au: /* ring */
        if (b == 'A') return 0x00C5u; if (b == 'a') return 0x00E5u;
        if (b == 'U') return 0x016Eu; if (b == 'u') return 0x016Fu;
        break;
    case 0x030Cu: /* caron */
        switch (b) {
        case 'C': return 0x010Cu; case 'c': return 0x010Du;
        case 'D': return 0x010Eu; case 'd': return 0x010Fu;
        case 'E': return 0x011Au; case 'e': return 0x011Bu;
        case 'N': return 0x0147u; case 'n': return 0x0148u;
        case 'R': return 0x0158u; case 'r': return 0x0159u;
        case 'S': return 0x0160u; case 's': return 0x0161u;
        case 'T': return 0x0164u; case 't': return 0x0165u;
        case 'Z': return 0x017Du; case 'z': return 0x017Eu;
        default: break;
        } break;
    case 0x0327u: /* cedilla */
        if (b == 'C') return 0x00C7u; if (b == 'c') return 0x00E7u;
        if (b == 'S') return 0x015Eu; if (b == 's') return 0x015Fu;
        break;
    case 0x0328u: /* ogonek */
        if (b == 'A') return 0x0104u; if (b == 'a') return 0x0105u;
        if (b == 'E') return 0x0118u; if (b == 'e') return 0x0119u;
        break;
    default: break;
    }
    return 0u;
}

/* Arabic contextual forms: [isolated, final, initial, medial].
 * Dual-joining if initial != 0, right-joining otherwise. */
typedef struct {
    uint16_t cp;
    uint16_t forms[4];
} arabic_form_t;

static const arabic_form_t s_arabic_forms[] = {
    {0x0621u, {0xFE80u, 0,       0,       0}},
    {0x0622u, {0xFE81u, 0xFE82u, 0,       0}},
    {0x0623u, {0xFE83u, 0xFE84u, 0,       0}},
    {0x0624u, {0xFE85u, 0xFE86u, 0,       0}},
    {0x0625u, {0xFE87u, 0xFE88u, 0,       0}},
    {0x0626u, {0xFE89u, 0xFE8Au, 0xFE8Bu, 0xFE8Cu}},
    {0x0627u, {0xFE8Du, 0xFE8Eu, 0,       0}},
    {0x0628u, {0xFE8Fu, 0xFE90u, 0xFE91u, 0xFE92u}},
    {0x0629u, {0xFE93u, 0xFE94u, 0,       0}},
    {0x062Au, {0xFE95u, 0xFE96u, 0xFE97u, 0xFE98u}},
    {0x062Bu, {0xFE99u, 0xFE9Au, 0xFE9Bu, 0xFE9Cu}},
    {0x062Cu, {0xFE9Du, 0xFE9Eu, 0xFE9Fu, 0xFEA0u}},
    {0x062Du, {0xFEA1u, 0xFEA2u, 0xFEA3u, 0xFEA4u}},
    {0x062Eu, {0xFEA5u, 0xFEA6u, 0xFEA7u, 0xFEA8u}},
    {0x062Fu, {0xFEA9u, 0xFEAAu, 0,       0}},
    {0x0630u, {0xFEABu, 0xFEACu, 0,       0}},
    {0x0631u, {0xFEADu, 0xFEAEu, 0,       0}},
    {0x0632u, {0xFEAFu, 0xFEB0u, 0,       0}},
    {0x0633u, {0xFEB1u, 0xFEB2u, 0xFEB3u, 0xFEB4u}},
    {0x0634u, {0xFEB5u, 0xFEB6u, 0xFEB7u, 0xFEB8u}},
    {0x0635u, {0xFEB9u, 0xFEBAu, 0xFEBBu, 0xFEBCu}},
    {0x0636u, {0xFEBDu, 0xFEBEu, 0xFEBFu, 0xFEC0u}},
    {0x0637u, {0xFEC1u, 0xFEC2u, 0xFEC3u, 0xFEC4u}},
    {0x0638u, {0xFEC5u, 0xFEC6u, 0xFEC7u, 0xFEC8u}},
    {0x0639u, {0xFEC9u, 0xFECAu, 0xFECBu, 0xFECCu}},
    {0x063Au, {0xFECDu, 0xFECEu, 0xFECFu, 0xFED0u}},
    {0x0641u, {0xFED1u, 0xFED2u, 0xFED3u, 0xFED4u}},
    {0x0642u, {0xFED5u, 0xFED6u, 0xFED7u, 0xFED8u}},
    {0x0643u, {0xFED9u, 0xFEDAu, 0xFEDBu, 0xFEDCu}},
    {0x0644u, {0xFEDDu, 0xFEDEu, 0xFEDFu, 0xFEE0u}},
    {0x0645u, {0xFEE1u, 0xFEE2u, 0xFEE3u, 0xFEE4u}},
    {0x0646u, {0xFEE5u, 0xFEE6u, 0xFEE7u, 0xFEE8u}},
    {0x0647u, {0xFEE9u, 0xFEEAu, 0xFEEBu, 0xFEECu}},
    {0x0648u, {0xFEEDu, 0xFEEEu, 0,       0}},
    {0x0649u, {0xFEEFu, 0xFEF0u, 0,       0}},
    {0x064Au, {0xFEF1u, 0xFEF2u, 0xFEF3u, 0xFEF4u}},
    {0x067Eu, {0xFB56u, 0xFB57u, 0xFB58u, 0xFB59u}},
    {0x0686u, {0xFB7Au, 0xFB7Bu, 0xFB7Cu, 0xFB7Du}},
    {0x0698u, {0xFB8Au, 0xFB8Bu, 0,       0}},
    {0x06A9u, {0xFB8Eu, 0xFB8Fu, 0xFB90u, 0xFB91u}},
    {0x06AFu, {0xFB92u, 0xFB93u, 0xFB94u, 0xFB95u}},
    {0x06CCu, {0xFBFCu, 0xFBFDu, 0xFBFEu, 0xFBFFu}},
};

static const arabic_form_t *lookup_arabic(uint32_t cp) {
    if (cp < 0x0621u || cp > 0x06CCu) return NULL;
    int lo = 0, hi = (int)(sizeof(s_arabic_forms) / sizeof(s_arabic_forms[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t v = s_arabic_forms[mid].cp;
        if (v == cp) return &s_arabic_forms[mid];
        if (v < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

static int is_rtl_letter(uint32_t cp) {
    return (cp >= 0x0590u && cp <= 0x064Au) ||
           (cp >= 0x066Eu && cp <= 0x06FFu) ||
           (cp >= 0xFB50u && cp <= 0xFEFCu);
}

static int is_rtl_neutral(uint32_t cp) {
    return cp == ' ' || cp == 0xA0u || cp == '-' || cp == ':' || cp == '.' || cp == ',' ||
           cp == '!' || cp == '?' || cp == '/' || cp == '(' || cp == ')' ||
           cp == 0x060Cu || cp == 0x061Bu || cp == 0x061Fu;
}

static uint32_t decode_utf8(const char **p) {
    const unsigned char *s = (const unsigned char *)(*p);
    if (!*s) return 0;
    uint32_t cp;
    if (s[0] < 0x80u) {
        cp = s[0];
        *p += 1;
    } else if ((s[0] & 0xE0u) == 0xC0u && (s[1] & 0xC0u) == 0x80u) {
        cp = ((uint32_t)(s[0] & 0x1Fu) << 6) | (uint32_t)(s[1] & 0x3Fu);
        *p += 2;
    } else if ((s[0] & 0xF0u) == 0xE0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u) {
        cp = ((uint32_t)(s[0] & 0x0Fu) << 12) | ((uint32_t)(s[1] & 0x3Fu) << 6) | (uint32_t)(s[2] & 0x3Fu);
        *p += 3;
    } else if ((s[0] & 0xF8u) == 0xF0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u && (s[3] & 0xC0u) == 0x80u) {
        cp = ((uint32_t)(s[0] & 0x07u) << 18) | ((uint32_t)(s[1] & 0x3Fu) << 12) | ((uint32_t)(s[2] & 0x3Fu) << 6) | (uint32_t)(s[3] & 0x3Fu);
        *p += 4;
    } else {
        cp = '?';
        *p += 1;
    }
    return cp;
}

int ipo_font_shape_utf8(const char *utf8, ipo_shaped_glyph_t *out, int out_cap) {
    if (!utf8 || !*utf8) return 0;

    /* Count raw codepoints */
    int raw_cap = 0;
    for (const char *t = utf8; *t; ) {
        decode_utf8(&t);
        raw_cap++;
    }
    if (raw_cap == 0) return 0;

    ipo_shaped_glyph_t stack_buf[128];
    ipo_shaped_glyph_t *arr = (raw_cap <= 128) ? stack_buf : (ipo_shaped_glyph_t *)malloc((size_t)raw_cap * sizeof(ipo_shaped_glyph_t));
    if (!arr) return 0;

    int n = 0;
    int has_rtl = 0;
    int has_arabic = 0;
    const char *p = utf8;
    while (*p) {
        uint32_t cp = decode_utf8(&p);
        if (!cp) break;
        if (ipo_font_is_zero_width(cp)) continue;
        if (n > 0 && ipo_font_is_combining(cp)) {
            uint32_t comp = ipo_font_compose_pair(arr[n - 1].cp, cp);
            if (comp) {
                arr[n - 1].cp = comp;
                continue;
            }
        }
        uint8_t fl = 0;
        if (ipo_font_is_combining(cp)) fl |= IPO_SHAPED_OVERSTRIKE;
        if (is_rtl_letter(cp)) {
            fl |= IPO_SHAPED_RTL;
            has_rtl = 1;
            if (cp >= 0x0600u && cp <= 0x06FFu) has_arabic = 1;
        }
        arr[n].cp = cp;
        arr[n].flags = fl;
        n++;
    }

    if (has_arabic) {
        int out_n = 0;
        for (int i = 0; i < n; i++) {
            uint32_t cur = arr[i].cp;
            const arabic_form_t *af = lookup_arabic(cur);
            if (!af || (arr[i].flags & IPO_SHAPED_OVERSTRIKE)) {
                arr[out_n++] = arr[i];
                continue;
            }
            /* Skip combining harakat to find joinable previous letter */
            int prev_join = 0;
            for (int k = i - 1; k >= 0; k--) {
                if (arr[k].flags & IPO_SHAPED_OVERSTRIKE) continue;
                if (arr[k].cp == 0x0640u) { prev_join = 1; break; } /* tatweel */
                const arabic_form_t *pf = lookup_arabic(arr[k].cp);
                if (pf && pf->forms[2] != 0) prev_join = 1;
                break;
            }
            /* Lam-Alef ligature fold if next non-mark letter is 0622/0623/0625/0627 */
            if (cur == 0x0644u) {
                int j = i + 1;
                while (j < n && (arr[j].flags & IPO_SHAPED_OVERSTRIKE)) j++;
                if (j < n) {
                    uint32_t nc = arr[j].cp;
                    uint32_t lig = 0;
                    if (nc == 0x0622u) lig = prev_join ? 0xFEF6u : 0xFEF5u;
                    else if (nc == 0x0623u) lig = prev_join ? 0xFEF8u : 0xFEF7u;
                    else if (nc == 0x0625u) lig = prev_join ? 0xFEFAu : 0xFEF9u;
                    else if (nc == 0x0627u) lig = prev_join ? 0xFEFCu : 0xFEFBu;
                    if (lig) {
                        arr[out_n].cp = lig;
                        arr[out_n].flags = IPO_SHAPED_RTL;
                        out_n++;
                        /* Keep any harakat that were between Lam and Alef */
                        for (int m = i + 1; m < j; m++) arr[out_n++] = arr[m];
                        i = j;
                        continue;
                    }
                }
            }
            /* Find next non-mark letter for trailing join check */
            int next_join = 0;
            if (af->forms[2] != 0) {
                for (int j = i + 1; j < n; j++) {
                    if (arr[j].flags & IPO_SHAPED_OVERSTRIKE) continue;
                    if (arr[j].cp == 0x0640u) { next_join = 1; break; }
                    const arabic_form_t *nf = lookup_arabic(arr[j].cp);
                    if (nf && nf->forms[1] != 0) next_join = 1;
                    break;
                }
            }
            int fidx = 0;
            if (prev_join && next_join && af->forms[3]) fidx = 3;
            else if (next_join && af->forms[2]) fidx = 2;
            else if (prev_join && af->forms[1]) fidx = 1;
            else fidx = 0;
            arr[out_n].cp = af->forms[fidx];
            arr[out_n].flags = IPO_SHAPED_RTL;
            out_n++;
        }
        n = out_n;
    }

    if (has_rtl) {
        /* Reorder RTL runs visually while keeping base+overstrike clusters and embedded numbers LTR */
        int i = 0;
        while (i < n) {
            if (!(arr[i].flags & IPO_SHAPED_RTL)) { i++; continue; }
            int rstart = i;
            int rend = i;
            for (int k = i; k < n; k++) {
                uint32_t c = arr[k].cp;
                if ((arr[k].flags & IPO_SHAPED_RTL) || (arr[k].flags & IPO_SHAPED_OVERSTRIKE) ||
                    (c >= '0' && c <= '9') || (c >= 0x0660u && c <= 0x0669u) ||
                    is_rtl_neutral(c)) {
                    if (arr[k].flags & IPO_SHAPED_RTL) rend = k + 1;
                    continue;
                }
                break;
            }
            /* First flip base+overstrike clusters within [rstart, rend) by reversing clusters */
            /* Copy out slice */
            ipo_shaped_glyph_t *tmp = (ipo_shaped_glyph_t *)malloc((size_t)(rend - rstart) * sizeof(ipo_shaped_glyph_t));
            if (tmp) {
                int rlen = rend - rstart;
                int dst = 0;
                int pos = rend - 1;
                while (pos >= rstart) {
                    /* If pos lands inside overstrike marks, walk to its cluster base */
                    int c_end = pos + 1;
                    while (pos > rstart && (arr[pos].flags & IPO_SHAPED_OVERSTRIKE)) pos--;
                    int c_start = pos;
                    uint32_t c0 = arr[c_start].cp;
                    if ((c0 >= '0' && c0 <= '9') || (c0 >= 0x0660u && c0 <= 0x0669u)) {
                        /* Keep multi-digit numbers LTR inside RTL run */
                        int d_start = c_start;
                        while (d_start > rstart) {
                            uint32_t pc = arr[d_start - 1].cp;
                            if ((pc >= '0' && pc <= '9') || (pc >= 0x0660u && pc <= 0x0669u) ||
                                ((pc == '.' || pc == ',') && d_start - 1 > rstart &&
                                 arr[d_start - 2].cp >= '0' && arr[d_start - 2].cp <= '9')) {
                                d_start--;
                            } else break;
                        }
                        for (int m = d_start; m < c_end; m++) tmp[dst++] = arr[m];
                        pos = d_start - 1;
                    } else {
                        for (int m = c_start; m < c_end; m++) tmp[dst++] = arr[m];
                        pos = c_start - 1;
                    }
                }
                memcpy(arr + rstart, tmp, (size_t)rlen * sizeof(ipo_shaped_glyph_t));
                free(tmp);
            }
            i = rend;
        }
    }

    int copy_n = n < out_cap ? n : out_cap;
    if (out && copy_n > 0) memcpy(out, arr, (size_t)copy_n * sizeof(ipo_shaped_glyph_t));
    if (arr != stack_buf) free(arr);
    return n;
}

int ipo_font_measure_glyph(int style_idx, int height, uint32_t cp) {
    if (ipo_font_is_zero_width(cp)) return 0;
    resolved_glyph_t rg;
    if (!resolve_glyph(style_idx, height, cp, &rg)) return -1;
    if ((rg.flags & 1u) || rg.adv == 0) return 0;
    if (height == rg.src_h) return rg.adv;
    int scaled = (rg.adv * height + (rg.src_h >> 1)) / rg.src_h;
    return (scaled < 1) ? 1 : scaled;
}

int ipo_font_text_width(int style_idx, int height, const char *utf8) {
    if (!utf8 || !*utf8) return 0;
    ipo_shaped_glyph_t stack_out[128];
    int total = ipo_font_shape_utf8(utf8, stack_out, 128);
    ipo_shaped_glyph_t *arr = stack_out;
    ipo_shaped_glyph_t *heap_arr = NULL;
    if (total > 128) {
        heap_arr = (ipo_shaped_glyph_t *)malloc((size_t)total * sizeof(ipo_shaped_glyph_t));
        if (heap_arr) {
            ipo_font_shape_utf8(utf8, heap_arr, total);
            arr = heap_arr;
        } else {
            total = 128;
        }
    }
    int w = 0;
    for (int i = 0; i < total; i++) {
        if (arr[i].flags & (IPO_SHAPED_OVERSTRIKE | IPO_SHAPED_ZEROWIDTH)) continue;
        int adv = ipo_font_measure_glyph(style_idx, height, arr[i].cp);
        if (adv < 0) adv = ipo_font_measure_fallback(height);
        w += adv;
    }
    if (heap_arr) free(heap_arr);
    return w;
}

int ipo_font_draw_glyph_rgb32(uint32_t *dst, int dst_stride,
                              int clip_x1, int clip_y1, int clip_x2, int clip_y2,
                              int x, int y, int style_idx, int height,
                              uint32_t cp, uint32_t fg_rgb) {
    if (ipo_font_is_zero_width(cp)) return 0;
    if (height < 6) height = 6;
    resolved_glyph_t rg;
    if (!resolve_glyph(style_idx, height, cp, &rg)) {
        /* Draw clean crisp tofu box fallback in UI */
        int tw = ipo_font_measure_fallback(height);
        int bx = x + 1, by = y + 2;
        int bw = tw - 2, bh = height - 4;
        if (bw >= 3 && bh >= 4) {
            uint32_t rgb = fg_rgb & 0x00FFFFFFu;
            for (int ry = by; ry < by + bh; ry++) {
                if (ry < clip_y1 || ry >= clip_y2) continue;
                for (int rx = bx; rx < bx + bw; rx++) {
                    if (rx < clip_x1 || rx >= clip_x2) continue;
                    if (ry == by || ry == by + bh - 1 || rx == bx || rx == bx + bw - 1)
                        dst[ry * dst_stride + rx] = rgb;
                }
            }
        }
        return tw;
    }

    int is_comb = (rg.flags & 1u) ? 1 : 0;
    int adv = is_comb ? 0 :
              ((height == rg.src_h) ? rg.adv : ((rg.adv * height + (rg.src_h >> 1)) / rg.src_h));
    if (!is_comb && adv < 1) adv = 1;
    if (rg.bw == 0 || rg.bh == 0) return adv;

    int fg_luma = luma255(fg_rgb);
    int synth_bold = (style_idx == IPO_FONT_BOLD && rg.bits1 != NULL) ? 1 : 0;

    if (height == rg.src_h && rg.alpha4 && !synth_bold) {
        int gx0 = x + rg.xoff;
        int gy0 = y + rg.yoff;
        int stride4 = (rg.bw + 1) >> 1;
        for (int row = 0; row < rg.bh; row++) {
            int dy = gy0 + row;
            if (dy < clip_y1 || dy >= clip_y2) continue;
            uint32_t *drow = dst + dy * dst_stride;
            const uint8_t *arow = rg.alpha4 + row * stride4;
            for (int col = 0; col < rg.bw; col++) {
                int dx = gx0 + col;
                if (dx < clip_x1 || dx >= clip_x2) continue;
                uint8_t b = arow[col >> 1];
                uint8_t a4 = (col & 1) ? (b & 0x0Fu) : (b >> 4);
                if (a4 == 0) continue;
                uint32_t bg = drow[dx];
                int dol = fg_luma < luma255(bg);
                drow[dx] = blend_rgb_alpha(bg, fg_rgb, a4, dol);
            }
        }
        return adv;
    }

    int sh = rg.src_h;
    int x0_s = (height == sh) ? rg.xoff : ((rg.xoff * height) / sh);
    int y0_s = (height == sh) ? rg.yoff : ((rg.yoff * height) / sh);
    int bw_s = (height == sh) ? rg.bw : ((rg.bw * height + sh - 1) / sh);
    int bh_s = (height == sh) ? rg.bh : ((rg.bh * height + sh - 1) / sh);
    if (bw_s < 1) bw_s = 1;
    if (bh_s < 1) bh_s = 1;
    if (synth_bold && rg.bw > 0) bw_s += 1;

    for (int row = 0; row < bh_s; row++) {
        int dy = y + y0_s + row;
        if (dy < clip_y1 || dy >= clip_y2) continue;
        uint32_t *drow = dst + dy * dst_stride;
        for (int col = 0; col < bw_s; col++) {
            int dx = x + x0_s + col;
            if (dx < clip_x1 || dx >= clip_x2) continue;
            uint8_t a4 = resampled_cov15(&rg, bw_s, bh_s, col, row, synth_bold);
            if (a4 == 0) continue;
            uint32_t bg = drow[dx];
            int dol = fg_luma < luma255(bg);
            drow[dx] = blend_rgb_alpha(bg, fg_rgb, a4, dol);
        }
    }
    return adv;
}

int ipo_font_raster_cell_bgr0(uint32_t *out_bgr0, int out_w, int out_h,
                              int style_idx, int height, int italic,
                              uint32_t cp, uint32_t fg_rgb, uint32_t bg_rgb) {
    resolved_glyph_t rg;
    if (!resolve_glyph(style_idx, height, cp, &rg)) return -1;

    uint32_t bg_clean = bg_rgb & 0x00FFFFFFu;
    uint32_t fg_clean = fg_rgb & 0x00FFFFFFu;
    int total = out_w * out_h;
    for (int i = 0; i < total; i++) out_bgr0[i] = bg_clean;
    if (rg.bw == 0 || rg.bh == 0) return 0;

    int dol = luma255(fg_clean) < luma255(bg_clean);
    int sh = rg.src_h;
    int synth_bold = (style_idx == IPO_FONT_BOLD && rg.bits1 != NULL) ? 1 : 0;
    int baseline = (height == sh) ? rg.src_base : ((rg.src_base * height + (sh >> 1)) / sh);

    if (height == sh && !italic && rg.alpha4 && !synth_bold && rg.xoff + rg.bw <= out_w) {
        int gx0 = rg.xoff;
        int gy0 = rg.yoff;
        int stride4 = (rg.bw + 1) >> 1;
        for (int row = 0; row < rg.bh; row++) {
            int dy = gy0 + row;
            if (dy < 0 || dy >= out_h) continue;
            uint32_t *drow = out_bgr0 + dy * out_w;
            const uint8_t *arow = rg.alpha4 + row * stride4;
            for (int col = 0; col < rg.bw; col++) {
                int dx = gx0 + col;
                if (dx < 0 || dx >= out_w) continue;
                uint8_t b = arow[col >> 1];
                uint8_t a4 = (col & 1) ? (b & 0x0Fu) : (b >> 4);
                if (a4 == 0) continue;
                drow[dx] = blend_rgb_alpha(bg_clean, fg_clean, a4, dol);
            }
        }
        return 0;
    }

    int ox = (height == sh) ? rg.xoff : ((rg.xoff * height + (sh >> 1)) / sh);
    int oy = (height == sh) ? rg.yoff : ((rg.yoff * height + (sh >> 1)) / sh);
    int bw_s = (height == sh) ? rg.bw : ((rg.bw * height + sh - 1) / sh);
    int bh_s = (height == sh) ? rg.bh : ((rg.bh * height + sh - 1) / sh);
    if (bw_s < 1) bw_s = 1;
    if (bh_s < 1) bh_s = 1;
    if (synth_bold) bw_s += 1;
    /* Fit smoothly inside out_w if a wide char was constrained inside a monospace cell */
    if (ox >= 0 && ox + bw_s > out_w && out_w > 2) {
        bw_s = out_w - ox;
    } else if (ox < 0 && bw_s > out_w) {
        ox = 0;
        bw_s = out_w;
    }

    for (int row = 0; row < bh_s; row++) {
        int dy = oy + row;
        if (dy < 0 || dy >= out_h) continue;
        int shear = italic ? ((baseline - dy) * 20) / 100 : 0;
        uint32_t *drow = out_bgr0 + dy * out_w;
        for (int col = 0; col < bw_s; col++) {
            int dx = ox + col + shear;
            if (dx < 0 || dx >= out_w) continue;
            uint8_t a4 = resampled_cov15(&rg, bw_s, bh_s, col, row, synth_bold);
            if (a4 == 0) continue;
            drow[dx] = blend_rgb_alpha(bg_clean, fg_clean, a4, dol);
        }
    }
    return 0;
}

int ipo_font_measure_fallback(int height) {
    if (height < 6) height = 6;
    int w = (height * 6 + 5) / 10;
    return w < 5 ? 5 : w;
}

int ipo_font_raster_fallback_bgr0(uint32_t *out_bgr0, int out_w, int out_h,
                                  int height, uint32_t fg_rgb, uint32_t bg_rgb) {
    (void)height;
    uint32_t bg_clean = bg_rgb & 0x00FFFFFFu;
    uint32_t fg_clean = fg_rgb & 0x00FFFFFFu;
    int total = out_w * out_h;
    for (int i = 0; i < total; i++) out_bgr0[i] = bg_clean;
    int bx0 = 1, by0 = 2, bx1 = out_w - 2, by1 = out_h - 3;
    if (bx1 <= bx0 || by1 <= by0) return 0;
    for (int y = by0; y <= by1; y++) {
        for (int x = bx0; x <= bx1; x++) {
            if (y == by0 || y == by1 || x == bx0 || x == bx1) {
                out_bgr0[y * out_w + x] = fg_clean;
            }
        }
    }
    return 0;
}
"""


if __name__ == "__main__":
    main()
