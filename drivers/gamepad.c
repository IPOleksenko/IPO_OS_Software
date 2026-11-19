#include "kmd_api.h"
#include "virtio_input.h"

/* Loadable IPO_OS Gamepad Driver (.kmd)
 * Supports:
 *   1. VirtIO Input Gamepad (0x1AF4:0x1052 with BTN_GAMEPAD or serial "IPO-GAMEPAD")
 *   2. ISA PC Gameport (I/O port 0x201) buttons & dual-axis joystick
 *   3. USB UHCI host detection (PCI 0x0C:0x03)
 *   4. Kernel driver command interface (`gamepad ...`) & ioctl injection
 * Emits standard gamepad button/hat events (DPAD_LEFT/RIGHT, A, B, X, Y, START) to `ipo-gamepad`.
 */

#define GP_GAMEPORT_IO   0x201u

#define GP_MASK_LEFT     (1u << 0)
#define GP_MASK_RIGHT    (1u << 1)
#define GP_MASK_A        (1u << 2)
#define GP_MASK_B        (1u << 3)
#define GP_MASK_X        (1u << 4)
#define GP_MASK_Y        (1u << 5)
#define GP_MASK_START    (1u << 6)

static uint32_t        s_gp_dev_id = 0u;
static vio_input_dev_t s_vio_gp;
static bool            s_gameport_ok = false;
static bool            s_uhci_found = false;
static uint32_t        s_btn_mask = 0u;
static uint32_t        s_gp_abs_x = 16384u;

static uint32_t        s_auto_release_deadline = 0u;
static uint32_t        s_auto_release_code = 0u;

static void gp_emit_btn(uint32_t code, bool down) {
    if (s_gp_dev_id == 0u) return;
    ipo_input_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = IPO_EV_KEY;
    ev.code  = code;
    ev.value = down ? 1 : 0;
    ipo_input_dev_emit(s_gp_dev_id, &ev, 0u);
    ev.type  = IPO_EV_SYN;
    ev.code  = 0u;
    ev.value = 0;
    ipo_input_dev_emit(s_gp_dev_id, &ev, 0u);
}

static void gp_set_mask_bit(uint32_t mask_bit, uint32_t btn_code, bool down) {
    bool was = (s_btn_mask & mask_bit) != 0u;
    if (was == down) return;
    if (down) s_btn_mask |= mask_bit;
    else      s_btn_mask &= ~mask_bit;
    gp_emit_btn(btn_code, down);
}

static bool gp_vio_match(const vio_input_dev_t *probe) {
    if (vio_str_contains_ci(probe->serial, "IPO-GAMEPAD")) return true;
    if (vio_has_ev_bit(probe->dev_cfg, 0x01u, 0x130u)) return true; /* BTN_SOUTH / BTN_GAMEPAD */
    if (vio_str_contains_ci(probe->name, "gamepad")) return true;
    if (vio_str_contains_ci(probe->name, "joystick")) return true;
    return false;
}

