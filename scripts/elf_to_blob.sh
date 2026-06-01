#!/bin/bash
# Convert ELF to assembly blob for kernel embedding

INPUT=$1
OUTPUT=$2

if [ -z "$INPUT" ] || [ -z "$OUTPUT" ]; then
    echo "Usage: $0 <input.elf> <output.S>"
    exit 1
fi

NAME=$(basename "$INPUT" .elf | tr '-' '_')

echo '.intel_syntax noprefix' > "$OUTPUT"
echo '.section .rodata, "a"' >> "$OUTPUT"
echo ".global user_${NAME}_start" >> "$OUTPUT"
echo ".global user_${NAME}_end" >> "$OUTPUT"
echo "user_${NAME}_start:" >> "$OUTPUT"

# Output bytes as .byte directives
xxd -i "$INPUT" | grep -v '^unsigned char' | grep -v '^unsigned int' | \
    sed 's/};$/\nuser_'"${NAME}"'_end:/' | \
    sed 's/^/  .byte /' | \
    sed 's/,/, /g' >> "$OUTPUT"
