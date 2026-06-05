/* sprintf stub for VibeOS nano — libc.a might be stale */
#include <stdio.h>
#include <stdarg.h>

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return r;
}
