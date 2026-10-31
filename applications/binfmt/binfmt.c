#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <syscall.h>

/**
 * Dynamic string duplication with malloc.
 */
static char *dup_str(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *res = (char *)malloc(len + 1);
    if (res) {
        memcpy(res, s, len + 1);
    }
    return res;
}

/**
 * Format entry structure for registration.
 */
typedef struct {
    char *name;
    uint8_t *magic;
    uint32_t magic_len;
    uint32_t offset;
    char *runner;
    char *description;
} binfmt_reg_entry_t;

/**
 * Dynamic list of format entries without fixed size limits.
 */
typedef struct {
    binfmt_reg_entry_t *items;
    size_t count;
    size_t capacity;
} binfmt_reg_table_t;

static void binfmt_table_init(binfmt_reg_table_t *table) {
    table->capacity = 4;
    table->count = 0;
    table->items = (binfmt_reg_entry_t *)malloc(table->capacity * sizeof(binfmt_reg_entry_t));
}

static void binfmt_table_add(binfmt_reg_table_t *table,
                             const char *name,
                             const void *magic_bytes,
                             uint32_t magic_len,
                             uint32_t offset,
                             const char *runner,
                             const char *description) {
    if (table->count >= table->capacity) {
        table->capacity *= 2;
        binfmt_reg_entry_t *new_items = (binfmt_reg_entry_t *)realloc(table->items, table->capacity * sizeof(binfmt_reg_entry_t));
        if (!new_items) {
            printf("[binfmt] Error: Out of memory expanding format table!\n");
            return;
        }
        table->items = new_items;
    }

    binfmt_reg_entry_t *entry = &table->items[table->count++];
    entry->name = dup_str(name);
    entry->magic_len = magic_len;
    entry->offset = offset;
    entry->runner = dup_str(runner);
    entry->description = dup_str(description ? description : "");

    entry->magic = (uint8_t *)malloc(magic_len > 0 ? magic_len : 1);
    if (entry->magic && magic_bytes && magic_len > 0) {
        memcpy(entry->magic, magic_bytes, magic_len);
    }
}

static void binfmt_table_free(binfmt_reg_table_t *table) {
    if (!table || !table->items) return;
    for (size_t i = 0; i < table->count; i++) {
        if (table->items[i].name) free(table->items[i].name);
        if (table->items[i].magic) free(table->items[i].magic);
        if (table->items[i].runner) free(table->items[i].runner);
        if (table->items[i].description) free(table->items[i].description);
    }
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->capacity = 0;
}

/**
 * Parses a string containing optional hex escape sequences like \x7f into raw bytes.
 * Dynamically expands output buffer using malloc/realloc without any static limits.
 */
static uint8_t *parse_magic_string(const char *input, uint32_t *out_len) {
    if (!input || !out_len) return NULL;

    size_t cap = 16;
    size_t len = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) return NULL;

    const char *p = input;
    while (*p != '\0') {
        uint8_t byte = 0;
        if (p[0] == '\\' && p[1] == 'x' && isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3])) {
            char hex[3] = { p[2], p[3], '\0' };
            byte = (uint8_t)strtoul(hex, NULL, 16);
            p += 4;
        } else if (p[0] == '\\' && p[1] == 'n') {
            byte = '\n';
            p += 2;
        } else if (p[0] == '\\' && p[1] == 'r') {
            byte = '\r';
            p += 2;
        } else if (p[0] == '\\' && p[1] == 't') {
            byte = '\t';
            p += 2;
        } else if (p[0] == '\\' && p[1] == '0') {
            byte = '\0';
            p += 2;
        } else if (p[0] == '\\' && p[1] == '\\') {
            byte = '\\';
            p += 2;
        } else {
            byte = (uint8_t)*p;
            p++;
        }

        if (len >= cap) {
            cap *= 2;
            uint8_t *nb = (uint8_t *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
        }
        buf[len++] = byte;
    }

    *out_len = (uint32_t)len;
    return buf;
}

/**
 * Populates table with all standard formats required to launch all existing
 * applications and scripts in the IPO_OS ecosystem.
 */
