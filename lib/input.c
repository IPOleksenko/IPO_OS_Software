#include <driver/input/keyboard.h>
#include <driver/input/mouse.h>
#include <driver/input/keymap/keymap.h>
#include <driver/input/keymap/dynamic_keymap.h>
#include <syscall.h>
#include <string.h>

void keyboard_poll(void) {
    ipo_syscall(IPO_SYSCALL_KBD_POLL, 0u, NULL);
}

uint8_t keyboard_get_scancode(void) {
    return (uint8_t)ipo_syscall(IPO_SYSCALL_KBD_GET_SCANCODE, 0u, NULL);
}

uint8_t keyboard_wait_scancode(void) {
    return (uint8_t)ipo_syscall(IPO_SYSCALL_KBD_WAIT_SCANCODE, 0u, NULL);
}

void keyboard_set_app_input_mode(bool enabled) {
    uint32_t arg = enabled ? 1u : 0u;
    ipo_syscall(IPO_SYSCALL_KBD_SET_APP_MODE, 1u, &arg);
}

bool keyboard_is_app_input_mode(void) {
    return true;
}

void keyboard_flush_queue(void) {
    ipo_syscall(IPO_SYSCALL_KBD_FLUSH, 0u, NULL);
}

void keyboard_flush_app_queue(void) {
    ipo_syscall(IPO_SYSCALL_KBD_FLUSH, 0u, NULL);
}

void keyboard_flush_hardware(void) {
    ipo_syscall(IPO_SYSCALL_KBD_FLUSH, 0u, NULL);
}

void keyboard_clear_key_state(void) {
    ipo_syscall(IPO_SYSCALL_KBD_FLUSH, 0u, NULL);
}

uint8_t keyboard_get_modifiers(void) {
    return (uint8_t)ipo_syscall(IPO_SYSCALL_KBD_GET_MODIFIERS, 0u, NULL);
}

bool keyboard_is_ctrl_pressed(void) {
    return (keyboard_get_modifiers() & KBD_MOD_CTRL) != 0;
}

bool keyboard_is_shift_pressed(void) {
    return (keyboard_get_modifiers() & KBD_MOD_SHIFT) != 0;
}

bool keyboard_is_alt_pressed(void) {
    return (keyboard_get_modifiers() & KBD_MOD_ALT) != 0;
}

void update_hot_key_state(uint8_t scancode) {
    (void)scancode;
}

char* get_keymap(void) {
    static char dynamic_table[256];
    bool shift = keyboard_is_shift_pressed();
    uint32_t args[4] = { 1u, (uint32_t)(uintptr_t)dynamic_table, (uint32_t)sizeof(dynamic_table), (uint32_t)shift };
    ipo_syscall(IPO_SYSCALL_KEYMAP_GET, 4u, args);
    return dynamic_table;
}

const char* keyboard_get_key_string(uint8_t scancode) {
    static char buf[32];
    buf[0] = '\0';
    uint32_t args[3] = { (uint32_t)scancode, (uint32_t)(uintptr_t)buf, (uint32_t)sizeof(buf) };
    uint32_t res = ipo_syscall(IPO_SYSCALL_KBD_GET_KEY_STRING, 3u, args);
    if (res != 0 && buf[0] != '\0') {
        return buf;
    }
    if (res != 0 && res != 1 && res != IPO_SYSCALL_ENOSYS) {
        return (const char *)(uintptr_t)res;
    }
    return NULL;
}

char get_char(uint8_t scancode) {
    const char *s = keyboard_get_key_string(scancode);
    if (s != NULL && s[0] != '\0') {
        return s[0];
    }
    return 0;
}

/* Mouse API */
void mouse_get_state(mouse_state_t *out_state) {
    if (!out_state) return;
    uint32_t arg = (uint32_t)(uintptr_t)out_state;
    ipo_syscall(IPO_SYSCALL_MOUSE_GET_STATE, 1u, &arg);
}

void mouse_get_position(int32_t *out_x, int32_t *out_y) {
    mouse_state_t ms;
    mouse_get_state(&ms);
    if (out_x) *out_x = ms.x;
    if (out_y) *out_y = ms.y;
}

bool mouse_is_pressed(int button) {
    mouse_state_t ms;
    mouse_get_state(&ms);
    if (button == MOUSE_BTN_LEFT) return ms.left_button;
    if (button == MOUSE_BTN_RIGHT) return ms.right_button;
    if (button == MOUSE_BTN_MIDDLE) return ms.middle_button;
    return (ms.buttons & (uint32_t)button) != 0;
}

