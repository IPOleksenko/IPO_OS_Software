#include <dlfcn.h>
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include <syscall.h>
#include <stdarg.h>
#include "ipo_libc_internal.h"

static pthread_mutex_t s_dl_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

static void set_dlerror(const char *msg) {
    ipo_libc_tcb_t *tcb = ipo_libc_get_tcb();
    if (!msg) {
        if (tcb->dlerror_buf) tcb->dlerror_buf[0] = '\0';
        tcb->has_dlerror = 0;
        return;
    }
    size_t len = strlen(msg);
    char *nb = (char *)realloc(tcb->dlerror_buf, len + 1);
    if (!nb) return;
    tcb->dlerror_buf = nb;
    memcpy(tcb->dlerror_buf, msg, len + 1);
    tcb->has_dlerror = 1;
}

static char *dl_alloc_vprintf(const char *fmt, va_list ap) {
    va_list aq;
    va_copy(aq, ap);
    int len = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);
    if (len < 0) return NULL;
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) return NULL;
    vsnprintf(buf, (size_t)len + 1, fmt, ap);
    return buf;
}

static char *dl_alloc_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *buf = dl_alloc_vprintf(fmt, ap);
    va_end(ap);
    return buf;
}

static void set_dlerror_fmt(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *msg = dl_alloc_vprintf(fmt, ap);
    va_end(ap);
    if (!msg) return;
    set_dlerror(msg);
    free(msg);
}

/* User registered symbols list (dlregister_symbol) */
typedef struct registered_sym {
    char *name;
    void *addr;
    struct registered_sym *next;
} registered_sym_t;

static registered_sym_t *s_custom_syms = NULL;

/* Loaded Dynamic Modules descriptor */
typedef struct dl_module {
    char *path;
    uint16_t elf_type; /* ET_REL or ET_DYN */
    int ref_count;
    int flags;
    void *image_base;
    uint32_t image_size;
    uint32_t min_vaddr;

    /* For ET_REL sections */
    void **section_addrs;
    uint32_t *section_sizes;
    uint32_t num_sections;

    /* For ET_REL COMMON symbols */
    uint32_t *common_addrs;

    /* Exported symbols and strings for runtime dlsym() */
    Elf32_Sym *symtab;
    uint32_t num_syms;
    char *strtab;
    uint32_t strtab_size;

    /* Constructors and Destructors */
    void (*init_fn)(void);
    void (*fini_fn)(void);
    void (**init_array)(void);
    size_t init_array_count;
    void (**fini_array)(void);
    size_t fini_array_count;

    struct dl_module *next;
} dl_module_t;

static int dl_range_ok(uint32_t off, uint32_t len, uint32_t total) {
    return off <= total && len <= (total - off);
}

typedef void (*dl_ctor_fn_t)(void);

static bool dl_append_ctor(dl_ctor_fn_t **arr, size_t *count, size_t *cap, uintptr_t raw_addr) {
    if (raw_addr == 0u || raw_addr == (uintptr_t)-1) return true;
    if (*count == *cap) {
        size_t ncap = (*cap > 0u) ? (*cap * 2u) : 8u;
        dl_ctor_fn_t *narr = (dl_ctor_fn_t *)realloc(*arr, ncap * sizeof(dl_ctor_fn_t));
        if (!narr) return false;
        *arr = narr;
        *cap = ncap;
    }
    (*arr)[(*count)++] = (dl_ctor_fn_t)raw_addr;
    return true;
}

static void dl_collect_sec_array(const dl_module_t *mod, const Elf32_Ehdr *eh,
                                 const Elf32_Shdr *shdrs, uint32_t target_type,
                                 dl_ctor_fn_t **arr, size_t *count, size_t *cap) {
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (shdrs[i].sh_type != target_type || shdrs[i].sh_size < sizeof(uintptr_t)) {
            continue;
        }
        const uintptr_t *entries = NULL;
        if (mod->elf_type == ET_REL) {
            if (i < mod->num_sections && mod->section_addrs && mod->section_addrs[i]) {
                entries = (const uintptr_t *)mod->section_addrs[i];
            }
        } else if (mod->elf_type == ET_DYN) {
            if (shdrs[i].sh_addr >= mod->min_vaddr) {
                uint32_t off = shdrs[i].sh_addr - mod->min_vaddr;
                if (mod->image_base && off <= mod->image_size &&
                    shdrs[i].sh_size <= mod->image_size - off) {
                    entries = (const uintptr_t *)((const uint8_t *)mod->image_base + off);
                }
            }
        }
        if (!entries) continue;
        size_t n = (size_t)(shdrs[i].sh_size / sizeof(uintptr_t));
        for (size_t k = 0; k < n; k++) {
            (void)dl_append_ctor(arr, count, cap, entries[k]);
        }
    }
}

static void dl_collect_init_fini_arrays(dl_module_t *mod, const uint8_t *file_buf, size_t file_size) {
    if (!mod || !file_buf || file_size < sizeof(Elf32_Ehdr)) return;
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)file_buf;
    if (eh->e_shoff == 0u || eh->e_shnum == 0u || eh->e_shentsize < sizeof(Elf32_Shdr)) return;
    if (eh->e_shoff > file_size) return;
    uint32_t sh_bytes = (uint32_t)eh->e_shnum * (uint32_t)eh->e_shentsize;
    if (sh_bytes / (uint32_t)eh->e_shentsize != (uint32_t)eh->e_shnum) return;
    if (sh_bytes > file_size - eh->e_shoff) return;
    const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(file_buf + eh->e_shoff);

    size_t icap = 0u, fcap = 0u;
    dl_collect_sec_array(mod, eh, shdrs, SHT_PREINIT_ARRAY, &mod->init_array, &mod->init_array_count, &icap);
    dl_collect_sec_array(mod, eh, shdrs, SHT_INIT_ARRAY, &mod->init_array, &mod->init_array_count, &icap);
    dl_collect_sec_array(mod, eh, shdrs, SHT_FINI_ARRAY, &mod->fini_array, &mod->fini_array_count, &fcap);
}

