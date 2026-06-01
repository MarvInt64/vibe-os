OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* Force a single PT_LOAD segment at the user base; the kernel loader expects
 * exactly one LOAD at PROCESS_USER_TEMPLATE_BASE. .bss is NOBITS (no file
 * bytes) but lives in the same segment so mem_size covers it. */
PHDRS
{
    all PT_LOAD FLAGS(7);
}

SECTIONS
{
    . = 0x20000000;

    .all : {
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(8);
        PROVIDE(__init_array_start = .);
        KEEP(*(.init_array))
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        PROVIDE(__init_array_end = .);
        . = ALIGN(8);
        PROVIDE(__fini_array_start = .);
        KEEP(*(.fini_array))
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        PROVIDE(__fini_array_end = .);
        *(.data)
        *(.data.*)
    } :all

    .bss (NOLOAD) : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    } :all

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
    }
}