bool mouse_is_in_region(int32_t x, int32_t y, int32_t w, int32_t h) {
    mouse_state_t ms;
    mouse_get_state(&ms);
    return (ms.x >= x && ms.x < (x + w) && ms.y >= y && ms.y < (y + h));
}

bool mouse_is_clicked_in_region(int32_t x, int32_t y, int32_t w, int32_t h, int button) {
    return mouse_is_in_region(x, y, w, h) && mouse_is_pressed(button);
}

bool mouse_check_click(int32_t x, int32_t y, int32_t w, int32_t h) {
    return mouse_is_clicked_in_region(x, y, w, h, MOUSE_BTN_LEFT);
}

/* Dynamic Keymap API */
void dynamic_keymap_set_app_mode(bool is_app) {
    (void)is_app;
}

void dynamic_keymap_cycle_next(void) {
    ipo_syscall(IPO_SYSCALL_KEYMAP_CYCLE_NEXT, 0u, NULL);
}

void dynamic_keymap_cycle_prev(void) {
    ipo_syscall(IPO_SYSCALL_KEYMAP_CYCLE_PREV, 0u, NULL);
}

const char* dynamic_keymap_get_name(void) {
    static char name_buf[64];
    uint32_t args[2] = { (uint32_t)(uintptr_t)name_buf, (uint32_t)sizeof(name_buf) };
    if (ipo_syscall(IPO_SYSCALL_KEYMAP_GET_NAME, 2u, args) == 0) {
        return name_buf;
    }
    return "English";
}

int dynamic_keymap_set(const char *name, const keymap_entry_t *entries, uint32_t count) {
    uint32_t args[3] = { (uint32_t)(uintptr_t)name, (uint32_t)(uintptr_t)entries, count };
    return ipo_syscall(IPO_SYSCALL_KEYMAP_SET, 3u, args);
}

int dynamic_keymap_register_with_font(const char *name, const keymap_entry_t *entries, uint32_t count,
                                      const dynamic_glyph_def_t *glyphs, uint32_t glyph_count) {
    (void)glyphs; (void)glyph_count;
    return dynamic_keymap_set(name, entries, count);
}

int dynamic_keymap_disable(const char *name_or_id) {
    uint32_t args[1] = { (uint32_t)(uintptr_t)name_or_id };
    return ipo_syscall(IPO_SYSCALL_KEYMAP_DISABLE, 1u, args);
}

int dynamic_keymap_enable(const char *name_or_id) {
    uint32_t args[1] = { (uint32_t)(uintptr_t)name_or_id };
    return ipo_syscall(IPO_SYSCALL_KEYMAP_ENABLE, 1u, args);
}

int dynamic_keymap_remove(const char *name_or_id) {
    uint32_t args[1] = { (uint32_t)(uintptr_t)name_or_id };
    return ipo_syscall(IPO_SYSCALL_KEYMAP_REMOVE, 1u, args);
}

bool dynamic_keymap_is_active(void) {
    return ipo_syscall(IPO_SYSCALL_KEYMAP_IS_ACTIVE, 0u, NULL) != 0;
}

uint32_t dynamic_keymap_get_count(void) {
    uint32_t args[1] = { 3u };
    return ipo_syscall(IPO_SYSCALL_KEYMAP_GET, 1u, args);
}

bool dynamic_keymap_is_slot_enabled(uint32_t index) {
    uint32_t args[2] = { 6u, index };
    return ipo_syscall(IPO_SYSCALL_KEYMAP_GET, 2u, args) != 0;
}

const char* dynamic_keymap_get_slot_name(uint32_t index) {
    static char slot_name_buf[64];
    slot_name_buf[0] = '\0';
    uint32_t args[4] = { 5u, index, (uint32_t)(uintptr_t)slot_name_buf, (uint32_t)sizeof(slot_name_buf) };
    ipo_syscall(IPO_SYSCALL_KEYMAP_GET, 4u, args);
    return slot_name_buf;
}

void dynamic_keymap_reset(void) {
    uint32_t args[3] = { 0u, 0u, 0u };
    ipo_syscall(IPO_SYSCALL_KEYMAP_SET, 3u, args);
}

uint32_t dynamic_keymap_get_entries_count(uint32_t slot_idx) {
    uint32_t args[2] = { 4u, slot_idx };
    return ipo_syscall(IPO_SYSCALL_KEYMAP_GET, 2u, args);
}

uint32_t dynamic_keymap_copy_entries(uint32_t slot_idx, keymap_entry_t *out_entries, uint32_t max_count) {
    if (!out_entries || max_count == 0) return 0;
    uint32_t args[4] = { 2u, slot_idx, (uint32_t)(uintptr_t)out_entries, max_count };
    return ipo_syscall(IPO_SYSCALL_KEYMAP_GET, 4u, args);
}

