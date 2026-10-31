.code32
.global _start
.global call_struct_entry
.extern loader_main

.section .entry, "ax"
_start:
    jmp loader_main

.section .text, "ax"
call_struct_entry:
    pushl %ebp
    movl %esp, %ebp
    pushl 24(%ebp)
    pushl 20(%ebp)
    pushl 16(%ebp)
    pushl 12(%ebp)
    movl 8(%ebp), %eax
    call *%eax
    movl %ebp, %esp
    popl %ebp
    ret
