/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Marvin Kicha <https://github.com/MarvInt64/> */

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

    if (header->type != ELF_TYPE_EXEC || header->machine != ELF_MACHINE_X86_64 || header->version != ELF_VERSION_CURRENT) {
        return 0;
    }

    if (header->program_header_offset + ((uint64_t)header->program_header_entry_size * header->program_header_count) > size) {
        return 0;
    }

    return 1;
}