static void dl_free_module(dl_module_t *mod) {
    if (!mod) return;
    if (mod->path) free(mod->path);
    if (mod->image_base) free(mod->image_base);
    if (mod->section_addrs) free(mod->section_addrs);
    if (mod->section_sizes) free(mod->section_sizes);
    if (mod->common_addrs) free(mod->common_addrs);
    if (mod->symtab) free(mod->symtab);
    if (mod->strtab) free(mod->strtab);
    if (mod->init_array) free(mod->init_array);
    if (mod->fini_array) free(mod->fini_array);
    free(mod);
}

static dl_module_t *s_modules = NULL;

/* Built-in standard symbol table */
typedef struct {
    const char *name;
    void *addr;
} builtin_sym_t;

#define BUILTIN_SYM(fn) { #fn, (void *)&fn }

extern uintptr_t __stack_chk_guard;
extern void __stack_chk_fail(void);

static const builtin_sym_t s_builtin_syms[] = {
    /* stdio */
    BUILTIN_SYM(printf),
    BUILTIN_SYM(fprintf),
    BUILTIN_SYM(sprintf),
    BUILTIN_SYM(snprintf),
    BUILTIN_SYM(vprintf),
    BUILTIN_SYM(vfprintf),
    BUILTIN_SYM(vsprintf),
    BUILTIN_SYM(vsnprintf),
    BUILTIN_SYM(puts),
    BUILTIN_SYM(putchar),
    BUILTIN_SYM(getchar),
    BUILTIN_SYM(fputc),
    BUILTIN_SYM(fputs),
    BUILTIN_SYM(fgetc),
    BUILTIN_SYM(fgets),
    BUILTIN_SYM(fopen),
    BUILTIN_SYM(fclose),
    BUILTIN_SYM(fread),
    BUILTIN_SYM(fwrite),
    BUILTIN_SYM(fseek),
    BUILTIN_SYM(ftell),
    BUILTIN_SYM(rewind),
    BUILTIN_SYM(fflush),
    BUILTIN_SYM(feof),
    BUILTIN_SYM(ferror),
    BUILTIN_SYM(clearerr),
    BUILTIN_SYM(perror),
    BUILTIN_SYM(remove),
    BUILTIN_SYM(rename),
    BUILTIN_SYM(stdin),
    BUILTIN_SYM(stdout),
    BUILTIN_SYM(stderr),

    /* stdlib */
    BUILTIN_SYM(malloc),
    BUILTIN_SYM(free),
    BUILTIN_SYM(calloc),
    BUILTIN_SYM(realloc),
    BUILTIN_SYM(exit),
    BUILTIN_SYM(abort),
    BUILTIN_SYM(atexit),
    BUILTIN_SYM(atoi),
    BUILTIN_SYM(atol),
    BUILTIN_SYM(strtol),
    BUILTIN_SYM(strtoul),
    BUILTIN_SYM(rand),
    BUILTIN_SYM(srand),
    BUILTIN_SYM(qsort),
    BUILTIN_SYM(bsearch),
    BUILTIN_SYM(abs),
    BUILTIN_SYM(getenv),
    BUILTIN_SYM(setenv),

    /* string */
    BUILTIN_SYM(strlen),
    BUILTIN_SYM(strcpy),
    BUILTIN_SYM(strncpy),
    BUILTIN_SYM(strcat),
    BUILTIN_SYM(strncat),
    BUILTIN_SYM(strcmp),
    BUILTIN_SYM(strncmp),
    BUILTIN_SYM(strchr),
    BUILTIN_SYM(strrchr),
    BUILTIN_SYM(strstr),
    BUILTIN_SYM(strdup),
    BUILTIN_SYM(strtok),
    BUILTIN_SYM(strspn),
    BUILTIN_SYM(strcspn),
    BUILTIN_SYM(strpbrk),
    BUILTIN_SYM(memset),
    BUILTIN_SYM(memcpy),
    BUILTIN_SYM(memmove),
    BUILTIN_SYM(memcmp),
    BUILTIN_SYM(memchr),

    /* unistd & sys */
    BUILTIN_SYM(sleep),
    BUILTIN_SYM(usleep),
    BUILTIN_SYM(open),
    BUILTIN_SYM(close),
    BUILTIN_SYM(read),
    BUILTIN_SYM(write),
    BUILTIN_SYM(lseek),
    BUILTIN_SYM(stat),
    BUILTIN_SYM(fstat),
    BUILTIN_SYM(ipo_syscall),

    /* math */
    BUILTIN_SYM(sin),
    BUILTIN_SYM(cos),
    BUILTIN_SYM(tan),
    BUILTIN_SYM(sqrt),
    BUILTIN_SYM(pow),
    BUILTIN_SYM(fabs),
    BUILTIN_SYM(floor),
    BUILTIN_SYM(ceil),
    BUILTIN_SYM(exp),
    BUILTIN_SYM(log),

    /* dlfcn */
    BUILTIN_SYM(dlopen),
    BUILTIN_SYM(dlsym),
    BUILTIN_SYM(dlclose),
    BUILTIN_SYM(dlerror),
    BUILTIN_SYM(dladdr),
    BUILTIN_SYM(dlregister_symbol),

    /* compiler runtime & extended POSIX */
    BUILTIN_SYM(__stack_chk_guard),
    BUILTIN_SYM(__stack_chk_fail),
    BUILTIN_SYM(__errno_location),
    BUILTIN_SYM(strerror),
    BUILTIN_SYM(strerror_r),
    BUILTIN_SYM(posix_memalign),
    BUILTIN_SYM(aligned_alloc),
    BUILTIN_SYM(rand_r),
    BUILTIN_SYM(mmap),
    BUILTIN_SYM(munmap),
    BUILTIN_SYM(mprotect),
    BUILTIN_SYM(clock_gettime),
    BUILTIN_SYM(nanosleep),
    BUILTIN_SYM(pipe),
    BUILTIN_SYM(dup),
    BUILTIN_SYM(dup2),
    BUILTIN_SYM(fcntl),
    BUILTIN_SYM(poll),
    BUILTIN_SYM(select),
    BUILTIN_SYM(pthread_create),
    BUILTIN_SYM(pthread_join),
    BUILTIN_SYM(pthread_detach),
    BUILTIN_SYM(pthread_mutex_lock),
    BUILTIN_SYM(pthread_mutex_unlock),
    BUILTIN_SYM(pthread_cond_wait),
    BUILTIN_SYM(pthread_cond_signal),

    { NULL, NULL }
};

