#ifndef VIBEOS_MACHINE_ENDIAN_H
#define VIBEOS_MACHINE_ENDIAN_H

/* Byte-order macros. VibeOS targets little-endian aarch64 and x86_64; key off
 * the compiler's __BYTE_ORDER__ builtin so this stays correct either way.
 * Provides what code expecting a BSD-style <machine/endian.h> needs. */

#define LITTLE_ENDIAN 1234
#define BIG_ENDIAN    4321
#define PDP_ENDIAN    3412

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define BYTE_ORDER BIG_ENDIAN
#else
#define BYTE_ORDER LITTLE_ENDIAN
#endif

#endif
