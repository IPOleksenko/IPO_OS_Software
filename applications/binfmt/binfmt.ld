ENTRY(_start)

SECTIONS
{
    . = 0x00800000;

    .text : ALIGN(4)
    {
        *(.entry)
        *(.text.startup)
        *(.text.main)
        *(.text*)
        KEEP(*(SORT_NONE(.init)))
        KEEP(*(SORT_NONE(.fini)))
        *(.rodata*)
        *(.rdata*)
    }

    .data : ALIGN(4)
    {
        *(.data*)
        *(.got*)
    }

    .rel.dyn : ALIGN(4)
    {
        __rel_start = .;
        *(.rel.*)
        __rel_end = .;
    }

    .bss : ALIGN(4)
    {
        PROVIDE_HIDDEN (__bss_start = .);
        *(COMMON)
        *(.bss*)
        PROVIDE_HIDDEN (__bss_end = .);
    }

    _end = .;
    PROVIDE(end = .);

    /DISCARD/ :
    {
        *(.interp)
        *(.dynsym)
        *(.dynstr)
        *(.gnu.hash)
        *(.hash)
        *(.dynamic)
        *(.eh_frame*)
        *(.gcc_except_table*)
        *(.comment)
        *(.note*)
    }
}