int dlregister_symbol(const char *name, void *addr) {
    if (!name || !*name || !addr) return -1;
    pthread_mutex_lock(&s_dl_lock);
    registered_sym_t *cur = s_custom_syms;
    while (cur) {
        if (strcmp(cur->name, name) == 0) {
            cur->addr = addr;
            pthread_mutex_unlock(&s_dl_lock);
            return 0;
        }
        cur = cur->next;
    }
    registered_sym_t *node = (registered_sym_t *)malloc(sizeof(registered_sym_t));
    if (!node) {
        pthread_mutex_unlock(&s_dl_lock);
        return -1;
    }
    node->name = strdup(name);
    if (!node->name) {
        free(node);
        pthread_mutex_unlock(&s_dl_lock);
        return -1;
    }
    node->addr = addr;
    node->next = s_custom_syms;
    s_custom_syms = node;
    pthread_mutex_unlock(&s_dl_lock);
    return 0;
}

static void *dlsym_in_module(dl_module_t *mod, const char *symbol) {
    if (!mod || !mod->symtab || !mod->strtab || !symbol) return NULL;
    for (uint32_t i = 0; i < mod->num_syms; i++) {
        Elf32_Sym *sym = &mod->symtab[i];
        if (sym->st_shndx == SHN_UNDEF) continue;
        uint8_t bind = ELF32_ST_BIND(sym->st_info);
        if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
        if (sym->st_name >= mod->strtab_size) continue;
        const char *name = mod->strtab + sym->st_name;
        if (strcmp(name, symbol) == 0) {
            if (mod->elf_type == ET_REL) {
                if (sym->st_shndx == SHN_ABS) {
                    return (void *)sym->st_value;
                } else if (sym->st_shndx == SHN_COMMON) {
                    return mod->common_addrs ? (void *)mod->common_addrs[i] : NULL;
                } else if (sym->st_shndx < mod->num_sections && mod->section_addrs[sym->st_shndx]) {
                    return (void *)((uint32_t)mod->section_addrs[sym->st_shndx] + sym->st_value);
                }
            } else if (mod->elf_type == ET_DYN) {
                if (sym->st_shndx == SHN_ABS) {
                    return (void *)sym->st_value;
                }
                uint32_t off = (sym->st_value >= mod->min_vaddr)
                    ? (sym->st_value - mod->min_vaddr)
                    : sym->st_value;
                return (void *)((uint32_t)mod->image_base + off);
            }
        }
    }
    return NULL;
}

static void *dlsym_resolve_after(dl_module_t *skip_through, const char *name) {
    if (!name || !*name) return NULL;

    /* If resolving normally (skip_through == NULL), check host symbols first */
    if (!skip_through) {
        registered_sym_t *cs = s_custom_syms;
        while (cs) {
            if (strcmp(cs->name, name) == 0) {
                return cs->addr;
            }
            cs = cs->next;
        }
        dl_module_t *m = s_modules;
        while (m) {
            if (m->flags & RTLD_GLOBAL) {
                void *res = dlsym_in_module(m, name);
                if (res) return res;
            }
            m = m->next;
        }
    } else {
        dl_module_t *m = skip_through->next;
        while (m) {
            if (m->flags & RTLD_GLOBAL) {
                void *res = dlsym_in_module(m, name);
                if (res) return res;
            }
            m = m->next;
        }
    }

    /* Fall back to built-in libc / system symbols (next after host modules for RTLD_NEXT too) */
    for (size_t i = 0; s_builtin_syms[i].name != NULL; i++) {
        if (strcmp(s_builtin_syms[i].name, name) == 0) {
            return s_builtin_syms[i].addr;
        }
    }
    return NULL;
}

static void *dlsym_resolve_global(const char *name) {
    return dlsym_resolve_after(NULL, name);
}

char *dlerror(void) {
    ipo_libc_tcb_t *tcb = ipo_libc_get_tcb();
    if (!tcb->has_dlerror) return NULL;
    tcb->has_dlerror = 0;
    return tcb->dlerror_buf;
}

