/* VibeOS arm64 — writefile: create a file, write text, read it back.
 *
 * Proves the creat + write (to disk) + read round trip from EL0.
 * Usage:  exec /bin/writefile /tmp/note.txt Hello world
 */
#include "libc/vlibc.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        putln("usage: writefile <path> [words...]");
        return 1;
    }

    long fd = sys_creat(argv[1]);
    if (fd < 0) {
        puts_("writefile: cannot create ");
        putln(argv[1]);
        return 1;
    }

    /* Write the remaining args (space-separated) into the file. */
    long written = 0;
    for (int a = 2; a < argc; a++) {
        if (a > 2) written += sys_write((int)fd, " ", 1);
        written += sys_write((int)fd, argv[a], strlen_(argv[a]));
    }
    written += sys_write((int)fd, "\n", 1);
    sys_close((int)fd);

    puts_("writefile: wrote ");
    put_uint((unsigned long)written);
    puts_(" bytes to ");
    putln(argv[1]);

    /* Read it back to confirm it landed on disk. */
    putln("--- reading back ---");
    long rfd = sys_open(argv[1]);
    if (rfd < 0) { putln("writefile: reopen failed"); return 1; }
    char buf[256];
    for (;;) {
        long n = sys_read((int)rfd, buf, sizeof(buf));
        if (n <= 0) break;
        sys_write(1, buf, (unsigned long)n);
    }
    sys_close((int)rfd);
    putln("--- done ---");
    return 0;
}
