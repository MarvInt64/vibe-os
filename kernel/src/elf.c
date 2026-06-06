/*
 * MIT License
 *
 * Copyright (c) 2026 Marvin Kicha
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "elf.h"

int elf64_validate(const void *image, size_t size) {
    const struct elf64_header *header = (const struct elf64_header *)image;

    if (image == 0 || size < sizeof(struct elf64_header)) {
        return 0;
    }

    if (header->ident[0] != ELF_MAGIC0 ||
        header->ident[1] != ELF_MAGIC1 ||
        header->ident[2] != ELF_MAGIC2 ||
        header->ident[3] != ELF_MAGIC3) {
        return 0;
    }

    if (header->ident[4] != ELF_CLASS_64 || header->ident[5] != ELF_DATA_LSB) {
        return 0;
    }

#ifdef ARCH_ARM64
    #define ELF_MACHINE_NATIVE ELF_MACHINE_AARCH64
#else
    #define ELF_MACHINE_NATIVE ELF_MACHINE_X86_64
#endif
    if (header->type != ELF_TYPE_EXEC || header->machine != ELF_MACHINE_NATIVE || header->version != ELF_VERSION_CURRENT) {
        return 0;
    }

    if (header->program_header_offset + ((uint64_t)header->program_header_entry_size * header->program_header_count) > size) {
        return 0;
    }

    return 1;
}