static uint8_t *load_file_buffer(const char *path, uint32_t *out_size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz <= 0) {
        fclose(fp);
        return NULL;
    }
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (rd != (size_t)sz) {
        free(buf);
        return NULL;
    }
    if (out_size) *out_size = (uint32_t)sz;
    return buf;
}

void *dlopen(const char *filename, int flags) {
    /* If filename is NULL, return RTLD_DEFAULT */
    if (!filename || !*filename) {
        return RTLD_DEFAULT;
    }

    pthread_mutex_lock(&s_dl_lock);

    /* Check if already loaded by matching path */
    dl_module_t *cur = s_modules;
    while (cur) {
        if (cur->path && strcmp(cur->path, filename) == 0) {
            cur->ref_count++;
            cur->flags |= (flags & RTLD_GLOBAL);
            pthread_mutex_unlock(&s_dl_lock);
            return (void *)cur;
        }
        cur = cur->next;
    }

    if (flags & RTLD_NOLOAD) {
        set_dlerror_fmt("dlopen: module '%s' not loaded", filename);
        pthread_mutex_unlock(&s_dl_lock);
        return NULL;
    }

    /* Locate file: try exact path, then fallback to /applications/ and /lib/ */
    uint32_t file_size = 0;
    uint8_t *file_buf = load_file_buffer(filename, &file_size);
    char *effective_path = strdup(filename);

    if (!file_buf && !strchr(filename, '/')) {
        free(effective_path);
        effective_path = dl_alloc_printf("/applications/%s", filename);
        if (effective_path) {
            file_buf = load_file_buffer(effective_path, &file_size);
        }
        if (!file_buf) {
            free(effective_path);
            effective_path = dl_alloc_printf("/lib/%s", filename);
            if (effective_path) {
                file_buf = load_file_buffer(effective_path, &file_size);
            }
        }
    }

    if (!file_buf) {
        free(effective_path);
        set_dlerror_fmt("dlopen: unable to open file '%s'", filename);
        pthread_mutex_unlock(&s_dl_lock);
        return NULL;
    }

    /* Validate ELF header */
    if (file_size < sizeof(Elf32_Ehdr)) {
        set_dlerror("dlopen: file too small for ELF header");
        free(effective_path);
        free(file_buf);
        pthread_mutex_unlock(&s_dl_lock);
        return NULL;
    }
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)file_buf;
    if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 || eh->e_ident[EI_MAG3] != ELFMAG3) {
        set_dlerror("dlopen: invalid ELF magic header");
        free(effective_path);
        free(file_buf);
        pthread_mutex_unlock(&s_dl_lock);
        return NULL;
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS32 || eh->e_machine != EM_386) {
        set_dlerror("dlopen: unsupported architecture (expected 32-bit ELF)");
        free(effective_path);
        free(file_buf);
        pthread_mutex_unlock(&s_dl_lock);
        return NULL;
    }
    if (eh->e_type != ET_REL && eh->e_type != ET_DYN) {
        set_dlerror("dlopen: ELF type must be ET_REL or ET_DYN");
        free(effective_path);
        free(file_buf);
        pthread_mutex_unlock(&s_dl_lock);
        return NULL;
    }

    dl_module_t *mod = (dl_module_t *)calloc(1, sizeof(dl_module_t));
    if (!mod) {
        set_dlerror("dlopen: out of memory allocating module descriptor");
        free(effective_path);
        free(file_buf);
        pthread_mutex_unlock(&s_dl_lock);
        return NULL;
    }

    mod->path = effective_path ? effective_path : strdup(filename);
    mod->elf_type = eh->e_type;
    mod->ref_count = 1;
    mod->flags = flags;

    if (eh->e_type == ET_REL) {
        /* Relocatable Object File (gcc -c / tcc -c) */
        if (eh->e_shoff == 0 || eh->e_shnum == 0 ||
            eh->e_shentsize < sizeof(Elf32_Shdr) ||
            (uint32_t)eh->e_shnum > UINT32_MAX / eh->e_shentsize ||
            !dl_range_ok(eh->e_shoff, (uint32_t)eh->e_shnum * (uint32_t)eh->e_shentsize, file_size)) {
            set_dlerror("dlopen: invalid section headers in ET_REL file");
            dl_free_module(mod); free(file_buf);
            pthread_mutex_unlock(&s_dl_lock);
            return NULL;
        }

        const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(file_buf + eh->e_shoff);
        mod->num_sections = eh->e_shnum;
        mod->section_addrs = (void **)calloc(eh->e_shnum, sizeof(void *));
        mod->section_sizes = (uint32_t *)calloc(eh->e_shnum, sizeof(uint32_t));
        if (!mod->section_addrs || !mod->section_sizes) {
            set_dlerror("dlopen: out of memory for section tables");
            dl_free_module(mod); free(file_buf);
            pthread_mutex_unlock(&s_dl_lock);
            return NULL;
        }

        /* Find Symbol Table and String Table */
        for (uint16_t i = 0; i < eh->e_shnum; i++) {
            if (shdrs[i].sh_type == SHT_SYMTAB) {
                if (!dl_range_ok(shdrs[i].sh_offset, shdrs[i].sh_size, file_size)) {
                    set_dlerror("dlopen: invalid ET_REL symtab bounds");
                    dl_free_module(mod); free(file_buf);
                    pthread_mutex_unlock(&s_dl_lock);
                    return NULL;
                }
                mod->num_syms = shdrs[i].sh_size / sizeof(Elf32_Sym);
                if (shdrs[i].sh_size > 0u) {
                    mod->symtab = (Elf32_Sym *)malloc(shdrs[i].sh_size);
                    if (!mod->symtab) {
                        set_dlerror("dlopen: out of memory allocating symtab");
                        dl_free_module(mod); free(file_buf);
                        pthread_mutex_unlock(&s_dl_lock);
                        return NULL;
                    }
                    memcpy(mod->symtab, file_buf + shdrs[i].sh_offset, shdrs[i].sh_size);
                }
                uint32_t str_idx = shdrs[i].sh_link;
                if (str_idx < eh->e_shnum &&
                    dl_range_ok(shdrs[str_idx].sh_offset, shdrs[str_idx].sh_size, file_size)) {
                    mod->strtab_size = shdrs[str_idx].sh_size;
                    if (mod->strtab_size > 0u) {
                        mod->strtab = (char *)malloc(shdrs[str_idx].sh_size);
                        if (!mod->strtab) {
                            set_dlerror("dlopen: out of memory allocating strtab");
                            dl_free_module(mod); free(file_buf);
                            pthread_mutex_unlock(&s_dl_lock);
                            return NULL;
                        }
                        memcpy(mod->strtab, file_buf + shdrs[str_idx].sh_offset, shdrs[str_idx].sh_size);
                    }
                }
                break;
            }
        }

        /* Calculate total contiguous memory size for SHF_ALLOC sections */
        uint32_t total_alloc = 0;
        uint32_t *sec_offsets = (uint32_t *)calloc(eh->e_shnum, sizeof(uint32_t));
        if (!sec_offsets) {
            set_dlerror("dlopen: out of memory for section offsets");
            dl_free_module(mod); free(file_buf);
            pthread_mutex_unlock(&s_dl_lock);
            return NULL;
        }
        for (uint16_t i = 0; i < eh->e_shnum; i++) {
            if (shdrs[i].sh_flags & SHF_ALLOC) {
                uint32_t align = shdrs[i].sh_addralign > 0 ? shdrs[i].sh_addralign : 4;
                if (align & (align - 1u)) align = 4u;
                if (total_alloc > UINT32_MAX - align) {
                    free(sec_offsets); dl_free_module(mod); free(file_buf);
                    set_dlerror("dlopen: section alignment overflow");
                    pthread_mutex_unlock(&s_dl_lock);
                    return NULL;
                }
                total_alloc = (total_alloc + align - 1) & ~(align - 1);
                sec_offsets[i] = total_alloc;
                if (shdrs[i].sh_size > UINT32_MAX - total_alloc) {
                    free(sec_offsets); dl_free_module(mod); free(file_buf);
                    set_dlerror("dlopen: section size overflow");
                    pthread_mutex_unlock(&s_dl_lock);
                    return NULL;
                }
                total_alloc += shdrs[i].sh_size;
            }
        }

        /* Also allocate space for SHN_COMMON symbols */
        if (mod->symtab && mod->num_syms > 0u) {
            mod->common_addrs = (uint32_t *)calloc(mod->num_syms, sizeof(uint32_t));
            if (!mod->common_addrs) {
                free(sec_offsets); dl_free_module(mod); free(file_buf);
                set_dlerror("dlopen: out of memory allocating COMMON table");
                pthread_mutex_unlock(&s_dl_lock);
                return NULL;
            }
            for (uint32_t s = 0; s < mod->num_syms; s++) {
                if (mod->symtab[s].st_shndx == SHN_COMMON) {
                    uint32_t align = mod->symtab[s].st_value > 0 ? mod->symtab[s].st_value : 4;
                    if (align & (align - 1u)) align = 4u;
                    total_alloc = (total_alloc + align - 1) & ~(align - 1);
                    mod->common_addrs[s] = total_alloc;
                    if (mod->symtab[s].st_size > UINT32_MAX - total_alloc) {
                        free(sec_offsets); dl_free_module(mod); free(file_buf);
                        set_dlerror("dlopen: COMMON size overflow");
                        pthread_mutex_unlock(&s_dl_lock);
                        return NULL;
                    }
                    total_alloc += mod->symtab[s].st_size;
                }
            }
        }

        uint8_t *image = (uint8_t *)malloc(total_alloc + 64);
        if (!image) {
            set_dlerror("dlopen: out of memory for module image");
            free(sec_offsets); free(file_buf);
            dl_free_module(mod);
            pthread_mutex_unlock(&s_dl_lock);
            return NULL;
        }
        memset(image, 0, total_alloc + 64);
        mod->image_base = image;
        mod->image_size = total_alloc;

        /* Place sections into allocated image */
        for (uint16_t i = 0; i < eh->e_shnum; i++) {
            if (shdrs[i].sh_flags & SHF_ALLOC) {
                void *dest = image + sec_offsets[i];
                mod->section_addrs[i] = dest;
                mod->section_sizes[i] = shdrs[i].sh_size;
                if (shdrs[i].sh_type != SHT_NOBITS && shdrs[i].sh_offset < file_size) {
                    uint32_t copy_bytes = (shdrs[i].sh_offset + shdrs[i].sh_size <= file_size) ?
                                           shdrs[i].sh_size : (file_size - shdrs[i].sh_offset);
                    memcpy(dest, file_buf + shdrs[i].sh_offset, copy_bytes);
                }
            }
        }

        /* Adjust COMMON addresses relative to image pointer */
        if (mod->symtab && mod->common_addrs) {
            for (uint32_t s = 0; s < mod->num_syms; s++) {
                if (mod->symtab[s].st_shndx == SHN_COMMON) {
                    mod->common_addrs[s] += (uint32_t)image;
                }
            }
        }
        free(sec_offsets);

        /* Apply Relocations (SHT_REL) */
        for (uint16_t i = 0; i < eh->e_shnum; i++) {
            if (shdrs[i].sh_type == SHT_REL) {
                uint32_t target_sec = shdrs[i].sh_info;
                if (target_sec >= eh->e_shnum || !mod->section_addrs[target_sec]) {
                    continue;
                }
                if (!dl_range_ok(shdrs[i].sh_offset, shdrs[i].sh_size, file_size)) {
                    set_dlerror("dlopen: ET_REL relocation section out of bounds");
                    free(file_buf);
                    dl_free_module(mod);
                    pthread_mutex_unlock(&s_dl_lock);
                    return NULL;
                }
                uint8_t *target_base = (uint8_t *)mod->section_addrs[target_sec];
                uint32_t target_size = mod->section_sizes[target_sec];
                const Elf32_Rel *relocs = (const Elf32_Rel *)(file_buf + shdrs[i].sh_offset);
                uint32_t num_relocs = shdrs[i].sh_size / sizeof(Elf32_Rel);

                for (uint32_t r = 0; r < num_relocs; r++) {
                    const Elf32_Rel *rel = &relocs[r];
                    uint32_t sym_idx = ELF32_R_SYM(rel->r_info);
                    uint8_t rel_type = ELF32_R_TYPE(rel->r_info);
                    if (!mod->symtab || sym_idx >= mod->num_syms) continue;
                    if (!dl_range_ok(rel->r_offset, 4u, target_size)) continue;

                    Elf32_Sym *sym = &mod->symtab[sym_idx];
                    uint32_t sym_val = 0;

                    if (sym->st_shndx == SHN_UNDEF) {
                        const char *sname = (mod->strtab && sym->st_name < mod->strtab_size) ?
                                            (mod->strtab + sym->st_name) : "";
                        void *resolved = dlsym_resolve_global(sname);
                        if (!resolved) {
                            if (ELF32_ST_BIND(sym->st_info) == STB_WEAK) {
                                sym_val = 0;
                            } else {
                                set_dlerror_fmt("dlopen: unresolved external symbol '%s'", sname);
                                free(file_buf);
                                dl_free_module(mod);
                                pthread_mutex_unlock(&s_dl_lock);
                                return NULL;
                            }
                        } else {
                            sym_val = (uint32_t)resolved;
                        }
                    } else if (sym->st_shndx == SHN_ABS) {
                        sym_val = sym->st_value;
                    } else if (sym->st_shndx == SHN_COMMON) {
                        sym_val = mod->common_addrs ? mod->common_addrs[sym_idx] : 0;
                    } else if (sym->st_shndx < eh->e_shnum && mod->section_addrs[sym->st_shndx]) {
                        sym_val = (uint32_t)mod->section_addrs[sym->st_shndx] + sym->st_value;
                    }

                    uint32_t *patch_loc = (uint32_t *)(target_base + rel->r_offset);
                    if (rel_type == R_386_32) {
                        *patch_loc += sym_val;
                    } else if (rel_type == R_386_PC32) {
                        *patch_loc += (sym_val - (uint32_t)patch_loc);
                    }
                }
            }
        }

    } else {
        /* Shared Library (ET_DYN, e.g. gcc -shared -fPIC) */
        if (eh->e_phoff == 0 || eh->e_phnum == 0 ||
            eh->e_phentsize < sizeof(Elf32_Phdr) ||
            (uint32_t)eh->e_phnum > UINT32_MAX / eh->e_phentsize ||
            !dl_range_ok(eh->e_phoff, (uint32_t)eh->e_phnum * (uint32_t)eh->e_phentsize, file_size)) {
            set_dlerror("dlopen: invalid program headers in ET_DYN file");
            dl_free_module(mod); free(file_buf);
            pthread_mutex_unlock(&s_dl_lock);
            return NULL;
        }

        const Elf32_Phdr *phdrs = (const Elf32_Phdr *)(file_buf + eh->e_phoff);
        uint32_t min_vaddr = 0xFFFFFFFF;
        uint32_t max_vaddr = 0;
        for (uint16_t i = 0; i < eh->e_phnum; i++) {
            if (phdrs[i].p_type == PT_LOAD) {
                if (phdrs[i].p_filesz > phdrs[i].p_memsz ||
                    !dl_range_ok(phdrs[i].p_offset, phdrs[i].p_filesz, file_size) ||
                    phdrs[i].p_memsz > UINT32_MAX - phdrs[i].p_vaddr) {
                    set_dlerror("dlopen: invalid PT_LOAD bounds in ET_DYN");
                    dl_free_module(mod); free(file_buf);
                    pthread_mutex_unlock(&s_dl_lock);
                    return NULL;
                }
                if (phdrs[i].p_vaddr < min_vaddr) min_vaddr = phdrs[i].p_vaddr;
                if (phdrs[i].p_vaddr + phdrs[i].p_memsz > max_vaddr) {
                    max_vaddr = phdrs[i].p_vaddr + phdrs[i].p_memsz;
                }
            }
        }

        if (max_vaddr <= min_vaddr) {
            set_dlerror("dlopen: invalid PT_LOAD segment bounds");
            dl_free_module(mod); free(file_buf);
            pthread_mutex_unlock(&s_dl_lock);
            return NULL;
        }

        uint32_t total_mem = max_vaddr - min_vaddr;
        uint8_t *image = (uint8_t *)malloc(total_mem + 4096);
        if (!image) {
            set_dlerror("dlopen: out of memory for ET_DYN image");
            dl_free_module(mod); free(file_buf);
            pthread_mutex_unlock(&s_dl_lock);
            return NULL;
        }
        memset(image, 0, total_mem + 4096);
        mod->image_base = image;
        mod->image_size = total_mem;
        mod->min_vaddr = min_vaddr;

        /* Load PT_LOAD segments */
        for (uint16_t i = 0; i < eh->e_phnum; i++) {
            if (phdrs[i].p_type == PT_LOAD && phdrs[i].p_filesz > 0u) {
                uint8_t *dest = image + (phdrs[i].p_vaddr - min_vaddr);
                memcpy(dest, file_buf + phdrs[i].p_offset, phdrs[i].p_filesz);
            }
        }

        /* Read Symbol & String Table from SHT_DYNSYM or SHT_SYMTAB */
        if (eh->e_shoff > 0 && eh->e_shnum > 0 &&
            eh->e_shentsize >= sizeof(Elf32_Shdr) &&
            (uint32_t)eh->e_shnum <= UINT32_MAX / eh->e_shentsize &&
            dl_range_ok(eh->e_shoff, (uint32_t)eh->e_shnum * (uint32_t)eh->e_shentsize, file_size)) {
            const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(file_buf + eh->e_shoff);
            for (uint16_t i = 0; i < eh->e_shnum; i++) {
                if ((shdrs[i].sh_type == SHT_DYNSYM || (!mod->symtab && shdrs[i].sh_type == SHT_SYMTAB)) &&
                    dl_range_ok(shdrs[i].sh_offset, shdrs[i].sh_size, file_size)) {
                    mod->num_syms = shdrs[i].sh_size / sizeof(Elf32_Sym);
                    mod->symtab = (Elf32_Sym *)malloc(shdrs[i].sh_size);
                    if (mod->symtab) {
                        memcpy(mod->symtab, file_buf + shdrs[i].sh_offset, shdrs[i].sh_size);
                    }
                    uint32_t str_idx = shdrs[i].sh_link;
                    if (str_idx < eh->e_shnum &&
                        dl_range_ok(shdrs[str_idx].sh_offset, shdrs[str_idx].sh_size, file_size)) {
                        mod->strtab_size = shdrs[str_idx].sh_size;
                        mod->strtab = (char *)malloc(shdrs[str_idx].sh_size);
                        if (mod->strtab) {
                            memcpy(mod->strtab, file_buf + shdrs[str_idx].sh_offset, shdrs[str_idx].sh_size);
                        }
                    }
                }
            }

            /* Apply Dynamic Relocations (SHT_REL) */
            for (uint16_t i = 0; i < eh->e_shnum; i++) {
                if (shdrs[i].sh_type == SHT_REL &&
                    dl_range_ok(shdrs[i].sh_offset, shdrs[i].sh_size, file_size)) {
                    const Elf32_Rel *relocs = (const Elf32_Rel *)(file_buf + shdrs[i].sh_offset);
                    uint32_t num_relocs = shdrs[i].sh_size / sizeof(Elf32_Rel);
                    for (uint32_t r = 0; r < num_relocs; r++) {
                        const Elf32_Rel *rel = &relocs[r];
                        uint32_t sym_idx = ELF32_R_SYM(rel->r_info);
                        uint8_t rel_type = ELF32_R_TYPE(rel->r_info);
                        uint32_t rel_offset = rel->r_offset >= min_vaddr ? (rel->r_offset - min_vaddr) : rel->r_offset;
                        if (!dl_range_ok(rel_offset, 4u, total_mem)) continue;

                        uint32_t *patch_loc = (uint32_t *)(image + rel_offset);

                        if (rel_type == R_386_RELATIVE) {
                            *patch_loc += (uint32_t)image - min_vaddr;
                        } else if (rel_type == R_386_GLOB_DAT || rel_type == R_386_JMP_SLOT ||
                                   rel_type == R_386_32 || rel_type == R_386_PC32) {
                            uint32_t sym_val = 0;
                            if (mod->symtab && sym_idx < mod->num_syms) {
                                Elf32_Sym *sym = &mod->symtab[sym_idx];
                                if (sym->st_shndx == SHN_UNDEF) {
                                    const char *sname = (mod->strtab && sym->st_name < mod->strtab_size) ?
                                                        (mod->strtab + sym->st_name) : "";
                                    void *res = dlsym_resolve_global(sname);
                                    if (res) {
                                        sym_val = (uint32_t)res;
                                    }
                                } else if (sym->st_shndx == SHN_ABS) {
                                    sym_val = sym->st_value;
                                } else {
                                    uint32_t off = sym->st_value >= min_vaddr ? (sym->st_value - min_vaddr) : sym->st_value;
                                    sym_val = (uint32_t)image + off;
                                }
                            }
                            if (rel_type == R_386_32) {
                                *patch_loc += sym_val;
                            } else if (rel_type == R_386_PC32) {
                                *patch_loc += sym_val - (uint32_t)patch_loc;
                            } else {
                                *patch_loc = sym_val;
                            }
                        }
                    }
                }
            }
        }
    }

    dl_collect_init_fini_arrays(mod, file_buf, file_size);
    free(file_buf);

    /* Look for initialization entrypoints: _init, init, or module_init */
    void (*init_fn)(void) = (void (*)(void))dlsym_in_module(mod, "_init");
    if (!init_fn) init_fn = (void (*)(void))dlsym_in_module(mod, "init");
    if (!init_fn) init_fn = (void (*)(void))dlsym_in_module(mod, "module_init");
    mod->init_fn = init_fn;

    void (*fini_fn)(void) = (void (*)(void))dlsym_in_module(mod, "_fini");
    if (!fini_fn) fini_fn = (void (*)(void))dlsym_in_module(mod, "fini");
    if (!fini_fn) fini_fn = (void (*)(void))dlsym_in_module(mod, "module_fini");
    mod->fini_fn = fini_fn;

    /* Insert into active modules list before running init */
    mod->next = s_modules;
    s_modules = mod;

    /* Call module initialization if present */
    if (mod->init_fn) {
        mod->init_fn();
    }
    for (size_t i = 0; i < mod->init_array_count; i++) {
        if (mod->init_array[i]) {
            mod->init_array[i]();
        }
    }

    pthread_mutex_unlock(&s_dl_lock);
    return (void *)mod;
}

