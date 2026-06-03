#ifndef VIBEOS_SYS_STAT_H
#define VIBEOS_SYS_STAT_H
#include <stdint.h>
#define S_ISREG(m) (((m) & 0xF000) == 0x8000)
#define S_ISDIR(m) (((m) & 0xF000) == 0x4000)
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
struct stat { uint32_t st_mode; uint64_t st_size; };
static inline int stat(const char *p, struct stat *s) { (void)p;(void)s; return -1; }

#ifdef __cplusplus
extern "C" {
#endif

int mkdir(const char *path, int mode);

#ifdef __cplusplus
}
#endif

#endif
