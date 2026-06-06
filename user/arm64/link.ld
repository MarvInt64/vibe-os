/* VibeOS arm64 userspace linker script.
 *
 * EL0 programs are linked at VA 0x90000000. The kernel's EL0-alias maps
 * VA 0x80000000.. → PA 0x40000000.., so 0x90000000 is backed by PA 0x50000000,
 * well clear of the kernel image (0x40200000) and heap (0x40800000..).
 *
 * _start is the entry point. The whole image is one contiguous LOAD segment.
 */
ENTRY(_start)

SECTIONS {
    . = 0x90000000;

    .text : {
        *(.text._start)   /* ensure _start is first */
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : { *(.note* .comment .eh_frame*) }
}
