#ifndef DRIVERS_KMD_API_H
#define DRIVERS_KMD_API_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Kernel driver structures & exported kernel APIs for loadable .kmd modules.
 * Must stay ABI-identical to IPO_OS/src/include/.
 */

typedef struct driver {
    char *name;
    char *description;
    uint32_t flags;

    int  (*init)(void);
    int  (*cleanup)(void);
    void (*on_tick)(void);
    bool (*on_key)(uint8_t scancode, bool is_break);
    int  (*on_command)(const char *cmd, int argc, char **argv);
    char (*on_char_output)(char c);

    int  (*read)(void *buffer, size_t size, uint32_t offset);
    int  (*write)(const void *buffer, size_t size, uint32_t offset);
    int  (*ioctl)(uint32_t cmd, void *arg);

    void *owner;
    struct driver *next;
} driver_t;

#define DRIVER_FLAG_KERNEL          (1u << 0)
#define DRIVER_FLAG_USER            (1u << 1)
#define DRIVER_FLAG_ACTIVE          (1u << 2)
#define DRIVER_FLAG_CONTINUOUS_TICK (1u << 3)

typedef struct {
    const char *name;
    uint16_t    bus_type;
    uint16_t    vendor_id;
    uint16_t    product_id;
    uint16_t    pad0;
    uint32_t    caps;
} ipo_input_dev_desc_t;

typedef struct {
    uint32_t dev_id;
    uint16_t type;
    uint16_t flags;
    uint32_t code;
    int32_t  value;
    uint64_t timestamp_ms;
} ipo_input_event_t;

typedef struct {
    uint32_t chan_mask;
    uint8_t  byte;
    uint8_t  chan;
    uint8_t  pad0;
    uint8_t  pad1;
    uint64_t timestamp_ms;
} ipo_ps2_byte_event_t;

typedef struct {
    uint8_t  cmd;
    uint8_t  port_id;
    uint8_t  param_len;
    uint8_t  reply_len;
    const uint8_t *params;
    uint8_t *reply;
    uint32_t flags;
} ipo_ps2_transact_req_t;

#define IPO_EV_SYN               0x00u
#define IPO_EV_KEY               0x01u
#define IPO_EV_REL               0x02u
#define IPO_EV_ABS               0x03u
#define IPO_EV_RAW               0x04u

#define IPO_INPUT_CAP_KEY        (1u << 0)
#define IPO_INPUT_CAP_REL        (1u << 1)
#define IPO_INPUT_CAP_ABS        (1u << 2)
#define IPO_INPUT_CAP_RAW        (1u << 4)

#define IPO_PS2_CHAN_KBD         (1u << 0)
#define IPO_PS2_CHAN_AUX         (1u << 1)
#define IPO_PS2_HOOK_OBSERVE     0x00u
#define IPO_PS2_HOOK_EXCLUSIVE   (1u << 0)
#define IPO_PS2_PORT_CTRL        0u
#define IPO_PS2_PORT_KBD         1u
#define IPO_PS2_PORT_AUX         2u

#define IPO_MMIO_CACHE_DISABLE   (1u << 0)
#define IPO_MMIO_WRITE_THROUGH   (1u << 1)

/* Bus types matching IPO_OS/src/include/kernel/device.h */
#define KMD_BUS_PCI              1u
#define KMD_BUS_ISA              2u
#define KMD_BUS_PS2              6u
#define KMD_BUS_USB              7u
#define KMD_BUS_VIRTUAL          8u

/* Standard input device event codes shared between .kmd drivers and applications */
#define IPO_BTN_SOUTH            0x130u  /* Gamepad A */
#define IPO_BTN_EAST             0x131u  /* Gamepad B */
#define IPO_BTN_NORTH            0x133u  /* Gamepad Y */
#define IPO_BTN_WEST             0x134u  /* Gamepad X */
#define IPO_BTN_START            0x13Bu  /* Gamepad START */
#define IPO_BTN_DPAD_UP          0x220u
#define IPO_BTN_DPAD_DOWN        0x221u
#define IPO_BTN_DPAD_LEFT        0x222u
#define IPO_BTN_DPAD_RIGHT       0x223u

