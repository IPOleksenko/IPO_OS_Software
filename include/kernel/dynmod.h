/*
 * Dynamic kernel symbol lookup, patching, and syscall hooking definitions.
 */

#ifndef KERNEL_DYNMOD_H
#define KERNEL_DYNMOD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <syscall.h>

#ifndef _IPO_SYSCALL_HANDLER_T_DEFINED
#define _IPO_SYSCALL_HANDLER_T_DEFINED
typedef uint32_t (*ipo_syscall_handler_t)(uint32_t, uint32_t, uint32_t *);
#endif

#define DYNMOD_AUTH_TOKEN       0x49504F4Du /* "IPOM" */

#define DYNMOD_OK               0
#define DYNMOD_ERR_PARAM       -1
#define DYNMOD_ERR_ADDR        -2
#define DYNMOD_ERR_NOT_FOUND   -3
#define DYNMOD_ERR_PERM        -4
#define DYNMOD_ERR_NOMEM       -5
#define DYNMOD_ERR_CONFLICT    -6

typedef enum {
    DYNMOD_TYPE_NONE         = 0,
    DYNMOD_TYPE_VAR          = 1,
    DYNMOD_TYPE_FUNC_HOOK    = 2,
    DYNMOD_TYPE_CODE_PATCH   = 3,
    DYNMOD_TYPE_SYSCALL_HOOK = 4
} dynmod_type_t;

typedef struct {
    uint32_t id;
    uint32_t type;
    uint32_t owner_pid;
    uint32_t target_addr;
    uint32_t replacement_addr;
    uint32_t size;
    uint8_t  *orig_bytes;
    uint32_t orig_cap;
    uint8_t  *new_bytes;
    uint32_t new_cap;
} ipo_dynmod_info_t;

void     dynmod_init(void);
int      dynmod_authorize(uint32_t pid, uint32_t token);
void     dynmod_revoke(uint32_t pid);
bool     dynmod_is_authorized(uint32_t pid);

int      dynmod_resolve_symbol(const char *name, uint32_t *out_addr);
int      dynmod_read_mem(uintptr_t addr, void *out_buf, size_t size);
int      dynmod_write_var(uintptr_t addr, const void *in_buf, size_t size,
                          uint32_t *out_patch_id, uint32_t caller_pid);
int      dynmod_hook_function(uintptr_t target_fn, uintptr_t replacement_fn,
                              uint32_t *out_patch_id, uint32_t caller_pid);
int      dynmod_patch_code(uintptr_t target_addr, const void *new_bytes, size_t size,
                           uint32_t *out_patch_id, uint32_t caller_pid);
int      dynmod_hook_syscall(uint32_t syscall_num, ipo_syscall_handler_t new_handler,
                             ipo_syscall_handler_t *out_old_handler,
                             uint32_t *out_patch_id, uint32_t caller_pid);
int      dynmod_restore(uint32_t patch_id, uint32_t caller_pid);
uint32_t dynmod_list(ipo_dynmod_info_t *out_arr, uint32_t max_count);

#endif /* KERNEL_DYNMOD_H */
