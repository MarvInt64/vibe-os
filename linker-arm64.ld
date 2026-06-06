/* VibeOS arm64 linker script.
 *
 * QEMU virt machine: RAM at 0x40000000. We load the kernel at 0x40200000
 * (2 MB above RAM start) so the device tree blob (DTB) can sit at 0x40000000.
 *
 * The output is a raw ELF64 AArch64 binary passed to QEMU with -kernel.
 */

ENTRY(_start)

SECTIONS {
    . = 0x40200000;

    /* Boot code must come first so _start lands at the load address */
    .text.boot : {
        *(.text.boot)
    }

    /* Exception vectors must be 2048-byte aligned */
    .text.vectors : ALIGN(2048) {
        *(.text.vectors)
    }

    /* Regular code */
    .text : ALIGN(4096) {
        *(.text .text.*)
    }

    /* Read-only data */
    .rodata : ALIGN(4096) {
        *(.rodata .rodata.*)
    }

    /* Mutable data */
    .data : ALIGN(4096) {
        *(.data .data.*)
    }

    /* Uninitialized data (cleared by boot.S) */
    .bss : ALIGN(4096) {
        __bss_start = .;
        *(.bss .bss.*)
        *(.bss.boot)       /* boot stack */
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }

    /* Discard x86-ism sections if they slip through */
    /DISCARD/ : {
        *(.multiboot)
        *(.eh_frame)
        *(.note*)
    }
}
