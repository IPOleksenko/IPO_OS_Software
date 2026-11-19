/* Linker script for headerless bare-metal IPO_OS Kernel Modules.
 * Linked at VMA 0x00000000 with --emit-relocs in a 2-pass build so
 * kmd_init_entry at offset 0 self-relocates internal references and
 * resolves external kernel symbols before executing kmd_main_init().
 */
ENTRY(kmd_init_entry)

SECTIONS {
    . = 0x00000000;
    _kmd_start = .;

    .text : ALIGN(1) {
        *(.text.entry)
        *(.text .text.*)
        *(.rodata .rodata.*)
    }
    . = ALIGN(32);
    _kmd_text_end = .;

    .data : ALIGN(32) {
        _kmd_data_start = .;
        *(.data .data.*)
        *(.data.rel.ro .data.rel.ro.*)
        . = ALIGN(4);
        *(.kmd_self_relocs)
    }
    . = ALIGN(32);
    _kmd_data_end = .;

    .bss : ALIGN(32) {
        _kmd_bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(16);
    }
    _kmd_bss_end = .;

    /DISCARD/ : {
        *(.note*)
        *(.comment*)
        *(.eh_frame*)
        *(.eh_frame_hdr*)
    }
}
