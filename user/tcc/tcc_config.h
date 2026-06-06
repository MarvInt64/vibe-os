/*
 * TCC configuration for VibeOS — x86_64 ELF target, no dynamic linking.
 *
 * This header is injected via -include so it is visible to every TCC
 * translation unit before any system headers are pulled in.
 */
#ifndef TCC_CONFIG_VIBEOS_H
#define TCC_CONFIG_VIBEOS_H

/* ---- Version string --------------------------------------------------- */
#define TCC_VERSION "0.9.28rc-vibeos"

/* ---- Target: x86_64 ELF (no PE, no MachO) ----------------------------- */
#define TCC_TARGET_X86_64 1

/* ---- Static only — no dlopen / dynamic loading ------------------------ */
#define CONFIG_TCC_STATIC 1

/* ---- Paths inside VibeOS (overrides defaults in tcc.h) ---------------- */
/* TCC's own runtime directory (headers, helper libs) */
#define CONFIG_TCCDIR "/lib/tcc"

/* Where to find crt*.o files */
#define CONFIG_TCC_CRTPREFIX "/lib"

/* System include paths: {B} = CONFIG_TCCDIR, colon-separated */
#define CONFIG_TCC_SYSINCLUDEPATHS "{B}/include:/usr/include"

/* Library search paths */
#define CONFIG_TCC_LIBPATHS "{B}:/lib"

/* No dynamic linker on VibeOS — static binaries only */
#define CONFIG_TCC_ELFINTERP ""

/* No sysroot prefix */
#define CONFIG_SYSROOT ""

/* ---- Disable optional features ---------------------------------------- */
/* No built-in bounds checker (adds runtime overhead) */
#undef CONFIG_TCC_BCHECK
/* No stack backtrace support */
#undef CONFIG_TCC_BACKTRACE
/* No semaphore-based locking (single-threaded compiler) */
#define CONFIG_TCC_SEMLOCK 0

/* ---- Default predefines: keep enabled (needed for standard macros) ---- */
#define CONFIG_TCC_PREDEFS 1

/* ---- No triplet subdirectory (keeps paths simple) --------------------- */
#undef CONFIG_TRIPLET

/* ---- Use minimal DWARF debug info (v4, small) ------------------------- */
#define CONFIG_DWARF_VERSION 4

#endif /* TCC_CONFIG_VIBEOS_H */
