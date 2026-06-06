/* VibeOS arm64 — cat, written as a normal int main(argc, argv) program.
 *
 * Demonstrates the arm64 libc + crt0: argv parsing, file I/O via syscalls.
 * Usage (from the kernel shell):  exec /bin/cat /journal.log
 */
#include "libc/vlibc.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        putln("usage: cat <file> [file...]");
        return 1;
    }

    for (int a = 1; a < argc; a++) {
        long fd = sys_open(argv[a]);
        if (fd < 0) {
            puts_("cat: cannot open ");
            putln(argv[a]);
            continue;
        }
        char buf[512];
        for (;;) {
            long n = sys_read((int)fd, buf, sizeof(buf));
            if (n <= 0) break;
            sys_write(1, buf, (unsigned long)n);
        }
        sys_close((int)fd);
    }
    return 0;
}
