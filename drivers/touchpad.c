#include "kmd_api.h"
#include "virtio_input.h"

/* Loadable IPO_OS Touchpad Driver (.kmd)
 * Supports:
 *   1. VirtIO Multitouch PCI (0x1AF4:0x1052 with ABS_MT_SLOT capability)
 *   2. PS/2 Synaptics Absolute + W-mode Touchpad via AUX hook / transactions
 *   3. Kernel driver command interface (`touchpad ...`) for diagnostics & testing
 * Emits normalized multitouch IPO_EV_ABS / IPO_EV_KEY / IPO_EV_SYN events to `ipo-touchpad`.
 */

typedef struct {
    int32_t tracking_id;
    int32_t x_norm; /* 0 .. 10000 */
    int32_t y_norm; /* 0 .. 10000 */
    bool    active;
    bool    auto_up;
} tp_slot_t;

static uint32_t       s_tp_dev_id = 0u;
static uint32_t       s_ps2_hook = 0u;
static bool           s_synaptics_ok = false;
static vio_input_dev_t s_vio_tp;
static tp_slot_t     *s_slots = NULL;
static int            s_slots_cap = 0;
static int32_t        s_cur_slot = 0;

/* PS/2 Synaptics 6-byte packet accumulator */
static uint8_t s_syn_pkt[6];
static uint8_t s_syn_idx = 0u;

/* Timed synthetic release / multi-finger hold deadline (0 = none) */
static uint32_t s_auto_up_deadline = 0u;

typedef struct {
    uint16_t type;
    uint16_t code;
    int32_t  value;
} tp_dbg_ev_t;
static tp_dbg_ev_t *s_dbg_events = NULL;
static uint32_t     s_dbg_cap = 0u;
static uint32_t     s_dbg_total = 0u;
static ipo_ps2_byte_event_t *s_ps2_evs = NULL;
static uint32_t     s_ps2_evs_cap = 0u;

static bool tp_ensure_slot(int slot) {
    if (slot < 0 || slot >= 1024) return false;
    if (slot < s_slots_cap) return true;
    int new_cap = s_slots_cap > 0 ? s_slots_cap : 8;
    while (new_cap <= slot) new_cap *= 2;
    tp_slot_t *ns = (tp_slot_t *)krealloc(s_slots, (size_t)new_cap * sizeof(tp_slot_t));
    if (!ns) return false;
    memset(ns + s_slots_cap, 0, (size_t)(new_cap - s_slots_cap) * sizeof(tp_slot_t));
    s_slots = ns;
    s_slots_cap = new_cap;
    return true;
}

static bool tp_record_dbg_event(uint16_t type, uint16_t code, int32_t value) {
    if (s_dbg_total >= s_dbg_cap) {
        uint32_t new_cap = (s_dbg_cap > 0u) ? (s_dbg_cap * 2u) : 16u;
        tp_dbg_ev_t *ne = (tp_dbg_ev_t *)krealloc(s_dbg_events, (size_t)new_cap * sizeof(tp_dbg_ev_t));
        if (!ne) return false;
        s_dbg_events = ne;
        s_dbg_cap = new_cap;
    }
    s_dbg_events[s_dbg_total].type  = type;
    s_dbg_events[s_dbg_total].code  = code;
    s_dbg_events[s_dbg_total].value = value;
    s_dbg_total++;
    return true;
}

static void tp_emit(uint16_t type, uint32_t code, int32_t value) {
    if (s_tp_dev_id == 0u) return;
    ipo_input_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    ipo_input_dev_emit(s_tp_dev_id, &ev, 0u);
}

static uint32_t tp_active_count(void) {
    uint32_t cnt = 0u;
    for (int i = 0; i < s_slots_cap; i++) {
        if (s_slots[i].active) cnt++;
    }
    return cnt;
}

