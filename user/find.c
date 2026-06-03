#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *g_name = NULL;
static int        g_type  = 0; /* 0=any, 'f'=file, 'd'=dir */

static int match_glob(const char *name, const char *pat) {
    if (!pat || !pat[0]) return 1;
    if (!pat[1] && pat[0] == '*') return 1;
    while (*pat && *name) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;
            while (*name) { if (match_glob(name, pat)) return 1; name++; }
            return 0;
        }
        if (*pat != '?' && *pat != *name) return 0;
        pat++; name++;
    }
    while (*pat == '*') pat++;
    return !*pat && !*name;
}

static void find_dir(const char *path) {
    char entry[256], child[512];
    int idx = 0, type;
    while (readdir_at(path, idx++, entry, sizeof(entry), &type) > 0) {
        if (!entry[0]) break;
        if (entry[0] == '.' && (!entry[1] || (entry[1] == '.' && !entry[2]))) continue;

        int plen = (int)strlen(path);
        int elen = (int)strlen(entry);
        if (plen + elen + 2 > (int)sizeof(child)) continue;
        memcpy(child, path, plen);
        child[plen] = '/';
        memcpy(child + plen + 1, entry, elen + 1);

        int is_dir = (type == 2);
        int print = 1;
        if (g_type == 'f' && is_dir)  print = 0;
        if (g_type == 'd' && !is_dir) print = 0;
        if (g_name && !match_glob(entry, g_name)) print = 0;
        if (print) puts(child);

        if (is_dir) find_dir(child);
    }
}

int main(int argc, char *argv[]) {
    const char *root = ".";
    int i = 1;
    if (i < argc && argv[i][0] != '-') root = argv[i++];
    for (; i < argc; i++) {
        if (strcmp(argv[i], "-name") == 0 && i+1 < argc) g_name = argv[++i];
        else if (strcmp(argv[i], "-type") == 0 && i+1 < argc) g_type = argv[++i][0];
        else { fprintf(stderr, "find: unknown: %s\n", argv[i]); return 1; }
    }
    find_dir(root);
    return 0;
}
