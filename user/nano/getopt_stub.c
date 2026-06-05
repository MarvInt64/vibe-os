#include <string.h>
int   optind = 1;
char *optarg = 0;
int getopt(int argc, char *const argv[], const char *optstring) {
    (void)optstring;
    if (optind >= argc) return -1;
    return -1;
}
struct option { const char *name; int has_arg; int *flag; int val; };
int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex) {
    (void)longopts;
    if (longindex) *longindex = -1;
    return getopt(argc, argv, optstring);
}
