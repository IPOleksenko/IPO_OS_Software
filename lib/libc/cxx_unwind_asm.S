.code32
.text

/* _Unwind_Regs layout (40 bytes):
 *   0:  eax
 *   4:  ecx
 *   8:  edx
 *   12: ebx
 *   16: esp
 *   20: ebp
 *   24: esi
 *   28: edi
 *   32: eip
 *   36: cfa
 */

.macro CAPTURE_AND_CALL target
    movl 4(%esp), %eax
    movl (%esp), %ecx
    leal 4(%esp), %edx
    subl $52, %esp
    movl $0, 0(%esp)
    movl $0, 4(%esp)
    movl $0, 8(%esp)
    movl %ebx, 12(%esp)
    movl %edx, 16(%esp)
    movl %ebp, 20(%esp)
    movl %esi, 24(%esp)
    movl %edi, 28(%esp)
    movl %ecx, 32(%esp)
    movl %edx, 36(%esp)
    movl %esp, %ecx
    pushl %ecx
    pushl %eax
    call \target
    addl $60, %esp
    ret
.endm

.global _Unwind_RaiseException
.type _Unwind_RaiseException, @function
_Unwind_RaiseException:
    CAPTURE_AND_CALL _Unwind_RaiseException_impl

.global _Unwind_Resume
.type _Unwind_Resume, @function
_Unwind_Resume:
    CAPTURE_AND_CALL _Unwind_Resume_impl

.global _Unwind_Resume_or_Rethrow
.type _Unwind_Resume_or_Rethrow, @function
_Unwind_Resume_or_Rethrow:
    CAPTURE_AND_CALL _Unwind_Resume_or_Rethrow_impl

.global _Unwind_Backtrace
.type _Unwind_Backtrace, @function
_Unwind_Backtrace:
    movl 4(%esp), %eax          /* trace fn */
    movl 8(%esp), %edx          /* trace arg */
    movl (%esp), %ecx           /* ret eip */
    pushl %eax                  /* save trace fn temporarily */
    leal 8(%esp), %eax          /* caller esp */
    subl $44, %esp
    movl $0, 0(%esp)
    movl $0, 4(%esp)
    movl $0, 8(%esp)
    movl %ebx, 12(%esp)
    movl %eax, 16(%esp)
    movl %ebp, 20(%esp)
    movl %esi, 24(%esp)
    movl %edi, 28(%esp)
    movl %ecx, 32(%esp)
    movl %eax, 36(%esp)
    movl %esp, %ecx             /* &regs */
    movl 44(%esp), %eax         /* trace fn */
    pushl %ecx
    pushl %edx
    pushl %eax
    call _Unwind_Backtrace_impl
    addl $60, %esp
    ret

.global _Unwind_InstallContext
.type _Unwind_InstallContext, @function
_Unwind_InstallContext:
    movl 4(%esp), %ecx
    movl 16(%ecx), %esp
    pushl 32(%ecx)
    movl 12(%ecx), %ebx
    movl 20(%ecx), %ebp
    movl 24(%ecx), %esi
    movl 28(%ecx), %edi
    movl 0(%ecx), %eax
    movl 8(%ecx), %edx
    xorl %ecx, %ecx
    ret
