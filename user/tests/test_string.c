/*
 * test_string.c — TCC string operations and stdio formatting.
 * Compile inside VibeOS:  /bin/tcc test_string.c -o test_string && ./test_string
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int failures = 0;

#define CHECK(expr, msg) do { \
    if (!(expr)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("  ok: %s\n", msg); } \
} while(0)

int main(void) {
    printf("=== test_string: strings, printf, ctype ===\n");

    /* strlen */
    CHECK(strlen("") == 0, "strlen(\"\")==0");
    CHECK(strlen("hello") == 5, "strlen(\"hello\")==5");
    CHECK(strlen("hello world") == 11, "strlen(\"hello world\")==11");

    /* strcmp */
    CHECK(strcmp("abc", "abc") == 0, "strcmp equal");
    CHECK(strcmp("abc", "abd") < 0, "strcmp less");
    CHECK(strcmp("abd", "abc") > 0, "strcmp greater");
    CHECK(strcmp("", "") == 0, "strcmp empty");

    /* strncmp */
    CHECK(strncmp("abcdef", "abcxyz", 3) == 0, "strncmp first 3 match");
    CHECK(strncmp("abcd", "abce", 3) == 0, "strncmp limit 3");

    /* strcpy / strcat */
    char buf[64];
    strcpy(buf, "hello");
    CHECK(strcmp(buf, "hello") == 0, "strcpy");
    strcat(buf, " world");
    CHECK(strcmp(buf, "hello world") == 0, "strcat");
    strcat(buf, "!");
    CHECK(strcmp(buf, "hello world!") == 0, "strcat append");

    /* strchr */
    CHECK(strchr("hello", 'e') != NULL, "strchr found");
    CHECK(strchr("hello", 'z') == NULL, "strchr not found");
    CHECK(*strchr("hello", 'e') == 'e', "strchr correct char");

    /* strrchr */
    const char *last = strrchr("hello world", 'l');
    CHECK(last && *last == 'l' && last > strchr("hello world", 'l'),
          "strrchr finds last 'l'");

    /* strstr */
    CHECK(strstr("hello world", "world") != NULL, "strstr found");
    CHECK(strstr("hello", "xyz") == NULL, "strstr not found");

    /* memcpy */
    char src[] = "test data";
    char dst[16];
    memcpy(dst, src, sizeof(src));
    CHECK(strcmp(dst, "test data") == 0, "memcpy string");

    /* memmove (overlapping) */
    char overlap[] = "1234567890";
    memmove(overlap + 3, overlap, 5);  /* "1231234590" */
    CHECK(overlap[3] == '1' && overlap[4] == '2', "memmove overlap forward");
    char overlap2[] = "1234567890";
    memmove(overlap2, overlap2 + 3, 5);  /* "4567867890" */
    CHECK(overlap2[0] == '4' && overlap2[1] == '5', "memmove overlap backward");

    /* memset */
    char block[16];
    memset(block, 0x42, 16);
    int all42 = 1;
    for (int i = 0; i < 16; i++) if (block[i] != 0x42) all42 = 0;
    CHECK(all42, "memset 0x42");

    /* snprintf */
    char fmt[64];
    snprintf(fmt, sizeof(fmt), "%d + %d = %d", 2, 3, 5);
    CHECK(strcmp(fmt, "2 + 3 = 5") == 0, "snprintf integers");

    snprintf(fmt, sizeof(fmt), "pi ≈ %.2f", 3.14159);
    CHECK(strcmp(fmt, "pi ≈ 3.14") == 0, "snprintf float");

    snprintf(fmt, sizeof(fmt), "%#x", 255);
    CHECK(strcmp(fmt, "0xff") == 0, "snprintf hex");

    snprintf(fmt, sizeof(fmt), "%-4s", "x");
    CHECK(strcmp(fmt, "x   ") == 0, "snprintf left-align");

    /* snprintf truncation */
    snprintf(buf, 8, "hello world");
    CHECK(strlen(buf) < 8, "snprintf truncation");

    /* sscanf */
    int parsed_a, parsed_b;
    int n = sscanf("42 -7", "%d %d", &parsed_a, &parsed_b);
    CHECK(n == 2, "sscanf returns 2 fields");
    CHECK(parsed_a == 42 && parsed_b == -7, "sscanf integers");

    float pf;
    n = sscanf("3.14", "%f", &pf);
    CHECK(n == 1, "sscanf float count");
    CHECK(pf > 3.13 && pf < 3.15, "sscanf float value");

    /* ctype */
    CHECK(isalpha('A'), "isalpha('A')");
    CHECK(isalpha('z'), "isalpha('z')");
    CHECK(!isalpha('5'), "!isalpha('5')");
    CHECK(isdigit('0'), "isdigit('0')");
    CHECK(isdigit('9'), "isdigit('9')");
    CHECK(!isdigit('A'), "!isdigit('A')");
    CHECK(isspace(' '), "isspace(' ')");
    CHECK(isspace('\n'), "isspace('\\n')");
    CHECK(toupper('a') == 'A', "toupper('a')=='A'");
    CHECK(tolower('Z') == 'z', "tolower('Z')=='z'");

    printf("=== test_string: %d failure(s) ===\n", failures);
    return failures ? 1 : 0;
}
