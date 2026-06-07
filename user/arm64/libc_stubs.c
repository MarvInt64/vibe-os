/* VibeOS arm64 — tiny libc stubs for symbols the shared libc/crt0 reference
 * but that need an arch- or runtime-specific definition not yet ported.
 *
 *  __extenddftf2    — double → long double (binary128) conversion, pulled in
 *                     by printf float paths. arm64 long double is 128-bit; we
 *                     don't support long-double formatting yet, so provide a
 *                     minimal widening that just preserves the double value's
 *                     low half (good enough to satisfy the linker; %Lf unused).
 *
 * Note: __cxa_finalize is NOT stubbed here — the C++ runtime (cxxabi.cpp,
 * appended to libc.a) provides the real one.  arm64 now has C++ GUI apps
 * (dock/topbar/taskmgr), so a duplicate stub here caused a link conflict.
 */
#include <stdint.h>

/* __extenddftf2 (double → binary128) is pulled in by strtold(); no current app
 * uses long double, so provide a bare symbol to satisfy the linker. Declared
 * with a void signature to avoid needing real quad-float codegen. If it is ever
 * actually called we trap, making misuse obvious rather than silently wrong. */
__attribute__((used, noreturn)) void __extenddftf2(void) {
    __asm__ volatile("brk #0");
    __builtin_unreachable();
}