static void gp_vio_cb(uint16_t type, uint16_t code, int32_t value, void *ctx) {
    (void)ctx;
    if (type == 0x01u) { /* EV_KEY */
        bool down = (value != 0);
        switch (code) {
            case IPO_BTN_DPAD_LEFT:  gp_set_mask_bit(GP_MASK_LEFT,  IPO_BTN_DPAD_LEFT,  down); break;
            case IPO_BTN_DPAD_RIGHT: gp_set_mask_bit(GP_MASK_RIGHT, IPO_BTN_DPAD_RIGHT, down); break;
            case IPO_BTN_SOUTH:      gp_set_mask_bit(GP_MASK_A,     IPO_BTN_SOUTH,      down); break;
            case IPO_BTN_EAST:       gp_set_mask_bit(GP_MASK_B,     IPO_BTN_EAST,       down); break;
            case IPO_BTN_WEST:       gp_set_mask_bit(GP_MASK_X,     IPO_BTN_WEST,       down); break;
            case IPO_BTN_NORTH:      gp_set_mask_bit(GP_MASK_Y,     IPO_BTN_NORTH,      down); break;
            case IPO_BTN_START:      gp_set_mask_bit(GP_MASK_START, IPO_BTN_START,      down); break;
            /* Handle button events when USB/virtual tablet advertises serial=IPO-GAMEPAD */
            case 0x110u: /* BTN_LEFT -> Gamepad A */
                gp_set_mask_bit(GP_MASK_A, IPO_BTN_SOUTH, down);
                break;
            case 0x111u: /* BTN_RIGHT -> Gamepad Y (Fullscreen) */
                gp_set_mask_bit(GP_MASK_Y, IPO_BTN_NORTH, down);
                break;
            case 0x112u: /* BTN_MIDDLE -> Gamepad START (Exit) */
                gp_set_mask_bit(GP_MASK_START, IPO_BTN_START, down);
                break;
            case 0x113u: /* BTN_SIDE -> Gamepad X (Move Left) */
                gp_set_mask_bit(GP_MASK_X, IPO_BTN_WEST, down);
                break;
            case 0x114u: /* BTN_EXTRA -> Gamepad B (Move Right) */
                gp_set_mask_bit(GP_MASK_B, IPO_BTN_EAST, down);
                break;
            default:
                gp_emit_btn(code, down);
                break;
        }
    } else if (type == 0x03u) { /* EV_ABS */
        if (code == 0x00u) { /* ABS_X (0..32767) -> DPAD Left / Right */
            s_gp_abs_x = (uint32_t)value;
            bool left  = (value >= 0 && value < 11000);
            bool right = (value > 21767);
            gp_set_mask_bit(GP_MASK_LEFT,  IPO_BTN_DPAD_LEFT,  left);
            gp_set_mask_bit(GP_MASK_RIGHT, IPO_BTN_DPAD_RIGHT, right);
        } else if (code == 0x10u) { /* ABS_HAT0X (-1, 0, +1) */
            gp_set_mask_bit(GP_MASK_LEFT,  IPO_BTN_DPAD_LEFT,  value < 0);
            gp_set_mask_bit(GP_MASK_RIGHT, IPO_BTN_DPAD_RIGHT, value > 0);
        }
    }
}

static void gp_poll_gameport(void) {
    if (!s_gameport_ok) return;
    uint8_t status = inb(GP_GAMEPORT_IO);
    if (status == 0xFFu) return;
    /* Gameport buttons bits 4..7 are active low */
    gp_set_mask_bit(GP_MASK_A, IPO_BTN_SOUTH, (status & 0x10u) == 0u);
    gp_set_mask_bit(GP_MASK_B, IPO_BTN_EAST,  (status & 0x20u) == 0u);
    gp_set_mask_bit(GP_MASK_X, IPO_BTN_WEST,  (status & 0x40u) == 0u);
    gp_set_mask_bit(GP_MASK_Y, IPO_BTN_NORTH, (status & 0x80u) == 0u);
}

static void gp_poll_task(void) {
    if (s_vio_gp.active) {
        vio_poll_events(&s_vio_gp, gp_vio_cb, NULL);
    }
    if (s_gameport_ok) {
        gp_poll_gameport();
    }
    if (s_auto_release_deadline != 0u && timer_millis() >= s_auto_release_deadline) {
        s_auto_release_deadline = 0u;
        if (s_auto_release_code != 0u) {
            gp_emit_btn(s_auto_release_code, false);
            s_auto_release_code = 0u;
        }
    }
}

static bool gp_name_to_code(const char *s, uint32_t *out_code) {
    if (!s || !out_code) return false;
    if (strcmp(s, "left") == 0 || strcmp(s, "dpad_left") == 0)  { *out_code = IPO_BTN_DPAD_LEFT;  return true; }
    if (strcmp(s, "right") == 0 || strcmp(s, "dpad_right") == 0) { *out_code = IPO_BTN_DPAD_RIGHT; return true; }
    if (strcmp(s, "a") == 0 || strcmp(s, "south") == 0)         { *out_code = IPO_BTN_SOUTH;      return true; }
    if (strcmp(s, "b") == 0 || strcmp(s, "east") == 0)          { *out_code = IPO_BTN_EAST;       return true; }
    if (strcmp(s, "x") == 0 || strcmp(s, "west") == 0)          { *out_code = IPO_BTN_WEST;       return true; }
    if (strcmp(s, "y") == 0 || strcmp(s, "north") == 0)         { *out_code = IPO_BTN_NORTH;      return true; }
    if (strcmp(s, "start") == 0)                                { *out_code = IPO_BTN_START;      return true; }
    return false;
}

static int gp_parse_int(const char *s) {
    if (!s) return 0;
    int val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return val;
}

