/*
 * Userspace wrappers for extended input, shortcut, PS/2 hook, IRQ, and MMIO syscalls.
 */

#include <kernel/input_ext.h>
#include <driver/input/mouse.h>
#include <syscall.h>

int ipo_input_poll_key_ext(ipo_key_event_t *out_ev) {
    return ipo_sys_input_poll_key_ext(out_ev);
}

bool ipo_input_get_key_state(uint32_t keycode) {
    return ipo_sys_input_get_key_state(keycode) != 0;
}

uint32_t ipo_input_get_modifiers(void) {
    return ipo_sys_input_get_modifiers();
}

uint32_t ipo_shortcut_register(const ipo_shortcut_spec_t *spec) {
    return ipo_sys_shortcut_register(spec);
}

int ipo_shortcut_unregister(uint32_t shortcut_id) {
    return ipo_sys_shortcut_unregister(shortcut_id);
}

int ipo_shortcut_poll(ipo_shortcut_event_t *out_ev) {
    return ipo_sys_shortcut_poll(out_ev);
}

uint32_t ipo_input_dev_register(const ipo_input_dev_desc_t *desc) {
    return ipo_sys_input_dev_register(desc);
}

int ipo_input_dev_unregister(uint32_t dev_id) {
    return ipo_sys_input_dev_unregister(dev_id);
}

int ipo_input_dev_emit(uint32_t dev_id, const ipo_input_event_t *ev) {
    return ipo_sys_input_dev_emit(dev_id, ev);
}

uint32_t ipo_input_dev_list(ipo_input_dev_info_t *out_arr, uint32_t max_count) {
    return ipo_sys_input_dev_list(out_arr, max_count);
}

int ipo_input_dev_poll_event(uint32_t dev_filter_id, ipo_input_event_t *out_ev) {
    return ipo_sys_input_dev_poll_event(dev_filter_id, out_ev);
}

int ipo_input_dev_get_state(uint32_t dev_id, uint16_t type, uint32_t code, int32_t *out_val) {
    return ipo_sys_input_dev_get_state(dev_id, type, code, out_val);
}

void mouse_inject_event(int32_t dx_or_x, int32_t dy_or_y, int32_t dz,
                        uint32_t buttons_mask, bool is_absolute) {
    (void)ipo_sys_mouse_inject(dx_or_x, dy_or_y, dz, buttons_mask, is_absolute);
}

uint32_t ipo_ps2_hook_register(uint32_t chan_mask, uint32_t flags) {
    return ipo_sys_ps2_hook_register(chan_mask, flags);
}

int ipo_ps2_hook_unregister(uint32_t handle) {
    return ipo_sys_ps2_hook_unregister(handle);
}

uint32_t ipo_ps2_hook_poll(uint32_t handle, ipo_ps2_byte_event_t *out_events, uint32_t max_count) {
    return ipo_sys_ps2_hook_poll(handle, out_events, max_count);
}

int ipo_ps2_transact(const ipo_ps2_transact_req_t *req) {
    return ipo_sys_ps2_transact(req);
}

uint32_t ipo_irq_subscribe(uint8_t irq) {
    return ipo_sys_irq_subscribe(irq);
}

int ipo_irq_unsubscribe(uint32_t handle) {
    return ipo_sys_irq_unsubscribe(handle);
}

uint32_t ipo_irq_poll(uint32_t handle, uint32_t *out_total, uint64_t *out_last_ms) {
    return ipo_sys_irq_poll(handle, out_total, out_last_ms);
}

uint32_t ipo_irq_get_count(uint8_t irq) {
    return ipo_sys_irq_get_count(irq);
}

uint32_t ipo_irq_register_cb(uint8_t irq, ipo_irq_cb_t fn, void *ctx) {
    return ipo_sys_irq_register_cb(irq, fn, ctx);
}

int ipo_irq_unregister_cb(uint32_t handle) {
    return ipo_sys_irq_unregister_cb(handle);
}

void *ipo_hw_map_mmio(uint32_t phys_addr, uint32_t size, uint32_t flags) {
    return ipo_sys_hw_map_mmio(phys_addr, size, flags);
}

int ipo_hw_unmap_mmio(void *virt_addr, uint32_t size) {
    return ipo_sys_hw_unmap_mmio(virt_addr, size);
}

void *ipo_hw_alloc_dma(uint32_t size, uint32_t alignment, uint32_t *out_phys) {
    return ipo_sys_hw_alloc_dma(size, alignment, out_phys);
}

int ipo_hw_free_dma(void *virt_addr) {
    return ipo_sys_hw_free_dma(virt_addr);
}

uint32_t ipo_hw_virt_to_phys(const void *virt_addr) {
    return ipo_sys_hw_virt_to_phys(virt_addr);
}

uint32_t ipo_pci_get_count(void) {
    return ipo_sys_pci_get_count();
}

int ipo_pci_get_device(uint32_t index, ipo_pci_dev_info_t *out_info) {
    return ipo_sys_pci_get_device(index, out_info);
}

uint32_t ipo_pci_config_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t width) {
    return ipo_sys_pci_config_read(bus, slot, func, offset, width);
}

int ipo_pci_config_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t width, uint32_t value) {
    return ipo_sys_pci_config_write(bus, slot, func, offset, width, value);
}

int ipo_pci_enable_bus_master(uint8_t bus, uint8_t slot, uint8_t func) {
    return ipo_sys_pci_enable_bus_master(bus, slot, func);
}

uint32_t ipo_device_get_count(void) {
    return ipo_sys_device_get_count();
}

int ipo_device_get_info(uint32_t index, ipo_device_info_t *out_info) {
    return ipo_sys_device_get_info(index, out_info);
}

uint32_t ipo_device_register(const ipo_device_info_t *info) {
    return ipo_sys_device_register(info);
}

int ipo_device_unregister(uint32_t dev_id) {
    return ipo_sys_device_unregister(dev_id);
}