static void populate_ecosystem_formats(binfmt_reg_table_t *table) {
    /* 1. 32-bit ELF Executable (\x7fELF) */
    const uint8_t elf_magic[4] = { 0x7F, 'E', 'L', 'F' };
    binfmt_table_add(table, "ELF32", elf_magic, 4, 0,
                     "/applications/elf_loader",
                     "32-bit ELF Executable (\\x7fELF)");

    /* 2. Lua precompiled bytecode (\x1bLua) */
    const uint8_t lua_bytecode_magic[4] = { 0x1B, 'L', 'u', 'a' };
    binfmt_table_add(table, "LuaBytecode", lua_bytecode_magic, 4, 0,
                     "/applications/lua",
                     "Lua Precompiled Bytecode");

    /* 2. Python script shebangs and tags */
    binfmt_table_add(table, "PythonHashbang", "#!/applications/python", 22, 0,
                     "/applications/python",
                     "Python Script (#!/applications/python)");

    binfmt_table_add(table, "PythonHashbangBin", "#!/bin/python", 13, 0,
                     "/applications/python",
                     "Python Script (#!/bin/python)");

    binfmt_table_add(table, "PythonHashbangUsr", "#!/usr/bin/python", 17, 0,
                     "/applications/python",
                     "Python Script (#!/usr/bin/python)");

    binfmt_table_add(table, "PythonTag", "#python", 7, 0,
                     "/applications/python",
                     "Python Script Header Tag (#python)");

    /* 4. Lua script shebangs and tags */
    binfmt_table_add(table, "LuaHashbang", "#!/applications/lua", 19, 0,
                     "/applications/lua",
                     "Lua Script (#!/applications/lua)");

    binfmt_table_add(table, "LuaHashbangBin", "#!/bin/lua", 10, 0,
                     "/applications/lua",
                     "Lua Script (#!/bin/lua)");

    binfmt_table_add(table, "LuaTag", "#lua", 4, 0,
                     "/applications/lua",
                     "Lua Script Header Tag (#lua)");

    /* 5. TinyCC in-OS C script execution (//tcc, /*tcc, #!/applications/tcc) */
    binfmt_table_add(table, "TccHashbang", "#!/applications/tcc", 19, 0,
                     "/applications/tcc -run",
                     "TinyCC Direct C Script (#!/applications/tcc)");

    binfmt_table_add(table, "TccComment1", "//tcc", 5, 0,
                     "/applications/tcc -run",
                     "TinyCC Direct C Script Header (//tcc)");

    binfmt_table_add(table, "TccComment2", "/*tcc", 5, 0,
                     "/applications/tcc -run",
                     "TinyCC Direct C Script Header (/*tcc)");

    /* 6. Shell scripts */
    binfmt_table_add(table, "ShHashbang", "#!/bin/sh", 9, 0,
                     "/applications/sh",
                     "POSIX Shell Script (#!/bin/sh)");

    binfmt_table_add(table, "ShHashbangApps", "#!/applications/sh", 18, 0,
                     "/applications/sh",
                     "Shell Script (#!/applications/sh)");

    binfmt_table_add(table, "ShTag", "#sh", 3, 0,
                     "/applications/sh",
                     "Shell Script Header Tag (#sh)");
}

/**
 * Registers all ecosystem formats into the kernel.
 */
static int cmd_init(bool verbose) {
    binfmt_reg_table_t table;
    binfmt_table_init(&table);
    populate_ecosystem_formats(&table);

    if (verbose) {
        printf("=================================================================\n");
        printf(" IPO_OS Dynamic Binary Format Registry\n");
        printf(" Registering formats for all ecosystem applications and scripts...\n");
        printf("=================================================================\n");
    }

    int registered = 0;
    int errors = 0;

    for (size_t i = 0; i < table.count; i++) {
        binfmt_reg_entry_t *e = &table.items[i];
        int res = ipo_binfmt_register(e->name, e->magic, e->magic_len, e->offset, e->runner);
        if (res == 0) {
            registered++;
            if (verbose) {
                printf("  [+] %-18s -> %-24s (%s)\n", e->name, e->runner, e->description);
            }
        } else {
            errors++;
            printf("  [-] %-18s FAILED (error %d)\n", e->name, res);
        }
    }

    if (verbose) {
        printf("-----------------------------------------------------------------\n");
        printf("Result: %d format(s) registered successfully (%d errors).\n", registered, errors);
        printf("Kernel default loader remains strictly bare-metal (0x800000).\n");
        printf("Non-bare-metal files are matched dynamically by header bytes.\n");
        printf("=================================================================\n");
    }

    binfmt_table_free(&table);
    return (errors == 0) ? 0 : 1;
}

