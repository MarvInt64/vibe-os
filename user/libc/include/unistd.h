#ifndef VIBEOS_UNISTD_H
#define VIBEOS_UNISTD_H
#include <stddef.h>
#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#ifdef __cplusplus
extern "C" {
#endif

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int     close(int fd);
void    _exit(int code) __attribute__((noreturn));
off_t   lseek(int fd, off_t offset, int whence);

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Cooperative scheduler yield (VibeOS apps must yield to share the CPU). */
void    sched_yield_(void);

#ifdef __cplusplus
}
#endif

#endif
