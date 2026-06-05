#ifndef VIBEOS_SYS_STAT_H
#define VIBEOS_SYS_STAT_H
#include <stdint.h>

/*
 * Bit tests for the st_mode field returned by stat().
 * The upper four bits encode the file type; the lower nine are rwxrwxrwx.
 * These values match ext2 and POSIX (S_IFREG = 0x8000, S_IFDIR = 0x4000).
 */
#define S_ISREG(m)  (((m) & 0xF000u) == 0x8000u)  /* regular file */
#define S_ISDIR(m)  (((m) & 0xF000u) == 0x4000u)  /* directory    */

/* Permission bit constants (POSIX names). */
#define S_IRUSR 0400u   /* owner read  */
#define S_IWUSR 0200u   /* owner write */
#define S_IXUSR 0100u   /* owner exec  */
#define S_IRGRP 0040u   /* group read  */
#define S_IWGRP 0020u   /* group write */
#define S_IXGRP 0010u   /* group exec  */
#define S_IROTH 0004u   /* other read  */
#define S_IWOTH 0002u   /* other write */
#define S_IXOTH 0001u   /* other exec  */
#define S_IRWXU (S_IRUSR|S_IWUSR|S_IXUSR)
#define S_IRWXG (S_IRGRP|S_IWGRP|S_IXGRP)
#define S_IRWXO (S_IROTH|S_IWOTH|S_IXOTH)

/*
 * File metadata returned by stat().
 * st_uid / st_gid are the owner user-/group-ID stored in the ext2 inode.
 */
struct stat {
    uint32_t st_mode;   /* file type + permission bits */
    uint64_t st_size;   /* byte size */
    uint16_t st_uid;    /* owner user ID  */
    uint16_t st_gid;    /* owner group ID */
};

/* These are C-linkage symbols in libc (sys.c), so wrap the declarations in
 * extern "C" for C++ callers — otherwise the call site is name-mangled and
 * fails to link against the unmangled libc symbol. */
#ifdef __cplusplus
extern "C" {
#endif

int stat(const char *path, struct stat *s);

/*
 * chmod — change the permission bits of 'path'.
 * Only the file owner or root may call this.
 * 'mode' is the new octal permission bits (0–0777), e.g. 0644 or 0755.
 * Returns 0 on success, -1 on error (EPERM, ENOENT).
 */
int chmod(const char *path, int mode);

/*
 * chown — change the owner and group of 'path'.
 * Only root (uid 0) may call this.
 * Returns 0 on success, -1 on error.
 */
int chown(const char *path, unsigned int uid, unsigned int gid);

int mkdir(const char *path, int mode);

#ifdef __cplusplus
}
#endif

#endif
