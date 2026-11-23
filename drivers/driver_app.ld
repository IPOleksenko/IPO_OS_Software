ENTRY(_start)

SECTIONS
{
    . = 0x00800000;

    .text : ALIGN(4)
    {
        *(.entry)
        *(.text*)
        *(.rodata*)
    }

    .data : ALIGN(4)
    {
        *(.data*)
    }

    .bss : ALIGN(4)
    {
        *(.bss*)
        *(COMMON)
    }
}
