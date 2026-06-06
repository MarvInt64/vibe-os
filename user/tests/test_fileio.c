/*
 * test_fileio.c — TCC file I/O test: fopen, fwrite, fread, fseek, ftell.
 * Compile inside VibeOS:  /bin/tcc test_fileio.c -o test_fileio && ./test_fileio
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TMPFILE "/tmp/tcc_fiotest.txt"

static int failures = 0;

#define CHECK(expr, msg) do { \
    if (!(expr)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("  ok: %s\n", msg); } \
} while(0)

int main(void) {
    printf("=== test_fileio: fopen/fwrite/fread/fseek ===\n");

    /* Write file */
    FILE *fp = fopen(TMPFILE, "w");
    CHECK(fp != NULL, "fopen /tmp/fiotest.txt for write");

    const char *data = "Hello VibeOS!\nLine two.\nLine three.\n";
    size_t wr = fwrite(data, 1, strlen(data), fp);
    CHECK(wr == strlen(data), "fwrite correct byte count");
    CHECK(fclose(fp) == 0, "fclose after write");

    /* Read back (text mode) */
    fp = fopen(TMPFILE, "r");
    CHECK(fp != NULL, "fopen for read");

    char buf[128];
    size_t rd = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[rd] = '\0';
    CHECK(rd == strlen(data), "fread all bytes");
    CHECK(strcmp(buf, data) == 0, "fread correct content");
    CHECK(fclose(fp) == 0, "fclose after read");

    /* fgets */
    fp = fopen(TMPFILE, "r");
    CHECK(fp != NULL, "fopen for fgets");
    char line[64];
    CHECK(fgets(line, sizeof(line), fp) != NULL, "fgets line 1");
    CHECK(strcmp(line, "Hello VibeOS!\n") == 0, "fgets line 1 content");
    CHECK(fclose(fp) == 0, "fclose after fgets");

    /* fseek / ftell */
    fp = fopen(TMPFILE, "r");
    CHECK(fp != NULL, "fopen for seek");
    CHECK(fseek(fp, 6, SEEK_SET) == 0, "fseek to offset 6");
    CHECK(ftell(fp) == 6, "ftell == 6");

    char chk[8];
    rd = fread(chk, 1, 7, fp);
    chk[rd] = '\0';
    CHECK(strcmp(chk, "VibeOS") == 0, "fread after fseek 'VibeOS'");

    /* SEEK_END */
    CHECK(fseek(fp, -6, SEEK_END) == 0, "fseek SEEK_END -6");
    char last[8];
    rd = fread(last, 1, 7, fp);
    last[rd] = '\0';
    CHECK(strncmp(last, "three.", 6) == 0, "fread from near end");

    CHECK(fclose(fp) == 0, "fclose after seeks");

    /* Append mode */
    fp = fopen(TMPFILE, "a");
    CHECK(fp != NULL, "fopen for append");
    wr = fwrite("APPEND\n", 1, 7, fp);
    CHECK(wr == 7, "fwrite APPEND");
    CHECK(fclose(fp) == 0, "fclose append");

    /* Verify append */
    fp = fopen(TMPFILE, "r");
    CHECK(fseek(fp, -7, SEEK_END) == 0, "seek to appended line");
    char app[8];
    fread(app, 1, 7, fp); app[7] = '\0';
    CHECK(strcmp(app, "APPEND\n") == 0, "appended content correct");
    fclose(fp);

    /* Cleanup */
    remove(TMPFILE);

    printf("=== test_fileio: %d failure(s) ===\n", failures);
    return failures ? 1 : 0;
}
