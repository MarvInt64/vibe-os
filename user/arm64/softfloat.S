/* Assembly stubs for 128-bit soft-float functions.
 * clang generates calls to these on aarch64 for long double ops.
 * TCC doesn't actually use them — just need to satisfy the linker.
 * All stubs return 0 (x0=0, x1=0 for 128-bit returns). */

.section .text

.macro STUB name
    .global \name
    .type \name, %function
\name:
    mov x0, #0
    mov x1, #0
    ret
.endm

STUB __addtf3
STUB __subtf3
STUB __multf3
STUB __divtf3
STUB __eqtf2
STUB __netf2
STUB __lttf2
STUB __gttf2
STUB __letf2
STUB __getf2
STUB __fixtfdi
STUB __fixunstfdi
STUB __floatunsitf
STUB __floatunditf
STUB __extendsftf2
STUB __trunctfsf2
STUB __trunctfdf2
