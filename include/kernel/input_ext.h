#ifndef USER_KERNEL_INPUT_EXT_H
#define USER_KERNEL_INPUT_EXT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Extended 32-bit Keycode Encoding
 * - 0x00000001 .. 0x0000007F : Set-1 single-byte scancodes (known or unknown)
 * - 0x0000E000 .. 0x0000E07F : 0xE0-prefixed extended scancodes (known or unknown)
 * - 0x00E10000 .. 0x00E17F7F : 0xE1 multi-byte scancodes
 * - 0x00010000 .. 0x7FFFFFFF : External driver / vendor-defined button codes
 */
#define IPO_KEY_MAKE_EXT_E0(sc)    (0x0000E000u | ((uint32_t)(sc) & 0x7Fu))
#define IPO_KEY_MAKE_EXT_E1(b1,b2) (0x00E10000u | (((uint32_t)(b1) & 0x7Fu) << 8) | ((uint32_t)(b2) & 0x7Fu))

#define IPO_KEY_FLAG_BREAK         (1u << 0)
#define IPO_KEY_FLAG_EXT_E0        (1u << 1)
#define IPO_KEY_FLAG_EXT_E1        (1u << 2)
#define IPO_KEY_FLAG_REPEAT        (1u << 3)
#define IPO_KEY_FLAG_INJECTED      (1u << 4)

#define IPO_MOD_SHIFT              (1u << 0)
#define IPO_MOD_CTRL               (1u << 1)
#define IPO_MOD_ALT                (1u << 2)
#define IPO_MOD_SUPER              (1u << 3)
#define IPO_MOD_CAPS               (1u << 4)
#define IPO_MOD_NUM                (1u << 5)

/* Shortcut Registration Flags */
#define IPO_SHORTCUT_FLAG_GLOBAL        (1u << 0)
#define IPO_SHORTCUT_FLAG_CONSUME       (1u << 1)
#define IPO_SHORTCUT_FLAG_EXACT_MODS    (1u << 2)
#define IPO_SHORTCUT_FLAG_ON_RELEASE    (1u << 3)
#define IPO_SHORTCUT_FLAG_PASSIVE       (1u << 4)
#define IPO_SHORTCUT_FLAG_ALLOW_REPEAT  (1u << 5)

#ifndef IPO_HOTKEY_MODE_DEFAULT
#define IPO_HOTKEY_MODE_DEFAULT         0x00u
#define IPO_HOTKEY_MODE_DISABLE_OS      0x01u
#define IPO_HOTKEY_MODE_OVERRIDE_APP    0x02u
#define IPO_HOTKEY_MODE_INCLUDE_CTRL_C  0x04u
#endif

/* Generic Input Event Types & Capabilities */
#define IPO_EV_SYN                 0x00u
#define IPO_EV_KEY                 0x01u
#define IPO_EV_REL                 0x02u
#define IPO_EV_ABS                 0x03u
#define IPO_EV_RAW                 0x04u

#define IPO_INPUT_CAP_KEY              (1u << 0)
#define IPO_INPUT_CAP_REL              (1u << 1)
#define IPO_INPUT_CAP_ABS              (1u << 2)
#define IPO_INPUT_CAP_KEYBOARD_ROUTE   (1u << 3)
#define IPO_INPUT_CAP_RAW              (1u << 4)

/* PS/2 Hook Channels, Flags & Transact Ports */
#define IPO_PS2_CHAN_KBD           (1u << 0)
#define IPO_PS2_CHAN_AUX           (1u << 1)

#define IPO_PS2_HOOK_OBSERVE       0x00u
#define IPO_PS2_HOOK_EXCLUSIVE     (1u << 0)

#define IPO_PS2_PORT_CTRL          0u
#define IPO_PS2_PORT_KBD           1u
#define IPO_PS2_PORT_AUX           2u

/* MMIO Mapping Flags */
#define IPO_MMIO_CACHE_DISABLE     (1u << 0)
#define IPO_MMIO_WRITE_THROUGH     (1u << 1)

typedef struct {
    uint32_t keycode;
    uint32_t modifiers;
    uint32_t flags;
    uint32_t source_dev_id;
    uint64_t timestamp_ms;
    uint8_t *raw_bytes;
    uint32_t raw_cap;
    uint32_t raw_len;
} ipo_key_event_t;

typedef struct {
    uint32_t id;
    uint32_t owner_pid;
    uint32_t window_handle;
    uint32_t mod_mask;
    uint32_t key_count;
    const uint32_t *keys;
    uint32_t flags;
    uint32_t user_tag;
} ipo_shortcut_spec_t;

typedef struct {
    uint32_t shortcut_id;
    uint32_t user_tag;
    uint32_t trigger_keycode;
    uint32_t modifiers;
    uint32_t window_handle;
    uint32_t reserved;
    uint64_t timestamp_ms;
} ipo_shortcut_event_t;

typedef struct {
    const char *name;
    uint16_t    bus_type;
    uint16_t    vendor_id;
    uint16_t    product_id;
    uint16_t    pad0;
    uint32_t    caps;
} ipo_input_dev_desc_t;

