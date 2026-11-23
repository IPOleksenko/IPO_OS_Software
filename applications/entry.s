.code32
.global _start

.extern main
.extern __libc_start_main
.extern environ
.extern __progname
.weak __bss_start
.weak __bss_end

.section .entry, "ax"
_start:
    /* Reset FPU state */
    fninit

    /* Zero out .bss section */
    movl $__bss_start, %edi
    movl $__bss_end, %ecx
    subl %edi, %ecx
    jbe .bss_done
    xorl %eax, %eax
    cld
    movl %ecx, %edx
    shrl $2, %ecx
    rep stosl
    movl %edx, %ecx
    andl $3, %ecx
    rep stosb
.bss_done:

    pushl %ebp
    movl %esp, %ebp

    movl 8(%ebp), %eax   /* argc */
    movl 12(%ebp), %edx  /* argv */
    movl 16(%ebp), %ecx  /* envp */

    testl %ecx, %ecx
    jz .no_env
    cmpl $0, (%ecx)
    jz .no_env
    movl %ecx, environ
.no_env:
    testl %edx, %edx
    jz .no_progname
    movl (%edx), %esi
    movl %esi, __progname
.no_progname:

    andl $-16, %esp
    pushl %ecx
    pushl %edx
    pushl %eax
    pushl $main
    call __libc_start_main
    addl $16, %esp

    movl %ebp, %esp
    popl %ebp
    ret