static dl_module_t *dl_find_module_by_addr(const void *addr) {
    uint32_t a = (uint32_t)(uintptr_t)addr;
    dl_module_t *m = s_modules;
    while (m) {
        if (m->image_base && m->image_size) {
            uint32_t base = (uint32_t)(uintptr_t)m->image_base;
            if (a >= base && a < base + m->image_size) {
                return m;
            }
        }
        m = m->next;
    }
    return NULL;
}

void *dlsym(void *handle, const char *symbol) {
    if (!symbol || !*symbol) {
        set_dlerror("dlsym: empty symbol name");
        return NULL;
    }

    pthread_mutex_lock(&s_dl_lock);

    /* If handle is RTLD_NEXT, search modules loaded after caller's module */
    if (handle == RTLD_NEXT) {
        dl_module_t *caller_mod = dl_find_module_by_addr(__builtin_return_address(0));
        void *res = dlsym_resolve_after(caller_mod, symbol);
        pthread_mutex_unlock(&s_dl_lock);
        if (!res) {
            set_dlerror_fmt("dlsym(RTLD_NEXT): symbol '%s' not found", symbol);
        }
        return res;
    }

    /* If handle is NULL or RTLD_DEFAULT, search all loaded modules, then global symbols */
    if (!handle || handle == RTLD_DEFAULT) {
        dl_module_t *m = s_modules;
        while (m) {
            void *res = dlsym_in_module(m, symbol);
            if (res) {
                pthread_mutex_unlock(&s_dl_lock);
                return res;
            }
            m = m->next;
        }
        void *res = dlsym_resolve_global(symbol);
        pthread_mutex_unlock(&s_dl_lock);
        if (res) return res;

        set_dlerror_fmt("dlsym: symbol '%s' not found globally", symbol);
        return NULL;
    }

    /* Validate handle against active module list to avoid UAF on stale handles */
    dl_module_t *mod = (dl_module_t *)handle;
    dl_module_t *cur = s_modules;
    while (cur && cur != mod) {
        cur = cur->next;
    }
    if (!cur) {
        pthread_mutex_unlock(&s_dl_lock);
        set_dlerror("dlsym: invalid or closed module handle");
        return NULL;
    }

    void *res = dlsym_in_module(mod, symbol);
    const char *mpath = mod->path;
    pthread_mutex_unlock(&s_dl_lock);
    if (!res) {
        set_dlerror_fmt("dlsym: symbol '%s' not found in module '%s'",
                        symbol, mpath ? mpath : "unnamed");
        return NULL;
    }

    return res;
}

