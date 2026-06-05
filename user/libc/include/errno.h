#ifndef VIBEOS_ERRNO_H
#define VIBEOS_ERRNO_H

/* Values mirror kernel/include/syscall.h (enum syscall_error). Syscalls return
 * negative error codes; libc wrappers set errno to the positive value. */
#ifdef __cplusplus
extern "C" {
#endif

extern int errno;

#ifdef __cplusplus
}
#endif

#define EPERM        1
#define ENOENT       2
#define EIO          5
#define EBADF        9
#define ENOMEM      12
#define EACCES      13
#define EFAULT      14
#define EEXIST      17
#define ENOTDIR     20
#define EISDIR      21
#define EINVAL      22
#define EMFILE      24
#define EFBIG       27
#define ENOSPC      28
#define EROFS       30
#define ENAMETOOLONG 36
#define ENOSYS      38
#define EINTR        4  /* Interrupted system call */
#define ENFILE      23
#define ERANGE      34  /* Result too large */
#define EDOM        33  /* Math argument out of domain */

#endif