static void tp_emit_finger_down(int slot, int32_t tracking_id, int32_t x_norm, int32_t y_norm) {
    if (!tp_ensure_slot(slot)) return;
    if (x_norm < 0) x_norm = 0;
    if (x_norm > 10000) x_norm = 10000;
    if (y_norm < 0) y_norm = 0;
    if (y_norm > 10000) y_norm = 10000;

    s_slots[slot].active = true;
    s_slots[slot].tracking_id = tracking_id;
    s_slots[slot].x_norm = x_norm;
    s_slots[slot].y_norm = y_norm;

    tp_emit(IPO_EV_ABS, IPO_ABS_MT_SLOT, slot);
    tp_emit(IPO_EV_ABS, IPO_ABS_MT_TRACKING_ID, tracking_id);
    tp_emit(IPO_EV_ABS, IPO_ABS_MT_POSITION_X, x_norm);
    tp_emit(IPO_EV_ABS, IPO_ABS_MT_POSITION_Y, y_norm);
    tp_emit(IPO_EV_KEY, IPO_BTN_TOUCH, 1);
    tp_emit(IPO_EV_ABS, IPO_ABS_X, x_norm);
    tp_emit(IPO_EV_ABS, IPO_ABS_Y, y_norm);
    tp_emit(IPO_EV_SYN, 0u, (int32_t)tp_active_count());
}

static void tp_emit_finger_up(int slot) {
    if (slot < 0 || slot >= s_slots_cap) return;
    if (!s_slots[slot].active) return;
    s_slots[slot].active = false;
    s_slots[slot].tracking_id = -1;
    s_slots[slot].auto_up = false;

    uint32_t rem = tp_active_count();
    tp_emit(IPO_EV_ABS, IPO_ABS_MT_SLOT, slot);
    tp_emit(IPO_EV_ABS, IPO_ABS_MT_TRACKING_ID, -1);
    if (rem == 0u) {
        tp_emit(IPO_EV_KEY, IPO_BTN_TOUCH, 0);
    }
    tp_emit(IPO_EV_SYN, 0u, (int32_t)rem);
}

static bool tp_vio_match(const vio_input_dev_t *probe) {
    if (vio_str_contains_ci(probe->serial, "IPO-GAMEPAD")) return false;
    if (vio_has_ev_bit(probe->dev_cfg, 0x03u, 0x2Fu)) return true; /* ABS_MT_SLOT */
    if (vio_str_contains_ci(probe->name, "multitouch")) return true;
    if (vio_str_contains_ci(probe->name, "touchpad")) return true;
    return false;
}

static void tp_vio_cb(uint16_t type, uint16_t code, int32_t value, void *ctx) {
    (void)ctx;
    tp_record_dbg_event(type, code, value);
    if (type == 0x03u) { /* EV_ABS */
        if (code == 0x2Fu) { /* ABS_MT_SLOT */
            if (value >= 0 && tp_ensure_slot(value)) s_cur_slot = value;
            tp_emit(IPO_EV_ABS, IPO_ABS_MT_SLOT, s_cur_slot);
        } else if (code == 0x39u) { /* ABS_MT_TRACKING_ID */
            if (tp_ensure_slot(s_cur_slot)) {
                if (value >= 0) {
                    s_slots[s_cur_slot].active = true;
                    s_slots[s_cur_slot].tracking_id = value;
                } else {
                    s_slots[s_cur_slot].active = false;
                    s_slots[s_cur_slot].tracking_id = -1;
                }
            }
            tp_emit(IPO_EV_ABS, IPO_ABS_MT_TRACKING_ID, value);
        } else if (code == 0x35u) { /* ABS_MT_POSITION_X (0..32767 -> 0..10000) */
            int32_t norm = (value * 10000) / 32767;
            if (norm < 0) norm = 0;
            if (norm > 10000) norm = 10000;
            if (tp_ensure_slot(s_cur_slot)) s_slots[s_cur_slot].x_norm = norm;
            tp_emit(IPO_EV_ABS, IPO_ABS_MT_POSITION_X, norm);
        } else if (code == 0x36u) { /* ABS_MT_POSITION_Y (0..32767 -> 0..10000) */
            int32_t norm = (value * 10000) / 32767;
            if (norm < 0) norm = 0;
            if (norm > 10000) norm = 10000;
            if (tp_ensure_slot(s_cur_slot)) s_slots[s_cur_slot].y_norm = norm;
            tp_emit(IPO_EV_ABS, IPO_ABS_MT_POSITION_Y, norm);
        }
    } else if (type == 0x01u) { /* EV_KEY */
        if (code == 0x14Au) { /* BTN_TOUCH */
            tp_emit(IPO_EV_KEY, IPO_BTN_TOUCH, value ? 1 : 0);
        }
    } else if (type == 0x00u && code == 0x00u) { /* SYN_REPORT */
        tp_emit(IPO_EV_SYN, 0u, (int32_t)tp_active_count());
    }
}

