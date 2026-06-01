#!/bin/bash
INPUT="$1"
OUTPUT="$2"

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

# Use xxd to generate hex dump and convert to .byte directives
xxd -p "$INPUT" | tr -d '\n' | fold -w 40 | while read line; do
    bytes=$(echo "$line" | sed 's/../0x&,/g' | sed 's/,$//')
    echo "  .byte $bytes" >> "$OUTPUT"
done

echo "" >> "$OUTPUT"
echo "user_${NAME}_end:" >> "$OUTPUT"
