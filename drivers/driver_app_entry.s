.code32
.global _start
.extern driver_app_main

.section .entry, "ax"
_start:
    jmp driver_app_main
