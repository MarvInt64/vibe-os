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

/* access — check file accessibility. Stub: always succeeds. */
static inline int access(const char *path, int mode) {
    (void)path; (void)mode;
    return 0;  /* assume always accessible */
}

/* isatty — check if fd is a terminal. Stub: STDIN/STDOUT/STDERR = yes. */
static inline int isatty(int fd) { return (fd >= 0 && fd <= 2) ? 1 : 0; }

/* dup2 — duplicate a file descriptor. Stub: no-op. */
int dup2(int oldfd, int newfd);

/* execvp — execute a program (stub: no fork/exec on VibeOS) */
static inline int execvp(const char *file, char *const argv[]) {
    (void)file; (void)argv;
    return -1;
}

#define X_OK  1
#define W_OK  2
#define R_OK  4
#define F_OK  0

/* getopt / getopt_long */
extern int   optind;
extern char *optarg;
int getopt(int argc, char *const argv[], const char *optstring);

struct option { const char *name; int has_arg; int *flag; int val; };
#define no_argument        0
#define required_argument  1
int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex);

/* User/group identity.
 * getuid/getgid — return the effective uid/gid of the calling process.
 * setuid       — change effective uid.  Root (uid 0) may set any uid;
 *                other users may only re-set their own uid.
 *                Returns 0 on success, -EPERM on permission error. */
uid_t getuid(void);
gid_t getgid(void);
int   setuid(uid_t uid);

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