/**
 * Lists all currently registered formats from the kernel.
 */
static int cmd_list(void) {
    int res = ipo_binfmt_list();
    if (res != 0) {
        printf("[binfmt] Failed to list formats (error %d)\n", res);
        return 1;
    }
    return 0;
}

/**
 * Registers a new format specified via CLI arguments.
 */
static int cmd_add(int argc, char **argv) {
    if (argc < 6) {
        printf("Usage: binfmt add <name> <magic_string> <offset> <runner_command>\n");
        printf("Example: binfmt add WebAssembly \"\\x00asm\" 0 \"/applications/wasm_runner\"\n");
        return 1;
    }

    const char *name = argv[2];
    const char *magic_str = argv[3];
    uint32_t offset = (uint32_t)strtoul(argv[4], NULL, 0);
    const char *runner = argv[5];

    uint32_t magic_len = 0;
    uint8_t *magic = parse_magic_string(magic_str, &magic_len);
    if (!magic || magic_len == 0) {
        printf("[binfmt] Error: Invalid magic string '%s'\n", magic_str);
        if (magic) free(magic);
        return 1;
    }

    int res = ipo_binfmt_register(name, magic, magic_len, offset, runner);
    if (res == 0) {
        printf("[binfmt] Successfully registered format '%s':\n", name);
        printf("         Offset: %u, Length: %u\n", offset, magic_len);
        printf("         Runner: %s\n", runner);
    } else {
        printf("[binfmt] Failed to register format '%s' (error %d)\n", name, res);
    }

    free(magic);
    return (res == 0) ? 0 : 1;
}

/**
 * Unregisters a format by name.
 */
static int cmd_remove(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: binfmt remove <name>\n");
        return 1;
    }

    const char *name = argv[2];
    int res = ipo_binfmt_unregister(name);
    if (res == 0) {
        printf("[binfmt] Successfully unregistered format '%s'\n", name);
    } else {
        printf("[binfmt] Failed to unregister format '%s' (error %d)\n", name, res);
    }
    return (res == 0) ? 0 : 1;
}

/**
 * Prints help message.
 */
static void print_help(const char *prog_name) {
    printf("IPO_OS Executable Format Manager (binfmt)\n\n");
    printf("Usage:\n");
    printf("  %s                           Register all standard ecosystem formats\n", prog_name);
    printf("  %s init                      Register all standard ecosystem formats\n", prog_name);
    printf("  %s list                      List all currently registered formats\n", prog_name);
    printf("  %s add <name> <magic> <off> <runner>  Register custom binary format\n", prog_name);
    printf("  %s remove <name>             Unregister format by name\n", prog_name);
    printf("  %s help                      Show this help message\n\n", prog_name);
    printf("Magic format supports hex escape sequences, e.g. \"\\x7fELF\" or \"\\x1bLua\".\n");
    printf("Matching is strictly content-based by header bytes, independent of file names.\n");
}

int main(int argc, char **argv) {
    if (argc <= 1) {
        return cmd_init(true);
    }

    const char *cmd = argv[1];
    if (strcmp(cmd, "init") == 0) {
        return cmd_init(true);
    } else if (strcmp(cmd, "list") == 0 || strcmp(cmd, "ls") == 0) {
        return cmd_list();
    } else if (strcmp(cmd, "add") == 0) {
        return cmd_add(argc, argv);
    } else if (strcmp(cmd, "remove") == 0 || strcmp(cmd, "del") == 0 || strcmp(cmd, "rm") == 0) {
        return cmd_remove(argc, argv);
    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_help(argv[0]);
        return 0;
    } else {
        printf("Unknown command: %s\n", cmd);
        print_help(argv[0]);
        return 1;
    }
}
