#include <stdint.h>

#define IPO_SYSCALL_PRINT         0x1001u
#define IPO_SYSCALL_KMOD_LOAD_MEM 0x11A3u

extern const char kmd_driver_name[];
extern const uint8_t kmd_payload_start[];
extern const uint8_t kmd_payload_end[];

static uint32_t raw_syscall(uint32_t num, uint32_t argc, uint32_t *argv) {
    uint32_t ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(argc), "c"(argv)
        : "memory"
    );
    return ret;
}

static void raw_print(const char *s) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)s;
    (void)raw_syscall(IPO_SYSCALL_PRINT, 1u, args);
}

int driver_app_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    uint32_t payload_size = (uint32_t)(uintptr_t)(kmd_payload_end - kmd_payload_start);
    uint32_t mod_id = 0u;
    uint32_t args[4];
    args[0] = (uint32_t)(uintptr_t)kmd_driver_name;
    args[1] = (uint32_t)(uintptr_t)kmd_payload_start;
    args[2] = payload_size;
    args[3] = (uint32_t)(uintptr_t)&mod_id;

    int rc = (int)raw_syscall(IPO_SYSCALL_KMOD_LOAD_MEM, 4u, args);
    if (rc == 0) {
        return 0;
    }
    if (rc == -5) {
        raw_print("[driver] ");
        raw_print(kmd_driver_name);
        raw_print(": already registered.\n");
        return 0;
    }
    raw_print("[driver] ");
    raw_print(kmd_driver_name);
    raw_print(": failed to register driver via syscall.\n");
    return 1;
}