#define IPO_ABS_X                0x00u
#define IPO_ABS_Y                0x01u
#define IPO_ABS_MT_SLOT          0x2Fu
#define IPO_ABS_MT_POSITION_X    0x35u   /* 0 .. 10000 normalized (0.0 .. 1.0) */
#define IPO_ABS_MT_POSITION_Y    0x36u   /* 0 .. 10000 normalized (0.0 .. 1.0) */
#define IPO_ABS_MT_TRACKING_ID   0x39u   /* >= 0 down, -1 up */
#define IPO_BTN_TOUCH            0x14Au
#define IPO_BTN_TOOL_FINGER      0x145u
#define IPO_BTN_TOOL_DOUBLETAP   0x14Du

typedef struct {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsys_vendor_id;
    uint16_t subsys_id;
    uint8_t  revision_id;
    uint32_t bar0;
    uint8_t  irq;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  header_type;
    uint32_t *bar;
    size_t   bar_count;
    size_t   bar_cap;
    char    *vendor_name;
    char    *device_name;
} kmd_pci_device_t;

typedef void (*async_task_fn_t)(void);
typedef void (*irq_handler_fn_t)(uint8_t irq, void *ctx);

/* Kernel exported functions resolved dynamically by bare-metal kmd_crt.s via resolve_sym() */
extern int      driver_register(driver_t *drv);
extern int      driver_unregister(const char *name);
extern int      driver_unregister_force(const char *name);

extern uint32_t ipo_input_dev_register(const ipo_input_dev_desc_t *desc, uint32_t owner_pid);
extern int      ipo_input_dev_unregister(uint32_t dev_id, uint32_t caller_pid);
extern int      ipo_input_dev_emit(uint32_t dev_id, const ipo_input_event_t *ev, uint32_t caller_pid);

extern uint32_t ipo_ps2_hook_register(uint32_t chan_mask, uint32_t flags, uint32_t owner_pid);
extern int      ipo_ps2_hook_unregister(uint32_t handle, uint32_t caller_pid);
extern uint32_t ipo_ps2_hook_poll(uint32_t handle, uint32_t caller_pid,
                                  ipo_ps2_byte_event_t *out_events, uint32_t max_count);
extern int      ipo_ps2_transact(const ipo_ps2_transact_req_t *req);
extern void     keyboard_poll(void);

extern int      async_start_task(const char *name, uint32_t interval_ms, async_task_fn_t fn);
extern int      async_stop_task(const char *name);
extern uint32_t timer_millis(void);

extern uint32_t irq_register_handler(uint8_t irq, irq_handler_fn_t fn, void *ctx, uint32_t owner_pid);
extern int      irq_unregister_handler(uint32_t handle, uint32_t caller_pid);
extern bool     irq_enable(uint8_t irq);

extern uint32_t pci_get_count(void);
extern const kmd_pci_device_t *pci_get_device(uint32_t index);
extern bool     pci_find_device(uint16_t vendor_id, uint16_t device_id, kmd_pci_device_t *out_dev);
extern bool     pci_find_class(uint8_t class_code, uint8_t subclass, kmd_pci_device_t *out_dev);
extern void     pci_enable_bus_mastering(const kmd_pci_device_t *dev);
extern uint32_t pci_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
extern uint16_t pci_read_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
extern uint8_t  pci_read_byte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
extern void     pci_write_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);
extern void     pci_write_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val);

extern void    *ipo_hw_map_mmio(uint32_t phys_addr, uint32_t size, uint32_t flags, uint32_t owner_pid);
extern int      ipo_hw_unmap_mmio(void *virt_addr, uint32_t size, uint32_t caller_pid);
extern void    *ipo_hw_alloc_dma(uint32_t size, uint32_t alignment, uint32_t *out_phys, uint32_t owner_pid);
extern int      ipo_hw_free_dma(void *virt_addr, uint32_t caller_pid);
extern uint32_t ipo_hw_virt_to_phys(const void *virt_addr, uint32_t caller_pid);

extern void    *kmalloc(size_t size);
extern void    *krealloc(void *ptr, size_t size);
extern void     kfree(void *ptr);
extern void    *memset(void *dst, int c, size_t n);
extern void    *memcpy(void *dst, const void *src, size_t n);
extern int      memcmp(const void *a, const void *b, size_t n);
extern int      strcmp(const char *a, const char *b);
extern char    *strncpy(char *dst, const char *src, size_t n);
extern size_t   strlen(const char *s);
extern int      serial_printf(const char *fmt, ...);

extern uint8_t  inb(uint16_t port);
extern void     outb(uint16_t port, uint8_t value);
extern uint16_t inw(uint16_t port);
extern void     outw(uint16_t port, uint16_t value);
extern uint32_t inl(uint16_t port);
extern void     outl(uint16_t port, uint32_t value);

#endif /* DRIVERS_KMD_API_H */