static int gp_on_command(const char *cmd, int argc, char **argv) {
    (void)cmd;
    if (argc < 1 || strcmp(argv[0], "gamepad") != 0) return -1;
    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        serial_printf("[gamepad] dev_id=%u virtio=%d (%s) gameport=%d uhci=%d abs_x=%u mask=0x%x\n",
                      s_gp_dev_id, s_vio_gp.active ? 1 : 0,
                      (s_vio_gp.active && s_vio_gp.name) ? s_vio_gp.name : "none",
                      s_gameport_ok ? 1 : 0, s_uhci_found ? 1 : 0,
                      s_gp_abs_x, s_btn_mask);
        return 0;
    }
    uint32_t code = 0u;
    if (strcmp(argv[1], "down") == 0 && argc >= 3 && gp_name_to_code(argv[2], &code)) {
        gp_emit_btn(code, true);
        return 0;
    }
    if (strcmp(argv[1], "up") == 0 && argc >= 3 && gp_name_to_code(argv[2], &code)) {
        gp_emit_btn(code, false);
        return 0;
    }
    if (strcmp(argv[1], "tap") == 0 && argc >= 3 && gp_name_to_code(argv[2], &code)) {
        int ms = (argc >= 4) ? gp_parse_int(argv[3]) : 90;
        if (ms < 20) ms = 20;
        gp_emit_btn(code, true);
        s_auto_release_code = code;
        s_auto_release_deadline = timer_millis() + (uint32_t)ms;
        return 0;
    }
    return 0;
}

static int gp_read(void *buf, size_t size, uint32_t offset) {
    (void)offset;
    if (!buf || size < sizeof(uint32_t)) return -1;
    memcpy(buf, &s_btn_mask, sizeof(s_btn_mask));
    return (int)sizeof(s_btn_mask);
}

static int gp_ioctl(uint32_t cmd, void *arg) {
    if (!arg) return -1;
    uint32_t code = *(const uint32_t *)arg;
    if (cmd == 1u) { gp_emit_btn(code, true); return 0; }
    if (cmd == 2u) { gp_emit_btn(code, false); return 0; }
    return -1;
}

static int gp_drv_init(void) { return 0; }
static int gp_drv_cleanup(void) { return 0; }

static driver_t s_gp_drv = {
    .name        = "gamepad",
    .description = "Gamepad/Controller Driver (VirtIO-Input + PC Gameport + USB HID)",
    .flags       = DRIVER_FLAG_KERNEL | DRIVER_FLAG_ACTIVE | DRIVER_FLAG_CONTINUOUS_TICK,
    .init        = gp_drv_init,
    .cleanup     = gp_drv_cleanup,
    .on_tick     = gp_poll_task,
    .on_command  = gp_on_command,
    .read        = gp_read,
    .ioctl       = gp_ioctl,
};

int kmd_main_init(void) {
    memset(&s_vio_gp, 0, sizeof(s_vio_gp));
    s_btn_mask = 0u;

    bool has_vio = vio_probe_and_init(&s_vio_gp, gp_vio_match);

    uint8_t gp_val = inb(GP_GAMEPORT_IO);
    s_gameport_ok = (gp_val != 0xFFu);

    kmd_pci_device_t usb_dev;
    s_uhci_found = pci_find_class(0x0Cu, 0x03u, &usb_dev);

    ipo_input_dev_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.name       = "ipo-gamepad";
    desc.bus_type   = has_vio ? KMD_BUS_PCI : (s_gameport_ok ? KMD_BUS_ISA : KMD_BUS_VIRTUAL);
    desc.vendor_id  = has_vio ? VIRTIO_PCI_VENDOR_ID : 0x1D6Bu;
    desc.product_id = has_vio ? VIRTIO_PCI_DEV_INPUT : 0x0001u;
    desc.caps       = IPO_INPUT_CAP_KEY | IPO_INPUT_CAP_ABS;
    s_gp_dev_id = ipo_input_dev_register(&desc, 0u);
    if (s_gp_dev_id == 0u) return -1;

    driver_register(&s_gp_drv);
    async_start_task("gamepad_poll", 5u, gp_poll_task);

    serial_printf("[gamepad] kmd initialized (dev_id=%u virtio=%d gameport=%d uhci=%d)\n",
                  s_gp_dev_id, has_vio ? 1 : 0, s_gameport_ok ? 1 : 0, s_uhci_found ? 1 : 0);
    return 0;
}

int kmd_main_cleanup(void) {
    async_stop_task("gamepad_poll");
    driver_unregister("gamepad");
    if (s_vio_gp.active) {
        vio_cleanup_dev(&s_vio_gp);
    }
    if (s_gp_dev_id != 0u) {
        ipo_input_dev_unregister(s_gp_dev_id, 0u);
        s_gp_dev_id = 0u;
    }
    serial_printf("[gamepad] kmd unloaded\n");
    return 0;
}
