/* Embedded TrueType font (DejaVu Sans) for appfont. .incbin pulls the raw
 * bytes straight into .rodata; the path is relative to the build CWD (repo
 * root), matching how the Makefile invokes the assembler. */
.section .rodata
.align 4
.global g_font_ttf
.global g_font_ttf_end
g_font_ttf:
    .incbin "third_party/stb/font.ttf"
g_font_ttf_end:
