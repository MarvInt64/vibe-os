#ifndef VIBEOS_FCNTL_H
#define VIBEOS_FCNTL_H
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0100
#define O_TRUNC   01000
#define O_APPEND  02000
#define O_NONBLOCK 04000
/* Declare the underlying C helpers with C linkage so this header can be
 * safely included from both C and C++ translation units. */
#ifdef __cplusplus
extern "C" {
#endif
int vos_open_path(const char *path);
int vos_creat_path(const char *path);
#ifdef __cplusplus
}
#endif

/* open() dispatches to read-open or create-open depending on flags. */
static inline int open(const char *path, int flags, ...) {
    if ((flags & O_CREAT) || (flags & O_WRONLY) || (flags & O_RDWR))
        return vos_creat_path(path);
    return vos_open_path(path);
}
#endif
