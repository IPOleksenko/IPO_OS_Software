.code32
.global _start
.extern main
.extern __rel_start
.extern __rel_end

.section .entry, "ax"
_start:
    pushl %ebx
    pushl %esi
    pushl %edi
    call 1f
1:  popl %ecx
    subl $(1b - _start), %ecx
    movl %ecx, %edx
    subl $0x00800000, %edx
    testl %edx, %edx
    jz 3f
    leal (__rel_start - _start)(%ecx), %esi
    leal (__rel_end - _start)(%ecx), %edi
2:  cmpl %edi, %esi
    jae 3f
    movl 0(%esi), %eax
    cmpb $8, 4(%esi)
    jne 4f
    addl %edx, %eax
    addl %edx, (%eax)
4:  addl $8, %esi
    jmp 2b
3:  popl %edi
    popl %esi
    popl %ebx
    jmp main

