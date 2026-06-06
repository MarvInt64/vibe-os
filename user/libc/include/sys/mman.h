#ifndef VIBEOS_SYS_MMAN_H
#define VIBEOS_SYS_MMAN_H

/* mmap / munmap stubs — VibeOS has no mmap syscall.
 * TCC's -run support uses mmap for JIT code pages; without it, -run
 * compiles but will fail at runtime (which is acceptable for now). */

#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define MAP_PRIVATE    0x02
#define MAP_ANONYMOUS  0x20

#define MAP_FAILED ((void *)0)

void *mmap(void *addr, unsigned long len, int prot, int flags, int fd, long off);
int   munmap(void *addr, unsigned long len);
int   mprotect(void *addr, unsigned long len, int prot);

#endif