int dlclose(void *handle) {
    if (!handle || handle == RTLD_DEFAULT || handle == RTLD_NEXT) {
        return 0;
    }

    pthread_mutex_lock(&s_dl_lock);
    dl_module_t *prev = NULL;
    dl_module_t *cur = s_modules;
    while (cur) {
        if (cur == (dl_module_t *)handle) break;
        prev = cur;
        cur = cur->next;
    }

    if (!cur) {
        pthread_mutex_unlock(&s_dl_lock);
        set_dlerror("dlclose: invalid module handle");
        return -1;
    }

    cur->ref_count--;
    if (cur->ref_count <= 0) {
        if (prev) {
            prev->next = cur->next;
        } else {
            s_modules = cur->next;
        }
        for (size_t i = cur->fini_array_count; i > 0u; i--) {
            if (cur->fini_array && cur->fini_array[i - 1u]) {
                cur->fini_array[i - 1u]();
            }
        }
        if (cur->fini_fn) {
            cur->fini_fn();
        }
        dl_free_module(cur);
    }

    pthread_mutex_unlock(&s_dl_lock);
    return 0;
}

int dladdr(const void *addr, Dl_info *info) {
    if (!info) return 0;
    info->dli_fname = NULL;
    info->dli_fbase = NULL;
    info->dli_sname = NULL;
    info->dli_saddr = NULL;

    pthread_mutex_lock(&s_dl_lock);
    uint32_t a = (uint32_t)(uintptr_t)addr;
    dl_module_t *m = dl_find_module_by_addr(addr);
    if (m) {
        info->dli_fname = m->path;
        info->dli_fbase = m->image_base;

        uint32_t best_diff = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < m->num_syms; i++) {
            if (m->strtab && m->symtab[i].st_name < m->strtab_size) {
                const char *sname = m->strtab + m->symtab[i].st_name;
                void *saddr = dlsym_in_module(m, sname);
                if (saddr && (uint32_t)(uintptr_t)saddr <= a) {
                    uint32_t diff = a - (uint32_t)(uintptr_t)saddr;
                    if (diff < best_diff) {
                        best_diff = diff;
                        info->dli_sname = sname;
                        info->dli_saddr = saddr;
                    }
                }
            }
        }
        pthread_mutex_unlock(&s_dl_lock);
        return 1;
    }

    pthread_mutex_unlock(&s_dl_lock);
    return 0;
}

