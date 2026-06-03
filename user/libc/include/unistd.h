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
pid_t        getpid(void);
pid_t        getppid(void);
unsigned int sleep(unsigned int seconds);
int          usleep(useconds_t usec);
int          unlink(const char *pathname);
int          rmdir(const char *pathname);
char        *getcwd(char *buf, size_t size);
int          chdir(const char *path);

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Cooperative scheduler yield (VibeOS apps must yield to share the CPU). */
void    sched_yield_(void);

/* Read one directory entry at index `idx` from directory `path`.
 * Returns 1 if an entry was found and name_out filled, 0 if end of dir,
 * <0 on error.  *type_out is set to 1=file, 2=directory. */
int readdir_at(const char *path, int idx, char *name_out, int cap, int *type_out);

#ifdef __cplusplus
}
#endif

#endif
