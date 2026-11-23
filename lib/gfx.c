#include <ipo/gfx.h>
#include <syscall.h>
#include <vga.h>
#include <string.h>

uint32_t ipo_gfx_api_version(void) {
    return (uint32_t)ipo_syscall(IPO_SYSCALL_GFX_API_VERSION, 0u, NULL);
}

int ipo_gfx_get_caps(ipo_gfx_caps_t *out_caps) {
    if (!out_caps) return -1;
    uint32_t args[1] = { (uint32_t)(uintptr_t)out_caps };
    return ipo_syscall(IPO_SYSCALL_GFX_GET_CAPS, 1u, args);
}

int ipo_gfx_enum_modes(uint32_t index, ipo_gfx_mode_info_t *out_mode) {
    if (!out_mode) return -1;
    uint32_t args[2] = { index, (uint32_t)(uintptr_t)out_mode };
    return ipo_syscall(IPO_SYSCALL_GFX_ENUM_MODES, 2u, args);
}

int ipo_gfx_set_mode(uint32_t width, uint32_t height, uint32_t bpp) {
    uint32_t args[3] = { width, height, bpp };
    return ipo_syscall(IPO_SYSCALL_GFX_SET_MODE, 3u, args);
}

int ipo_gfx_ensure_hd_mode(uint32_t pref_w, uint32_t pref_h) {
    ipo_gfx_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = (uint32_t)sizeof(caps);
    if (ipo_gfx_get_caps(&caps) != 0) {
        return -1;
    }
    if ((caps.hw_bpp_mask & (1u << 4)) == 0u ||
        caps.max_hw_width < 800u || caps.max_hw_height < 600u) {
        return -1;
    }
    if (caps.active_hw_bpp >= 15u && caps.active_width >= 800u) {
        return 0;
    }
    uint32_t tw = pref_w ? pref_w : 1280u;
    uint32_t th = pref_h ? pref_h : 720u;
    if (tw > caps.max_hw_width) {
        tw = 800u;
    }
    if (th > caps.max_hw_height) {
        th = 600u;
    }
    if (ipo_gfx_set_mode(tw, th, 32u) == 0) {
        return 0;
    }
    if ((tw != 800u || th != 600u) && ipo_gfx_set_mode(800u, 600u, 32u) == 0) {
        return 0;
    }
    return -1;
}

int ipo_gfx_get_mode(ipo_gfx_mode_info_t *out_mode) {
    if (!out_mode) return -1;
    uint32_t args[1] = { (uint32_t)(uintptr_t)out_mode };
    return ipo_syscall(IPO_SYSCALL_GFX_GET_MODE, 1u, args);
}

int ipo_gfx_select_best_formats(const ipo_color_select_req_t *req,
                                ipo_pixel_format_t *out_render,
                                ipo_pixel_format_t *out_hw) {
    if (!req || !out_render || !out_hw) return -1;
    uint32_t args[3] = {
        (uint32_t)(uintptr_t)req,
        (uint32_t)(uintptr_t)out_render,
        (uint32_t)(uintptr_t)out_hw
    };
    int rc = ipo_syscall(IPO_SYSCALL_GFX_SELECT_FORMAT, 3u, args);
    if (rc == 0) {
        if (out_render->format_id != IPO_COLOR_FMT_CUSTOM) {
            ipo_color_format_from_id((ipo_color_format_id_t)out_render->format_id, out_render);
        }
        if (out_hw->format_id != IPO_COLOR_FMT_CUSTOM) {
            ipo_color_format_from_id((ipo_color_format_id_t)out_hw->format_id, out_hw);
        }
    }
    return rc;
}

int ipo_gfx_present_window(wm_window_t *win, const ipo_gfx_surface_desc_t *surf) {
    if (!win || !surf) return -1;
    uint32_t args[2] = { (uint32_t)(uintptr_t)win, (uint32_t)(uintptr_t)surf };
    return ipo_syscall(IPO_SYSCALL_GFX_PRESENT_WINDOW, 2u, args);
}

int ipo_gfx_present_screen(const ipo_gfx_surface_desc_t *surf) {
    if (!surf) return -1;
    uint32_t args[1] = { (uint32_t)(uintptr_t)surf };
    return ipo_syscall(IPO_SYSCALL_GFX_PRESENT_SCREEN, 1u, args);
}

int ipo_gfx_get_stats(ipo_gfx_stats_t *out_stats) {
    if (!out_stats) return -1;
    uint32_t args[1] = { (uint32_t)(uintptr_t)out_stats };
    return ipo_syscall(IPO_SYSCALL_GFX_GET_STATS, 1u, args);
}

int ipo_gfx_sync_palette_32(uint32_t *out_xrgb256) {
    if (!out_xrgb256) return -1;
    uint32_t args[1] = { (uint32_t)(uintptr_t)out_xrgb256 };
    return ipo_syscall(IPO_SYSCALL_GFX_GET_PALETTE32, 1u, args);
}

static inline int gfx_log_to_phys_coord(int l, int s, int hw) {
    if (s <= 0) return l;
    if (l <= 0) return (l * hw) / s;
    return (l * hw + s - 1) / s;
}

void ipo_gfx_wm_window_phys_rect(const wm_window_t *win,
                                 int *out_px, int *out_py, int *out_pw, int *out_ph) {
    int hw_w = 0, hw_h = 0;
    ipo_gfx_caps_t caps;
    if (ipo_gfx_get_caps(&caps) == 0 && caps.active_width > 0u && caps.active_height > 0u) {
        hw_w = (int)caps.active_width;
        hw_h = (int)caps.active_height;
    } else {
        vga_gfx_get_screen_bounds(&hw_w, &hw_h);
    }
    if (hw_w <= 0 || hw_h <= 0) { hw_w = 320; hw_h = 200; }
    if (!win) {
        if (out_px) *out_px = 0;
        if (out_py) *out_py = 0;
        if (out_pw) *out_pw = hw_w;
        if (out_ph) *out_ph = hw_h;
        return;
    }
    int sw = 0, sh = 0;
    wm_get_screen_bounds(&sw, &sh);
    if (sw <= 0 || sh <= 0) { sw = 320; sh = 200; }
    int px0 = gfx_log_to_phys_coord((int)win->x, sw, hw_w);
    int py0 = gfx_log_to_phys_coord((int)win->y, sh, hw_h);
    int px1 = gfx_log_to_phys_coord((int)win->x + (int)win->w, sw, hw_w);
    int py1 = gfx_log_to_phys_coord((int)win->y + (int)win->h, sh, hw_h);
    if (out_px) *out_px = px0;
    if (out_py) *out_py = py0;
    if (out_pw) *out_pw = (px1 > px0) ? (px1 - px0) : 1;
    if (out_ph) *out_ph = (py1 > py0) ? (py1 - py0) : 1;
}