static void tp_probe_ps2_synaptics(void) {
    /* Synaptics magic sequence: 4x Set Resolution 0 (0xE8 0x00) + Status Request (0xE9).
     * Response byte 1 == 0x47 confirms Synaptics TouchPad controller. */
    uint8_t zero = 0x00u;
    uint8_t stat[3] = { 0, 0, 0 };
    for (int i = 0; i < 4; i++) {
        ipo_ps2_transact_req_t req;
        memset(&req, 0, sizeof(req));
        req.cmd = 0xE8u;
        req.port_id = IPO_PS2_PORT_AUX;
        req.param_len = 1u;
        req.params = &zero;
        if (ipo_ps2_transact(&req) != 0) return;
    }
    ipo_ps2_transact_req_t sreq;
    memset(&sreq, 0, sizeof(sreq));
    sreq.cmd = 0xE9u;
    sreq.port_id = IPO_PS2_PORT_AUX;
    sreq.reply_len = 3u;
    sreq.reply = stat;
    if (ipo_ps2_transact(&sreq) == 0 && stat[1] == 0x47u && stat[2] != 0x00u) {
        s_synaptics_ok = true;
        serial_printf("[touchpad] Synaptics PS/2 touchpad detected (id=%02x.%02x)\n", stat[0], stat[2]);
    }
}

static void tp_process_synaptics_byte(uint8_t b) {
    if (!s_synaptics_ok) return;
    if (s_syn_idx == 0u && (b & 0xC8u) != 0x80u) return;
    if (s_syn_idx == 3u && (b & 0xC8u) != 0xC0u) { s_syn_idx = 0u; return; }
    s_syn_pkt[s_syn_idx++] = b;
    if (s_syn_idx < 6u) return;
    s_syn_idx = 0u;

    uint8_t w = (uint8_t)(((s_syn_pkt[0] & 0x30u) >> 2) |
                          ((s_syn_pkt[0] & 0x04u) >> 1) |
                          ((s_syn_pkt[3] & 0x04u) >> 2));
    uint8_t z = s_syn_pkt[2];
    int32_t x = (int32_t)(((uint32_t)(s_syn_pkt[3] & 0x10u) << 8) |
                          ((uint32_t)(s_syn_pkt[1] & 0x0Fu) << 8) |
                          (uint32_t)s_syn_pkt[4]);
    int32_t y = (int32_t)(((uint32_t)(s_syn_pkt[3] & 0x20u) << 7) |
                          ((uint32_t)(s_syn_pkt[1] & 0xF0u) << 4) |
                          (uint32_t)s_syn_pkt[5]);
    /* Typical Synaptics coordinates: 1472..5472 X, 1408..4448 Y */
    int32_t xn = (x <= 1472) ? 0 : ((x >= 5472) ? 10000 : ((x - 1472) * 10000) / 4000);
    int32_t yn = (y <= 1408) ? 10000 : ((y >= 4448) ? 0 : (10000 - ((y - 1408) * 10000) / 3040));

    if (z < 12) {
        if (s_slots[0].active) tp_emit_finger_up(0);
        if (s_slots[1].active) tp_emit_finger_up(1);
    } else {
        tp_emit_finger_down(0, 1, xn, yn);
        if (w == 0u) {
            tp_emit_finger_down(1, 2, xn, yn);
        } else if (s_slots[1].active) {
            tp_emit_finger_up(1);
        }
    }
}

