/* C runtime stub for relocatable bare-metal IPO_OS kernel modules.
 * Headerless flat binary: resides at offset 0x00000000 (.text.entry).
 * Entry ABI: int kmd_init_entry(void *(*resolve_sym)(const char *),
 *                               kmod_init_meta_t *out_meta);
 */
.section .text.entry, "ax", @progbits
.global kmd_init_entry
.extern kmd_main_init
.extern _kmd_reloc_tbl
.extern _kmd_ext_tbl
.extern _kmd_meta_tbl

kmd_init_entry:
    push %ebp
    mov %esp, %ebp
    push %ebx
    push %esi
    push %edi

    call 1f
1:
    pop %ebx
    sub $(1b - kmd_init_entry), %ebx   /* %ebx = runtime base address */

    /* 1. Apply internal R_386_32 relocations */
    mov $_kmd_reloc_tbl, %esi
    add %ebx, %esi
    mov (%esi), %ecx
    add $4, %esi
    test %ecx, %ecx
    jz .Lext_relocs
.Lint_loop:
    mov (%esi), %eax
    add %ebx, (%ebx, %eax, 1)
    add $4, %esi
    dec %ecx
    jnz .Lint_loop

.Lext_relocs:
    /* 2. Resolve and patch external kernel symbols via resolve_sym (8(%ebp)) */
    mov $_kmd_ext_tbl, %esi
    add %ebx, %esi
    push %esi                          /* -16(%ebp) = runtime base of _kmd_ext_tbl */
    mov (%esi), %edi                   /* %edi = external symbol count */
    add $4, %esi
    test %edi, %edi
    jz .Lfill_meta

.Lsym_loop:
    mov 8(%ebp), %eax                  /* resolve_sym */
    test %eax, %eax
    jz .Lfail

    mov (%esi), %edx                   /* delta from _kmd_ext_tbl to string */
    add -16(%ebp), %edx                /* %edx = runtime symbol name pointer */
    add $4, %esi
    mov (%esi), %ecx                   /* %ecx = reloc count for symbol */
    add $4, %esi

    push %ecx
    push %edx
    call *%eax                         /* %eax = resolved kernel symbol addr */
    add $4, %esp
    pop %ecx

    test %eax, %eax
    jz .Lfail

.Lrel_loop:
    test %ecx, %ecx
    jz .Lsym_next
    mov (%esi), %edx
    add $4, %esi
    test $0x80000000, %edx
    jnz .Lrel32
    /* ABS32: *(uint32_t *)(base + offset) += target_addr */
    add %eax, (%ebx, %edx, 1)
    dec %ecx
    jmp .Lrel_loop
.Lrel32:
    and $0x7FFFFFFF, %edx
    /* REL32: *(uint32_t *)(base + offset) += (target_addr - base) */
    push %eax
    sub %ebx, %eax
    add %eax, (%ebx, %edx, 1)
    pop %eax
    dec %ecx
    jmp .Lrel_loop

.Lsym_next:
    dec %edi
    jnz .Lsym_loop

.Lfill_meta:
    /* 3. Populate optional kmod_init_meta_t *out_meta (12(%ebp)) */
    mov 12(%ebp), %edx
    test %edx, %edx
    jz .Lcall_main
    mov $_kmd_meta_tbl, %esi
    add %ebx, %esi

    mov 0(%esi), %eax
    test %eax, %eax
    jz 4f
    add %esi, %eax
    mov %eax, 0(%edx)                  /* out_meta->name */
4:
    mov 4(%esi), %eax
    test %eax, %eax
    jz 5f
    add %esi, %eax
    mov %eax, 4(%edx)                  /* out_meta->author */
5:
    mov 8(%esi), %eax
    test %eax, %eax
    jz 6f
    add %esi, %eax
    mov %eax, 8(%edx)                  /* out_meta->description */
6:
    mov 12(%esi), %eax                 /* cleanup offset from base (-1 = none) */
    cmp $-1, %eax
    je .Lcall_main
    add %ebx, %eax
    mov %eax, 12(%edx)                 /* out_meta->cleanup */

.Lcall_main:
    lea -12(%ebp), %esp
    call kmd_main_init
    jmp .Ldone

.Lfail:
    lea -12(%ebp), %esp
    mov $-7, %eax

.Ldone:
    pop %edi
    pop %esi
    pop %ebx
    pop %ebp
    ret
