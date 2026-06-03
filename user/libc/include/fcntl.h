#ifndef VIBEOS_FCNTL_H
#define VIBEOS_FCNTL_H
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0100
#define O_TRUNC   01000
#define O_APPEND  02000
#define O_NONBLOCK 04000
/* open() maps to SYS_OPEN (read) or SYS_CREAT (write). */
static inline int open(const char *path, int flags, ...) {
    extern int vos_open_path(const char *);
    extern int vos_creat_path(const char *);
    if (flags & O_WRONLY) return vos_creat_path(path);
    return vos_open_path(path);
}
#endif