static void tp_poll_task(void) {
    keyboard_poll();
    if (s_vio_tp.active) {
        vio_poll_events(&s_vio_tp, tp_vio_cb, NULL);
    }
    if (s_ps2_hook != 0u) {
        if (!s_ps2_evs) {
            s_ps2_evs_cap = 16u;
            s_ps2_evs = (ipo_ps2_byte_event_t *)kmalloc((size_t)s_ps2_evs_cap * sizeof(ipo_ps2_byte_event_t));
        }
        while (s_ps2_evs && s_ps2_evs_cap > 0u) {
            uint32_t cap = s_ps2_evs_cap;
            uint32_t n = ipo_ps2_hook_poll(s_ps2_hook, 0u, s_ps2_evs, cap);
            for (uint32_t i = 0; i < n; i++) {
                tp_process_synaptics_byte(s_ps2_evs[i].byte);
            }
            if (n < cap) break;
            uint32_t new_cap = cap * 2u;
            if (new_cap > 1024u) break;
            ipo_ps2_byte_event_t *nb = (ipo_ps2_byte_event_t *)krealloc(
                s_ps2_evs, (size_t)new_cap * sizeof(ipo_ps2_byte_event_t));
            if (!nb) break;
            s_ps2_evs = nb;
            s_ps2_evs_cap = new_cap;
        }
    }
    if (s_auto_up_deadline != 0u && timer_millis() >= s_auto_up_deadline) {
        s_auto_up_deadline = 0u;
        for (int s = 0; s < s_slots_cap; s++) {
            if (s_slots[s].auto_up) {
                tp_emit_finger_up(s);
            }
        }
    }
}

static int tp_parse_int(const char *s) {
    if (!s) return 0;
    int sign = 1, val = 0;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return sign * val;
}

static int tp_on_command(const char *cmd, int argc, char **argv) {
    (void)cmd;
    if (argc < 1 || strcmp(argv[0], "touchpad") != 0) return -1;
    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        serial_printf("[touchpad] dev_id=%u virtio=%d (%s) synaptics=%d active_fingers=%u vio_ev=%u\n",
                      s_tp_dev_id, s_vio_tp.active ? 1 : 0,
                      (s_vio_tp.active && s_vio_tp.name) ? s_vio_tp.name : "none",
                      s_synaptics_ok ? 1 : 0, tp_active_count(), s_dbg_total);
        if (argc >= 3 && strcmp(argv[2], "log") == 0) {
            for (uint32_t i = 0; i < s_dbg_total; i++) {
                tp_dbg_ev_t *e = &s_dbg_events[i];
                serial_printf("  ev[%u]: type=%u code=0x%x val=%d\n", i, e->type, e->code, e->value);
            }
        }
        return 0;
    }
    if (strcmp(argv[1], "tap") == 0 && argc >= 3) {
        int32_t x = 5000;
        if (strcmp(argv[2], "left") == 0) x = 2500;
        else if (strcmp(argv[2], "right") == 0) x = 7500;
        else x = tp_parse_int(argv[2]);
        int ms = (argc >= 4) ? tp_parse_int(argv[3]) : 80;
        if (ms < 20) ms = 20;
        tp_emit_finger_down(0, 100, x, 5000);
        if (tp_ensure_slot(0)) s_slots[0].auto_up = true;
        s_auto_up_deadline = timer_millis() + (uint32_t)ms;
        return 0;
    }
    if (strcmp(argv[1], "twohold") == 0) {
        int ms = (argc >= 3) ? tp_parse_int(argv[2]) : 2150;
        if (ms < 50) ms = 50;
        tp_emit_finger_down(0, 200, 3500, 5000);
        tp_emit_finger_down(1, 201, 6500, 5000);
        if (tp_ensure_slot(0)) s_slots[0].auto_up = true;
        if (tp_ensure_slot(1)) s_slots[1].auto_up = true;
        s_auto_up_deadline = timer_millis() + (uint32_t)ms;
        return 0;
    }
    if (strcmp(argv[1], "up") == 0) {
        for (int i = 0; i < s_slots_cap; i++) tp_emit_finger_up(i);
        s_auto_up_deadline = 0u;
        return 0;
    }
    return 0;
}

