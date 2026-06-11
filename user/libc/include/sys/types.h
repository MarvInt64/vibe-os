#ifndef VIBEOS_SYS_TYPES_H
#define VIBEOS_SYS_TYPES_H
#include <stddef.h>
typedef long ssize_t;
typedef long off_t;
typedef unsigned long dev_t;
typedef unsigned long ino_t;
typedef unsigned int  nlink_t;
typedef int          pid_t;
typedef unsigned int uid_t;       /* user ID  — matches ext2 uint16 range */
typedef unsigned int gid_t;       /* group ID — matches ext2 uint16 range */
typedef unsigned int mode_t;      /* file permission bits */
typedef unsigned int useconds_t;
typedef long         time_t;
#endif