typedef struct {
    uint32_t id;
    uint32_t owner_pid;
    uint32_t device_node_id;
    uint32_t caps;
    uint16_t bus_type;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t pad0;
    uint32_t event_count;
    char    *name;
    uint32_t name_cap;
    uint32_t name_len;
} ipo_input_dev_info_t;

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

typedef struct __attribute__((packed)) {
    uint8_t  bus, slot, func, irq;
    uint16_t vendor_id, device_id;
    uint16_t subsys_vendor_id, subsys_id;
    uint8_t  class_code, subclass, prog_if, revision_id;
    uint8_t  header_type, pad0;
    uint16_t pad1;
    uint32_t *bars;
    uint32_t  bars_cap;
    uint32_t  bar_count;
} ipo_pci_dev_info_t;

typedef struct {
    uint32_t id;
    uint32_t parent_id;
    uint32_t capabilities;
    uint32_t type;
    uint32_t state;
    uint32_t bus_type;
    uint8_t  pci_bus, pci_slot, pci_func, irq;
    uint16_t vendor_id, device_id;
    uint16_t io_base, io_size;
    uint64_t mmio_base;
    uint32_t mmio_size;
    uint64_t size_bytes;
    char    *name;
    uint32_t name_cap;
    uint32_t name_len;
    char    *vendor;
    uint32_t vendor_cap;
    uint32_t vendor_len;
    char    *model;
    uint32_t model_cap;
    uint32_t model_len;
    char    *description;
    uint32_t desc_cap;
    uint32_t desc_len;
    char    *driver_name;
    uint32_t driver_name_cap;
    uint32_t driver_name_len;
} ipo_device_info_t;

typedef void (*ipo_irq_cb_t)(uint8_t irq, void *ctx);

/* Extended Input & Custom Shortcuts */
int      ipo_input_poll_key_ext(ipo_key_event_t *out_ev);
bool     ipo_input_get_key_state(uint32_t keycode);
uint32_t ipo_input_get_modifiers(void);
uint32_t ipo_shortcut_register(const ipo_shortcut_spec_t *spec);
int      ipo_shortcut_unregister(uint32_t shortcut_id);
int      ipo_shortcut_poll(ipo_shortcut_event_t *out_ev);

/* Generic Input Device Registry & Event Injection */
uint32_t ipo_input_dev_register(const ipo_input_dev_desc_t *desc);
int      ipo_input_dev_unregister(uint32_t dev_id);
int      ipo_input_dev_emit(uint32_t dev_id, const ipo_input_event_t *ev);
uint32_t ipo_input_dev_list(ipo_input_dev_info_t *out_arr, uint32_t max_count);
int      ipo_input_dev_poll_event(uint32_t dev_filter_id, ipo_input_event_t *out_ev);
int      ipo_input_dev_get_state(uint32_t dev_id, uint16_t type, uint32_t code, int32_t *out_val);

/* Low-Level PS/2 Byte Hooks & Controller Transactions */
uint32_t ipo_ps2_hook_register(uint32_t chan_mask, uint32_t flags);
int      ipo_ps2_hook_unregister(uint32_t handle);
uint32_t ipo_ps2_hook_poll(uint32_t handle, ipo_ps2_byte_event_t *out_events, uint32_t max_count);
int      ipo_ps2_transact(const ipo_ps2_transact_req_t *req);

/* Hardware IRQ Primitives */
uint32_t ipo_irq_subscribe(uint8_t irq);
int      ipo_irq_unsubscribe(uint32_t handle);
uint32_t ipo_irq_poll(uint32_t handle, uint32_t *out_total, uint64_t *out_last_ms);
uint32_t ipo_irq_get_count(uint8_t irq);
uint32_t ipo_irq_register_cb(uint8_t irq, ipo_irq_cb_t fn, void *ctx);
int      ipo_irq_unregister_cb(uint32_t handle);

/* Low-Level MMIO / DMA Memory Primitives */
void    *ipo_hw_map_mmio(uint32_t phys_addr, uint32_t size, uint32_t flags);
int      ipo_hw_unmap_mmio(void *virt_addr, uint32_t size);
void    *ipo_hw_alloc_dma(uint32_t size, uint32_t alignment, uint32_t *out_phys);
int      ipo_hw_free_dma(void *virt_addr);
uint32_t ipo_hw_virt_to_phys(const void *virt_addr);

/* PCI Enumeration & Configuration Primitives */
uint32_t ipo_pci_get_count(void);
int      ipo_pci_get_device(uint32_t index, ipo_pci_dev_info_t *out_info);
uint32_t ipo_pci_config_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t width);
int      ipo_pci_config_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t width, uint32_t value);
int      ipo_pci_enable_bus_master(uint8_t bus, uint8_t slot, uint8_t func);

/* Unified Device Registry Primitives */
uint32_t ipo_device_get_count(void);
int      ipo_device_get_info(uint32_t index, ipo_device_info_t *out_info);
uint32_t ipo_device_register(const ipo_device_info_t *info);
int      ipo_device_unregister(uint32_t dev_id);

#ifdef __cplusplus
}
#endif

#endif /* USER_KERNEL_INPUT_EXT_H */
