OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    . = 1M;

    .multiboot ALIGN(8) :
    {
        KEEP(*(.multiboot))
    }

    .text ALIGN(4K) :
    {
        *(.text*)
    }

    .rodata ALIGN(4K) :  
    {
        *(.rodata .rodata.*)
        KEEP(*(.rodata .rodata.*))
        . = ALIGN(8);
        PROVIDE(__kernel_init_array_start = .);
        KEEP(*(.init_array))
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        PROVIDE(__kernel_init_array_end = .);
        . = ALIGN(8);
        PROVIDE(__kernel_fini_array_start = .);
        KEEP(*(.fini_array))
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        PROVIDE(__kernel_fini_array_end = .);
    }

    .data ALIGN(4K) :
    {
        *(.data*)
    }

    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss*)
    }
    . = ALIGN(4K);
    PROVIDE(__kernel_end = .);

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