static int tp_read(void *buf, size_t size, uint32_t offset) {
    (void)offset;
    if (!buf || size < sizeof(uint32_t)) return -1;
    uint32_t count = tp_active_count();
    memcpy(buf, &count, sizeof(count));
    return (int)sizeof(count);
}

static int tp_drv_init(void) { return 0; }
static int tp_drv_cleanup(void) { return 0; }

static driver_t s_tp_drv = {
    .name        = "touchpad",
    .description = "Multi-touch Touchpad Driver (VirtIO-Multitouch + PS/2 Synaptics)",
    .flags       = DRIVER_FLAG_KERNEL | DRIVER_FLAG_ACTIVE | DRIVER_FLAG_CONTINUOUS_TICK,
    .init        = tp_drv_init,
    .cleanup     = tp_drv_cleanup,
    .on_tick     = tp_poll_task,
    .on_command  = tp_on_command,
    .read        = tp_read,
};

int kmd_main_init(void) {
    memset(&s_vio_tp, 0, sizeof(s_vio_tp));

    bool has_vio = vio_probe_and_init(&s_vio_tp, tp_vio_match);
    tp_probe_ps2_synaptics();
    s_ps2_hook = ipo_ps2_hook_register(IPO_PS2_CHAN_AUX, IPO_PS2_HOOK_OBSERVE, 0u);

    ipo_input_dev_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.name       = "ipo-touchpad";
    desc.bus_type   = has_vio ? KMD_BUS_PCI : (s_synaptics_ok ? KMD_BUS_PS2 : KMD_BUS_VIRTUAL);
    desc.vendor_id  = has_vio ? VIRTIO_PCI_VENDOR_ID : 0x1D6Bu;
    desc.product_id = has_vio ? VIRTIO_PCI_DEV_INPUT : 0x0002u;
    desc.caps       = IPO_INPUT_CAP_ABS | IPO_INPUT_CAP_KEY;
    s_tp_dev_id = ipo_input_dev_register(&desc, 0u);
    if (s_tp_dev_id == 0u) return -1;

    driver_register(&s_tp_drv);
    async_start_task("touchpad_poll", 5u, tp_poll_task);

    serial_printf("[touchpad] kmd initialized (dev_id=%u virtio=%d synaptics=%d)\n",
                  s_tp_dev_id, has_vio ? 1 : 0, s_synaptics_ok ? 1 : 0);
    return 0;
}

int kmd_main_cleanup(void) {
    async_stop_task("touchpad_poll");
    driver_unregister("touchpad");
    if (s_ps2_hook != 0u) {
        ipo_ps2_hook_unregister(s_ps2_hook, 0u);
        s_ps2_hook = 0u;
    }
    if (s_vio_tp.active) {
        vio_cleanup_dev(&s_vio_tp);
    }
    if (s_tp_dev_id != 0u) {
        ipo_input_dev_unregister(s_tp_dev_id, 0u);
        s_tp_dev_id = 0u;
    }
    if (s_slots) {
        kfree(s_slots);
        s_slots = NULL;
        s_slots_cap = 0;
    }
    if (s_dbg_events) {
        kfree(s_dbg_events);
        s_dbg_events = NULL;
        s_dbg_cap = 0u;
        s_dbg_total = 0u;
    }
    if (s_ps2_evs) {
        kfree(s_ps2_evs);
        s_ps2_evs = NULL;
        s_ps2_evs_cap = 0u;
    }
    serial_printf("[touchpad] kmd unloaded\n");
    return 0;
}
