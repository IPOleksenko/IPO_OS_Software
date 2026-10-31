#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <elf.h>
#include <file_system/ipo_fs.h>

#define SYS_WRITE             0x1002u
#define SYS_FS_OPEN           0x1011u
#define SYS_FS_READ           0x1012u
#define SYS_FS_STAT           0x1015u
#define SYS_FS_CLOSE          0x1018u
#define SYS_SBRK              0x1045u
#define SYS_SET_RETURN_VALUE  0x1029u

static inline int raw_syscall(uint32_t num, uint32_t argc, uint32_t *argv) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(argc), "c"(argv)
        : "memory"
    );
    return ret;
}

static void mem_set(void *dst, uint8_t val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) {
        d[i] = val;
    }
}

static void mem_copy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static size_t str_len(const char *s) {
    if (!s) return 0;
    size_t len = 0;
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

static inline int sys_open(const char *path) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)path;
    return raw_syscall(SYS_FS_OPEN, 1u, args);
}

static inline int sys_close(int fd) {
    uint32_t args[1];
    args[0] = (uint32_t)fd;
    return raw_syscall(SYS_FS_CLOSE, 1u, args);
}

static inline int sys_read(int fd, void *buf, uint32_t count, uint32_t offset) {
    uint32_t args[4];
    args[0] = (uint32_t)fd;
    args[1] = (uint32_t)(uintptr_t)buf;
    args[2] = count;
    args[3] = offset;
    return raw_syscall(SYS_FS_READ, 4u, args);
}

static inline int sys_stat(const char *path, struct ipo_inode *st) {
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)path;
    args[1] = (uint32_t)(uintptr_t)st;
    return raw_syscall(SYS_FS_STAT, 2u, args);
}

static inline void *sys_sbrk(int32_t inc) {
    uint32_t args[1];
    args[0] = (uint32_t)inc;
    int res = raw_syscall(SYS_SBRK, 1u, args);
    return (void *)(intptr_t)res;
}

static inline void *heap_alloc(size_t size) {
    size_t aligned = (size + 15u) & ~15u;
    if (aligned == 0) aligned = 16;
    void *p = sys_sbrk((int32_t)aligned);
    if (p == (void *)-1 || p == NULL) return NULL;
    return p;
}

static bool is_entry_struct_return(const uint8_t *code, size_t max_len) {
    if (!code || max_len < 5) return false;

    for (int step = 0; step < 5; step++) {
        if (max_len >= 5 && code[0] == 0xE9) {
            int32_t rel = *(const int32_t *)(code + 1);
            int32_t target_offset = 5 + rel;
            if (target_offset >= 0 && (size_t)target_offset < max_len) {
                code += target_offset;
                max_len -= (size_t)target_offset;
            } else {
                break;
            }
        } else if (max_len >= 2 && code[0] == 0xEB) {
            int8_t rel = (int8_t)code[1];
            int32_t target_offset = 2 + (int32_t)rel;
            if (target_offset >= 0 && (size_t)target_offset < max_len) {
                code += target_offset;
                max_len -= (size_t)target_offset;
            } else {
                break;
            }
        } else {
            break;
        }
    }

    size_t scan_limit = max_len < 512 ? max_len : 512;
    for (size_t i = 0; i + 2 < scan_limit; i++) {
        if (code[i] == 0xC2 && code[i + 1] == 0x04 && code[i + 2] == 0x00) {
            return true;
        }
        if (code[i] == 0xC3) {
            if (i + 4 < scan_limit && code[i + 1] == 0x55 && code[i + 2] == 0x89 && code[i + 3] == 0xE5) {
                return false;
            }
        }
    }
    return false;
}

extern void call_struct_entry(uint32_t entry_addr, void *ret_buf, int argc, char **argv, char **envp);

