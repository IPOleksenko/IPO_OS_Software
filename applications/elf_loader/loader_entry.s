.code32
.global _start
.global call_struct_entry
.global call_struct_entry_stack
.global call_app_entry_stack
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

call_struct_entry_stack:
    pushl %ebp
    movl %esp, %ebp
    pushl %esi
    pushl %edi
    pushl %ebx

    movl 8(%ebp), %edx   /* entry_addr */
    movl 12(%ebp), %eax  /* struct_buf */
    movl 16(%ebp), %ebx  /* argc */
    movl 20(%ebp), %ecx  /* argv */
    movl 24(%ebp), %esi  /* envp */
    movl 28(%ebp), %edi  /* new_stack_top */

    testl %edi, %edi
    jz .call_struct_current

    /* Switch stack while preserving current esp in edi */
    movl %esp, %edi
    movl 28(%ebp), %esp
    pushl %esi          /* envp */
    pushl %ecx          /* argv */
    pushl %ebx          /* argc */
    pushl %eax          /* struct_buf */
    call *%edx
    movl %edi, %esp     /* restore original esp */
    jmp .struct_done

.call_struct_current:
    pushl %esi
    pushl %ecx
    pushl %ebx
    pushl %eax
    call *%edx
    addl $16, %esp

.struct_done:
    popl %ebx
    popl %edi
    popl %esi
    movl %ebp, %esp
    popl %ebp
    ret

call_app_entry_stack:
    pushl %ebp
    movl %esp, %ebp
    pushl %esi
    pushl %edi
    pushl %ebx

    movl 8(%ebp), %edx   /* entry_addr */
    movl 12(%ebp), %eax  /* argc */
    movl 16(%ebp), %ebx  /* argv */
    movl 20(%ebp), %ecx  /* envp */
    movl 24(%ebp), %esi  /* new_stack_top */

    testl %esi, %esi
    jz .call_app_current

    /* Switch stack while preserving current esp in edi */
    movl %esp, %edi
    movl %esi, %esp
    subl $4, %esp  /* keep 16-byte alignment at call */
    pushl %ecx  /* envp */
    pushl %ebx  /* argv */
    pushl %eax  /* argc */
    call *%edx
    movl %edi, %esp  /* restore original esp */
    jmp .app_done

.call_app_current:
    pushl %ecx  /* envp */
    pushl %ebx  /* argv */
    pushl %eax  /* argc */
    call *%edx
    addl $12, %esp

.app_done:
    popl %ebx
    popl %edi
    popl %esi
    movl %ebp, %esp
    popl %ebp
    ret
