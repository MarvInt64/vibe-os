#ifndef VIBEOS_DIRENT_H
#define VIBEOS_DIRENT_H
/* Minimal stub — DOOM only uses dirent for config file search which we skip. */
#include <stddef.h>
#define DT_REG 8
#define DT_DIR 4
struct dirent { char d_name[256]; unsigned char d_type; };
typedef struct { int dummy; } DIR;
static inline DIR *opendir(const char *p) { (void)p; return (DIR *)0; }
static inline struct dirent *readdir(DIR *d) { (void)d; return (struct dirent *)0; }
static inline int closedir(DIR *d) { (void)d; return 0; }
static inline void rewinddir(DIR *d) { (void)d; }
static inline void seekdir(DIR *d, long loc) { (void)d; (void)loc; }
static inline long telldir(DIR *d) { (void)d; return 0; }
#endif