uint64_t loader_main(int argc, char **argv, char **envp) {
    if (argc < 2 || !argv || !argv[1]) {
        return 1;
    }

    const char *target_path = argv[1];

    struct ipo_inode st;
    if (sys_stat(target_path, &st) != 0 || st.size < sizeof(Elf32_Ehdr)) {
        return 127;
    }

    uint32_t file_size = (uint32_t)st.size;

    int fd = sys_open(target_path);
    if (fd < 0) {
        return 127;
    }

    Elf32_Ehdr ehdr;
    if (sys_read(fd, &ehdr, sizeof(Elf32_Ehdr), 0) != (int)sizeof(Elf32_Ehdr)) {
        sys_close(fd);
        return 126;
    }

    if (ehdr.e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr.e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr.e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr.e_ident[EI_MAG3] != ELFMAG3 ||
        ehdr.e_ident[EI_CLASS] != ELFCLASS32 ||
        ehdr.e_machine != EM_386 ||
        ehdr.e_type != ET_EXEC ||
        ehdr.e_phoff == 0 ||
        ehdr.e_phnum == 0) {
        sys_close(fd);
        return 126;
    }

    uint32_t ph_bytes = (uint32_t)ehdr.e_phnum * sizeof(Elf32_Phdr);
    if (ehdr.e_phoff + ph_bytes > file_size) {
        sys_close(fd);
        return 126;
    }

    /* Read program headers on stack if small, or heap */
    Elf32_Phdr phdrs_stack[16];
    Elf32_Phdr *phdrs = phdrs_stack;
    if (ehdr.e_phnum > 16) {
        phdrs = (Elf32_Phdr *)heap_alloc(ph_bytes);
        if (!phdrs) {
            sys_close(fd);
            return 126;
        }
    }

    if (sys_read(fd, phdrs, ph_bytes, ehdr.e_phoff) != (int)ph_bytes) {
        sys_close(fd);
        return 126;
    }

    uint32_t min_vaddr = 0xFFFFFFFFu;
    uint32_t max_vaddr = 0u;
    int load_count = 0;

    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        load_count++;
        if (phdrs[i].p_vaddr < min_vaddr) {
            min_vaddr = phdrs[i].p_vaddr;
        }
        uint32_t seg_end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
        if (seg_end > max_vaddr) {
            max_vaddr = seg_end;
        }
    }

    if (load_count == 0 || min_vaddr >= max_vaddr) {
        sys_close(fd);
        return 126;
    }

    uint32_t total_span = max_vaddr - min_vaddr;
    uint8_t *staging = (uint8_t *)heap_alloc((size_t)total_span + 64u);
    if (!staging) {
        sys_close(fd);
        return 126;
    }

    mem_set(staging, 0, (size_t)total_span);

    /* Load PT_LOAD segments into staging */
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        if (phdrs[i].p_filesz > 0) {
            if (phdrs[i].p_offset + phdrs[i].p_filesz > file_size) {
                sys_close(fd);
                return 126;
            }
            uint32_t seg_offset = phdrs[i].p_vaddr - min_vaddr;
            if (sys_read(fd, staging + seg_offset, phdrs[i].p_filesz, phdrs[i].p_offset) != (int)phdrs[i].p_filesz) {
                sys_close(fd);
                return 126;
            }
        }
    }

    /* Apply SHT_REL relocations using temporary heap space that is reclaimed afterwards */
    bool has_relocs = false;
    uint32_t delta = (uint32_t)(uintptr_t)staging - min_vaddr;
    if (delta != 0 && ehdr.e_shoff != 0 && ehdr.e_shnum != 0) {
        uint32_t sh_bytes = (uint32_t)ehdr.e_shnum * sizeof(Elf32_Shdr);
        if (ehdr.e_shoff + sh_bytes <= file_size) {
            void *mark_before_reloc = sys_sbrk(0);
            Elf32_Shdr *shdrs = (Elf32_Shdr *)heap_alloc(sh_bytes);
            if (shdrs && sys_read(fd, shdrs, sh_bytes, ehdr.e_shoff) == (int)sh_bytes) {
                for (uint16_t s = 0; s < ehdr.e_shnum; s++) {
                    if (shdrs[s].sh_type != SHT_REL) continue;
                    has_relocs = true;
                    uint32_t target_sec = shdrs[s].sh_info;
                    if (target_sec >= ehdr.e_shnum || !(shdrs[target_sec].sh_flags & SHF_ALLOC)) {
                        continue;
                    }
                    uint32_t target_vaddr = shdrs[target_sec].sh_addr;
                    uint32_t rel_bytes = shdrs[s].sh_size;
                    uint32_t num_relocs = rel_bytes / sizeof(Elf32_Rel);
                    if (num_relocs == 0 || shdrs[s].sh_offset + rel_bytes > file_size) {
                        continue;
                    }

                    /* Load linked symbol table if valid */
                    Elf32_Sym *symtab = NULL;
                    uint32_t num_syms = 0;
                    uint32_t sym_sec = shdrs[s].sh_link;
                    if (sym_sec < ehdr.e_shnum && shdrs[sym_sec].sh_type == SHT_SYMTAB &&
                        shdrs[sym_sec].sh_size > 0 &&
                        shdrs[sym_sec].sh_offset + shdrs[sym_sec].sh_size <= file_size) {
                        symtab = (Elf32_Sym *)heap_alloc(shdrs[sym_sec].sh_size);
                        if (symtab && sys_read(fd, symtab, shdrs[sym_sec].sh_size, shdrs[sym_sec].sh_offset) == (int)shdrs[sym_sec].sh_size) {
                            num_syms = shdrs[sym_sec].sh_size / sizeof(Elf32_Sym);
                        } else {
                            symtab = NULL;
                        }
                    }

                    Elf32_Rel *relocs = (Elf32_Rel *)heap_alloc(rel_bytes);
                    if (relocs && sys_read(fd, relocs, rel_bytes, shdrs[s].sh_offset) == (int)rel_bytes) {
                        for (uint32_t r = 0; r < num_relocs; r++) {
                            uint8_t type = (uint8_t)ELF32_R_TYPE(relocs[r].r_info);
                            uint32_t sym_idx = ELF32_R_SYM(relocs[r].r_info);

                            if (symtab && sym_idx > 0 && sym_idx < num_syms) {
                                uint16_t shndx = symtab[sym_idx].st_shndx;
                                if (shndx == SHN_UNDEF || shndx == SHN_ABS) {
                                    continue;
                                }
                            }

                            if (type == R_386_32 || type == R_386_RELATIVE) {
                                uint32_t r_off = relocs[r].r_offset;
                                uint32_t patch_offset;
                                if (r_off >= min_vaddr && r_off < max_vaddr) {
                                    patch_offset = r_off - min_vaddr;
                                } else if (target_vaddr >= min_vaddr && (target_vaddr - min_vaddr + r_off) < total_span) {
                                    patch_offset = (target_vaddr - min_vaddr) + r_off;
                                } else {
                                    continue;
                                }
                                if (patch_offset + 4 <= total_span) {
                                    uint32_t orig_val = *(uint32_t *)(staging + patch_offset);
                                    if (orig_val != 0 && orig_val >= (min_vaddr - 0x10000u) && orig_val <= (max_vaddr + 0x10000u)) {
                                        *(uint32_t *)(staging + patch_offset) = orig_val + delta;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            /* Reclaim temporary relocation tables so target app starts with clean heap */
            void *mark_after_reloc = sys_sbrk(0);
            if (mark_before_reloc != (void *)-1 && mark_after_reloc != (void *)-1 &&
                (uintptr_t)mark_after_reloc > (uintptr_t)mark_before_reloc) {
                int32_t diff = -(int32_t)((uintptr_t)mark_after_reloc - (uintptr_t)mark_before_reloc);
                sys_sbrk(diff);
            }
        }
    }

    sys_close(fd);

    /* Check for embedded return format descriptor (IPOR magic: 0x524F5049 / "IPOR") */
    const char *detected_format = NULL;
    if (total_span >= 8) {
        uint32_t limit = total_span - 8;
        for (uint32_t i = 0; i <= limit; i++) {
            if (staging[i] == 0x49 && staging[i + 1] == 0x50 && staging[i + 2] == 0x4F && staging[i + 3] == 0x52) {
                uint32_t str_ptr = *(const uint32_t *)(staging + i + 4);
                const char *cand = NULL;
                if (str_ptr >= min_vaddr && str_ptr < min_vaddr + total_span) {
                    cand = (const char *)(staging + (str_ptr - min_vaddr));
                } else if (str_ptr >= (uint32_t)(uintptr_t)staging && str_ptr < (uint32_t)(uintptr_t)staging + total_span) {
                    cand = (const char *)(uintptr_t)str_ptr;
                }
                if (cand && cand[0] != '\0') {
                    detected_format = cand;
                    break;
                }
            }
        }
    }

    /* Save detected format string to stack before staging might be moved */
    char detected_format_buf[64];
    detected_format_buf[0] = '\0';
    if (detected_format) {
        size_t dlen = str_len(detected_format);
        if (dlen >= sizeof(detected_format_buf)) dlen = sizeof(detected_format_buf) - 1;
        mem_copy(detected_format_buf, detected_format, dlen);
        detected_format_buf[dlen] = '\0';
        detected_format = detected_format_buf;
    }

    /* Construct target argc/argv array in heap */
    int new_argc = argc - 1;
    char **new_argv = (char **)heap_alloc((size_t)(new_argc + 1) * sizeof(char *));
    if (!new_argv) {
        return 126;
    }
    for (int i = 0; i < new_argc; i++) {
        new_argv[i] = argv[i + 1];
    }
    new_argv[new_argc] = NULL;

    char *null_env[1];
    null_env[0] = NULL;
    char **pass_env = envp ? envp : null_env;

    uint32_t target_entry = ehdr.e_entry;
    uint32_t entry_offset = (target_entry >= min_vaddr && target_entry < max_vaddr) ? (target_entry - min_vaddr) : 0;
    bool is_struct = is_entry_struct_return(staging + entry_offset, (size_t)(total_span - entry_offset));
    bool copy_to_base = (!has_relocs && min_vaddr == 0x00800000u);

    if (copy_to_base) {
        if (detected_format && detected_format[0] != '\0') {
            uint32_t fmt_args[2];
            fmt_args[0] = 99u; /* Register embedded return format for process */
            fmt_args[1] = (uint32_t)(uintptr_t)detected_format;
            raw_syscall(SYS_SET_RETURN_VALUE, 2u, fmt_args);
        }

        uint32_t *sp = (uint32_t *)__builtin_frame_address(0);
        uint32_t kernel_ebp = sp[0];
        sp[2] = (uint32_t)new_argc;
        sp[3] = (uint32_t)(uintptr_t)new_argv;
        sp[4] = (uint32_t)(uintptr_t)pass_env;
        uint32_t final_esp = (uint32_t)(uintptr_t)(sp + 1);

        static const uint8_t tramp_perfect_bytes[30] = {
            0x55,                         /* push %ebp */
            0x89, 0xe5,                   /* mov %esp, %ebp */
            0x56,                         /* push %esi */
            0x57,                         /* push %edi */
            0x8b, 0x75, 0x08,             /* mov 8(%ebp), %esi (src) */
            0x8b, 0x7d, 0x0c,             /* mov 12(%ebp), %edi (dst) */
            0x8b, 0x4d, 0x10,             /* mov 16(%ebp), %ecx (len) */
            0xfc,                         /* cld */
            0xf3, 0xa4,                   /* rep movsb */
            0x8b, 0x55, 0x14,             /* mov 20(%ebp), %edx (entry) */
            0x8b, 0x45, 0x1c,             /* mov 28(%ebp), %eax (kernel_ebp) */
            0x8b, 0x65, 0x18,             /* mov 24(%ebp), %esp (final_esp) */
            0x89, 0xc5,                   /* mov %eax, %ebp (kernel_ebp) */
            0xff, 0xe2                    /* jmp *%edx */
        };
        uint8_t tramp_buf[32];
        mem_copy(tramp_buf, tramp_perfect_bytes, sizeof(tramp_perfect_bytes));
        typedef void (*tramp_perfect_fn)(const void *src, void *dst, uint32_t len, uint32_t entry, uint32_t final_esp, uint32_t kernel_ebp) __attribute__((noreturn));
        tramp_perfect_fn fn = (tramp_perfect_fn)(uintptr_t)tramp_buf;
        fn(staging, (void *)min_vaddr, total_span, target_entry, final_esp, kernel_ebp);
    }

    uint32_t entry_addr = (uint32_t)(uintptr_t)staging + entry_offset;

    if (is_struct) {
        uint8_t *struct_buf = (uint8_t *)heap_alloc(512);
        if (!struct_buf) {
            return 126;
        }
        mem_set(struct_buf, 0xDD, 512);

        call_struct_entry(entry_addr, struct_buf, new_argc, new_argv, pass_env);

        uint32_t written_size = 0;
        for (int k = 511; k >= 0; k--) {
            if (struct_buf[k] != 0xDD) {
                written_size = (uint32_t)k + 1u;
                break;
            }
        }

        char default_fmt[7];
        default_fmt[0] = 's';
        default_fmt[1] = 't';
        default_fmt[2] = 'r';
        default_fmt[3] = 'u';
        default_fmt[4] = 'c';
        default_fmt[5] = 't';
        default_fmt[6] = '\0';

        uint32_t syscall_args[4];
        syscall_args[0] = 3u; /* PROC_RET_STRUCT */
        syscall_args[1] = (uint32_t)(uintptr_t)struct_buf;
        syscall_args[2] = written_size;
        syscall_args[3] = (uint32_t)(uintptr_t)(detected_format ? detected_format : default_fmt);
        raw_syscall(SYS_SET_RETURN_VALUE, 4u, syscall_args);
        return 0;
    } else {
        if (detected_format) {
            uint32_t fmt_args[2];
            fmt_args[0] = 99u; /* Register embedded return format for process */
            fmt_args[1] = (uint32_t)(uintptr_t)detected_format;
            raw_syscall(SYS_SET_RETURN_VALUE, 2u, fmt_args);
        }
        uint32_t ret_eax = 0;
        uint32_t ret_edx = 0;
        __asm__ volatile(
            "pushl %3\n\t"
            "pushl %4\n\t"
            "pushl %5\n\t"
            "call *%2\n\t"
            "addl $12, %%esp\n\t"
            : "=a"(ret_eax), "=d"(ret_edx)
            : "r"(entry_addr), "r"(pass_env), "r"(new_argv), "r"(new_argc)
            : "ecx", "memory", "cc"
        );
        return ((uint64_t)ret_edx << 32) | (uint64_t)ret_eax;
    }
}
